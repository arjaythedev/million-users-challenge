#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <libpq-fe.h>

#include "include/Defines.hpp"

namespace HttpServer::Database::JSON
{
  // Every serializer writes directly to out.
  // No intermediate JSON strings or objects.

  // GET /health — Result: SELECT 1
  [[nodiscard]] Response health_response_json(const PGresult *result, const uint64_t &uptime_s, char *out);

  // GET /health when PostgreSQL is unavailable. HTTP 503.
  [[nodiscard]] Response health_degraded_response_json(std::string_view error, char *out);

  // GET /feed — five-column post SELECT, ORDER BY ... LIMIT 20.
  [[nodiscard]] Response feed_response_json(const PGresult *result, char *out);

  // GET /posts/:id — empty result produces HTTP 404.
  [[nodiscard]] Response post_response_json(const PGresult *result, char *out);

  // POST /posts — INSERT INTO posts ... RETURNING id, created_at.
  [[nodiscard]] Response create_post_response_json(const PGresult *result, std::string_view body, std::string_view username, char *out);

  // POST /posts/:id/like — affected_rows determines whether the like already existed.
  [[nodiscard]] Response like_response_json(const bool &like_created, const int64_t &post_id, char *out);

  // Ordinary API errors: {"error":"message"}
  [[nodiscard]] Response error_response_json(std::string_view message, const int &status, char *out);

  [[nodiscard]] std::string_view trim(std::string_view value) noexcept;

  [[nodiscard]] BodyError validate_create_post_body(std::string_view json, std::string_view &body) noexcept;

} // namespace HttpServer::Database::JSON
