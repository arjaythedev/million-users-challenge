#pragma once

#include <array>
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
    std::vector<std::array<phr_header, 64>> headers;
    std::vector<HttpServer::jwt_auth::Verifier> verifiers;
    std::vector<Queue::SPSC::Queue<HTTP_QUEUE_SIZE, HttpServer::io_packet_desc *>> queues;
    std::vector<std::jthread> workers;
    std::vector<signal_t> signals;

  private:
    HttpServer::Database::DBReader<DBREADERS_QUEUE_SIZE> db_reader;

  public:
    void wakeup_all();
    void wakeup_mask(const __uint128_t &mask);
    bool enqueue(HttpServer::io_packet_desc *ptr);

  public:
    Http();
    ~Http();

  public:
    template <MethodEnum>
    void handle(const unsigned &thread_id, const HeadState &state, const RequestHead &head, jwt_auth::Verifier &verifier, HttpServer::io_packet_desc *packet);

    void worker(std::stop_token stp, const unsigned thread_id);

  public:
    [[nodiscard]] static bool ascii_iequals(std::string_view a, std::string_view b) noexcept;

    [[nodiscard]] std::tuple<HeadState, RequestHead> parse_request_head(std::string_view input, const size_t &previous_length, std::array<phr_header, 64> &header);
  };

} // namespace HttpServer::Http
