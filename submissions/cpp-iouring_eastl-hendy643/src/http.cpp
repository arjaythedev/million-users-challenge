#include "http.hpp"

namespace feed
{
namespace
{
constexpr size_t kMaxHead = 16384;
constexpr size_t kMaxBody = 16384;

using SV = View;

char lower(const char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool iequals(const SV a, const SV b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (lower(a[i]) != b[i])
            return false;
    }
    return true;
}

SV trim(SV s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

bool has_token(SV list, const SV token)
{
    while (!list.empty())
    {
        const size_t comma = list.find(',');
        SV item = trim(list.substr(0, comma));
        item = trim(item.substr(0, item.find(';')));
        if (iequals(item, token))
            return true;
        if (comma == SV::npos)
            break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

View to_view(const SV s)
{
    return {s.data(), s.size()};
}
}

ParseResult parse_http_request(const View buffer)
{
    ParseResult result;
    const SV buf(buffer.data(), buffer.size());

    const size_t head_end = buf.find("\r\n\r\n");
    if (head_end == SV::npos)
    {
        result.status = buf.size() > kMaxHead ? ParseStatus::TooLarge : ParseStatus::Incomplete;
        return result;
    }
    if (head_end > kMaxHead)
    {
        result.status = ParseStatus::TooLarge;
        return result;
    }
    result.status = ParseStatus::Bad;

    SV head = buf.substr(0, head_end + 2);
    const size_t line_end = head.find("\r\n");
    const SV line = head.substr(0, line_end);
    head.remove_prefix(line_end + 2);
    const size_t sp1 = line.find(' ');
    if (sp1 == SV::npos || sp1 == 0)
        return result;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == SV::npos)
        return result;
    const SV method = line.substr(0, sp1);
    const SV target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const SV version = line.substr(sp2 + 1);
    if (target.empty() || target[0] != '/')
        return result;
    if (version != "HTTP/1.1" && version != "HTTP/1.0")
        return result;

    HttpRequest req;
    req.method = to_view(method);
    req.path = to_view(target.substr(0, target.find('?')));
    req.keep_alive = version == "HTTP/1.1";

    bool have_length = false;
    size_t length = 0;
    bool too_large = false;
    while (!head.empty())
    {
        const size_t eol = head.find("\r\n");
        const SV header = head.substr(0, eol);
        head.remove_prefix(eol + 2);
        const size_t colon = header.find(':');
        if (colon == SV::npos || colon == 0)
            return result;
        const SV name = header.substr(0, colon);
        const SV value = trim(header.substr(colon + 1));

        if (iequals(name, "content-length"))
        {
            if (value.empty() || value.size() > 18)
                return result;
            size_t n = 0;
            for (const char c : value)
            {
                if (c < '0' || c > '9')
                    return result;
                n = n * 10 + static_cast<size_t>(c - '0');
            }
            if (have_length && n != length)
                return result;
            have_length = true;
            length = n;
            too_large = n > kMaxBody;
        }
        else if (iequals(name, "transfer-encoding"))
        {
            return result;
        }
        else if (iequals(name, "authorization"))
        {
            req.authorization = to_view(value);
        }
        else if (iequals(name, "connection"))
        {
            if (has_token(value, "close"))
            {
                req.keep_alive = false;
            }
            else if (has_token(value, "keep-alive"))
            {
                req.keep_alive = true;
            }
        }
        else if (iequals(name, "accept-encoding"))
        {
            if (has_token(value, "gzip"))
                req.accepts_gzip = true;
        }
    }

    if (too_large)
    {
        result.status = ParseStatus::TooLarge;
        return result;
    }
    const size_t body_start = head_end + 4;
    if (buf.size() < body_start + length)
    {
        result.status = ParseStatus::Incomplete;
        return result;
    }
    req.body = to_view(buf.substr(body_start, length));
    result.status = ParseStatus::Complete;
    result.consumed = body_start + length;
    result.request = req;
    return result;
}
}
