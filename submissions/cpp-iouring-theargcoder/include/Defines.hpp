#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace HttpServer
{
  inline constexpr unsigned CHUNK_SIZE = 4096 * 5;
  inline constexpr unsigned RING_BUFFER_SIZE = 1 << 14; // 1 << 16 blows up for some reason

  // Total chunks split 50/50 between RX and TX to satisfy the assertions
  inline constexpr unsigned RX_FRAME_COUNT = RING_BUFFER_SIZE;
  inline constexpr unsigned TX_FRAME_COUNT = RING_BUFFER_SIZE;
  inline constexpr unsigned CHUNK_COUNT = RX_FRAME_COUNT + TX_FRAME_COUNT;

  inline constexpr unsigned UMEM_LEN = CHUNK_SIZE * CHUNK_COUNT; // 512 MB UMEM pool
  inline constexpr uint16_t BUF_GID = 1;

  inline constexpr unsigned RX_FRAME_BEGIN = 0;
  inline constexpr unsigned TX_FRAME_BEGIN = RX_FRAME_COUNT;
  inline constexpr uint64_t TX_UMEM_BASE = static_cast<uint64_t>(TX_FRAME_BEGIN) * CHUNK_SIZE;

  inline constexpr uint32_t PENDING_RECV_QUEUE_SIZE = 1U << 20; // 1'048'576

  static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE - 1U)) == 0, "RING_BUFFER_SIZE must be a power of two");
  static_assert(RX_FRAME_COUNT == RING_BUFFER_SIZE, "RX frame count must match descriptor ring size");
  static_assert(TX_FRAME_COUNT == RING_BUFFER_SIZE, "TX frame count must match descriptor ring size");

  static_assert(CHUNK_SIZE > 0, "CHUNK_SIZE must be nonzero");
  static_assert(TX_FRAME_COUNT > 0, "TX_FRAME_COUNT must be nonzero");
  static_assert(TX_UMEM_BASE <= UMEM_LEN, "TX_UMEM_BASE exceeds UMEM");
  static_assert(TX_FRAME_COUNT <= (UMEM_LEN - TX_UMEM_BASE) / CHUNK_SIZE, "TX frame allocation exceeds UMEM bounds");

  static_assert((PENDING_RECV_QUEUE_SIZE & (PENDING_RECV_QUEUE_SIZE - 1U)) == 0);

  inline constexpr unsigned HTTP_QUEUE_SIZE = RING_BUFFER_SIZE; // so its 1:1 ratio
  inline constexpr unsigned DBREADERS_QUEUE_SIZE = 1 << 14;     // so its 1:2 ratio

  struct io_packet_desc
  {
    uint64_t addr{}; // Offset into UMEM, not a userspace pointer.
    uint32_t len{};
    uint32_t flags{};
    int client_fd{ -1 };
  };

  namespace Http
  {
    enum class ErrorCode : uint8_t
    {
      UnknownPath,
      InvalidPostId,
      PostNotFound,
      MissingBearerToken,
      InvalidOrExpiredToken,
      InvalidTokenPayload,
      MalformedJsonBody,
      BodyRequired,
      BodyTooLong,
      InternalServerError,
      // count
      Count
    };

    inline static constexpr std::array<std::string_view, static_cast<size_t>(ErrorCode::Count)> JSON_ERROR_MESSAGES
        = { { R"({"error":"not found"})", R"({"error":"invalid post id"})", R"({"error":"post not found"})", R"({"error":"missing bearer token"})",
              R"({"error":"invalid or expired token"})", R"({"error":"invalid token payload"})", R"({"error":"malformed JSON body"})", R"({"error":"body is required"})",
              R"({"error":"body must be at most 500 characters"})", R"({"error":"internal server error"})" } };

    static std::string_view ErrorCodeErrorMessageStr(const ErrorCode &err)
    {
      return JSON_ERROR_MESSAGES[static_cast<unsigned>(err)];
    }

    static std::string_view ErrorCodeHttpNumberStr(const ErrorCode &err)
    {
      switch(err)
      {
      case ErrorCode::UnknownPath:
      case ErrorCode::PostNotFound:
        return "404";
      case ErrorCode::MissingBearerToken:
      case ErrorCode::InvalidOrExpiredToken:
      case ErrorCode::InvalidTokenPayload:
        return "401";
      case ErrorCode::MalformedJsonBody:
      case ErrorCode::InvalidPostId:
      case ErrorCode::BodyRequired:
      case ErrorCode::BodyTooLong:
        return "400";
      case ErrorCode::InternalServerError:
      case ErrorCode::Count:
      default:
        return "500"; // Fallback
      }
    }

    enum class HeadState : uint8_t
    {
      COMPLETE,
      INCOMPLETE,
      MALFORMED
    };

    enum class AuthPolicy : uint8_t
    {
      NONE,
      OPTIONAL,
      REQUIRED,
      ERROR
    };

    enum class MethodEnum : uint8_t
    {
      GET,
      POST,
      PUT,
      DELETE,
      INVALID,
    };

    struct AuthDecision
    {
      bool accepted;
      uint64_t viewer;
      int status;
    };

    struct RequestHead
    {
      std::string_view method;
      std::string_view path;
      std::optional<std::string_view> authorization;
      size_t header_bytes{ 0 };
      int minor_version{ 0 };
      bool duplicate_authorization{ false };
    };

    static MethodEnum toMethodEnum(std::string_view view)
    {
      // clang-format off
    if (view == "GET") {return MethodEnum::GET;}
    else if (view == "POST") {return MethodEnum::POST;}
    else if (view == "PUT") {return MethodEnum::PUT;}
    else if (view == "DELETE") {return MethodEnum::DELETE;}
    else {return MethodEnum::INVALID;}
      // clang-format on
    }
    static unsigned craft_http_error_response(char *packet, const ErrorCode &error)
    {
      constexpr std::string_view HTTP_TYPE = "HTTP/1.1 ";
      constexpr std::string_view HTTP_ERROR = " ERROR\r\n";
      constexpr std::string_view HTTP_CONTENT_TYPE = "Content-Type: application/json; charset=utf-8\r\n";
      constexpr std::string_view HTTP_CONTENT_LEN = "Content-Length: ";
      constexpr std::string_view HTTP_CONNECTION = "\r\nConnection: keep-alive\r\n\r\n";

      size_t len = 0;
      const std::string_view HTTP_ERROR_CODE = ErrorCodeHttpNumberStr(error);
      const std::string_view HTTP_ERROR_MSG = ErrorCodeErrorMessageStr(error);

      std::memcpy(&packet[len], HTTP_TYPE.data(), HTTP_TYPE.size()), len += HTTP_TYPE.size();
      std::memcpy(&packet[len], HTTP_ERROR_CODE.data(), HTTP_ERROR_CODE.size()), len += HTTP_ERROR_CODE.size();
      std::memcpy(&packet[len], HTTP_ERROR.data(), HTTP_ERROR.size()), len += HTTP_ERROR.size();
      std::memcpy(&packet[len], HTTP_CONTENT_TYPE.data(), HTTP_CONTENT_TYPE.size()), len += HTTP_CONTENT_TYPE.size();
      std::memcpy(&packet[len], HTTP_CONTENT_LEN.data(), HTTP_CONTENT_LEN.size()), len += HTTP_CONTENT_LEN.size();

      const auto &[ptr_2, errc_2] = std::to_chars(&packet[len], &packet[len] + 10, HTTP_ERROR_MSG.size());
      len += static_cast<size_t>(ptr_2 - &packet[len]);

      std::memcpy(&packet[len], HTTP_CONNECTION.data(), HTTP_CONNECTION.size()), len += HTTP_CONNECTION.size();
      std::memcpy(&packet[len], HTTP_ERROR_MSG.data(), HTTP_ERROR_MSG.size()), len += HTTP_ERROR_MSG.size();

      return static_cast<unsigned>(len);
    }

  } // namespace Http

  namespace Database
  {
    static std::string_view status_line(int status) noexcept
    {
      switch(status)
      {
      case 200:
        return "HTTP/1.1 200 OK\r\n";
      case 201:
        return "HTTP/1.1 201 Created\r\n";
      case 400:
        return "HTTP/1.1 400 Bad Request\r\n";
      case 401:
        return "HTTP/1.1 401 Unauthorized\r\n";
      case 404:
        return "HTTP/1.1 404 Not Found\r\n";
      case 503:
        return "HTTP/1.1 503 Service Unavailable\r\n";
      default:
        return "HTTP/1.1 500 Internal Server Error\r\n";
      }
    };
  } // namespace Database

  namespace Database::JSON
  {
    enum class BodyError : uint8_t
    {
      None,
      MalformedJson,
      BodyRequired,
      BodyTooLong
    };

    struct Response
    {
      size_t size{};
      int status{};
    };

    [[nodiscard]] static Http::ErrorCode body_error_to_http_error(Database::JSON::BodyError error) noexcept
    {
      using enum Database::JSON::BodyError;

      switch(error)
      {
      case None:
        return Http::ErrorCode::Count;

      case MalformedJson:
        return Http::ErrorCode::MalformedJsonBody;

      case BodyRequired:
        return Http::ErrorCode::BodyRequired;

      case BodyTooLong:
        return Http::ErrorCode::BodyTooLong;
      }

      return Http::ErrorCode::InternalServerError;
    }

  } // namespace Database::JSON

} // namespace HttpServer
