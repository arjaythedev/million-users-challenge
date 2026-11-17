#include <csignal>
#include <print>

#include "include/Defines.hpp"
#include "include/HttpServer.hpp"
#include "include/Socket.hpp"

namespace
{
  std::atomic<bool> running{ true };

  void signal_handler(int signal)
  {
    if(signal == SIGTERM || signal == SIGINT)
    {
      std::print("Received stop signal ({}), shutting down gracefully...\n", signal);
      running.store(false, std::memory_order_seq_cst);
    }
  }

  int get_actual_vcpu_count()
  {
    cpu_set_t cs;
    CPU_ZERO(&cs);

    // Get the affinity mask of the current process (respects cgroups/docker)
    if(sched_getaffinity(0, sizeof(cs), &cs) == 0)
    {
      int count = CPU_COUNT(&cs);
      if(count > 0)
        return count;
    }

    // Fallback to configured processors if affinity fails
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if(nproc > 0)
      return static_cast<int>(nproc);

    return 4; // Safe baseline fallback for the challenge spec
  }

} // namespace

int main(int, char *[])
{
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGINT, signal_handler);

  try
  {
    HttpServer::DATABASE_URL = std::getenv("DATABASE_URL");
    HttpServer::JWT_SECRET = std::getenv("JWT_SECRET");
    HttpServer::HOST = std::getenv("HOST");
    HttpServer::PORT = std::getenv("PORT");
    HttpServer::EXTRA_PORT = std::getenv("EXTRA_PORT");

    const auto actual_cpu_count = get_actual_vcpu_count();
    HttpServer::N_WORKERS = actual_cpu_count;
    HttpServer::N_DB_READERS = actual_cpu_count;
    HttpServer::N_DB_WRITERS = actual_cpu_count;

    HttpServer::N_WORKERS = std::min(HttpServer::N_WORKERS, static_cast<unsigned>(HttpServer::MAX_SUPPORTED_WORKERS));

    std::println(stderr, "[startup] Initializing Socket");

    std::vector<std::jthread> threads;
    std::vector<HttpServer::Socket> sockets(HttpServer::N_WORKERS);

    std::println(stderr, "[startup] Socket initialized");

    HttpServer::start_time = std::chrono::steady_clock::now();

    std::println(stderr, "[startup] Initializing HTTP workers");

    std::println(stderr, "[startup] HTTP workers initialized");

    std::println(stderr, "[startup] Now running socket RX/TX function");

    for(unsigned i = 0; i < HttpServer::N_WORKERS - 1; i++)
    {
      threads.emplace_back([i, &sockets](std::stop_token) { sockets[i].rx_reciver(running); });
    }

    sockets.back().rx_reciver(running);

    threads.clear();
    sockets.clear();

    std::println(stderr, "[finish] SHUTTING DOWN ::socket");

    std::println(stderr, "[finish] Server stopped cleanly.\n");

    std::println(stderr, "[finish] Socket RX/TX workers initialized");
  }
  catch(std::exception &e)
  {
    std::println(stderr, "[ERROR] - {}", e.what());
  }
  catch(...)
  {
    std::println(stderr, "[FATAL] - unknow exception");
  }

  return 0;
}
