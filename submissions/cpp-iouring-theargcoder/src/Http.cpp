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
  Http::Http() : verifier(JWT_SECRET)
  {
    this->thread_rx = std::jthread([this](std::stop_token stp) { this->rx_worker(stp); });
  }

  Http::~Http()
  {
    this->thread_rx.request_stop();

    this->rx_signal.fetch_add(1, std::memory_order_release);
    this->rx_signal.notify_all();
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

  [[nodiscard]] std::tuple<HeadState, RequestHead> Http::parse_request_head(std::string_view input, const size_t &previous_length)
  {
    const char *method = nullptr;
    const char *path = nullptr;
    size_t method_len = 0;
    size_t path_len = 0;
    size_t header_count = headers.size();
    RequestHead out{};

    const int parsed = phr_parse_request(input.data(), input.size(), &method, &method_len, &path, &path_len, &out.minor_version, headers.data(), &header_count, previous_length);

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
      if(this->headers[i].name == nullptr)
      {
        return std::make_tuple(HeadState::MALFORMED, out);
      }

      const std::string_view name{ this->headers[i].name, this->headers[i].name_len };

      if(!ascii_iequals(name, "Authorization"))
      {
        continue;
      }

      if(out.authorization.has_value())
      {
        out.duplicate_authorization = true;
        return std::make_tuple(HeadState::MALFORMED, out);
      }

      out.authorization = std::string_view{ this->headers[i].value, this->headers[i].value_len };
    }

    return std::make_tuple(HeadState::COMPLETE, out);
  }

  template <>
  void Http::handle<MethodEnum::GET>(const HeadState &state, const RequestHead &head, HttpServer::io_packet_desc *rx_desc)
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

      HttpServer::socket->release_tx_packet(tx_desc);
      HttpServer::socket->release_rx_packet(rx_desc);

      // DO NOT enqueue this request. Its RX descriptor was released.
      return;
    }

    db_reader.enqueue(desc);
  }

  template <>
  void Http::handle<MethodEnum::POST>(const HeadState &state, const RequestHead &head, HttpServer::io_packet_desc *rx_desc)
  {
    ErrorCode error_code = ErrorCode::Count;

    Database::query_desc desc{};
    desc.packet = rx_desc;

    // Authenticate BEFORE parsing the like endpoint's ID.
    const auto auth_res = jwt_auth::verify_authorization(head.authorization, this->verifier);

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
      auto *tx_desc = HttpServer::socket->get_tx_packet_ptr();
      char *packet = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;
      size_t len = craft_http_error_response(packet, error_code);

#ifndef NDEBUG
      std::print("\nSENDING PACKET : \n");
      for(size_t i = 0; i < len; i++)
      {
        std::print("{}", packet[i]);
      }
      std::print("\n");
#endif

      tx_desc->client_fd = rx_desc->client_fd;
      tx_desc->len = static_cast<unsigned>(len);
      HttpServer::socket->release_tx_packet(tx_desc);
      HttpServer::socket->release_rx_packet(rx_desc);

      return;
    }

    db_reader.enqueue(desc);
  }

  void Http::rx_worker(std::stop_token stp)
  {
    std::print("started rx_worker thread \n");

    while(!stp.stop_requested())
    {
      const auto observed = this->rx_signal.load(std::memory_order_acquire);

      bool processed = false;

      HttpServer::io_packet_desc *rx_desc;

      for(bool valid = this->dequeue(rx_desc); valid; valid = this->dequeue(rx_desc))
      {
        processed = true;

        std::string_view view(HttpServer::socket->get_rx_umem_ptr() + rx_desc->addr, rx_desc->len);

        const auto [parsed, head] = this->parse_request_head(view, 0);

        const auto meth_enum = HttpServer::Http::toMethodEnum(head.method);

        switch(meth_enum)
        {
        case MethodEnum::GET:
          this->handle<MethodEnum::GET>(parsed, head, rx_desc);
          break;

        case MethodEnum::POST:
          this->handle<MethodEnum::POST>(parsed, head, rx_desc);
          break;

        case MethodEnum::PUT:
        case MethodEnum::DELETE:
        case MethodEnum::INVALID:
        default:
        {
          auto *tx_desc = HttpServer::socket->get_tx_packet_ptr();
          char *packet = HttpServer::socket->get_tx_umem_ptr() + tx_desc->addr;
          size_t len = craft_http_error_response(packet, ErrorCode::InternalServerError);

          tx_desc->client_fd = rx_desc->client_fd;
          tx_desc->len = static_cast<unsigned>(len);
          HttpServer::socket->release_tx_packet(tx_desc);
          HttpServer::socket->release_rx_packet(rx_desc);

          return;
        }
        break;
        }
      }

      db_reader.awake_all();

      if(!processed)
      {
        this->rx_signal.wait(observed, std::memory_order_acquire);
      }
    }

    std::print("stopped rx_worker thread \n");
  }

} // namespace HttpServer::Http
