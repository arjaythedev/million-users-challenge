// Routing and the five endpoints (SPEC.md). Each handler is a coroutine that awaits one Postgres segment.
#include <charconv>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "auth.h"
#include "bufpool.h"
#include "codec.h"
#include "json_out.h"
#include "worker.h"
#include "yyjson.h"

namespace {

const auto g_started = std::chrono::steady_clock::now();

Reply err(int status, const char *msg) {
    std::string b = R"({"error":")";
    b += msg;
    b += "\"}";
    return {status, std::move(b)};
}
Reply internal_error() { return err(500, "internal server error"); }

void put_int(std::string &o, int64_t v) {
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, v);
    o.append(b, size_t(r.ptr - b));
}

// {"id":..,"body":..,"created_at":..,"author":..,"like_count":..}: this key order is part of the contract.
void post_json(std::string &o, int64_t id, std::string_view body, int64_t created_us, std::string_view author,
               int64_t likes) {
    json::lit(o, R"({"id":)");
    put_int(o, id);
    json::lit(o, R"(,"body":)");
    json::str(o, body.data(), body.size());
    json::lit(o, R"(,"created_at":")");
    codec::append_ts(o, created_us);
    json::lit(o, R"(","author":)");
    json::str(o, author.data(), author.size());
    json::lit(o, R"(,"like_count":)");
    put_int(o, likes);
    o.push_back('}');
}

// Row of the feed / post statements: id, body, created_at, author, like_count.
void post_row_json(std::string &o, const Res &r, int row) {
    post_json(o, codec::be64(r.raw(row, 0)), r.text(row, 1), codec::be64(r.raw(row, 2)), r.text(row, 3),
              codec::be64(r.raw(row, 4)));
}

// A positive int64 in decimal, nothing else.
bool parse_id(std::string_view s, int64_t &id) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), id);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && id > 0 && s[0] != '+';
}

// Go's strings.TrimSpace: ASCII space plus the Unicode White_Space characters.
size_t space_len(const char *p, size_t n) {  // length of a whitespace character at p, 0 if none
    unsigned char c = static_cast<unsigned char>(p[0]);
    if (c == ' ' || (c >= '\t' && c <= '\r')) return 1;
    if (n >= 2 && c == 0xC2 && (static_cast<unsigned char>(p[1]) == 0x85 || static_cast<unsigned char>(p[1]) == 0xA0)) return 2;
    if (n >= 3) {
        unsigned char d = static_cast<unsigned char>(p[1]), e = static_cast<unsigned char>(p[2]);
        if (c == 0xE1 && d == 0x9A && e == 0x80) return 3;
        if (c == 0xE2 && d == 0x80 && ((e >= 0x80 && e <= 0x8A) || e == 0xA8 || e == 0xA9 || e == 0xAF)) return 3;
        if (c == 0xE2 && d == 0x81 && e == 0x9F) return 3;
        if (c == 0xE3 && d == 0x80 && e == 0x80) return 3;
    }
    return 0;
}
std::string_view trim_space(std::string_view s) {
    for (size_t k; !s.empty() && (k = space_len(s.data(), s.size())); ) s.remove_prefix(k);
    while (!s.empty()) {
        size_t cut = 0;
        for (size_t k = 1; k <= 3 && k <= s.size(); k++)  // a trailing whitespace character is 1..3 bytes long
            if (space_len(s.data() + s.size() - k, k) == k) {
                cut = k;
                break;
            }
        if (!cut) break;
        s.remove_suffix(cut);
    }
    return s;
}

// Length in UTF-16 code units (what the 500-character limit counts).
size_t utf16_len(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;
        n += c >= 0xF0 ? 2 : 1;
    }
    return n;
}

}  // namespace

Task<Reply> Worker::route(const Request &rq) {
    std::string_view path = rq.path;
    const std::string &m = rq.method;
    const bool get = m == "GET", post = m == "POST";

    if (path == "/health") {
        if (get) co_return co_await h_health();
        co_return err(404, "not found");
    }
    if (path == "/feed") {
        if (get) co_return co_await h_feed();
        co_return err(404, "not found");
    }
    if (path.substr(0, 6) != "/posts" || (path.size() > 6 && path[6] != '/')) co_return err(404, "not found");

    std::string_view rest = path.substr(path.size() > 6 ? 7 : 6);  // after "/posts" and its slash
    const bool collection = path.size() == 6;
    std::string_view id_text = rest, action;
    if (!collection) {
        size_t s = rest.find('/');
        if (s != std::string_view::npos) {
            id_text = rest.substr(0, s);
            action = rest.substr(s + 1);
        }
        if (id_text.empty()) co_return err(404, "not found");
    }

    const bool is_post_one = !collection && action.empty();
    const bool is_like = !collection && action == "like";
    if (!(collection && post) && !(is_post_one && get) && !(is_like && post)) co_return err(404, "not found");

    if (is_post_one) {
        int64_t id;
        if (!parse_id(id_text, id)) co_return err(400, "invalid post id");
        co_return co_await h_get_post(id);
    }

    // The write endpoints need a valid token; it is checked before the id or the body.
    std::string_view a = rq.auth;
    if (!rq.has_auth || a.substr(0, 7) != "Bearer ") co_return err(401, "missing bearer token");
    a.remove_prefix(7);
    while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) a.remove_prefix(1);
    while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) a.remove_suffix(1);
    int64_t user = 0;
    std::string username;
    switch (g_jwt.verify(a, time(nullptr), user, username)) {
    case JwtStatus::Ok: break;
    case JwtStatus::Invalid: co_return err(401, "invalid or expired token");
    case JwtStatus::BadPayload: co_return err(401, "invalid token payload");
    }
    if (collection) co_return co_await h_create_post(rq, user, std::move(username));
    int64_t id;
    if (!parse_id(id_text, id)) co_return err(400, "invalid post id");
    co_return co_await h_like(user, id);
}

Task<Reply> Worker::h_health() {
    QVec qv;
    qv.push_back(Query("ping", {}));
    Rows rows = co_await db(std::move(qv));
    if (!rows.all_ok(1)) {
        co_return Reply{503, R"({"status":"degraded","db":"unreachable","error":"database unreachable"})"};
    }
    std::string o = R"({"status":"ok","db":"ok","uptime_s":)";
    put_int(o, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_started).count());
    o.push_back('}');
    co_return Reply{200, std::move(o)};
}

Task<Reply> Worker::h_feed() {
    QVec qv;
    qv.push_back(Query("feed", {}));
    Rows rows = co_await db(std::move(qv));
    if (!rows.all_ok(1)) [[unlikely]] co_return internal_error();
    const Res &r = rows[0];
    std::string o;
    take_buf(o);
    o.reserve(4096);
    json::lit(o, R"({"posts":[)");
    for (int i = 0; i < r.rows(); i++) {
        if (i) o.push_back(',');
        post_row_json(o, r, i);
    }
    json::lit(o, "]}");
    co_return Reply{200, std::move(o)};
}

Task<Reply> Worker::h_get_post(int64_t id) {
    QVec qv;
    qv.push_back(Query("post", {id}));
    Rows rows = co_await db(std::move(qv));
    if (!rows.all_ok(1)) [[unlikely]] co_return internal_error();
    const Res &r = rows[0];
    if (r.rows() == 0) co_return err(404, "post not found");
    std::string o;
    take_buf(o);
    o.reserve(512);
    json::lit(o, R"({"post":)");
    post_row_json(o, r, 0);
    o.push_back('}');
    co_return Reply{200, std::move(o)};
}

Task<Reply> Worker::h_create_post(const Request &rq, int64_t user, std::string username) {
    std::string_view raw = rq.body;
    std::string_view body;
    if (!trim_space(raw).empty()) {
        yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(raw.data()), raw.size(), YYJSON_READ_STOP_WHEN_DONE, nullptr, nullptr);
        if (!doc) co_return err(400, "malformed JSON body");
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (yyjson_is_obj(root)) {
            yyjson_val *b = yyjson_obj_get(root, "body");
            if (yyjson_is_str(b)) body = std::string_view(yyjson_get_str(b), yyjson_get_len(b));
        } else if (!yyjson_is_null(root)) {
            yyjson_doc_free(doc);
            co_return err(400, "malformed JSON body");
        }
        // `body` points into the document: copy it before the document goes away
        std::string text(body);
        yyjson_doc_free(doc);
        std::string_view t = trim_space(text);
        if (t.empty()) co_return err(400, "body is required");
        if (utf16_len(t) > 500) co_return err(400, "body must be at most 500 characters");
        QVec qv;
        qv.push_back(Query("post_insert", {user, t}));
        Rows rows = co_await db_write(std::move(qv));
        if (!rows.all_ok(1) || rows[0].rows() != 1) [[unlikely]] co_return internal_error();
        std::string o;
        take_buf(o);
        o.reserve(768);
        json::lit(o, R"({"post":)");
        post_json(o, codec::be64(rows[0].raw(0, 0)), t, codec::be64(rows[0].raw(0, 1)), username, 0);
        o.push_back('}');
        co_return Reply{201, std::move(o)};
    }
    co_return err(400, "body is required");
}

Task<Reply> Worker::h_like(int64_t user, int64_t post_id) {
    QVec qv;
    qv.push_back(Query("like_insert", {user, post_id}));
    Rows rows = co_await db_write(std::move(qv));
    if (!rows.ok || rows.r.size() != 1) [[unlikely]] co_return internal_error();
    const PgResult *p = rows[0].p;
    if (p->status != PgStatus::Ok) {
        if (p->state() == "23503") co_return err(404, "post not found");  // foreign key violation
        co_return internal_error();
    }
    const bool fresh = rows[0].rows() > 0;
    std::string o = R"({"liked":true,"already_liked":)";
    o += fresh ? "false" : "true";
    o += R"(,"post_id":)";
    put_int(o, post_id);
    o.push_back('}');
    co_return Reply{fresh ? 201 : 200, std::move(o)};
}
