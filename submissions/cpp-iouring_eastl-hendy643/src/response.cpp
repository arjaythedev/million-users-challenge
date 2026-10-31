#include "response.hpp"

#include <EASTL/vector.h>
#include <libdeflate.h>

#include <array>
#include <charconv>

#include "tuning.hpp"

namespace feed
{
namespace
{
const char *reason(const int status)
{
    switch (status)
    {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 404:
            return "Not Found";
        case 413:
            return "Payload Too Large";
        case 503:
            return "Service Unavailable";
        default:
            return "Internal Server Error";
    }
}

struct Compressor
{
    libdeflate_compressor *handle = nullptr;
    int level = 0;

    Compressor() = default;
    Compressor(const Compressor &) = delete;
    Compressor &operator=(const Compressor &) = delete;

    ~Compressor()
    {
        if (handle)
            libdeflate_free_compressor(handle);
    }

    bool use_level(const int wanted)
    {
        if (handle && level == wanted)
            return true;
        if (handle)
            libdeflate_free_compressor(handle);
        handle = libdeflate_alloc_compressor(wanted);
        level = wanted;
        return handle != nullptr;
    }
};

bool gzip_body(const View body, const char *&data, size_t &size)
{
    thread_local Compressor compressor;
    thread_local eastl::vector<unsigned char> scratch;
    if (!compressor.use_level(tuning::kGzipLevel))
        return false;
    scratch.resize(libdeflate_gzip_compress_bound(compressor.handle, body.size()));
    const size_t produced =
        libdeflate_gzip_compress(compressor.handle, body.data(), body.size(), scratch.data(), scratch.size());
    if (produced == 0 || produced >= body.size())
        return false;
    data = reinterpret_cast<const char *>(scratch.data());
    size = produced;
    return true;
}

void put(String &out, const View s)
{
    out.append(s.data(), s.size());
}

}

void append_number(String &out, const int64_t n)
{
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, n);
    out.append(buf, static_cast<size_t>(r.ptr - buf));
}

namespace
{
void append_timestamp(String &out, const View ts)
{
    out.append(ts.data(), 10);
    out.push_back('T');
    out.append(ts.data() + 11, 8);
    out.push_back('.');
    int digits = 0;
    if (size_t i = 19; i < ts.size() && ts[i] == '.')
    {
        for (++i; i < ts.size() && ts[i] >= '0' && ts[i] <= '9'; ++i)
        {
            if (digits < 3)
                out.push_back(ts[i]);
            digits++;
        }
    }
    for (; digits < 3; digits++)
        out.push_back('0');
    out.push_back('Z');
}

constexpr std::array<bool, 256> kEscape = [] {
    std::array<bool, 256> t{};
    for (size_t i = 0; i < 0x20; i++)
        t[i] = true;
    t['"'] = true;
    t['\\'] = true;
    return t;
}();
}

String format_created_at(const View postgres_timestamptz)
{
    if (postgres_timestamptz.size() < 19)
        return {postgres_timestamptz};
    String out;
    append_timestamp(out, postgres_timestamptz);
    return out;
}

void append_json_string(String &out, const View raw)
{
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    size_t run = 0;
    for (size_t i = 0; i < raw.size(); i++)
    {
        const auto c = static_cast<unsigned char>(raw[i]);
        if (!kEscape[c])
            continue;
        out.append(raw.data() + run, i - run);
        run = i + 1;
        switch (c)
        {
            case '"':
                put(out, "\\\"");
                break;
            case '\\':
                put(out, "\\\\");
                break;
            case '\n':
                put(out, "\\n");
                break;
            case '\r':
                put(out, "\\r");
                break;
            case '\t':
                put(out, "\\t");
                break;
            case '\b':
                put(out, "\\b");
                break;
            case '\f':
                put(out, "\\f");
                break;
            default:
                put(out, "\\u00");
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
        }
    }
    out.append(raw.data() + run, raw.size() - run);
    out.push_back('"');
}

void append_post(String &out, const Post &post)
{
    put(out, "{\"id\":");
    append_number(out, post.id);
    put(out, ",\"body\":");
    append_json_string(out, View(post.body));
    put(out, ",\"created_at\":");
    if (post.created_at.size() < 19)
    {
        append_json_string(out, View(post.created_at));
    }
    else
    {
        out.push_back('"');
        append_timestamp(out, View(post.created_at));
        out.push_back('"');
    }
    put(out, ",\"author\":");
    append_json_string(out, View(post.author));
    put(out, ",\"like_count\":");
    append_number(out, post.like_count);
    out.push_back('}');
}

String error_body(const View message)
{
    String out("{\"error\":");
    append_json_string(out, message);
    out.push_back('}');
    return out;
}

void append_http_response(String &out, const int status, const View body, const bool keep_alive, const bool gzip)
{
    const char *payload = body.data();
    size_t payload_size = body.size();
    const bool zipped = gzip && body.size() >= tuning::kMinGzipBody && gzip_body(body, payload, payload_size);

    put(out, "HTTP/1.1 ");
    append_number(out, status);
    out.push_back(' ');
    put(out, reason(status));
    put(out, "\r\nContent-Type: application/json\r\nContent-Length: ");
    append_number(out, static_cast<int64_t>(payload_size));
    if (zipped)
        put(out, "\r\nContent-Encoding: gzip");
    if (!keep_alive)
        put(out, "\r\nConnection: close");
    put(out, "\r\n\r\n");
    out.append(payload, payload_size);
}
}
