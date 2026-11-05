#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "include/Defines.hpp"

namespace pqxx
{
  class result;
} // namespace pqxx

namespace HttpServer::Database::JSON
{
  // Every serializer writes directly to out.
  // No intermediate JSON strings or objects.

  // GET /health
  // Result: SELECT 1
  [[nodiscard]] Response health_response_json(const pqxx::result &result, const uint64_t &uptime_s, char *out);

  // GET /health when PostgreSQL is unavailable.
  // HTTP 503.
  [[nodiscard]] Response health_degraded_response_json(std::string_view error, char *out);

  // GET /feed
  // Result: five-column post SELECT, ORDER BY ... LIMIT 20.
  [[nodiscard]] Response feed_response_json(const pqxx::result &result, char *out);

  // GET /posts/:id
  // Empty result produces HTTP 404.
  [[nodiscard]] Response post_response_json(const pqxx::result &result, char *out);

  // POST /posts
  // Result: INSERT INTO posts ... RETURNING id, created_at.
  // body must already be trimmed and validated.
  // username comes directly from the validated JWT.
  [[nodiscard]] Response create_post_response_json(const pqxx::result &result, std::string_view body, std::string_view username, char *out);

  // POST /posts/:id/like
  // Result: INSERT ... ON CONFLICT DO NOTHING.
  // affected_rows() determines whether the like already existed.
  [[nodiscard]] Response like_response_json(const pqxx::result &result, const int64_t &post_id, char *out);

  // Ordinary API errors:
  // {"error":"message"}
  [[nodiscard]] Response error_response_json(std::string_view message, const int &status, char *out);

  [[nodiscard]] std::string_view trim(std::string_view value) noexcept;

  [[nodiscard]] BodyError validate_create_post_body(std::string_view json, std::string_view &body) noexcept;

} // namespace HttpServer::Database::JSON
