#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <stop_token>
#include <string_view>
#include <thread>

#include "dependencies/picohttpparser.h"

#include "include/DBReader.hpp"
#include "include/Defines.hpp"
#include "include/Jwt.hpp"
#include "include/Queue.hpp"

namespace HttpServer::Http
{
  class Http
  {
  private:
    Queue::SPSC::Queue<HTTP_QUEUE_SIZE, HttpServer::io_packet_desc *> queue;

  public:
    std::array<phr_header, 64> headers{};
    HttpServer::jwt_auth::Verifier verifier;

  private:
    std::jthread thread_rx;
    alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> rx_signal = 0;

  private:
    HttpServer::Database::DBReader<DBREADERS_QUEUE_SIZE> db_reader;

  public:
    // clang-format off
    void wakeup_rx() { this->rx_signal.fetch_add(1, std::memory_order_seq_cst), this->rx_signal.notify_one(); }
    [[nodiscard]] bool dequeue(HttpServer::io_packet_desc *&ptr) { return this->queue.dequeue(ptr); }
    [[nodiscard]] bool enqueue(HttpServer::io_packet_desc *ptr) { return this->queue.enqueue(ptr); }
    // clang-format on

  public:
    Http();
    ~Http();

  public:
    template <MethodEnum>
    void handle(const HeadState &state, const RequestHead &head, HttpServer::io_packet_desc *);

    void rx_worker(std::stop_token);

  public:
    [[nodiscard]] static bool ascii_iequals(std::string_view a, std::string_view b) noexcept;

    [[nodiscard]] std::tuple<HeadState, RequestHead> parse_request_head(std::string_view input, const size_t &previous_length);
  };

} // namespace HttpServer::Http
