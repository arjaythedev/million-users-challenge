#include <atomic>
#include <csignal>
#include <print>

#include "include/HttpServer.hpp"

std::atomic<bool> running{ true };

namespace
{
  void signal_handler(int signal)
  {
    if(signal == SIGTERM || signal == SIGINT)
    {
      std::print("Received stop signal ({}), shutting down gracefully...\n", signal);
      running = false;
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

    HttpServer::N_WORKERS = static_cast<unsigned>(std::max(get_actual_vcpu_count() - 2, 1));

    std::println("[info] Numer of cpus is '{}'", HttpServer::N_WORKERS);

    std::println(stderr, "[startup] Initializing Socket");

    HttpServer::socket = new HttpServer::Socket();

    std::println(stderr, "[startup] Socket initialized");

    HttpServer::start_time = std::chrono::steady_clock::now();

    std::println(stderr, "[startup] Initializing HTTP workers");

    HttpServer::http = new HttpServer::Http::Http();

    std::println(stderr, "[startup] HTTP workers initialized");

    while(running)
    {
      HttpServer::io_packet_desc *pkt_desc = HttpServer::socket->wait_for_rx_packet(running);

      if(pkt_desc == nullptr)
      {
        if(!running)
        {
          break;
        }
        continue;
      }

      if(!HttpServer::http->enqueue(pkt_desc))
      {
        std::print("ERROR - failed to enqueue pkt_desc into http\n");
      }

      HttpServer::http->wakeup_rx();
    }

    if(HttpServer::http != nullptr)
    {
      std::print("SHUTTING DOWN ::http ");
      delete HttpServer::http;
    }

    if(HttpServer::socket != nullptr)
    {
      std::print("SHUTTING DOWN ::socket");
      delete HttpServer::socket;
    }

    std::print("Server stopped cleanly.\n");
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
