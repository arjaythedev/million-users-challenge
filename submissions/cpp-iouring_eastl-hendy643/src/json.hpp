#pragma once

#include <EASTL/vector.h>

#include <expected>

#include "types.hpp"

namespace feed
{

enum class JsonType
{
    String,
    Number,
    Other
};

struct JsonField
{
    String key;
    JsonType type;
    String text;
};

struct JsonDocument
{
    bool is_object = false;
    eastl::vector<JsonField> fields;

    const JsonField *find(View key) const;
};

bool parse_json(View text, JsonDocument &doc);

enum class BodyError
{
    MalformedJson,
    BodyRequired,
    BodyTooLong
};

std::expected<String, BodyError> parse_create_post_body(View raw);

}
