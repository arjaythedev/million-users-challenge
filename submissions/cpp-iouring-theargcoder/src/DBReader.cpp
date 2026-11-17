#include <libdeflate.h>
#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "include/DBReader.hpp"
#include "include/Defines.hpp"
#include "include/HttpServer.hpp"
#include "include/Json.hpp"
#include "include/Socket.hpp"

namespace
{
  // ===========================================================================
  // LIBPQ OWNERSHIP
  // ===========================================================================

  struct PGConnectionDeleter
  {
    void operator()(PGconn *db) const noexcept
    {
      if(db != nullptr)
        PQfinish(db);
    }
  };

  struct PGResultDeleter
  {
    void operator()(PGresult *result) const noexcept
    {
      if(result != nullptr)
        PQclear(result);
    }
  };

  using PGConnection = std::unique_ptr<PGconn, PGConnectionDeleter>;
  using PGResult = std::unique_ptr<PGresult, PGResultDeleter>;

  // ===========================================================================
  // CONNECTION INITIALIZATION
  // ===========================================================================

  void prepare_statement(PGconn *db, const char *name, const char *sql, int parameter_count)
  {
    PGResult result{ PQprepare(db, name, sql, parameter_count, nullptr) };

    if(!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK)
    {
      const char *error = result ? PQresultErrorMessage(result.get()) : PQerrorMessage(db);
      throw std::runtime_error(error != nullptr ? error : "PostgreSQL prepare failed");
    }
  }

  PGConnection connect_database()
  {
    if(HttpServer::DATABASE_URL == nullptr || *HttpServer::DATABASE_URL == '\0')
      throw std::runtime_error("Set DATABASE_URL");

    PGConnection db{ PQconnectdb(HttpServer::DATABASE_URL) };

    if(!db || PQstatus(db.get()) != CONNECTION_OK)
    {
      const char *error = db ? PQerrorMessage(db.get()) : "PQconnectdb failed";
      throw std::runtime_error(error != nullptr ? error : "PostgreSQL connection failed");
    }

    // The connection is still in blocking mode here. Keep session setup and
    // statement preparation outside pipeline mode.
    PGResult timezone{ PQexec(db.get(), "SET timezone = 'UTC';") };

    if(!timezone || PQresultStatus(timezone.get()) != PGRES_COMMAND_OK)
    {
      const char *error = timezone ? PQresultErrorMessage(timezone.get()) : PQerrorMessage(db.get());
      throw std::runtime_error(error != nullptr ? error : "Could not set PostgreSQL timezone");
    }

    return db;
  }

  void prepare_read_statements(PGconn *db)
  {
    prepare_statement(db, "get_health", "SELECT 1;", 0);
    prepare_statement(
        db, "get_feed",
        R"( SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20;)",
        0);
    prepare_statement(
        db, "get_post_by_id",
        R"( SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1;)",
        1);
  }

  void prepare_write_statements(PGconn *db)
  {
    prepare_statement(db, "create_post", R"( INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at;)", 2);
    prepare_statement(db, "create_like", R"( INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING)", 2);
  }

  void enter_pipeline_mode(PGconn *db)
  {
    // Pipeline mode should be used with a nonblocking libpq connection.
    if(PQsetnonblocking(db, 1) != 0)
      throw std::runtime_error(PQerrorMessage(db));

    if(PQenterPipelineMode(db) != 1)
      throw std::runtime_error(PQerrorMessage(db));
  }

  // ===========================================================================
  // INTEGER PARAMETER FORMATTING
  // ===========================================================================

  const char *format_u64(uint64_t value, char *buffer, size_t capacity)
  {
    if(capacity < 2)
      throw std::runtime_error("Integer parameter buffer is too small");

    const auto [end, error] = std::to_chars(buffer, buffer + capacity - 1, value);

    if(error != std::errc{})
      throw std::runtime_error("Could not format PostgreSQL integer parameter");

    *end = '\0';
    return buffer;
  }

  // ===========================================================================
  // PREPARED QUERY DISPATCH
  // ===========================================================================

  void send_read_query(PGconn *db, const HttpServer::Database::query_desc &item)
  {
    using namespace HttpServer::Database;

    int sent = 0;

    switch(item.query)
    {
    case Query::GetHealth:
      sent = PQsendQueryPrepared(db, "get_health", 0, nullptr, nullptr, nullptr, 0);
      break;

    case Query::GetFeed:
      sent = PQsendQueryPrepared(db, "get_feed", 0, nullptr, nullptr, nullptr, 0);
      break;

    case Query::GetPostById:
    {
      char post_id[32];
      const char *values[] = { format_u64(item.post_id, post_id, sizeof(post_id)) };
      sent = PQsendQueryPrepared(db, "get_post_by_id", 1, values, nullptr, nullptr, 0);
    }
    break;

    case Query::CreatePost:
    case Query::CreateLike:
    default:
      throw std::runtime_error("Non-read request reached read_worker");
    }

    if(sent != 1)
      throw std::runtime_error(PQerrorMessage(db));
  }

  void send_write_query(PGconn *db, const HttpServer::Database::query_desc &item)
  {
    using namespace HttpServer::Database;

    char user_id[32];
    char post_id[32];

    const char *user_id_value = format_u64(item.user_id, user_id, sizeof(user_id));

    int sent = 0;

    switch(item.query)
    {
    case Query::CreatePost:
    {
      // item.body refers to the request body in RX UMEM. libpq copies query
      // parameters into its output buffer during PQsendQueryPrepared(), so the
      // request buffer only needs to remain valid until this call returns.
      // PostgreSQL TEXT binary input is the raw text byte sequence.
      const char *values[] = { user_id_value, item.body.data() };
      const int lengths[] = { 0, static_cast<int>(item.body.size()) };
      const int formats[] = {
        0, // user_id: text format
        1  // body: binary TEXT format
      };

      sent = PQsendQueryPrepared(db, "create_post", 2, values, lengths, formats, 0);
    }
    break;

    case Query::CreateLike:
    {
      const char *values[] = { user_id_value, format_u64(item.post_id, post_id, sizeof(post_id)) };

      sent = PQsendQueryPrepared(db, "create_like", 2, values, nullptr, nullptr, 0);
    }
    break;

    case Query::GetHealth:
    case Query::GetFeed:
    case Query::GetPostById:
    default:
      throw std::runtime_error("Non-write request reached write_worker");
    }

    if(sent != 1)
      throw std::runtime_error(PQerrorMessage(db));
  }

  // ===========================================================================
  // DIRECT HTTP RESPONSE WRITER
  // ===========================================================================

  class HttpJsonResponse
  {
  private:
    char *packet{};
    char *content_length{};
    char *body{};
    size_t length{};

  public:
    HttpJsonResponse(char *_packet, int status, std::string_view date) noexcept : packet(_packet)
    {
      using namespace HttpServer::Database;
      const auto status_text = status_line(status);

      append(status_text);
      append(date);
      append("Content-Type: application/json; charset=utf-8\r\n");
      append("Content-Length: ");

      content_length = packet + length;
      append("          \r\n");
    }

    [[nodiscard]] char *get_body() const noexcept
    {
      return body;
    }

    void append_gzip_header() noexcept
    {
      append("Content-Encoding: gzip\r\n");
    }

    void finish_header()
    {
      append("Connection: keep-alive\r\n\r\n");
      body = packet + length;
    }

    [[nodiscard]] unsigned write_len(size_t payload_size)
    {
      const auto [end, error] = std::to_chars(content_length, content_length + 8, payload_size);
      (void)end;

      if(error != std::errc{})
        throw std::runtime_error("HTTP Content-Length overflow");

      length += payload_size;

      return static_cast<unsigned>(length);
    }

  private:
    void append(std::string_view value) noexcept
    {
      std::memcpy(packet + length, value.data(), value.size());
      length += value.size();
    }
  };

  void release_response(HttpServer::Database::query_desc &item, HttpServer::io_packet_desc *tx_desc, unsigned length)
  {
    auto *socket = reinterpret_cast<HttpServer::Socket *>(item.packet->socket_owner);

    tx_desc->len = length;
    tx_desc->client_fd = item.packet->client_fd;

    socket->wait_for_tx_release(tx_desc);
    socket->wait_for_rx_release(item.packet);
  }

  void send_database_error(HttpServer::Database::query_desc &item, HttpServer::Http::ErrorCode error)
  {
    auto *socket = reinterpret_cast<HttpServer::Socket *>(item.packet->socket_owner);
    HttpServer::io_packet_desc *tx_desc = socket->wait_for_tx_packet();
    auto *packet = socket->get_tx_umem_ptr() + tx_desc->addr;

    const unsigned length = HttpServer::Http::craft_http_error_response(packet, error);
    release_response(item, tx_desc, length);
  }

  [[nodiscard]] std::string_view username_view(const HttpServer::Database::query_desc &item) noexcept
  {
    return { item.username.data(), ::strnlen(item.username.data(), item.username.size()) };
  }

  // ===========================================================================
  // RESULT PROCESSING
  // ===========================================================================

  void process_read_result(HttpServer::Database::query_desc &request, PGresult *result, char *feed_buff, libdeflate_compressor *compressor)
  {
    using namespace HttpServer;
    using namespace HttpServer::Database;

    char date_buff[64];

    const auto now_sec = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::time_t time_t_now = std::chrono::system_clock::to_time_t(now_sec);
    const auto date = get_http_date(time_t_now, date_buff);

    auto *socket = reinterpret_cast<HttpServer::Socket *>(request.packet->socket_owner);
    io_packet_desc *tx_desc = socket->wait_for_tx_packet();
    auto *packet = socket->get_tx_umem_ptr() + tx_desc->addr;

    const ExecStatusType status = PQresultStatus(result);
    const bool query_ok = status == PGRES_TUPLES_OK;

    unsigned response_length = 0;

    if(request.query == Query::GetHealth && !query_ok)
    {
      const char *error = PQresultErrorMessage(result);
      const std::string_view error_view = error != nullptr ? std::string_view{ error } : std::string_view{ "PostgreSQL query failed" };

      try
      {
        HttpJsonResponse response(packet, 503, date);
        response.finish_header();

        const JSON::Response json = JSON::health_degraded_response_json(error_view, response.get_body());

        response_length = response.write_len(json.size);
      }
      catch(const std::exception &e)
      {
        response_length = Http::craft_http_error_response(packet, Http::ErrorCode::InternalServerError);

        std::println(stderr, "[error] health JSON serialization failed: {}", e.what());
      }

      release_response(request, tx_desc, response_length);
      return;
    }

    if(!query_ok)
    {
      response_length = Http::craft_http_error_response(packet, Http::ErrorCode::InternalServerError);

      release_response(request, tx_desc, response_length);
      return;
    }

    const int response_status = (request.query == Query::GetPostById && PQntuples(result) == 0) ? 404 : 200;

    try
    {
      HttpJsonResponse response(packet, response_status, date);
      JSON::Response json{};

      switch(request.query)
      {
      case Query::GetHealth:
        response.finish_header();

        json = JSON::health_response_json(
            result, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - HttpServer::start_time).count()),
            response.get_body());
        break;

      case Query::GetFeed:
        if(request.gzip)
        {
          json = JSON::feed_response_json(result, feed_buff);
          response.append_gzip_header();
          response.finish_header();

          const size_t compressed_size = libdeflate_gzip_compress(compressor, feed_buff, json.size, response.get_body(), CHUNK_SIZE);

          if(compressed_size == 0)
            throw std::runtime_error("gzip response buffer is too small");

          json.size = compressed_size;
        }
        else
        {
          response.finish_header();
          json = JSON::feed_response_json(result, response.get_body());
        }
        break;

      case Query::GetPostById:
        response.finish_header();
        json = JSON::post_response_json(result, response.get_body());
        break;

      case Query::CreatePost:
      case Query::CreateLike:
      default:
        throw std::runtime_error("Non-read query reached read_worker");
      }

      if(json.status != response_status)
        throw std::runtime_error("JSON serializer returned an unexpected HTTP status");

      response_length = response.write_len(json.size);
    }
    catch(const std::exception &e)
    {
      response_length = Http::craft_http_error_response(packet, Http::ErrorCode::InternalServerError);

      std::println(stderr, "[error] JSON serialization failed for query {}: {}", static_cast<int>(request.query), e.what());
    }

    release_response(request, tx_desc, response_length);
  }

  void process_write_result(HttpServer::Database::query_desc &request, PGresult *result)
  {
    using namespace HttpServer;
    using namespace HttpServer::Database;

    char date_buff[64];

    const auto now_sec = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::time_t time_t_now = std::chrono::system_clock::to_time_t(now_sec);
    const auto date = get_http_date(time_t_now, date_buff);

    const ExecStatusType status = PQresultStatus(result);

    bool success = false;
    bool like_created = false;
    bool post_not_found = false;

    if(request.query == Query::CreatePost)
    {
      success = status == PGRES_TUPLES_OK;

      // INSERT ... RETURNING id, created_at must return exactly one row.
      if(success && PQntuples(result) != 1)
        success = false;
    }
    else if(request.query == Query::CreateLike)
    {
      success = status == PGRES_COMMAND_OK;

      if(success)
      {
        const char *affected = PQcmdTuples(result);

        if(affected == nullptr)
        {
          success = false;
        }
        else
        {
          const std::string_view affected_rows{ affected };

          if(affected_rows == "1")
            like_created = true;
          else if(affected_rows == "0")
            like_created = false;
          else
            success = false;
        }
      }

      if(!success && status == PGRES_FATAL_ERROR)
      {
        const char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
        const char *constraint = PQresultErrorField(result, PG_DIAG_CONSTRAINT_NAME);

        post_not_found = sqlstate != nullptr && constraint != nullptr && std::strcmp(sqlstate, "23503") == 0 && std::strcmp(constraint, "likes_post_id_fkey") == 0;
      }
    }
    else
    {
      throw std::runtime_error("Non-write query reached write_worker");
    }

    auto *socket = reinterpret_cast<HttpServer::Socket *>(request.packet->socket_owner);
    io_packet_desc *tx_desc = socket->wait_for_tx_packet();
    auto *packet = socket->get_tx_umem_ptr() + tx_desc->addr;

    unsigned response_length = 0;

    if(!success)
    {
      const Http::ErrorCode error = post_not_found ? Http::ErrorCode::PostNotFound : Http::ErrorCode::InternalServerError;

      response_length = Http::craft_http_error_response(packet, error);
      release_response(request, tx_desc, response_length);
      return;
    }

    const int response_status = request.query == Query::CreatePost ? 201 : (like_created ? 201 : 200);

    try
    {
      HttpJsonResponse response(packet, response_status, date);
      JSON::Response json{};

      switch(request.query)
      {
      case Query::CreatePost:
        response.finish_header();
        json = JSON::create_post_response_json(result, request.body, username_view(request), response.get_body());
        break;

      case Query::CreateLike:
        response.finish_header();
        json = JSON::like_response_json(like_created, static_cast<int64_t>(request.post_id), response.get_body());
        break;

      case Query::GetHealth:
      case Query::GetFeed:
      case Query::GetPostById:
      default:
        throw std::runtime_error("Non-write query reached write_worker");
      }

      if(json.status != response_status)
        throw std::runtime_error("JSON serializer returned an unexpected HTTP status");

      response_length = response.write_len(json.size);
    }
    catch(const std::exception &e)
    {
      response_length = Http::craft_http_error_response(packet, Http::ErrorCode::InternalServerError);

      std::println(stderr, "[error] JSON serialization failed for query {}: {}", static_cast<int>(request.query), e.what());
    }

    release_response(request, tx_desc, response_length);
  }

  // ===========================================================================
  // NONBLOCKING PIPELINE EVENT LOOP
  // ===========================================================================

  enum class PendingKind : uint8_t
  {
    QUERY,
    SYNC
  };

  struct PendingOperation
  {
    PendingKind kind{};
    HttpServer::Database::query_desc request{};
  };

  struct CompletedResult
  {
    HttpServer::Database::query_desc request{};
    PGResult result{};
  };

  template <typename Queue, typename SendQuery, typename ProcessResult>
  void run_nonblocking_pipeline(std::stop_token stp, PGconn *db, Queue &queue, bool sync_each_query, SendQuery &&send_query, ProcessResult &&process_result)
  {
    using namespace HttpServer::Database;

    constexpr size_t MAX_DEPTH = HttpServer::MAX_QUERY_PIPELINE_DEPTH;
    constexpr size_t FLUSH_INTERVAL = 8;
    constexpr int POLL_TIMEOUT_MS = 1;

    static_assert(MAX_DEPTH > 0, "MAX_QUERY_PIPELINE_DEPTH must be greater than zero");

    const int socket_fd = PQsocket(db);

    if(socket_fd < 0)
      throw std::runtime_error(PQerrorMessage(db));

    // Protocol operations: QUERY entries and PIPELINE_SYNC entries.
    std::deque<PendingOperation> pending;

    // Results whose PGresult has been received but whose HTTP response
    // has not yet been generated.
    std::deque<CompletedResult> completed;

    std::vector<query_desc> batch;
    batch.reserve(MAX_DEPTH);

    // Number of requests submitted but not yet fully answered over HTTP.
    // This includes both queries waiting on PostgreSQL and completed
    // PostgreSQL results waiting for HTTP response processing.
    size_t outstanding = 0;

    // Number of queries whose PostgreSQL result boundary has not yet
    // been consumed.
    size_t in_flight = 0;

    // Number of entries at the front of completed that are ready for
    // HTTP processing (their PostgreSQL query boundary was consumed).
    size_t ready_to_process = 0;

    size_t read_epoch_queries = 0;
    size_t queries_since_flush = 0;

    bool awaiting_query_boundary = false;
    bool output_pending = false;

    auto fatal_invariant = [](const char *message) noexcept
    {
      std::println(stderr, "[fatal] {}", message);
      std::terminate();
    };

    auto flush_output = [&]() -> bool
    {
      const int result = PQflush(db);

      if(result < 0)
        throw std::runtime_error(PQerrorMessage(db));

      // true means libpq still has bytes to write.
      return result == 1;
    };

    auto send_sync = [&]()
    {
      if(PQsendPipelineSync(db) != 1)
        throw std::runtime_error(PQerrorMessage(db));

      pending.emplace_back(PendingOperation{ PendingKind::SYNC, {} });
    };

    auto send_flush_request = [&]()
    {
      if(PQsendFlushRequest(db) != 1)
        throw std::runtime_error(PQerrorMessage(db));
    };

    // Consume everything currently available from libpq.
    //
    // Important:
    // - This function does NOT serialize HTTP responses.
    // - Each query is associated with its result through FIFO ordering.
    // - A NULL result terminates the current query's result sequence.
    // - PGRES_PIPELINE_SYNC terminates a synchronization epoch.
    auto collect_ready_results = [&]()
    {
      for(;;)
      {
        if(PQisBusy(db) != 0)
          return;

        PGResult result{ PQgetResult(db) };

        if(!result)
        {
          // NULL is a query boundary only if we previously received
          // the PGresult belonging to that query.
          if(!awaiting_query_boundary)
            return;

          if(pending.empty() || pending.front().kind != PendingKind::QUERY)
            fatal_invariant("Unexpected PostgreSQL query boundary");

          pending.pop_front();
          awaiting_query_boundary = false;

          if(in_flight == 0)
            fatal_invariant("PostgreSQL in-flight counter underflow");

          --in_flight;
          ++ready_to_process;

          if(ready_to_process > completed.size())
            fatal_invariant("PostgreSQL completion FIFO invariant violated");

          continue;
        }

        const ExecStatusType status = PQresultStatus(result.get());

        if(status == PGRES_PIPELINE_SYNC)
        {
          if(awaiting_query_boundary || pending.empty() || pending.front().kind != PendingKind::SYNC)
          {
            fatal_invariant("Unexpected PostgreSQL pipeline sync");
          }

          pending.pop_front();
          continue;
        }

        if(awaiting_query_boundary || pending.empty() || pending.front().kind != PendingKind::QUERY)
        {
          fatal_invariant("PostgreSQL result/query ordering violation");
        }

        // Keep the RX packet alive. It will be released only after
        // process_result() has generated and submitted its HTTP response.
        completed.emplace_back(CompletedResult{ .request = pending.front().request, .result = std::move(result) });

        awaiting_query_boundary = true;
      }
    };

    for(;;)
    {
      // =====================================================================
      // 1. DISPATCH FIRST
      //
      // Always inspect the producer queue before processing completed HTTP
      // responses. This happens after a poll timeout AND after socket events.
      // =====================================================================

      if(outstanding > MAX_DEPTH || read_epoch_queries > MAX_DEPTH)
        fatal_invariant("Pipeline capacity invariant violated");

      const size_t outstanding_room = MAX_DEPTH - outstanding;

      const size_t epoch_room = sync_each_query ? outstanding_room : (MAX_DEPTH - read_epoch_queries);

      const size_t dispatch_room = std::min(outstanding_room, epoch_room);

      batch.clear();
      bool queue_empty = false;

      if(dispatch_room != 0)
      {
        query_desc item{};

        while(batch.size() < dispatch_room)
        {
          if(!queue.peek(item))
          {
            queue_empty = true;
            break;
          }

          batch.emplace_back(item);
        }
      }

      bool appended_output = false;

      if(!batch.empty())
      {
        for(const query_desc &request : batch)
        {
          // Append the prepared query to libpq's outgoing pipeline.
          send_query(db, request);

          pending.emplace_back(PendingOperation{ PendingKind::QUERY, request });

          ++in_flight;
          ++outstanding;

          appended_output = true;

          if(sync_each_query)
          {
            // Each write has its own implicit transaction boundary
            // and error-recovery point.
            send_sync();
          }
          else
          {
            ++read_epoch_queries;
            ++queries_since_flush;

            // Flush server responses periodically without ending
            // the current read pipeline epoch.
            if(queries_since_flush >= FLUSH_INTERVAL)
            {
              send_flush_request();
              queries_since_flush = 0;
            }
          }

          query_desc dequeued{};

          if(!queue.dequeue(dequeued) || dequeued.packet != request.packet)
            fatal_invariant("DB queue peek/dequeue invariant violated");
        }

        // Publish the consumer-side queue advancement once per batch.
        queue.sync();
      }

      if(!sync_each_query)
      {
        // Close the current read epoch when:
        // - the producer queue was observed empty, or
        // - the epoch reached MAX_DEPTH.
        //
        // We can enqueue the next epoch behind this sync without waiting
        // for the previous epoch's results.
        if(read_epoch_queries != 0 && (queue_empty || read_epoch_queries >= MAX_DEPTH))
        {
          send_sync();

          appended_output = true;
          read_epoch_queries = 0;
          queries_since_flush = 0;
        }
      }

      // PQsendQueryPrepared() only queues protocol data in libpq.
      // PQflush() attempts to transmit it without blocking.
      if(appended_output)
        output_pending = flush_output();

      // =====================================================================
      // 2. DRAIN AVAILABLE DATABASE RESULTS
      //
      // Collect PGresults first. Do not perform HTTP serialization here.
      // =====================================================================

      collect_ready_results();

      // =====================================================================
      // 3. PROCESS ONE COMPLETED HTTP RESPONSE
      //
      // Then return to the top, dispatching newly arrived queue work before
      // processing another completion.
      // =====================================================================

      if(ready_to_process != 0)
      {
        if(completed.empty())
          fatal_invariant("Completed PostgreSQL result FIFO is empty");

        CompletedResult &entry = completed.front();

        process_result(entry.request, entry.result.get());

        completed.pop_front();
        --ready_to_process;

        if(outstanding == 0)
          fatal_invariant("Outstanding DB request counter underflow");

        --outstanding;

        // Do not process the next response yet.
        // Return to queue dispatch first.
        continue;
      }

      // =====================================================================
      // 4. STOP CHECK
      // =====================================================================

      if(stp.stop_requested())
        break;

      // =====================================================================
      // 5. POLL DATABASE SOCKET
      //
      // POLLIN:  consume input.
      // POLLOUT: retry libpq output only when PQflush previously returned 1.
      //
      // Never process results directly in this section. Return to the top
      // first, so newly queued queries get priority.
      // =====================================================================

      short events = POLLIN;

      if(output_pending)
        events = static_cast<short>(events | POLLOUT);

      pollfd descriptor{ .fd = socket_fd, .events = events, .revents = 0 };

      const int ready = ::poll(&descriptor, 1, POLL_TIMEOUT_MS);

      if(ready < 0)
      {
        if(errno == EINTR)
          continue;

        throw std::system_error(errno, std::generic_category(), "PostgreSQL poll failed");
      }

      // A timeout is intentional: return to queue dispatch.
      if(ready == 0)
        continue;

      if(descriptor.revents & POLLNVAL)
        throw std::runtime_error("Invalid PostgreSQL socket");

      if(descriptor.revents & POLLERR)
        throw std::runtime_error(PQerrorMessage(db));

      if(descriptor.revents & POLLIN)
      {
        if(PQconsumeInput(db) != 1)
          throw std::runtime_error(PQerrorMessage(db));
      }

      if(output_pending && (descriptor.revents & POLLOUT))
        output_pending = flush_output();

      if(descriptor.revents & POLLHUP)
        throw std::runtime_error(PQerrorMessage(db));

      // Deliberately return to the top:
      // dispatch queued work before collecting/processing more results.
    }
  }

} // namespace

namespace HttpServer::Database
{
  template <size_t QUEUE_SIZE>
  DBReader<QUEUE_SIZE>::DBReader()
  {
    for(const auto [index, num_of_workers] : { std::make_tuple(WORKER_TYPE::READ, HttpServer::N_DB_READERS), std::make_tuple(WORKER_TYPE::WRITE, HttpServer::N_DB_WRITERS) })
    {
      const auto num_index = static_cast<uint8_t>(index);

      this->queues[num_index] = std::vector<Queue::MPSC::Queue<QUEUE_SIZE, query_desc>>(num_of_workers);

      for(unsigned i = 0; i < num_of_workers; ++i)
      {
        switch(index)
        {
        case WORKER_TYPE::READ:
          workers[num_index].emplace_back([this, i](std::stop_token stp) { this->read_worker(stp, i); });
          break;

        case WORKER_TYPE::WRITE:
          workers[num_index].emplace_back([this, i](std::stop_token stp) { this->write_worker(stp, i); });
          break;

        case WORKER_TYPE::COUNT:
          break;
        }
      }
    }
  }

  template <size_t QUEUE_SIZE>
  DBReader<QUEUE_SIZE>::~DBReader()
  {
    for(auto &vector : workers)
    {
      for(auto &thread : vector)
        thread.request_stop();
    }

    for(auto &vector : workers)
    {
      for(auto &thread : vector)
      {
        if(thread.joinable())
          thread.join();
      }
    }
  }

  template <size_t QUEUE_SIZE>
  void DBReader<QUEUE_SIZE>::enqueue(const WORKER_TYPE type, query_desc &view)
  {
    const auto type_index = static_cast<uint8_t>(type);
    const auto size = this->queues[type_index].size();

    const unsigned id = this->selector.value.fetch_add(1, std::memory_order_seq_cst) % size;

    bool enqueued = false;
    for(auto &queue : this->queues)
    {
      enqueued = this->queues[type_index][id].enqueue(view);
      if(enqueued)
        break;
    }

    if(!enqueued)
    {
      auto *socket = reinterpret_cast<HttpServer::Socket *>(view.packet->socket_owner);
      io_packet_desc *tx_desc = socket->wait_for_tx_packet();
      auto *ptr = socket->get_tx_umem_ptr() + tx_desc->addr;

      const auto len = Http::craft_http_error_response(ptr, Http::ErrorCode::InternalServerError);

      tx_desc->len = static_cast<uint32_t>(len);
      tx_desc->client_fd = view.packet->client_fd;

      socket->wait_for_tx_release(tx_desc);
      socket->wait_for_rx_release(view.packet);
      return;
    }
  }

  template <size_t QUEUE_SIZE>
  void DBReader<QUEUE_SIZE>::read_worker(std::stop_token stp, unsigned thr_id)
  {
    constexpr auto type_index = static_cast<uint8_t>(WORKER_TYPE::READ);

    auto &queue = this->queues[type_index][thr_id];

    PGConnection db = connect_database();
    prepare_read_statements(db.get());
    enter_pipeline_mode(db.get());

    char feed_buff[CHUNK_SIZE];

    using compress_t = std::unique_ptr<libdeflate_compressor, decltype(&libdeflate_free_compressor)>;

    compress_t compressor{ libdeflate_alloc_compressor(6), libdeflate_free_compressor };

    if(!compressor)
      throw std::runtime_error("Could not allocate libdeflate compressor");

    run_nonblocking_pipeline(
        stp, db.get(), queue, false, [](PGconn *connection, const query_desc &request) { send_read_query(connection, request); },
        [&](query_desc &request, PGresult *result) { process_read_result(request, result, feed_buff, compressor.get()); });
  }

  template <size_t QUEUE_SIZE>
  void DBReader<QUEUE_SIZE>::write_worker(std::stop_token stp, unsigned thr_id)
  {
    constexpr auto type_index = static_cast<uint8_t>(WORKER_TYPE::WRITE);

    auto &queue = this->queues[type_index][thr_id];

    PGConnection db = connect_database();
    prepare_write_statements(db.get());
    enter_pipeline_mode(db.get());

    run_nonblocking_pipeline(
        stp, db.get(), queue, true, [](PGconn *connection, const query_desc &request) { send_write_query(connection, request); },
        [](query_desc &request, PGresult *result) { process_write_result(request, result); });
  }

  // Explicit instantiations must follow the out-of-line member definitions.
  template class DBReader<8>;
  template class DBReader<DBREADERS_QUEUE_SIZE>;

} // namespace HttpServer::Database
