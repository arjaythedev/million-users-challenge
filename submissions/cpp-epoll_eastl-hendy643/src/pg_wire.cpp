#include "pg_wire.hpp"

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>
#include <sys/random.h>

#include <cstring>

namespace feed::pg
{
namespace
{
constexpr uint32_t kProtocol3 = 196608;
constexpr uint32_t kMaxFrame = 1u << 26;

void put_i16(String &out, const unsigned v)
{
    const char b[2] = {static_cast<char>(v >> 8), static_cast<char>(v)};
    out.append(b, 2);
}

void put_i32(String &out, const uint32_t v)
{
    const char b[4] = {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
                       static_cast<char>(v)};
    out.append(b, 4);
}

void put_cstr(String &out, const View s)
{
    out.append(s.data(), s.size());
    out.push_back('\0');
}

size_t begin(String &out, const char tag)
{
    if (tag != '\0')
        out.push_back(tag);
    const size_t at = out.size();
    put_i32(out, 0);
    return at;
}

void end(String &out, const size_t at)
{
    const auto len = static_cast<uint32_t>(out.size() - at);
    char *p = const_cast<char *>(out.data()) + at;
    p[0] = static_cast<char>(len >> 24);
    p[1] = static_cast<char>(len >> 16);
    p[2] = static_cast<char>(len >> 8);
    p[3] = static_cast<char>(len);
}

uint32_t get_u32(const char *p)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

int hex_value(const char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool percent_decode(const View in, String &out)
{
    for (size_t i = 0; i < in.size(); i++)
    {
        if (in[i] != '%')
        {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size())
            return false;
        const int hi = hex_value(in[i + 1]);
        const int lo = hex_value(in[i + 2]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}

size_t find_char(const View s, const char c, const size_t from = 0)
{
    for (size_t i = from; i < s.size(); i++)
    {
        if (s[i] == c)
            return i;
    }
    return s.size();
}

size_t rfind_char(const View s, const char c)
{
    for (size_t i = s.size(); i > 0; i--)
    {
        if (s[i - 1] == c)
            return i - 1;
    }
    return s.size();
}

bool starts_with(const View s, const View prefix)
{
    return s.size() >= prefix.size() && std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

String base64(const unsigned char *data, const size_t n)
{
    unsigned char buf[96];
    size_t len = 0;
    if (n > 64 || mbedtls_base64_encode(buf, sizeof buf, &len, data, n) != 0)
        return {};
    return {reinterpret_cast<const char *>(buf), len};
}

bool hmac(const unsigned char *key, const size_t key_len, const View data, unsigned char (&out)[32])
{
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len,
                           reinterpret_cast<const unsigned char *>(data.data()), data.size(), out) == 0;
}

void civil_from_days(int64_t z, int64_t &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
}

void put_digits(String &out, int64_t v, const int width)
{
    char buf[24];
    int n = 0;
    do
    {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v > 0);
    for (int i = n; i < width; i++)
        out.push_back('0');
    while (n > 0)
        out.push_back(buf[--n]);
}
} // namespace

std::expected<Target, String> parse_url(const View url)
{
    View rest = url;
    constexpr View kSchemes[] = {"postgresql://", "postgres://"};
    bool known = false;
    for (const View scheme : kSchemes)
    {
        if (starts_with(rest, scheme))
        {
            rest = rest.substr(scheme.size());
            known = true;
            break;
        }
    }
    if (!known)
        return std::unexpected(String("DATABASE_URL must start with postgres://"));

    size_t authority_end = rest.size();
    for (size_t i = 0; i < rest.size(); i++)
    {
        if (rest[i] == '/' || rest[i] == '?')
        {
            authority_end = i;
            break;
        }
    }
    const View authority = rest.substr(0, authority_end);
    View path = rest.substr(authority_end);
    if (!path.empty() && path[0] == '/')
        path = path.substr(1);
    path = path.substr(0, find_char(path, '?'));

    View userinfo, hostport = authority;
    if (const size_t at = rfind_char(authority, '@'); at != authority.size())
    {
        userinfo = authority.substr(0, at);
        hostport = authority.substr(at + 1);
    }

    Target t;
    const size_t colon = find_char(userinfo, ':');
    if (!percent_decode(userinfo.substr(0, colon), t.user))
        return std::unexpected(String("bad %-escape in the user"));
    if (colon < userinfo.size() && !percent_decode(userinfo.substr(colon + 1), t.password))
    {
        return std::unexpected(String("bad %-escape in the password"));
    }
    if (!percent_decode(path, t.database))
        return std::unexpected(String("bad %-escape in the database name"));
    if (t.user.empty())
        return std::unexpected(String("DATABASE_URL has no user"));
    if (t.database.empty())
        t.database = t.user;

    View host = hostport, port;
    bool has_port = false;
    if (!hostport.empty() && hostport[0] == '[')
    {
        const size_t close = find_char(hostport, ']');
        if (close == hostport.size())
            return std::unexpected(String("unterminated [ in the host"));
        host = hostport.substr(1, close - 1);
        const View after = hostport.substr(close + 1);
        if (!after.empty())
        {
            if (after[0] != ':')
                return std::unexpected(String("junk after the host"));
            port = after.substr(1);
            has_port = true;
        }
    }
    else
    {
        const size_t c = find_char(hostport, ':');
        host = hostport.substr(0, c);
        if (c < hostport.size())
        {
            port = hostport.substr(c + 1);
            has_port = true;
        }
    }
    if (has_port && port.empty())
        return std::unexpected(String("empty port"));
    for (const char c : port)
    {
        if (c < '0' || c > '9')
            return std::unexpected(String("the port is not a number"));
    }
    if (port.size() > 5)
        return std::unexpected(String("the port is not a number"));
    t.host = host.empty() ? String("localhost") : String(host);
    t.port = port.empty() ? String("5432") : String(port);
    return t;
}

void append_startup(String &out, const View user, const View database, const tuning::Setting *settings,
                    const size_t settings_count)
{
    const size_t at = begin(out, '\0');
    put_i32(out, kProtocol3);
    put_cstr(out, "user");
    put_cstr(out, user);
    if (!database.empty())
    {
        put_cstr(out, "database");
        put_cstr(out, database);
    }
    for (size_t i = 0; i < settings_count; i++)
    {
        put_cstr(out, settings[i].name);
        put_cstr(out, settings[i].value);
    }
    out.push_back('\0');
    end(out, at);
}

void append_parse(String &out, const View name, const View sql, const uint32_t *types, const size_t type_count)
{
    const size_t at = begin(out, 'P');
    put_cstr(out, name);
    put_cstr(out, sql);
    put_i16(out, static_cast<unsigned>(type_count));
    for (size_t i = 0; i < type_count; i++)
        put_i32(out, types[i]);
    end(out, at);
}

void append_bind(String &out, const View statement, const Value *params, const size_t param_count)
{
    const size_t at = begin(out, 'B');
    put_cstr(out, "");
    put_cstr(out, statement);
    put_i16(out, 1);
    put_i16(out, 1);
    put_i16(out, static_cast<unsigned>(param_count));
    for (size_t i = 0; i < param_count; i++)
    {
        put_i32(out, static_cast<uint32_t>(params[i].size));
        if (params[i].size > 0)
            out.append(params[i].data, static_cast<size_t>(params[i].size));
    }
    put_i16(out, 1);
    put_i16(out, 1);
    end(out, at);
}

void append_execute(String &out)
{
    const size_t at = begin(out, 'E');
    put_cstr(out, "");
    put_i32(out, 0);
    end(out, at);
}

void append_sync(String &out)
{
    end(out, begin(out, 'S'));
}

void append_password(String &out, const View password)
{
    const size_t at = begin(out, 'p');
    put_cstr(out, password);
    end(out, at);
}

void append_sasl_initial(String &out, const View mechanism, const View data)
{
    const size_t at = begin(out, 'p');
    put_cstr(out, mechanism);
    put_i32(out, static_cast<uint32_t>(data.size()));
    out.append(data.data(), data.size());
    end(out, at);
}

void append_sasl_response(String &out, const View data)
{
    const size_t at = begin(out, 'p');
    out.append(data.data(), data.size());
    end(out, at);
}

FrameStatus next_frame(const View in, size_t &off, Frame &frame)
{
    if (in.size() - off < 5)
        return FrameStatus::Incomplete;
    const uint32_t len = get_u32(in.data() + off + 1);
    if (len < 4 || len > kMaxFrame)
        return FrameStatus::Bad;
    if (in.size() - off < static_cast<size_t>(len) + 1)
        return FrameStatus::Incomplete;
    frame.type = in[off];
    frame.body = View(in.data() + off + 5, len - 4);
    off += static_cast<size_t>(len) + 1;
    return FrameStatus::Ok;
}

ErrorInfo parse_error(const View body)
{
    ErrorInfo info;
    size_t i = 0;
    while (i < body.size() && body[i] != '\0')
    {
        const char field = body[i++];
        const size_t start = i;
        while (i < body.size() && body[i] != '\0')
            i++;
        if (i >= body.size())
            break;
        const View value = body.substr(start, i - start);
        if (field == 'C')
            info.sqlstate = String(value);
        if (field == 'M')
            info.message = String(value);
        i++;
    }
    return info;
}

int64_t command_rows(View tag)
{
    while (!tag.empty() && tag.back() == '\0')
        tag.remove_suffix(1);
    size_t digits = 0;
    while (digits < tag.size() && tag[tag.size() - 1 - digits] >= '0' && tag[tag.size() - 1 - digits] <= '9')
    {
        digits++;
    }
    if (digits == 0 || digits > 18 || digits == tag.size() || tag[tag.size() - 1 - digits] != ' ')
        return -1;
    int64_t n = 0;
    for (size_t i = tag.size() - digits; i < tag.size(); i++)
        n = n * 10 + (tag[i] - '0');
    return n;
}

bool split_row(const View body, View *columns, const size_t count)
{
    if (body.size() < 2)
        return false;
    const size_t declared =
        (static_cast<size_t>(static_cast<unsigned char>(body[0])) << 8) | static_cast<unsigned char>(body[1]);
    if (declared != count)
        return false;
    size_t off = 2;
    for (size_t i = 0; i < count; i++)
    {
        if (body.size() - off < 4)
            return false;
        const auto len = static_cast<int32_t>(get_u32(body.data() + off));
        off += 4;
        if (len < 0)
        {
            columns[i] = View();
            continue;
        }
        if (body.size() - off < static_cast<size_t>(len))
            return false;
        columns[i] = View(body.data() + off, static_cast<size_t>(len));
        off += static_cast<size_t>(len);
    }
    return off == body.size();
}

bool decode_int(const View column, int64_t &out)
{
    if (column.data() == nullptr)
        return false;
    if (column.size() != 2 && column.size() != 4 && column.size() != 8)
        return false;
    uint64_t v = static_cast<unsigned char>(column[0]);
    for (size_t i = 1; i < column.size(); i++)
        v = (v << 8) | static_cast<unsigned char>(column[i]);
    const unsigned bits = static_cast<unsigned>(column.size()) * 8;
    if (bits < 64 && (v >> (bits - 1)) != 0)
        v |= ~uint64_t{0} << bits;
    out = static_cast<int64_t>(v);
    return true;
}

void append_timestamp_text(String &out, const int64_t micros)
{
    constexpr int64_t kDay = 86'400'000'000;
    constexpr int64_t kDaysTo1970 = 10'957;
    int64_t days = micros / kDay;
    int64_t rest = micros % kDay;
    if (rest < 0)
    {
        rest += kDay;
        days--;
    }
    int64_t year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days + kDaysTo1970, year, month, day);
    const int64_t secs = rest / 1'000'000;
    put_digits(out, year, 4);
    out.push_back('-');
    put_digits(out, month, 2);
    out.push_back('-');
    put_digits(out, day, 2);
    out.push_back(' ');
    put_digits(out, secs / 3600, 2);
    out.push_back(':');
    put_digits(out, secs / 60 % 60, 2);
    out.push_back(':');
    put_digits(out, secs % 60, 2);
    out.push_back('.');
    put_digits(out, rest % 1'000'000, 6);
    out.append("+00", 3);
}

bool decode_post(const View row, Post &post)
{
    View c[5];
    int64_t like_count = 0, created = 0;
    if (!split_row(row, c, 5) || !decode_int(c[0], post.id) || c[1].data() == nullptr || c[2].size() != 8 ||
        !decode_int(c[2], created) || c[3].data() == nullptr || !decode_int(c[4], like_count))
    {
        return false;
    }
    post.body = String(c[1]);
    append_timestamp_text(post.created_at, created);
    post.author = String(c[3]);
    post.like_count = static_cast<int32_t>(like_count);
    return true;
}

bool decode_created(const View row, NewPost &created)
{
    View c[2];
    int64_t micros = 0;
    if (!split_row(row, c, 2) || !decode_int(c[0], created.id) || c[1].size() != 8 || !decode_int(c[1], micros))
    {
        return false;
    }
    append_timestamp_text(created.created_at, micros);
    return true;
}

void batch_fates(const DbKind *kinds, const size_t count, const size_t error_at, const ErrorInfo *error, Fate *fates)
{
    if (error_at >= count)
    {
        for (size_t i = 0; i < count; i++)
            fates[i] = Fate::Deliver;
        return;
    }
    const auto is_write = [](const DbKind k) { return k == DbKind::Create || k == DbKind::Like; };
    const bool own_fault = count == 1 || (error && error->sqlstate.size() >= 2 && error->sqlstate[0] == '2' &&
                                          (error->sqlstate[1] == '2' || error->sqlstate[1] == '3'));
    for (size_t i = 0; i < count; i++)
    {
        if (i < error_at)
        {
            fates[i] = is_write(kinds[i]) ? Fate::Replay : Fate::Deliver;
        }
        else if (i == error_at)
        {
            fates[i] = own_fault ? Fate::Error : Fate::Replay;
        }
        else
        {
            fates[i] = Fate::Replay;
        }
    }
}

Scram::Scram(const View user, const View password, const View nonce) :
    password_(password),
    nonce_(nonce)
{
    first_bare_ = "n=";
    for (const char c : user)
    {
        if (c == '=')
            first_bare_.append("=3D", 3);
        else if (c == ',')
            first_bare_.append("=2C", 3);
        else
            first_bare_.push_back(c);
    }
    first_bare_.append(",r=", 3);
    first_bare_.append(nonce_.data(), nonce_.size());
}

String Scram::first_message() const
{
    String m("n,,");
    m.append(first_bare_.data(), first_bare_.size());
    return m;
}

std::expected<String, String> Scram::final_message(const View server_first)
{
    View nonce, salt_b64, iterations;
    size_t i = 0;
    while (i < server_first.size())
    {
        size_t e = find_char(server_first, ',', i);
        const View field = server_first.substr(i, e - i);
        if (field.size() >= 2 && field[1] == '=')
        {
            if (field[0] == 'r')
                nonce = field.substr(2);
            if (field[0] == 's')
                salt_b64 = field.substr(2);
            if (field[0] == 'i')
                iterations = field.substr(2);
        }
        i = e + 1;
    }
    if (nonce.size() <= nonce_.size() || !starts_with(nonce, View(nonce_)))
    {
        return std::unexpected(String("SCRAM: the server's nonce does not extend ours"));
    }
    unsigned char salt[128];
    size_t salt_len = 0;
    if (salt_b64.empty() ||
        mbedtls_base64_decode(salt, sizeof salt, &salt_len, reinterpret_cast<const unsigned char *>(salt_b64.data()),
                              salt_b64.size()) != 0)
    {
        return std::unexpected(String("SCRAM: bad salt"));
    }
    if (iterations.empty() || iterations.size() > 8)
        return std::unexpected(String("SCRAM: bad iteration count"));
    uint32_t count = 0;
    for (const char c : iterations)
    {
        if (c < '0' || c > '9')
            return std::unexpected(String("SCRAM: bad iteration count"));
        count = count * 10 + static_cast<uint32_t>(c - '0');
    }
    if (count == 0 || count > 10'000'000)
        return std::unexpected(String("SCRAM: bad iteration count"));

    unsigned char salted[32], client_key[32], stored_key[32], client_signature[32], server_key[32];
    if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char *>(password_.data()),
                                      password_.size(), salt, salt_len, count, sizeof salted, salted) != 0 ||
        !hmac(salted, sizeof salted, "Client Key", client_key) ||
        mbedtls_sha256(client_key, sizeof client_key, stored_key, 0) != 0 ||
        !hmac(salted, sizeof salted, "Server Key", server_key))
    {
        return std::unexpected(String("SCRAM: key derivation failed"));
    }

    String without_proof("c=biws,r=");
    without_proof.append(nonce.data(), nonce.size());
    String auth_message = first_bare_;
    auth_message.push_back(',');
    auth_message.append(server_first.data(), server_first.size());
    auth_message.push_back(',');
    auth_message.append(without_proof.data(), without_proof.size());

    if (!hmac(stored_key, sizeof stored_key, View(auth_message), client_signature) ||
        !hmac(server_key, sizeof server_key, View(auth_message), server_signature_))
    {
        return std::unexpected(String("SCRAM: signature failed"));
    }
    unsigned char proof[32];
    for (size_t b = 0; b < sizeof proof; b++)
        proof[b] = client_key[b] ^ client_signature[b];
    answered_ = true;

    String message = without_proof;
    message.append(",p=", 3);
    const String encoded = base64(proof, sizeof proof);
    message.append(encoded.data(), encoded.size());
    return message;
}

bool Scram::verify(const View server_final) const
{
    if (!answered_)
        return false;
    const String expected = base64(server_signature_, sizeof server_signature_);
    if (server_final.size() != expected.size() + 2 || server_final[0] != 'v' || server_final[1] != '=')
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < expected.size(); i++)
        diff |= static_cast<unsigned char>(server_final[i + 2] ^ expected[i]);
    return diff == 0;
}

String random_nonce()
{
    unsigned char raw[18];
    size_t got = 0;
    while (got < sizeof raw)
    {
        const ssize_t n = getrandom(raw + got, sizeof raw - got, 0);
        if (n > 0)
            got += static_cast<size_t>(n);
    }
    return base64(raw, sizeof raw);
}
} // namespace feed::pg
