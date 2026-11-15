
#include <algorithm>
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
  DBReader<QUEUE_SIZE>::DBReader() : queues(HttpServer::N_WORKERS), signals(HttpServer::N_WORKERS)
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
  void DBReader<QUEUE_SIZE>::awake(const unsigned thread_id)
  {
    signals[thread_id].value.fetch_add(1, std::memory_order_release);
    signals[thread_id].value.notify_one();
  }

  template <size_t QUEUE_SIZE>
  void DBReader<QUEUE_SIZE>::enqueue(query_desc &view, const unsigned thread_id)
  {
    if(!this->queues[thread_id].enqueue(view)) // this will NEVER happen
    {
      io_packet_desc *tx_desc = HttpServer::socket->wait_for_tx_packet();
      auto *ptr = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;

      // Malformed DB row or bug in a serializer. Reply 500 and drop the request instead of taking down the whole DB worker thread.
      const auto len = Http::craft_http_error_response(ptr, Http::ErrorCode::InternalServerError);
      tx_desc->len = static_cast<uint32_t>(len);
      tx_desc->client_fd = view.packet->client_fd;
      HttpServer::socket->wait_for_tx_release(tx_desc);
      HttpServer::socket->wait_for_rx_release(view.packet);
    }
  }

  namespace
  {
    enum class request_error : std::uint8_t
    {
      None,
      PostNotFound,
      InternalServerError
    };

    struct request_state
    {
      pqxx::result result{};

      request_error error{ request_error::None };

      bool like_created{};
    };

  } // namespace

  template <size_t QUEUE_SIZE>
  void DBReader<QUEUE_SIZE>::worker(std::stop_token stp, unsigned thr_id)
  {
    auto &queue = this->queues[thr_id];
    auto &signal = this->signals[thr_id];

    if(DATABASE_URL == nullptr || *DATABASE_URL == '\0')
    {
      throw std::runtime_error{ "Set DATABASE_URL to your Million Users Challenge PostgreSQL database." };
    }

    pqxx::connection db{ DATABASE_URL };

    {
      pqxx::work txn{ db };
      txn.exec("SET timezone = 'UTC'");
      txn.commit();
    }

    db.prepare("get_health", "SELECT 1");
    db.prepare("get_feed", "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) "
                           "FROM likes l WHERE l.post_id = p.id)::int AS like_count "
                           "FROM posts p JOIN users u ON u.id = p.user_id "
                           "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    db.prepare("get_post_by_id", "SELECT p.id, p.body, p.created_at, u.username, "
                                 "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
                                 "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1");
    db.prepare("create_post", " INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at");
    db.prepare("create_like", " INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING");

    // worker storage
    std::vector<query_desc> batch;
    std::vector<std::size_t> pending;

    std::vector<std::string> sql_queries;
    std::vector<pqxx::pipeline::query_id> query_ids;

    std::vector<pqxx::result> attempt_results;
    std::vector<request_state> states;

    constexpr auto MAX_DEPTH = HttpServer::MAX_QUERY_PIPELINE_DEPTH;

    batch.reserve(MAX_DEPTH);
    pending.reserve(MAX_DEPTH);

    sql_queries.reserve(MAX_DEPTH);
    query_ids.reserve(MAX_DEPTH);

    attempt_results.reserve(MAX_DEPTH);
    states.reserve(MAX_DEPTH);

    char date_buff[64];

    while(!stp.stop_requested())
    {
      const auto observed = signal.value.load(std::memory_order_acquire);

      query_desc probe{};

      if(!queue.peek(probe))
      {
        signal.value.wait(observed, std::memory_order_acquire);
        continue;
      }

      batch.clear();

      while(batch.size() < MAX_DEPTH)
      {
        batch.push_back(probe);

        if(batch.size() >= MAX_DEPTH || !queue.peek(probe))
        {
          break;
        }
      }

      const std::size_t batch_size = batch.size();

      if(batch_size == 0)
      {
        continue;
      }

      states.clear();
      states.resize(batch_size);

      attempt_results.clear();
      attempt_results.resize(batch_size);

      pending.clear();

      for(std::size_t i = 0; i < batch_size; ++i)
      {
        pending.push_back(i);
      }

      while(!pending.empty())
      {
        // Sentinel means the entire attempt succeeded.
        std::size_t failed_index = batch_size;

        bool failed_post_not_found = false;

        query_ids.clear();
        sql_queries.clear();

        {
          pqxx::work txn{ db };

          // ------------------------------------------------
          // Construct the SQL for the pending requests.
          //
          // Construct these strings before creating the
          // pipeline so quoting does not interact with
          // an active transaction_focus.
          // ------------------------------------------------

          for(const std::size_t idx : pending)
          {
            const auto &req = batch[idx];

            switch(req.query)
            {
            case Query::GetHealth:
            {
              sql_queries.emplace_back("EXECUTE get_health");
            }
            break;

            case Query::GetFeed:
            {
              sql_queries.emplace_back("EXECUTE get_feed");
            }
            break;

            case Query::GetPostById:
            {
              std::string sql = "EXECUTE get_post_by_id(";

              sql += std::to_string(req.post_id);
              sql += ')';

              sql_queries.emplace_back(std::move(sql));
            }
            break;

            case Query::CreatePost:
            {
              std::string sql = "EXECUTE create_post(";

              sql += std::to_string(req.user_id);
              sql += ',';
              sql += txn.quote(req.body);
              sql += ')';

              sql_queries.emplace_back(std::move(sql));
            }
            break;

            case Query::CreateLike:
            {
              std::string sql = "EXECUTE create_like(";

              sql += std::to_string(req.user_id);
              sql += ',';
              sql += std::to_string(req.post_id);
              sql += ')';

              sql_queries.emplace_back(std::move(sql));
            }
            break;

            default:
              std::terminate();
            }
          }

          // ------------------------------------------------
          // Execute the entire pending batch.
          // ------------------------------------------------

          {
            pqxx::pipeline pipe{ txn };

            // Retain the entire batch before sending. This prevents the default small retention capacity from splitting our application batch.
            pipe.retain(static_cast<int>(sql_queries.size()));

            for(const auto &sql : sql_queries)
            {
              // IMPORTANT: No trailing semicolon here. libpqxx inserts its own separators.
              query_ids.push_back(pipe.insert(sql));
            }

            // Send/complete all accumulated queries.
            pipe.complete();

            // Retrieve strictly in insertion order. Stop immediately at the first SQL error.
            for(std::size_t pos = 0; pos < pending.size(); ++pos)
            {
              const std::size_t idx = pending[pos];

              try
              {
                attempt_results[idx] = pipe.retrieve(query_ids[pos]);
              }
              catch(const pqxx::sql_error &error)
              {
                failed_index = idx;

                const auto sqlstate = error.sqlstate();

                const std::string_view sqlstate_view{ sqlstate.data(), sqlstate.size() };

                const std::string_view message{ error.what() };

                // Only the post FK violation means HTTP 404 according to the SPEC.
                failed_post_not_found = batch[idx].query == Query::CreateLike && sqlstate_view == "23503" && message.find("likes_post_id_fkey") != std::string_view::npos;

                if(!failed_post_not_found)
                {
                  std::println(stderr, "[db] SQL error: query={} sqlstate={} error={}", static_cast<int>(batch[idx].query), sqlstate_view, error.what());
                }

                // Do not retrieve subsequent query IDs. Their statements may have been skipped because the transaction was aborted.
                break;
              }
            }
          } // Pipeline is destroyed before the transaction.

          if(failed_index == batch_size)
          {
            // Nothing failed.
            // Commit ONCE for the entire batch. No response is published before this commit.
            txn.commit();

            // Only committed results become visible to
            // the HTTP response path.
            for(const std::size_t idx : pending)
            {
              states[idx].result = std::move(attempt_results[idx]);

              if(batch[idx].query == Query::CreateLike)
              {
                // INSERT affected_rows: 1 = new like 0 = already liked
                states[idx].like_created = states[idx].result.affected_rows() == 1;
              }
            }
          }

        } // Failed transaction is rolled back here.

        // ----------------------------------------------------
        // Successful attempt: all pending requests committed.
        // ----------------------------------------------------

        if(failed_index == batch_size)
        {
          break;
        }

        // ----------------------------------------------------
        // Failed attempt: classify only the offending request.
        // ----------------------------------------------------

        states[failed_index].error = failed_post_not_found ? request_error::PostNotFound : request_error::InternalServerError;

        // Remove ONLY the offending request. All other requests are retried in original order.
        const auto failed_it = std::ranges::find(pending, failed_index);

        if(failed_it == pending.end())
        {
          std::println(stderr, "[fatal] DB retry invariant violated");

          std::terminate();
        }

        pending.erase(failed_it);
      }

      // --------------------------------------------------------
      // All requests now have either:
      //
      // 1. A committed result, or
      // 2. An individually classified error.
      //
      // Dequeue exactly the original batch.
      // --------------------------------------------------------

      for(std::size_t ct = 0; ct < batch_size; ++ct)
      {
        query_desc dequeued{};

        if(!queue.dequeue(dequeued))
        {
          std::println(stderr, "[fatal] DB queue peek/dequeue invariant violated");

          std::terminate();
        }

        const auto &req = batch[ct];
        const auto &state = states[ct];

        std::size_t len = 0;

        io_packet_desc *tx_desc = HttpServer::socket->wait_for_tx_packet();

        auto *ptr = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;

        // ----------------------------------------------------
        // Error responses.
        // ----------------------------------------------------

        if(state.error == request_error::InternalServerError)
        {
          len = Http::craft_http_error_response(ptr, Http::ErrorCode::InternalServerError);

          tx_desc->len = static_cast<uint32_t>(len);
          tx_desc->client_fd = req.packet->client_fd;

          HttpServer::socket->wait_for_tx_release(tx_desc);
          HttpServer::socket->wait_for_rx_release(req.packet);

          continue;
        }

        if(state.error == request_error::PostNotFound || (req.query == Query::GetPostById && state.result.empty())) [[unlikely]]
        {
          len = Http::craft_http_error_response(ptr, Http::ErrorCode::PostNotFound);

          tx_desc->len = static_cast<uint32_t>(len);
          tx_desc->client_fd = req.packet->client_fd;

          HttpServer::socket->wait_for_tx_release(tx_desc);
          HttpServer::socket->wait_for_rx_release(req.packet);

          continue;
        }

        // ----------------------------------------------------
        // HTTP response headers.
        // ----------------------------------------------------

        constexpr std::string_view HTTP_OK_200 = "HTTP/1.1 200 OK\r\n";
        constexpr std::string_view HTTP_OK_201 = "HTTP/1.1 201 Created\r\n";
        constexpr std::string_view HTTP_CONTENT_TYPE = "Content-Type: application/json; charset=utf-8\r\n";
        constexpr std::string_view HTTP_CONTENT_LEN = "Content-Length: ";
        constexpr std::string_view HTTP_CONNECTION = "\r\nConnection: keep-alive\r\n\r\n";

        if(req.query == Query::CreatePost || (req.query == Query::CreateLike && state.like_created))
        {
          std::memcpy(&ptr[len], HTTP_OK_201.data(), HTTP_OK_201.size()), len += HTTP_OK_201.size();
        }
        else
        {
          std::memcpy(&ptr[len], HTTP_OK_200.data(), HTTP_OK_200.size()), len += HTTP_OK_200.size();
        }

        const auto now_sec = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const std::time_t time_t_now = std::chrono::system_clock::to_time_t(now_sec);
        const auto date = get_http_date(time_t_now, &date_buff[0]);

        std::memcpy(&ptr[len], date.data(), date.size()), len += date.size();
        std::memcpy(&ptr[len], HTTP_CONTENT_TYPE.data(), HTTP_CONTENT_TYPE.size()), len += HTTP_CONTENT_TYPE.size();
        std::memcpy(&ptr[len], HTTP_CONTENT_LEN.data(), HTTP_CONTENT_LEN.size()), len += HTTP_CONTENT_LEN.size();

        auto *len_ptr = &ptr[len];
        std::memset(&ptr[len], ' ', 8), len += 8;
        std::memcpy(&ptr[len], HTTP_CONNECTION.data(), HTTP_CONNECTION.size()), len += HTTP_CONNECTION.size();

        size_t payload_len = 0;
        JSON::Response res;
        try
        {
          switch(req.query)
          {
          case Query::GetHealth:
          {
            const uint64_t up_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - HttpServer::start_time).count());
            res = JSON::health_response_json(state.result, up_time, &ptr[len]);
            payload_len = res.size;
          }
          break;
          case Query::GetFeed:
          {
            res = JSON::feed_response_json(state.result, &ptr[len]);
            payload_len = res.size;
          }
          break;
          case Query::GetPostById:
          {
            res = JSON::post_response_json(state.result, &ptr[len]);
            payload_len = res.size;
          }
          break;
          case Query::CreatePost:
          {
            res = JSON::create_post_response_json(state.result, req.body, std::string_view{ req.username.data(), std::strlen(req.username.data()) }, &ptr[len]);
            payload_len = res.size;
          }
          break;
          case Query::CreateLike:
          {
            res = JSON::like_response_json(state.like_created, static_cast<int64_t>(req.post_id), &ptr[len]);
            payload_len = res.size;
          }
          break;

          default:
            throw std::runtime_error{ "Unknown database query during serialization" };
          }
        }
        catch(const std::exception &e)
        {
          // Serialization failure must not kill the DB worker.
          len = Http::craft_http_error_response(ptr, Http::ErrorCode::InternalServerError);

          tx_desc->len = static_cast<uint32_t>(len);
          tx_desc->client_fd = req.packet->client_fd;

          HttpServer::socket->wait_for_tx_release(tx_desc);
          HttpServer::socket->wait_for_rx_release(req.packet);

          std::println(stderr, "[error] JSON serialization failed for query {}: {}", static_cast<int>(req.query), e.what());

          continue;
        }

        // Write Content-Length into the reserved 8-byte field.
        std::to_chars(len_ptr, len_ptr + 8, payload_len);

        len += payload_len;

        tx_desc->len = static_cast<uint32_t>(len);
        tx_desc->client_fd = req.packet->client_fd;

        HttpServer::socket->wait_for_tx_release(tx_desc);
        HttpServer::socket->wait_for_rx_release(req.packet);
      }

      // Publish the consumed queue position.
      queue.sync();
    }
  }

} // namespace HttpServer::Database
