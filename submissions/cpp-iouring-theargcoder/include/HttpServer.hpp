#pragma once

#include "include/Http.hpp"
#include <chrono>

namespace HttpServer
{
  extern const char *DATABASE_URL;
  extern const char *JWT_SECRET;
  extern const char *HOST;
  extern const char *PORT;
  extern const char *EXTRA_PORT;

  extern unsigned N_WORKERS;
  extern unsigned N_DB_READERS;
  extern unsigned N_DB_WRITERS;

  extern std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<long, std::ratio<1, 1000000000>>> start_time;
} // namespace HttpServer
