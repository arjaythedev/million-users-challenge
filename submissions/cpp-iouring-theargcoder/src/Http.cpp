#include <cstddef>
#include <cstdint>
#include <string_view>

#include "include/DBReader.hpp"
#include "include/Defines.hpp"
#include "include/Http.hpp"
#include "include/HttpServer.hpp"
#include "include/Json.hpp"
#include "include/Jwt.hpp"
#include "include/Socket.hpp"

namespace HttpServer::Http
{
  Http::Http() : header{}, verifier(std::string_view(HttpServer::JWT_SECRET)) {};

  Http::~Http() = default;

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
    size_t header_count = this->header.size();
    RequestHead out{};

    const int parsed
        = phr_parse_request(input.data(), input.size(), &method, &method_len, &path, &path_len, &out.minor_version, this->header.data(), &header_count, previous_length);

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

    bool gzip_found = false;
    bool gzip_accepted = false;

    for(size_t i = 0; i < header_count; ++i)
    {
      if(this->header[i].name == nullptr)
      {
        return std::make_tuple(HeadState::MALFORMED, out);
      }

      const std::string_view name{ this->header[i].name, this->header[i].name_len };
      const std::string_view value{ this->header[i].value, this->header[i].value_len };

      if(ascii_iequals(name, "Authorization"))
      {
        if(out.authorization.has_value())
        {
          out.duplicate_authorization = true;
          return std::make_tuple(HeadState::MALFORMED, out);
        }

        out.authorization = value;
        continue;
      }

      if(!ascii_iequals(name, "Accept-Encoding"))
      {
        continue;
      }

      // Accept-Encoding can contain multiple comma-separated coding entries.
      size_t pos = 0;

      while(pos < value.size())
      {
        const size_t comma = value.find(',', pos);
        const size_t end = comma == std::string_view::npos ? value.size() : comma;

        std::string_view encoding = value.substr(pos, end - pos);

        // Trim HTTP optional whitespace.
        const size_t first = encoding.find_first_not_of(" \t");
        if(first != std::string_view::npos)
        {
          encoding.remove_prefix(first);

          const size_t last = encoding.find_last_not_of(" \t");
          encoding.remove_suffix(encoding.size() - last - 1);

          const size_t semicolon = encoding.find(';');
          const std::string_view token = encoding.substr(0, semicolon);

          if(ascii_iequals(token, "gzip"))
          {
            gzip_found = true;

            bool accepted = true;

            if(semicolon != std::string_view::npos)
            {
              std::string_view parameters = encoding.substr(semicolon + 1);

              // Explicit q=0 means gzip is not acceptable.
              const size_t q = parameters.find("q=");
              if(q != std::string_view::npos)
              {
                const std::string_view quality = parameters.substr(q + 2);
                accepted = !quality.empty() && quality.front() != '0';
              }
            }

            gzip_accepted = accepted;
          }
        }

        if(comma == std::string_view::npos)
        {
          break;
        }

        pos = comma + 1;
      }
    }

    out.gzip = gzip_found && gzip_accepted;

    return std::make_tuple(HeadState::COMPLETE, out);
  }

  template <>
  Database::WORKER_TYPE Http::handle<MethodEnum::GET>(const HeadState &state, const RequestHead &head, Database::query_desc &desc)
  {
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
      const int client_fd = desc.packet->client_fd;
      auto *socket = reinterpret_cast<HttpServer::Socket *>(desc.packet->socket_owner);

      auto *tx_desc = socket->wait_for_tx_packet();
      char *packet = socket->get_tx_umem_ptr() + tx_desc->addr;

      const size_t len = craft_http_error_response(packet, error_code);

      tx_desc->client_fd = client_fd;
      tx_desc->len = static_cast<uint32_t>(len);

      socket->wait_for_tx_release(tx_desc);
      socket->wait_for_rx_release(desc.packet);

      // DO NOT enqueue this request. Its RX descriptor was released.
      return Database::WORKER_TYPE::COUNT;
    }

    return Database::WORKER_TYPE::READ;
  }

  template <>
  Database::WORKER_TYPE Http::handle<MethodEnum::POST>(const HeadState &state, const RequestHead &head, Database::query_desc &desc)
  {
    ErrorCode error_code = ErrorCode::Count;

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
      auto *socket = reinterpret_cast<HttpServer::Socket *>(desc.packet->socket_owner);
      const std::string_view rx_ptr{ socket->get_rx_umem_ptr() + desc.packet->addr, desc.packet->len };

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
      auto *socket = reinterpret_cast<HttpServer::Socket *>(desc.packet->socket_owner);
      auto *tx_desc = socket->wait_for_tx_packet();
      char *packet = socket->get_tx_umem_ptr() + tx_desc->addr;
      size_t len = craft_http_error_response(packet, error_code);

      tx_desc->client_fd = desc.packet->client_fd;
      tx_desc->len = static_cast<unsigned>(len);
      socket->wait_for_tx_release(tx_desc);
      socket->wait_for_rx_release(desc.packet);

      return Database::WORKER_TYPE::COUNT;
    }

    return Database::WORKER_TYPE::WRITE;
  }

  void Http::handle_rx(io_packet_desc *rx_desc)
  {
    auto *socket = reinterpret_cast<HttpServer::Socket *>(rx_desc->socket_owner);
    std::string_view view(socket->get_rx_umem_ptr() + rx_desc->addr, rx_desc->len);

    const auto [parsed, head] = this->parse_request_head(view, 0);

    if(parsed == HeadState::INCOMPLETE)
    {
      socket->close_connection(rx_desc->client_fd);
      socket->wait_for_rx_release(rx_desc);
      return;
    }

    Database::WORKER_TYPE type;
    Database::query_desc query_desc{};
    query_desc.packet = rx_desc;
    query_desc.gzip = head.gzip;

    const auto meth_enum = HttpServer::Http::toMethodEnum(head.method);

    switch(meth_enum)
    {
    case MethodEnum::GET:
      type = this->handle<MethodEnum::GET>(parsed, head, query_desc);
      break;

    case MethodEnum::POST:
      type = this->handle<MethodEnum::POST>(parsed, head, query_desc);
      break;

    case MethodEnum::PUT:
    case MethodEnum::DELETE:
    case MethodEnum::INVALID:
    default:
    {
      auto *tx_desc = socket->wait_for_tx_packet();
      char *packet = socket->get_tx_umem_ptr() + tx_desc->addr;
      size_t len = craft_http_error_response(packet, ErrorCode::UnsupportedMethod);

      tx_desc->client_fd = rx_desc->client_fd;
      tx_desc->len = static_cast<unsigned>(len);
      socket->wait_for_tx_release(tx_desc);
      socket->wait_for_rx_release(rx_desc);

      return;
    }
    break;
    }

    switch(type)
    {
    case Database::WORKER_TYPE::READ:
      db_handler.enqueue(Database::WORKER_TYPE::READ, query_desc);
      break;
    case Database::WORKER_TYPE::WRITE:
      db_handler.enqueue(Database::WORKER_TYPE::WRITE, query_desc);
      break;
    case Database::WORKER_TYPE::COUNT:
    default:
      break;
    }

    //
  }

} // namespace HttpServer::Http
