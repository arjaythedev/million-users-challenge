#pragma once

#include <array>
#include <cstddef>
#include <stop_token>
#include <string_view>

#include "dependencies/picohttpparser.h"

#include "include/DBReader.hpp"
#include "include/Defines.hpp"
#include "include/Jwt.hpp"

namespace HttpServer::Http
{
  class Http
  {
  private:
    std::array<phr_header, 64> header;
    HttpServer::jwt_auth::Verifier verifier;

  private:
    HttpServer::Database::DBReader<DBREADERS_QUEUE_SIZE> db_handler;

  public:
    Http();
    ~Http();

  public:
    template <MethodEnum>
    Database::WORKER_TYPE handle(const HeadState &state, const RequestHead &head, Database::query_desc &packet);

    void handle_rx(io_packet_desc *rx_desc);

  public:
    [[nodiscard]] static bool ascii_iequals(std::string_view a, std::string_view b) noexcept;

    [[nodiscard]] std::tuple<HeadState, RequestHead> parse_request_head(std::string_view input, const size_t &previous_length);
  };

} // namespace HttpServer::Http
