#pragma once

#include "include/Http.hpp"
#include "include/Socket.hpp"
#include <chrono>

namespace HttpServer
{
  extern const char *DATABASE_URL;
  extern const char *JWT_SECRET;
  extern const char *HOST;
  extern const char *PORT;
  extern const char *EXTRA_PORT;

  extern unsigned N_WORKERS;

  extern Socket *socket;
  extern std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<long, std::ratio<1, 1000000000>>> start_time;
  extern HttpServer::Http::Http *http;
} // namespace HttpServer
