#include <atomic>
#include <cstddef>
#include <print>
#include <string_view>

#include "include/DBReader.hpp"
#include "include/Defines.hpp"
#include "include/Http.hpp"
#include "include/HttpServer.hpp"
#include "include/Json.hpp"
#include "include/Jwt.hpp"

namespace HttpServer::Http
{
  Http::Http() : headers(HttpServer::N_WORKERS), queues(HttpServer::N_WORKERS), signals(HttpServer::N_WORKERS)
  {
    for(size_t i = 0; i < HttpServer::N_WORKERS; ++i)
    {
      this->verifiers.emplace_back(std::string_view(HttpServer::JWT_SECRET));
    }

    for(size_t i = 0; i < HttpServer::N_WORKERS; i++)
    {
      workers.emplace_back([this, i](std::stop_token stp) { this->worker(stp, static_cast<unsigned>(i)); });
    }
  };

  Http::~Http()
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

  void Http::wakeup_mask(const std::bitset<MAX_SUPPORTED_WORKERS> &mask)
  {
    for(unsigned i = 0; i < HttpServer::N_WORKERS; ++i)
    {
      if(mask[i])
      {
        signals[i].value.fetch_add(1, std::memory_order_release);
        signals[i].value.notify_one();
      }
    }
  }

  void Http::enqueue(HttpServer::io_packet_desc *rx_desc)
  {
    const unsigned w = static_cast<unsigned>(rx_desc->client_fd) % HttpServer::N_WORKERS;
    if(!this->queues[w].enqueue(rx_desc))
    {
      if(auto *tx_desc = HttpServer::socket->get_tx_packet_ptr())
      {
        char *packet = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;
        const size_t len = craft_http_error_response(packet, ErrorCode::InternalServerError);

        tx_desc->client_fd = rx_desc->client_fd;
        tx_desc->len = static_cast<unsigned>(len);
        HttpServer::socket->wait_for_tx_release(tx_desc);
      }
      HttpServer::socket->wait_for_rx_release(rx_desc);
    }
  }

  bool Http::ascii_iequals(std::string_view a, std::string_view b) noexcept
  {
    if(a.size() != b.size())
    {
      return false;
    }

    for(size_t i = 0; i < a.size(); ++i)
    {
      char a_char = a[i];
      char b_char = b[i];

      if(a_char >= 'A' && a_char <= 'Z')
      {
        a_char = static_cast<char>(a_char - 'A' + 'a');
      }
      if(b_char >= 'A' && b_char <= 'Z')
      {
        b_char = static_cast<char>(b_char - 'A' + 'a');
      }

      if(a_char != b_char)
      {
        return false;
      }
    }

    return true;
  }

  [[nodiscard]] std::tuple<HeadState, RequestHead> Http::parse_request_head(std::string_view input, const size_t &previous_length, std::array<phr_header, 64> &header)
  {
    const char *method = nullptr;
    const char *path = nullptr;
    size_t method_len = 0;
    size_t path_len = 0;
    size_t header_count = header.size();
    RequestHead out{};

    const int parsed = phr_parse_request(input.data(), input.size(), &method, &method_len, &path, &path_len, &out.minor_version, header.data(), &header_count, previous_length);

    if(parsed == -2)
    {
      return std::make_tuple(HeadState::INCOMPLETE, out);
    }

    if(parsed < 0)
    {
      return std::make_tuple(HeadState::MALFORMED, out);
    }

    out.method = { method, method_len };
    out.path = { path, path_len };
    out.header_bytes = static_cast<size_t>(parsed);

    for(size_t i = 0; i < header_count; ++i)
    {
      // Reject obsolete folded headers rather than interpreting them differently from another HTTP implementation.
      if(header[i].name == nullptr)
      {
        return std::make_tuple(HeadState::MALFORMED, out);
      }

      const std::string_view name{ header[i].name, header[i].name_len };

      if(!ascii_iequals(name, "Authorization"))
      {
        continue;
      }

      if(out.authorization.has_value())
      {
        out.duplicate_authorization = true;
        return std::make_tuple(HeadState::MALFORMED, out);
      }

      out.authorization = std::string_view{ header[i].value, header[i].value_len };
    }

    return std::make_tuple(HeadState::COMPLETE, out);
  }

  template <>
  void Http::handle<MethodEnum::GET>(const unsigned &thread_id, const HeadState &state, const RequestHead &head, jwt_auth::Verifier & /*unused*/,
                                     HttpServer::io_packet_desc *rx_desc)
  {
    Database::query_desc desc{};
    desc.packet = rx_desc;

    ErrorCode error_code = ErrorCode::Count;

    if(state == HeadState::MALFORMED)
    {
      error_code = ErrorCode::InternalServerError;
    }
    else if(head.path == "/health")
    {
      desc.query = Database::Query::GetHealth;
    }
    else if(head.path == "/feed")
    {
      desc.query = Database::Query::GetFeed;
    }
    else if(head.path.starts_with("/posts/"))
    {
      const auto id_text = head.path.substr(7);

      int64_t id = 0;

      const auto [end, ec] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);

      if(ec != std::errc{} || end != id_text.data() + id_text.size() || id <= 0)
      {
        error_code = ErrorCode::InvalidPostId;
      }
      else
      {
        // Only construct a database request for a valid ID.
        desc.query = Database::Query::GetPostById;
        desc.post_id = static_cast<uint64_t>(id);
      }
    }
    else
    {
      error_code = ErrorCode::UnknownPath;
    }

    if(error_code != ErrorCode::Count)
    {
      const int client_fd = rx_desc->client_fd;

      auto *tx_desc = HttpServer::socket->wait_for_tx_packet();

      char *packet = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;

      const size_t len = craft_http_error_response(packet, error_code);

      tx_desc->client_fd = client_fd;
      tx_desc->len = static_cast<uint32_t>(len);

      HttpServer::socket->wait_for_tx_release(tx_desc);
      HttpServer::socket->wait_for_rx_release(rx_desc);

      // DO NOT enqueue this request. Its RX descriptor was released.
      return;
    }

    db_reader.enqueue(desc, thread_id);
  }

  template <>
  void Http::handle<MethodEnum::POST>(const unsigned &thread_id, const HeadState &state, const RequestHead &head, jwt_auth::Verifier &verifier, HttpServer::io_packet_desc *rx_desc)
  {
    ErrorCode error_code = ErrorCode::Count;

    Database::query_desc desc{};
    desc.packet = rx_desc;

    // Authenticate BEFORE parsing the like endpoint's ID.
    const auto auth_res = jwt_auth::verify_authorization(head.authorization, verifier);

    if(state == HeadState::MALFORMED)
    {
      error_code = ErrorCode::InternalServerError;
      goto error_mesage;
    }

    // auth requried for POST
    if(!auth_res.accepted(true))
    {
      switch(auth_res.state)
      {
      case jwt_auth::State::MISSING:
        error_code = ErrorCode::MissingBearerToken;
        goto error_mesage;

      case jwt_auth::State::INVALID:
        error_code = ErrorCode::InvalidOrExpiredToken;
        goto error_mesage;

      case jwt_auth::State::INVALID_PAYLOAD:
        error_code = ErrorCode::InvalidTokenPayload;
        goto error_mesage;

      case jwt_auth::State::AUTHENTICATED:
        error_code = ErrorCode::InternalServerError;
        goto error_mesage;
      }
    }

    if(head.path == "/posts")
    {
      desc.query = Database::Query::CreatePost;
    }
    else if(head.path.starts_with("/posts/") && head.path.ends_with("/like"))
    {
      desc.query = Database::Query::CreateLike;
    }
    else
    {
      error_code = ErrorCode::UnknownPath;
      goto error_mesage;
    }

    desc.user_id = auth_res.identity.user_id;
    desc.username = auth_res.identity.username;

    if(desc.query == Database::Query::CreateLike)
    {
      const auto id_text = head.path.substr(7, head.path.size() - 7 - 5);

      int64_t id = 0;

      const auto [end, ec] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);

      if(ec != std::errc{} || end != id_text.data() + id_text.size() || id <= 0)
      {
        error_code = ErrorCode::InvalidPostId;
        goto error_mesage;
      }

      desc.post_id = static_cast<uint64_t>(id);
    }
    else if(desc.query == Database::Query::CreatePost)
    {
      const std::string_view rx_ptr{ HttpServer::socket->get_rx_umem_ptr() + rx_desc->addr, rx_desc->len };

      const size_t header_end = rx_ptr.find("\r\n\r\n");

      if(header_end == std::string_view::npos)
      {
        error_code = ErrorCode::InternalServerError;
        goto error_mesage;
      }

      // Skip HTTP header terminator.
      const size_t body_offset = header_end + 4;

      const std::string_view request_body = rx_ptr.substr(body_offset);

      const auto validation = Database::JSON::validate_create_post_body(request_body, desc.body);

      error_code = Database::JSON::body_error_to_http_error(validation);

      if(error_code != ErrorCode::Count)
        goto error_mesage;
    }

  error_mesage:
    if(error_code != ErrorCode::Count)
    {
      auto *tx_desc = HttpServer::socket->wait_for_tx_packet();
      char *packet = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;
      size_t len = craft_http_error_response(packet, error_code);

      tx_desc->client_fd = rx_desc->client_fd;
      tx_desc->len = static_cast<unsigned>(len);
      HttpServer::socket->wait_for_tx_release(tx_desc);
      HttpServer::socket->wait_for_rx_release(rx_desc);

      return;
    }

    db_reader.enqueue(desc, thread_id);
  }

  void Http::worker(std::stop_token stp, const unsigned thread_id)
  {
    std::println(stderr, "[info] started rx_worker thread id '{}'", thread_id);

    auto &header = this->headers[thread_id];
    auto &queue = this->queues[thread_id];
    auto &signal = this->signals[thread_id];
    auto &verifier = this->verifiers[thread_id];

    while(!stp.stop_requested())
    {
      const auto observed = signal.value.load(std::memory_order_acquire);

      bool processed = false;

      HttpServer::io_packet_desc *rx_desc;

      for(bool valid = queue.dequeue(rx_desc); valid; valid = queue.dequeue(rx_desc))
      {
        processed = true;

        std::string_view view(HttpServer::socket->get_rx_umem_ptr() + rx_desc->addr, rx_desc->len);

        const auto [parsed, head] = this->parse_request_head(view, 0, header);

        if(parsed == HeadState::INCOMPLETE)
        {
          HttpServer::socket->close_connection(rx_desc->client_fd);
          HttpServer::socket->wait_for_rx_release(rx_desc);
          continue;
        }

        const auto meth_enum = HttpServer::Http::toMethodEnum(head.method);

        switch(meth_enum)
        {
        case MethodEnum::GET:
          this->handle<MethodEnum::GET>(thread_id, parsed, head, verifier, rx_desc);
          break;

        case MethodEnum::POST:
          this->handle<MethodEnum::POST>(thread_id, parsed, head, verifier, rx_desc);
          break;

        case MethodEnum::PUT:
        case MethodEnum::DELETE:
        case MethodEnum::INVALID:
        default:
        {
          auto *tx_desc = HttpServer::socket->wait_for_tx_packet();
          char *packet = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;
          size_t len = craft_http_error_response(packet, ErrorCode::UnsupportedMethod);

          tx_desc->client_fd = rx_desc->client_fd;
          tx_desc->len = static_cast<unsigned>(len);
          HttpServer::socket->wait_for_tx_release(tx_desc);
          HttpServer::socket->wait_for_rx_release(rx_desc);

          continue; // we use to suicide the thread .... talk about a bug and a half
        }
        break;
        }
      }

      if(processed)
      {
        db_reader.awake(thread_id);
      }

      if(!processed)
      {
        signal.value.wait(observed, std::memory_order_acquire);
      }
    }

    std::println(stderr, "[info] stopped rx_worker thread id '{}'", thread_id);
  }

} // namespace HttpServer::Http
