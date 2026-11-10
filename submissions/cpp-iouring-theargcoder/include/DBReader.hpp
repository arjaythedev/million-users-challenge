#pragma once

#include <cstring>
#include <string_view>
#include <thread>

#include <pqxx/pqxx>

#include "Queue.hpp"
#include "include/Defines.hpp"
#include "include/Jwt.hpp"

namespace HttpServer::Database
{
  enum class Query : std::uint8_t
  {
    GetHealth,
    GetFeed,
    GetPostById,
    CreatePost,
    CreateLike
  };

  struct query_desc
  {
    Query query{};
    HttpServer::io_packet_desc *packet{};

    uint64_t user_id{};
    uint64_t post_id{};

    std::string_view body{};
    std::array<char, jwt_auth::MAX_USERNAME_SIZE> username{};
  };

  template <size_t QUEUE_SIZE>
  class DBReader
  {
  private:
    std::vector<Queue::SPSC::Queue<QUEUE_SIZE, query_desc>> queues;
    std::vector<signal_t> signals;
    std::vector<std::jthread> workers;

  public:
    DBReader(const DBReader &) = delete;
    DBReader &operator=(const DBReader &) = delete;
    DBReader(DBReader &&) noexcept = delete;
    DBReader &operator=(DBReader &&) = delete;

    DBReader();
    ~DBReader();

  public:
    void awake(unsigned thr_id);

    void enqueue(query_desc &view, unsigned thr_id);

  private:
    void worker(std::stop_token stp, unsigned thr_id);
  };

} // namespace HttpServer::Database
