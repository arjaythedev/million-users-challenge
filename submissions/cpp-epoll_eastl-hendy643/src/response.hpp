#pragma once

#include <cstdint>

#include "types.hpp"

namespace feed
{

struct Post
{
    int64_t id = 0;
    String body;
    String created_at;
    String author;
    int32_t like_count = 0;
};

String format_created_at(View postgres_timestamptz);

void append_number(String &out, int64_t n);
void append_json_string(String &out, View raw);
void append_post(String &out, const Post &post);
String error_body(View message);

void append_http_response(String &out, int status, View body, bool keep_alive, bool gzip);

} // namespace feed
