
#include <atomic>
#include <chrono>
#include <cstring>
#include <print>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <pqxx/pqxx>

#include "include/DBReader.hpp"
#include "include/Defines.hpp"
#include "include/HttpServer.hpp"
#include "include/Json.hpp"

namespace HttpServer::Database
{
  // At the bottom of dbreader.cpp
  template class HttpServer::Database::DBReader<8>;
  template class HttpServer::Database::DBReader<DBREADERS_QUEUE_SIZE>;

  template <size_t QUEUE_SIZE>
  DBReader<QUEUE_SIZE>::DBReader() : queues(HttpServer::N_WORKERS), signals(HttpServer::N_WORKERS), selector(0)
  {
    for(size_t i = 0; i < HttpServer::N_WORKERS; i++)
    {
      workers.emplace_back([this, i](std::stop_token stp) { this->worker(stp, static_cast<unsigned>(i)); });
    }
  };

  template <size_t QUEUE_SIZE>
  DBReader<QUEUE_SIZE>::~DBReader()
  {
    for(auto &thread : workers)
    {
      thread.request_stop();
    }

    for(auto &signal : signals)
    {
      signal.value.fetch_add(1, std::memory_order_release);
      signal.value.notify_all();
    }

    for(auto &thread : workers)
    {
      if(thread.joinable())
      {
        thread.join();
      }
    }
  }

  template <size_t QUEUE_SIZE>
  void DBReader<QUEUE_SIZE>::awake_all()
  {
    for(auto &signal : this->signals)
    {
      signal.value.fetch_add(1, std::memory_order_release);
      signal.value.notify_one();
    }
  }

  template <size_t QUEUE_SIZE>
  void DBReader<QUEUE_SIZE>::enqueue(query_desc &view)
  {
    const auto id = this->selector.fetch_add(1, std::memory_order_seq_cst);
    if(!this->queues[id % HttpServer::N_WORKERS].enqueue(view)) // this will NEVER happen
    {
      HttpServer::socket->release_rx_packet(view.packet);
      std::print("failed to enqueue in DBReader");
    }
  }

  template <size_t QUEUE_SIZE>
  void DBReader<QUEUE_SIZE>::worker(std::stop_token stp, unsigned thr_id)
  {
    auto &queue = this->queues[thr_id];
    auto &signal = this->signals[thr_id];

    if(DATABASE_URL == nullptr || *DATABASE_URL == '\0')
    {
      throw std::runtime_error{ "Set DATABASE_URL to your local Conduit test database; this integration test connects over a Unix socket." };
    }

    pqxx::connection db = pqxx::connection(DATABASE_URL);
    {
      pqxx::work txn{ db };
      txn.exec("SET timezone = 'UTC';");
      txn.commit(); // Commit the session configuration
    }
    db.prepare("get_health", "SELECT 1;");
    db.prepare("get_feed",
               R"(
                    SELECT p.id, p.body, p.created_at, u.username,
                     (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
                    FROM posts p JOIN users u ON u.id = p.user_id
                    ORDER BY p.created_at DESC, p.id DESC LIMIT 20;
                  )");
    db.prepare("get_post_by_id",
               R"(
                    SELECT p.id, p.body, p.created_at, u.username,
                     (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
                    FROM posts p JOIN users u ON u.id = p.user_id
                    WHERE p.id = $1;
                 )");

    db.prepare("create_post",
               R"(
                    INSERT INTO posts (user_id, body)
                    VALUES ($1, $2) RETURNING id, created_at;
                 )");

    db.prepare("create_like",
               R"(
                    INSERT INTO likes (user_id, post_id)
                    VALUES ($1, $2)
                    ON CONFLICT (user_id, post_id) DO NOTHING
                 )");

    while(!stp.stop_requested())
    {
      const auto observed = signal.value.load(std::memory_order_acquire);

      query_desc endpt;

      bool is_valid = queue.peek(endpt);
      if(!is_valid)
      {
        signal.value.wait(observed, std::memory_order_acquire);
        continue;
      }

      pqxx::work txn(db);
      std::vector<pqxx::pipeline::query_id> query_ids;
      std::vector<pqxx::result> results;

      struct like_result
      {
        uint64_t user_id{};
        uint64_t post_id{};
        bool post_exists{ true };
        bool created{};
      };

      std::vector<like_result> like_results;

      unsigned pipe_ct = 0;

      {
        pqxx::pipeline pipe(txn);

        for(; is_valid && pipe_ct < HttpServer::MAX_QUERY_PIPELINE_DEPTH; is_valid = queue.peek(endpt), pipe_ct++)
        {
          switch(endpt.query)
          {
          case Query::GetHealth:
            query_ids.push_back(pipe.insert("EXECUTE get_health;"));
            break;
          case Query::GetFeed:
            query_ids.push_back(pipe.insert("EXECUTE get_feed;"));
            break;
          case Query::GetPostById:
            query_ids.push_back(pipe.insert("EXECUTE get_post_by_id(" + std::to_string(endpt.post_id) + ");"));
            break;
          case Query::CreatePost:
            query_ids.push_back(pipe.insert("EXECUTE create_post(" + std::to_string(endpt.user_id) + ',' + txn.quote(endpt.body) + ");"));
            break;
          case Query::CreateLike:
            like_results.push_back({ endpt.user_id, endpt.post_id });
            break;
          default:
            break;
          }
        }

        if(!query_ids.empty())
        {
          pipe.complete();
          results.reserve(query_ids.size());

          for(const auto query_id : query_ids)
          {
            results.emplace_back(pipe.retrieve(query_id));
          }
        }
      }

      if(pipe_ct > 0)
      {
        for(auto &like : like_results)
        {
          try
          {
            pqxx::subtransaction like_txn{ txn };

            const auto like_result = like_txn.exec_prepared("create_like", like.user_id, like.post_id);
            const auto affected_rows = like_result.affected_rows();

            like.created = affected_rows == 1;

            like_txn.commit();
          }
          catch(const pqxx::sql_error &error)
          {
            like.post_exists = false;
          }
        }

        txn.commit();

        bool valid = queue.dequeue(endpt);
        unsigned ct = 0;
        unsigned result_ct = 0;
        unsigned like_ct = 0;

        for(; valid && ct < pipe_ct; valid = queue.dequeue(endpt), ct++)
        {
          const pqxx::result *drained = nullptr;

          bool like_post_exists = true;
          bool like_created = false;

          if(endpt.query == Query::CreateLike)
          {
            like_post_exists = like_results[like_ct].post_exists;
            like_created = like_results[like_ct].created;
            like_ct++;
          }
          else
          {
            drained = &results[result_ct++];
          }

          size_t len = 0;
          io_packet_desc *tx_desc = HttpServer::socket->wait_for_tx_packet();

          if(tx_desc == nullptr)
          {
            std::println(stderr, "DBReader - failed to obtain tx_desc for client_fd {}", endpt.packet->client_fd);
            continue;
          }

          auto *ptr = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;

          // A foreign-key violation means the requested post does not exist.
          if(!like_post_exists || (endpt.query == Query::GetPostById && drained->empty())) [[unlikely]]
          {
            len = Http::craft_http_error_response(ptr, Http::ErrorCode::PostNotFound);
            tx_desc->len = static_cast<uint32_t>(len);
            tx_desc->client_fd = endpt.packet->client_fd;

            HttpServer::socket->release_tx_packet(tx_desc);
            HttpServer::socket->release_rx_packet(endpt.packet);
            continue;
          }

          constexpr std::string_view HTTP_OK_200 = "HTTP/1.1 200 OK\r\n";
          constexpr std::string_view HTTP_OK_201 = "HTTP/1.1 201 OK\r\n";
          constexpr std::string_view HTTP_CONTENT_TYPE = "Content-Type: application/json; charset=utf-8\r\n";
          constexpr std::string_view HTTP_CONTENT_LEN = "Content-Length: ";
          constexpr std::string_view HTTP_CONNECTION = "\r\nConnection: keep-alive\r\n\r\n";

          const auto now_sec = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

          // A repeated LIKE affects zero rows and must return 200. A new LIKE must return 201.
          if(endpt.query == Query::CreatePost || (endpt.query == Query::CreateLike && like_created))
          {
            std::memcpy(&ptr[len], HTTP_OK_201.data(), HTTP_OK_201.size()), len += HTTP_OK_201.size();
          }
          else
          {
            std::memcpy(&ptr[len], HTTP_OK_200.data(), HTTP_OK_200.size()), len += HTTP_OK_200.size();
          }

          const auto date = std::format("Date: {:%a, %d %b %Y %T} GMT \r\n", now_sec);
          std::memcpy(&ptr[len], date.data(), date.size()), len += date.size();
          std::memcpy(&ptr[len], HTTP_CONTENT_TYPE.data(), HTTP_CONTENT_TYPE.size()), len += HTTP_CONTENT_TYPE.size();
          std::memcpy(&ptr[len], HTTP_CONTENT_LEN.data(), HTTP_CONTENT_LEN.size()), len += HTTP_CONTENT_LEN.size();

          auto *len_ptr = &ptr[len];
          std::memset(&ptr[len], ' ', 8), len += 8;
          std::memcpy(&ptr[len], HTTP_CONNECTION.data(), HTTP_CONNECTION.size()), len += HTTP_CONNECTION.size();

          size_t payload_len = 0;

          JSON::Response res;
          switch(endpt.query)
          {
          case Query::GetHealth:
          {
            const uint64_t up_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - HttpServer::start_time).count());
            res = JSON::health_response_json(*drained, up_time, &ptr[len]);
            payload_len = res.size;
          }
          break;
          case Query::GetFeed:
          {
            res = JSON::feed_response_json(*drained, &ptr[len]);
            payload_len = res.size;
          }
          break;
          case Query::GetPostById:
          {
            res = JSON::post_response_json(*drained, &ptr[len]);
            payload_len = res.size;
          }
          break;
          case Query::CreatePost:
          {
            res = JSON::create_post_response_json(*drained, endpt.body, std::string_view{ endpt.username.data(), std::strlen(endpt.username.data()) }, &ptr[len]);
            payload_len = res.size;
          }
          break;
          case Query::CreateLike:
          {
            res = JSON::like_response_json(like_created, static_cast<int64_t>(endpt.post_id), &ptr[len]);
            payload_len = res.size;
          }
          break;
          }

          std::to_chars(len_ptr, len_ptr + 8, payload_len);
          len += payload_len;

#ifndef NDEBUG
          auto *ptr_o = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;
          std::print("SENDING PACKET : \n");
          for(size_t i = 0; i < len; i++)
          {
            std::print("{}", ptr_o[i]);
          }
          std::print("\n");
#endif

          tx_desc->len = static_cast<uint32_t>(len);
          tx_desc->client_fd = endpt.packet->client_fd;

          HttpServer::socket->release_tx_packet(tx_desc);
          HttpServer::socket->release_rx_packet(endpt.packet);
        }

        queue.sync();
      }
    }
  }

} // namespace HttpServer::Database
