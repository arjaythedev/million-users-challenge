// postgres/pool.cpp
// Пул libpq: 512 соединений, 4 prepared, unix-сокет, JSON-ответы.
// Протокол: <qname>\t<nparams>\t<p1_len>:<p1>\t...\n
// Ответ:    OK\t<json>\n  |  ERR\t<msg>\n
#include <libpq-fe.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <deque>
#include <thread>

static constexpr int POOL_SIZE = 512;
static const char* SOCK_PATH = "/tmp/feed_pool.sock";

// ---- 4 запроса из спеки, prepared ----
static const char* SQL_FEED =
    "SELECT p.id, p.body, p.created_at, u.username, "
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
    "FROM posts p JOIN users u ON u.id = p.user_id "
    "ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
static const char* SQL_POST_BY_ID =
    "SELECT p.id, p.body, p.created_at, u.username, "
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
    "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1";
static const char* SQL_INSERT_POST =
    "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
static const char* SQL_INSERT_LIKE =
    "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) "
    "ON CONFLICT (user_id, post_id) DO NOTHING";

// ---- пул ----
struct Pool {
    std::vector<PGconn*> all;
    std::deque<PGconn*> idle;
    std::mutex m;
    std::condition_variable cv;

    void init(const char* dsn) {
        for (int i = 0; i < POOL_SIZE; ++i) {
            PGconn* c = PQconnectdb(dsn);
            if (PQstatus(c) != CONNECTION_OK) {
                fprintf(stderr, "pg connect %d failed: %s\n", i, PQerrorMessage(c));
                exit(1);
            }

            PQexec(c, "SET TIME ZONE 'UTC'");
            
            PQprepare(c, "feed",        SQL_FEED,        0, nullptr);
            PQprepare(c, "post_by_id",  SQL_POST_BY_ID,  1, nullptr);
            PQprepare(c, "insert_post", SQL_INSERT_POST, 2, nullptr);
            PQprepare(c, "insert_like", SQL_INSERT_LIKE, 2, nullptr);
            all.push_back(c);
            idle.push_back(c);
        }
    }
    PGconn* acquire() {
        std::unique_lock lk(m);
        cv.wait(lk, [&]{ return !idle.empty(); });
        PGconn* c = idle.front(); idle.pop_front();
        return c;
    }
    void release(PGconn* c) {
        { std::lock_guard lk(m); idle.push_back(c); }
        cv.notify_one();
    }
};
static Pool g_pool;

// ---- JSON-эскейп ----
static void json_escape(std::string& out, const char* s, int len) {
    for (int i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); out += b; }
                else out += (char)ch;
        }
    }
}

// created_at: "2025-12-31 23:59:19.409123+00" -> "2025-12-31T23:59:19.409Z"
// Дробная часть может быть короче 3 цифр (PG обрезает trailing zeros):
// "2025-07-02 11:59:09.16+00" -> "2025-07-02T11:59:09.160Z"
// (truncate до миллисекунд, не round; правило 3: не в SQL)
static std::string fmt_created_at(const char* s) {
    std::string in(s);
    if (in.size() < 19) return in;
    std::string out;
    out.reserve(24);
    out.append(in, 0, 10); out += 'T';
    out.append(in, 11, 8); // HH:MM:SS
    out += '.';

    size_t dot = in.find('.', 19);
    size_t tz  = in.find_first_of("+-", 19);
    if (dot != std::string::npos && (tz == std::string::npos || dot < tz)) {
        size_t frac_end = (tz != std::string::npos) ? tz : in.size();
        std::string frac = in.substr(dot + 1, frac_end - dot - 1);
        if (frac.size() >= 3) {
            out.append(frac, 0, 3);           // truncate до 3
        } else {
            out += frac;                       // дополнить нулями
            out.append(3 - frac.size(), '0');
        }
    } else {
        out += "000";
    }
    out += 'Z';
    return out;
}

// ---- post-объект из PGresult ----
static void append_post(std::string& out, PGresult* r, int row) {
    int id        = atoi(PQgetvalue(r, row, 0));
    const char* b = PQgetvalue(r, row, 1);
    int blen      = PQgetlength(r, row, 1);
    const char* ca= PQgetvalue(r, row, 2);
    const char* au= PQgetvalue(r, row, 3);
    int aulen     = PQgetlength(r, row, 3);
    int lc        = atoi(PQgetvalue(r, row, 4));

    out += "{\"id\":"; out += std::to_string(id);
    out += ",\"body\":\""; json_escape(out, b, blen); out += '"';
    out += ",\"created_at\":\""; out += fmt_created_at(ca); out += '"';
    out += ",\"author\":\""; json_escape(out, au, aulen); out += '"';
    out += ",\"like_count\":"; out += std::to_string(lc); out += '}';
}

// ---- обработка одного запроса ----
static std::string handle(const std::string& qname, const std::vector<std::string>& params) {
    PGconn* c = g_pool.acquire();
    std::string out;
    PGresult* r = nullptr;

    if (qname == "feed") {
        r = PQexecPrepared(c, "feed", 0, nullptr, nullptr, nullptr, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) goto err;
        out = "{\"posts\":[";
        for (int i = 0; i < PQntuples(r); ++i) { if (i) out += ','; append_post(out, r, i); }
        out += "]}";
    }
    else if (qname == "post_by_id") {
        if (params.size() < 1) { g_pool.release(c); return "ERR\tbad params"; }
        {
            const char* v[] = { params[0].c_str() };
            r = PQexecPrepared(c, "post_by_id", 1, v, nullptr, nullptr, 0);
        }
        if (PQresultStatus(r) != PGRES_TUPLES_OK) goto err;
        if (PQntuples(r) == 0) { g_pool.release(c); PQclear(r); return "ERR\tpost not found"; }
        out = "{\"post\":"; append_post(out, r, 0); out += '}';
    }
    else if (qname == "insert_post") {
        if (params.size() < 2) { g_pool.release(c); return "ERR\tbad params"; }
        {
            const char* v[] = { params[0].c_str(), params[1].c_str() };
            r = PQexecPrepared(c, "insert_post", 2, v, nullptr, nullptr, 0);
        }
        if (PQresultStatus(r) != PGRES_TUPLES_OK) goto err;
        int id = atoi(PQgetvalue(r, 0, 0));
        std::string ca = fmt_created_at(PQgetvalue(r, 0, 1));
        out = "{\"id\":" + std::to_string(id) + ",\"created_at\":\"" + ca + "\"}";
    }
    else if (qname == "insert_like") {
        if (params.size() < 2) { g_pool.release(c); return "ERR\tbad params"; }
        {
            const char* v[] = { params[0].c_str(), params[1].c_str() };
            r = PQexecPrepared(c, "insert_like", 2, v, nullptr, nullptr, 0);
        }
        if (PQresultStatus(r) != PGRES_COMMAND_OK) {
            const char* sqlstate = PQresultErrorField(r, PG_DIAG_SQLSTATE);
            if (sqlstate && strcmp(sqlstate, "23503") == 0) {
                g_pool.release(c); PQclear(r); return "ERR\tpost not found";
            }
            goto err;
        }
        int affected = atoi(PQcmdTuples(r));
        bool already = (affected == 0);
        out = std::string("{\"liked\":true,\"already_liked\":") + (already ? "true" : "false");
        out += ",\"post_id\":" + params[1] + "}";
    }
    else if (qname == "health") {
        r = PQexec(c, "SELECT 1");
        if (PQresultStatus(r) != PGRES_TUPLES_OK) goto err;
        out = "{\"db\":\"ok\"}";
    }
    else {
        g_pool.release(c); return "ERR\tunknown query";
    }

    g_pool.release(c);
    PQclear(r);
    return "OK\t" + out;

err:
    {
        std::string msg = PQresultErrorMessage(r);
        g_pool.release(c); PQclear(r);
        return "ERR\t" + msg;
    }
}

// ---- парсинг протокола ----
static bool parse_line(const std::string& line, std::string& qname, std::vector<std::string>& params) {
    size_t p = 0;
    auto next_tab = [&]() -> size_t { auto t = line.find('\t', p); return t == std::string::npos ? line.size() : t; };
    size_t t = next_tab();
    qname = line.substr(0, t); p = (t == line.size() ? t : t + 1);
    if (p >= line.size()) return true; // без параметров
    t = next_tab();
    int n = atoi(line.substr(p, t - p).c_str()); p = (t == line.size() ? t : t + 1);
    for (int i = 0; i < n && p < line.size(); ++i) {
        auto colon = line.find(':', p);
        if (colon == std::string::npos) return false;
        int len = atoi(line.substr(p, colon - p).c_str());
        p = colon + 1;
        if (p + (size_t)len > line.size()) return false;
        params.emplace_back(line.substr(p, len));
        p += len;
        if (p < line.size() && line[p] == '\t') ++p;
    }
    return true;
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    const char* dsn = getenv("DATABASE_URL");
    if (!dsn) { fprintf(stderr, "DATABASE_URL not set\n"); return 1; }

    fprintf(stderr, "pool: connecting %d...\n", POOL_SIZE);
    g_pool.init(dsn);
    fprintf(stderr, "pool: ready\n");

    unlink(SOCK_PATH);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{}; sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof sa.sun_path - 1);
    if (bind(lfd, (sockaddr*)&sa, sizeof sa) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 1024) < 0) { perror("listen"); return 1; }
    chmod(SOCK_PATH, 0666);

    for (;;) {
        int fd = accept(lfd, nullptr, nullptr);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        std::thread([fd]{
            std::string buf;
            char tmp[4096];
            for (;;) {
                ssize_t n = read(fd, tmp, sizeof tmp);
                if (n <= 0) break;
                buf.append(tmp, (size_t)n);
                auto nl = buf.find('\n');
                if (nl != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    std::string qname; std::vector<std::string> params;
                    std::string resp;
                    if (!parse_line(line, qname, params)) resp = "ERR\tbad protocol\n";
                    else resp = handle(qname, params) + "\n";
                    (void)!write(fd, resp.data(), resp.size());
                    buf.erase(0, nl + 1);
                }
            }
            close(fd);
        }).detach();
    }
    close(lfd);
    return 0;
}