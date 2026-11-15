#include "include/HttpServer.hpp"

namespace HttpServer
{
  const char *DATABASE_URL = nullptr;
  const char *JWT_SECRET = nullptr;
  const char *HOST = nullptr;
  const char *PORT = nullptr;
  const char *EXTRA_PORT = nullptr;

  unsigned N_WORKERS = 0;

  Socket *socket = nullptr;

  std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<long, std::ratio<1, 1000000000>>> start_time = std::chrono::steady_clock::now();

  HttpServer::Http::Http *http = nullptr;
} // namespace HttpServer
