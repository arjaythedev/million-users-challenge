#pragma once

#include <cstddef>

#include "types.hpp"

namespace feed
{

struct HttpRequest
{
    View method, path;
    View authorization;
    View body;
    bool keep_alive = true;
    bool accepts_gzip = false;
};

enum class ParseStatus
{
    Incomplete,
    Complete,
    Bad,
    TooLarge
};

struct ParseResult
{
    ParseStatus status = ParseStatus::Incomplete;
    size_t consumed = 0;
    HttpRequest request;
};

ParseResult parse_http_request(View buffer);

}
