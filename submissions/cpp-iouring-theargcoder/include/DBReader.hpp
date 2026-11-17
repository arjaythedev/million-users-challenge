#pragma once

#include <cstring>
#include <string_view>
#include <thread>

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
    bool gzip{ false };

    std::string_view body{};
    std::array<char, jwt_auth::MAX_USERNAME_SIZE> username{};
  };

  enum class WORKER_TYPE : uint8_t
  {
    READ,
    WRITE,
    COUNT,
  };

  template <size_t QUEUE_SIZE>
  class DBReader
  {
  private:
    std::array<std::vector<Queue::MPSC::Queue<QUEUE_SIZE, query_desc>>, static_cast<uint8_t>(WORKER_TYPE::COUNT)> queues;
    std::array<std::vector<std::jthread>, static_cast<uint8_t>(WORKER_TYPE::COUNT)> workers;
    signal_t selector;

  public:
    DBReader(const DBReader &) = delete;
    DBReader &operator=(const DBReader &) = delete;
    DBReader(DBReader &&) noexcept = delete;
    DBReader &operator=(DBReader &&) = delete;

    DBReader();
    ~DBReader();

  public:
    void enqueue(WORKER_TYPE type, query_desc &view);

  private:
    void read_worker(std::stop_token stp, unsigned thr_id);
    void write_worker(std::stop_token stp, unsigned thr_id);
  };

} // namespace HttpServer::Database
