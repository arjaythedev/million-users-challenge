#pragma once

#include "types.hpp"

namespace feed
{
struct ServerConfig
{
    String host = "0.0.0.0";
    int port = 8080;
    int extra_port = 8081;
    String database_url;
    String jwt_secret;
};

int run_server(const ServerConfig &config);
} // namespace feed
