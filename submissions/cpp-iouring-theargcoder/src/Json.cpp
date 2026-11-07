
#include "include/Json.hpp"

#include <cstring>
#include <libpq-fe.h>

#include <pqxx/pqxx>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace HttpServer::Database::JSON
{
  namespace
  {
    // =========================================================================
    // DIRECT OUTPUT WRITER
    // =========================================================================

    class Writer
    {
    public:
      explicit Writer(char *out) noexcept : begin_(out), current_(out)
      {
      }

      void push(char value) noexcept
      {
        *current_++ = value;
      }

      void append(std::string_view value) noexcept
      {
        for(const char ch : value)
          *current_++ = ch;
      }

      [[nodiscard]] size_t size() const noexcept
      {
        return static_cast<size_t>(current_ - begin_);
      }

    private:
      char *begin_;
      char *current_;
    };

    // =========================================================================
    // RESULT VALIDATION
    // =========================================================================

    void require_columns(const pqxx::result &result, size_t expected)
    {
      if(std::cmp_not_equal(result.columns(), expected))
        throw std::runtime_error("Unexpected PostgreSQL column count");
    }

    void require_rows(const pqxx::result &result, size_t expected)
    {
      if(std::cmp_not_equal(result.size(), expected))
        throw std::runtime_error("Unexpected PostgreSQL row count");
    }

    void require_at_most_one_row(const pqxx::result &result)
    {
      if(result.size() > 1)
        throw std::runtime_error("Expected at most one PostgreSQL row");
    }

    // =========================================================================
    // PRIMITIVE FIELD ACCESS
    // =========================================================================

    [[nodiscard]] std::string_view field_view(const pqxx::result &result, size_t row, size_t column)
    {
      const auto field = result[static_cast<int>(row)][static_cast<int>(column)];

      if(field.is_null())
        throw std::runtime_error("Unexpected NULL PostgreSQL field");

      return field.view();
    }

    // =========================================================================
    // JSON STRING SERIALIZATION
    // =========================================================================

    void append_quoted(Writer &out, std::string_view value)
    {
      constexpr char hex[] = "0123456789abcdef";

      out.push('"');

      for(const char raw : value)
      {
        const auto ch = static_cast<unsigned char>(raw);

        switch(ch)
        {
        case '"':
          out.append("\\\"");
          break;

        case '\\':
          out.append("\\\\");
          break;

        case '\b':
          out.append("\\b");
          break;

        case '\f':
          out.append("\\f");
          break;

        case '\n':
          out.append("\\n");
          break;

        case '\r':
          out.append("\\r");
          break;

        case '\t':
          out.append("\\t");
          break;

        default:
          if(ch < 0x20)
          {
            out.append("\\u00");
            out.push(hex[ch >> 4]);
            out.push(hex[ch & 0x0f]);
          }
          else
          {
            out.push(static_cast<char>(ch));
          }

          break;
        }
      }

      out.push('"');
    }

    void append_field_string(Writer &out, const pqxx::result &result, size_t row, size_t column)
    {
      append_quoted(out, field_view(result, row, column));
    }

    // =========================================================================
    // INTEGER SERIALIZATION
    // =========================================================================

    // PostgreSQL textual integer representation is already decimal ASCII.
    //
    // Validate it and copy it directly into the destination.
    // No integer conversion or intermediate buffer.

    void append_integer(Writer &out, std::string_view value)
    {
      if(value.empty())
        throw std::runtime_error("Empty PostgreSQL integer");

      for(const char ch : value)
      {
        if(ch < '0' || ch > '9')
          throw std::runtime_error("Invalid PostgreSQL integer representation");
      }

      out.append(value);
    }

    void append_field_integer(Writer &out, const pqxx::result &result, size_t row, size_t column)
    {
      append_integer(out, field_view(result, row, column));
    }

    void append_u64(Writer &out, uint64_t value)
    {
      std::array<char, 20> buffer{};

      const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);

      if(error != std::errc{})
        throw std::runtime_error("Could not serialize unsigned integer");

      out.append(std::string_view(buffer.data(), static_cast<size_t>(end - buffer.data())));
    }

    void append_i64(Writer &out, int64_t value)
    {
      if(value <= 0)
        throw std::invalid_argument("Expected a positive integer");

      std::array<char, 20> buffer{};

      const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);

      if(error != std::errc{})
        throw std::runtime_error("Could not serialize integer");

      out.append(std::string_view(buffer.data(), static_cast<size_t>(end - buffer.data())));
    }

    // =========================================================================
    // BOOLEAN SERIALIZATION
    // =========================================================================

    void append_bool(Writer &out, bool value)
    {
      out.append(value ? "true" : "false");
    }

    // =========================================================================
    // TIMESTAMP SERIALIZATION
    // =========================================================================

    // Input: PostgreSQL timestamptz text representation.
    //
    // Example:
    // 2025-12-31 23:59:19.409123+00
    //
    // Output:
    // "2025-12-31T23:59:19.409Z"
    //
    // Requirements:
    //   - PostgreSQL connection timezone must be UTC.
    //   - DateStyle must be ISO, YMD.
    //   - Fractional precision is truncated to milliseconds.
    //   - No rounding.
    //   - No allocations.
    //   - No std::tm/gmtime_r conversions.

    void append_timestamp(Writer &out, std::string_view value)
    {
      // Minimum: YYYY-MM-DD HH:MM:SS+00
      if(value.size() < 22)
        throw std::runtime_error("Invalid PostgreSQL timestamp");

      if(value[4] != '-' || value[7] != '-' || value[10] != ' ' || value[13] != ':' || value[16] != ':')
      {
        throw std::runtime_error("Invalid PostgreSQL timestamp format");
      }

      // Locate the timezone.
      const size_t timezone_position = value.find_first_of("+-", 19);

      if(timezone_position == std::string_view::npos)
        throw std::runtime_error("Timestamp has no timezone");

      const auto timezone = value.substr(timezone_position);

      // Never emit a non-UTC timestamp with a Z suffix.
      if(timezone != "+00" && timezone != "+00:00")
        throw std::runtime_error("PostgreSQL connection timezone must be UTC");

      // Milliseconds from the fractional component.
      std::uint32_t milliseconds = 0;

      if(timezone_position != 19)
      {
        if(value[19] != '.')
          throw std::runtime_error("Invalid PostgreSQL timestamp fraction");

        constexpr size_t FRACTION_BEGIN = 20;

        const size_t fraction_size = timezone_position - FRACTION_BEGIN;

        // PostgreSQL TIMESTAMPTZ has at most six fractional digits.
        if(fraction_size == 0 || fraction_size > 6)
          throw std::runtime_error("Invalid PostgreSQL timestamp precision");

        // Copy the first three digits.
        // Pad with zeros if necessary.
        // Remaining digits are discarded, never rounded.
        for(size_t i = 0; i < 3; ++i)
        {
          milliseconds *= 10;

          if(i < fraction_size)
          {
            const char digit = value[FRACTION_BEGIN + i];

            if(digit < '0' || digit > '9')
              throw std::runtime_error("Invalid timestamp fraction");

            milliseconds += static_cast<std::uint32_t>(digit - '0');
          }
        }
      }

      constexpr std::array<size_t, 14> DIGIT_POSITIONS{ { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 } };

      for(const size_t position : DIGIT_POSITIONS)
      {
        if(value[position] < '0' || value[position] > '9')
          throw std::runtime_error("Invalid timestamp digits");
      }

      out.push('"');

      // YYYY-MM-DD
      out.append(value.substr(0, 10));

      out.push('T');

      // HH:MM:SS
      out.append(value.substr(11, 8));

      out.push('.');

      // Exactly three millisecond digits.
      const char ms[3]{ static_cast<char>('0' + (milliseconds / 100) % 10), static_cast<char>('0' + (milliseconds / 10) % 10), static_cast<char>('0' + milliseconds % 10) };

      out.append(std::string_view(ms, sizeof(ms)));

      out.append("Z\"");
    }

    void append_field_timestamp(Writer &out, const pqxx::result &result, size_t row, size_t column)
    {
      append_timestamp(out, field_view(result, row, column));
    }

    // =========================================================================
    // POST OBJECT
    // =========================================================================

    // Query columns:
    //
    // 0 id
    // 1 body
    // 2 created_at
    // 3 username
    // 4 like_count

    void append_post(Writer &out, const pqxx::result &result, size_t row)
    {
      out.append("{\"id\":");

      append_field_integer(out, result, row, 0);

      out.append(",\"body\":");

      append_field_string(out, result, row, 1);

      out.append(",\"created_at\":");

      append_field_timestamp(out, result, row, 2);

      out.append(",\"author\":");

      append_field_string(out, result, row, 3);

      out.append(",\"like_count\":");

      append_field_integer(out, result, row, 4);

      out.push('}');
    }

    // =========================================================================
    // ROOT RESPONSE HELPERS
    // =========================================================================

    [[nodiscard]] Response finish(Writer &writer, int status) noexcept
    {
      return { .size = writer.size(), .status = status };
    }

    [[nodiscard]] bool whitespace(char ch) noexcept
    {
      return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    }

    [[nodiscard]] bool read_hex_quad(const char *data, size_t size, size_t &position, uint32_t &value) noexcept
    {
      if(position > size || size - position < 4)
        return false;

      value = 0;

      for(size_t i = 0; i < 4; ++i)
      {
        const char ch = data[position++];

        value <<= 4;

        if(ch >= '0' && ch <= '9')
          value |= static_cast<uint32_t>(ch - '0');
        else if(ch >= 'a' && ch <= 'f')
          value |= static_cast<uint32_t>(ch - 'a' + 10);
        else if(ch >= 'A' && ch <= 'F')
          value |= static_cast<uint32_t>(ch - 'A' + 10);
        else
          return false;
      }

      return true;
    }

    void append_utf8(char *out, size_t &position, uint32_t codepoint) noexcept
    {
      if(codepoint <= 0x7F)
      {
        out[position++] = static_cast<char>(codepoint);
      }
      else if(codepoint <= 0x7FF)
      {
        out[position++] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[position++] = static_cast<char>(0x80 | (codepoint & 0x3F));
      }
      else if(codepoint <= 0xFFFF)
      {
        out[position++] = static_cast<char>(0xE0 | (codepoint >> 12));
        out[position++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[position++] = static_cast<char>(0x80 | (codepoint & 0x3F));
      }
      else
      {
        out[position++] = static_cast<char>(0xF0 | (codepoint >> 18));
        out[position++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out[position++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[position++] = static_cast<char>(0x80 | (codepoint & 0x3F));
      }
    }

    [[nodiscard]] bool decode_json_string_in_place(char *data, size_t size, size_t &consumed, size_t &decoded_size) noexcept
    {
      consumed = 0;
      decoded_size = 0;

      if(size == 0 || data[0] != '"')
        return false;

      size_t read = 1;
      size_t write = 1;

      while(read < size)
      {
        const unsigned char ch = static_cast<unsigned char>(data[read++]);

        // Closing quote.
        if(ch == '"')
        {
          consumed = read;
          decoded_size = write - 1;
          return true;
        }

        // Unescaped control characters are forbidden in JSON strings.
        if(ch < 0x20)
          return false;

        // Ordinary UTF-8 byte.
        if(ch != '\\')
        {
          data[write++] = static_cast<char>(ch);
          continue;
        }

        // Escape sequence.
        if(read >= size)
          return false;

        const char escaped = data[read++];

        switch(escaped)
        {
        case '"':
          data[write++] = '"';
          break;

        case '\\':
          data[write++] = '\\';
          break;

        case '/':
          data[write++] = '/';
          break;

        case 'b':
          data[write++] = '\b';
          break;

        case 'f':
          data[write++] = '\f';
          break;

        case 'n':
          data[write++] = '\n';
          break;

        case 'r':
          data[write++] = '\r';
          break;

        case 't':
          data[write++] = '\t';
          break;

        case 'u':
        {
          uint32_t codepoint = 0;

          if(!read_hex_quad(data, size, read, codepoint))
            return false;

          // UTF-16 high surrogate.
          if(codepoint >= 0xD800 && codepoint <= 0xDBFF)
          {
            // A high surrogate must be followed by \uXXXX.
            if(size - read < 6 || data[read] != '\\' || data[read + 1] != 'u')
            {
              return false;
            }

            read += 2;

            uint32_t low_surrogate = 0;

            if(!read_hex_quad(data, size, read, low_surrogate))
              return false;

            if(low_surrogate < 0xDC00 || low_surrogate > 0xDFFF)
              return false;

            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low_surrogate - 0xDC00);
          }
          else if(codepoint >= 0xDC00 && codepoint <= 0xDFFF)
          {
            // An isolated low surrogate is invalid.
            return false;
          }

          append_utf8(data, write, codepoint);
        }
        break;

        default:
          return false;
        }
      }

      // No closing quote.
      return false;
    }

  } // namespace

  // ===========================================================================
  // GET /health
  // ===========================================================================

  Response health_response_json(const pqxx::result &result, const uint64_t &uptime_s, char *out)
  {
    require_columns(result, 1);
    require_rows(result, 1);

    if(field_view(result, 0, 0) != "1")
      throw std::runtime_error("Health query returned an unexpected value");

    Writer writer(out);

    writer.append(R"({"status":"ok","db":"ok","uptime_s":)");

    append_u64(writer, uptime_s);

    writer.push('}');

    return finish(writer, 200);
  }

  // ===========================================================================
  // GET /health -- DEGRADED
  // ===========================================================================

  Response health_degraded_response_json(std::string_view error, char *out)
  {
    Writer writer(out);

    writer.append(R"({"status":"degraded","db":"unreachable","error":)");

    append_quoted(writer, error);

    writer.push('}');

    return finish(writer, 503);
  }

  // ===========================================================================
  // GET /feed
  // ===========================================================================

  Response feed_response_json(const pqxx::result &result, char *out)
  {
    require_columns(result, 5);

    if(result.size() > 20)
      throw std::runtime_error("Feed query returned more than 20 posts");

    Writer writer(out);

    writer.append("{\"posts\":[");

    const auto count = static_cast<size_t>(result.size());

    for(size_t row = 0; row < count; ++row)
    {
      if(row != 0)
        writer.push(',');

      append_post(writer, result, row);
    }

    writer.append("]}");

    return finish(writer, 200);
  }

  // ===========================================================================
  // GET /posts/:id
  // ===========================================================================

  Response post_response_json(const pqxx::result &result, char *out)
  {
    require_columns(result, 5);
    require_at_most_one_row(result);

    if(result.empty())
      return error_response_json("post not found", 404, out);

    Writer writer(out);

    writer.append("{\"post\":");

    append_post(writer, result, 0);

    writer.push('}');

    return finish(writer, 200);
  }

  // ===========================================================================
  // POST /posts
  // ===========================================================================

  // INSERT INTO posts (...) RETURNING id, created_at.
  //
  // Returned columns:
  //
  // 0 id
  // 1 created_at
  //
  // body and username are supplied from the validated request/JWT.
  // No additional user lookup is performed.

  Response create_post_response_json(const pqxx::result &result, std::string_view body, std::string_view username, char *out)
  {
    require_columns(result, 2);
    require_rows(result, 1);

    Writer writer(out);

    writer.append(R"({"post":{"id":)");

    append_field_integer(writer, result, 0, 0);

    writer.append(",\"body\":");

    append_quoted(writer, body);

    writer.append(",\"created_at\":");

    append_field_timestamp(writer, result, 0, 1);

    writer.append(",\"author\":");

    append_quoted(writer, username);

    writer.append(",\"like_count\":0}}");

    return finish(writer, 201);
  }

  // ===========================================================================
  // POST /posts/:id/like
  // ===========================================================================

  // INSERT INTO likes ...
  // ON CONFLICT (user_id, post_id) DO NOTHING
  //
  // affected_rows():
  // 1 -> newly inserted -> HTTP 201
  // 0 -> already existed -> HTTP 200
  // ===========================================================================
  // POST /posts/:id/like
  // ===========================================================================

  // INSERT INTO likes ...
  // ON CONFLICT (user_id, post_id) DO NOTHING
  //
  // affected_rows():
  // 1 -> newly inserted -> HTTP 201
  // 0 -> already existed -> HTTP 200
  Response like_response_json(const bool &like_created, const int64_t &post_id, char *out)
  {
    if(post_id <= 0)
      throw std::invalid_argument("Invalid post ID");

    const bool already_liked = !like_created;

    Writer writer(out);

    writer.append(R"({"liked":true,"already_liked":)");

    append_bool(writer, already_liked);
    writer.append(",\"post_id\":");

    append_i64(writer, post_id);

    writer.push('}');

    return finish(writer, like_created ? 201 : 200);
  }

  // ===========================================================================
  // ORDINARY ERRORS
  // ===========================================================================

  Response error_response_json(std::string_view message, const int &status, char *out)
  {
    Writer writer(out);

    writer.append("{\"error\":");

    append_quoted(writer, message);

    writer.push('}');

    return finish(writer, status);
  }

  std::string_view trim(std::string_view value) noexcept
  {
    while(!value.empty() && whitespace(value.front()))
      value.remove_prefix(1);

    while(!value.empty() && whitespace(value.back()))
      value.remove_suffix(1);

    return value;
  }

  BodyError validate_create_post_body(std::string_view json, std::string_view &body) noexcept
  {
    body = {};

    size_t position = 0;

    // Ignore leading JSON whitespace.
    while(position < json.size() && whitespace(json[position]))
      ++position;

    // An absent or whitespace-only request body means body is required.
    if(position == json.size())
      return BodyError::BodyRequired;

    // The root must be an object.
    if(json[position++] != '{')
      return BodyError::MalformedJson;

    while(position < json.size() && whitespace(json[position]))
      ++position;

    // An empty JSON object is valid JSON, but has no body property.
    if(position < json.size() && json[position] == '}')
    {
      ++position;

      while(position < json.size() && whitespace(json[position]))
        ++position;

      return position == json.size() ? BodyError::BodyRequired : BodyError::MalformedJson;
    }

    // Parse the property name.
    if(position >= json.size() || json[position] != '"')
      return BodyError::MalformedJson;

    const size_t key_begin = ++position;

    while(position < json.size() && json[position] != '"')
    {
      if(json[position] == '\\' || static_cast<unsigned char>(json[position]) < 0x20)
        return BodyError::MalformedJson;

      ++position;
    }

    if(position >= json.size())
      return BodyError::MalformedJson;

    const std::string_view key = json.substr(key_begin, position - key_begin);

    ++position;

    while(position < json.size() && whitespace(json[position]))
      ++position;

    if(position >= json.size() || json[position++] != ':')
      return BodyError::MalformedJson;

    while(position < json.size() && whitespace(json[position]))
      ++position;

    // The required property is absent.
    if(key != "body")
      return BodyError::BodyRequired;

    // A valid JSON value that isn't a string is not an acceptable body.
    // This handles numbers, booleans, null, arrays and objects.
    if(position >= json.size() || json[position] != '"')
      return BodyError::BodyRequired;

    auto *mutable_json = const_cast<char *>(json.data());

    size_t consumed = 0;
    size_t decoded_size = 0;

    if(!decode_json_string_in_place(mutable_json + position, json.size() - position, consumed, decoded_size))
    {
      return BodyError::MalformedJson;
    }

    // The opening quote occupies one byte.
    // The decoded body begins immediately after it.
    body = trim(std::string_view(mutable_json + position + 1, decoded_size));

    // Advance using the ORIGINAL encoded length, not the decoded length.
    position += consumed;

    while(position < json.size() && whitespace(json[position]))
      ++position;

    if(position >= json.size() || json[position++] != '}')
      return BodyError::MalformedJson;

    while(position < json.size() && whitespace(json[position]))
      ++position;

    if(position != json.size())
      return BodyError::MalformedJson;

    if(body.empty())
      return BodyError::BodyRequired;

    if(body.size() > 500)
      return BodyError::BodyTooLong;

    return BodyError::None;
  }

} // namespace HttpServer::Database::JSON
