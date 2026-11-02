#define _GNU_SOURCE
#include "postgres_fe.h"
#include "libpq-int.h"
#include "app.h"
#include <errno.h>
#include <linux/io_uring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>

/* libpq only logs in and prepares the statements. Once a connection is idle its socket moves to
   io_uring: queries are written as raw Bind/Execute/Sync messages and DataRows are serialized
   straight into the HTTP response, without libpq results, Describe round trips or extra syscalls. */

#define PIPELINE_LIMIT 256
#define WAITING_LIMIT 16384
#define CONNECT_TIMEOUT 5000
#define REQUEST_TIMEOUT 30000
#define INPUT_SIZE (256 * 1024)

typedef enum { DB_DOWN, DB_CONNECTING, DB_PREPARING, DB_READY } DbState;
typedef struct {
    Request *request;
    uint64_t sent_ms;
    unsigned rows;
    bool done, failed, retry, group_end;
} Pending;
typedef struct {
    PGconn *connection;
    DbState state;
    int registered_fd, fd;
    uint32_t interest;
    bool recv_busy, send_busy, synced, group_open, group_error;
    uint64_t open_ns;
    unsigned cursor;
    unsigned index, generation, head, count, prepared, batch;
    uint64_t retry_ms, connect_ms;
    Buffer in, out, sending;
    size_t sent;
    Pending pending[PIPELINE_LIMIT];
} DbConnection;
struct DbPool {
    int epoll_fd;
    char *url;
    unsigned reads, writes, total, batch, sticky[2];
    uint64_t now_ns, flush_ns;
    uint64_t now_ms, tick_ms;
    DbConnection *connections;
    Request *waiting_head[2], *waiting_tail[2];
    unsigned waiting_count[2];
};

static const char *const statement_names[] = { "health", "feed", "post", "create", "like" };
static const char *const statement_sql[] = {
    "SELECT 1",
    "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
    "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1",
    "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
    "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
};
static const int statement_parameters[] = { 0, 0, 1, 2, 2 };

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static uint32_t db_tag(DbConnection *db) { return (db->generation << 9) | db->index; }
static uint32_t get32(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) | ((uint32_t)u[2] << 8) | u[3];
}
static int64_t get64(const char *p) { return (int64_t)(((uint64_t)get32(p) << 32) | get32(p + 4)); }
static void put16(char *p, unsigned v) { p[0] = (char)(v >> 8); p[1] = (char)v; }
static void put32(char *p, uint32_t v) { p[0] = (char)(v >> 24); p[1] = (char)(v >> 16); p[2] = (char)(v >> 8); p[3] = (char)v; }
static void put64(char *p, int64_t v) { put32(p, (uint32_t)((uint64_t)v >> 32)); put32(p + 4, (uint32_t)v); }

static void database_error(Request *request, const char *message) {
    if (request->route != HEALTH) {
        request_error(request, 500, "internal server error");
        return;
    }
    request->response.len = 0;
    request->status = 503;
    if (!buffer_text(&request->response, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":") ||
        !buffer_json(&request->response, message, strlen(message)) ||
        !buffer_text(&request->response, "}"))
        request_error(request, 500, "internal server error");
}

static void disconnect_database(DbPool *pool, DbConnection *db, const char *message, bool complete) {
    char error[512];
    snprintf(error, sizeof(error), "%s", message ? message : "database unavailable");
    if (db->registered_fd >= 0)
        epoll_ctl(pool->epoll_fd, EPOLL_CTL_DEL, db->registered_fd, NULL);
    /* Wakes outstanding ring operations; their buffers stay allocated until they complete. */
    if (db->fd >= 0) shutdown(db->fd, SHUT_RDWR);
    db->fd = -1;
    db->registered_fd = -1;
    db->interest = 0;
    db->state = DB_DOWN;
    db->retry_ms = pool->now_ms + 1000;
    if (db->connection) PQfinish(db->connection);
    db->connection = NULL;
    db->out.len = 0;
    db->group_open = db->group_error = false;
    db->cursor = 0;
    while (db->count) {
        Pending *pending = &db->pending[db->head];
        Request *request = pending->request;
        memset(pending, 0, sizeof(*pending));
        db->head = (db->head + 1) % PIPELINE_LIMIT;
        db->count--;
        if (complete) {
            database_error(request, error);
            app_complete(request);
        } else {
            request_destroy(request);
        }
    }
    db->head = 0;
    db->prepared = 0;
}

static bool watch_database(DbPool *pool, DbConnection *db, uint32_t interest) {
    int fd = PQsocket(db->connection);
    if (fd < 0) return false;
    if (fd == db->registered_fd && interest == db->interest) return true;
    struct epoll_event event = { .events = interest | EPOLLRDHUP };
    event.data.u64 = DB_EVENT | ((uint64_t)db->generation << 9) | db->index;
    int operation = EPOLL_CTL_MOD;
    if (fd != db->registered_fd) {
        if (db->registered_fd >= 0)
            epoll_ctl(pool->epoll_fd, EPOLL_CTL_DEL, db->registered_fd, NULL);
        db->registered_fd = -1;
        operation = EPOLL_CTL_ADD;
    }
    if (epoll_ctl(pool->epoll_fd, operation, fd, &event) < 0) {
        if (operation != EPOLL_CTL_MOD || errno != ENOENT ||
            epoll_ctl(pool->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)
            return false;
    }
    db->registered_fd = fd;
    db->interest = interest;
    return true;
}

static bool flush_libpq(DbPool *pool, DbConnection *db) {
    int result = PQflush(db->connection);
    if (result < 0 || !watch_database(pool, db, EPOLLIN | (result ? EPOLLOUT : 0))) {
        disconnect_database(pool, db, PQerrorMessage(db->connection), true);
        return false;
    }
    return true;
}

static bool arm_recv(DbConnection *db) {
    if (db->in.cap - db->in.len < 8192) {
        size_t cap = db->in.cap ? db->in.cap * 2 : INPUT_SIZE;
        char *p = realloc(db->in.data, cap);
        if (!p) return false;
        db->in.head = 0;
        db->in.data = p;
        db->in.cap = cap;
    }
    struct io_uring_sqe *sqe = app_sqe(DB_KIND_RECV, db_tag(db));
    if (!sqe) return false;
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = db->fd;
    sqe->addr = (uint64_t)(uintptr_t)(db->in.data + db->in.len);
    sqe->len = (uint32_t)(db->in.cap - db->in.len);
    db->recv_busy = true;
    return true;
}

static void submit_send(DbConnection *db) {
    if (db->send_busy || db->fd < 0) return;
    if (db->sent == db->sending.len) {
        if (!db->out.len) return;
        Buffer swap = db->sending;
        db->sending = db->out;
        db->out = swap;
        db->out.len = 0;
        db->sent = 0;
    }
    struct io_uring_sqe *sqe = app_sqe(DB_KIND_SEND, db_tag(db));
    if (!sqe) return;
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = db->fd;
    sqe->addr = (uint64_t)(uintptr_t)(db->sending.data + db->sent);
    sqe->len = (uint32_t)(db->sending.len - db->sent);
    sqe->msg_flags = MSG_NOSIGNAL;
    db->send_busy = true;
}

/* The connection is idle after preparing: hand its socket to io_uring, unless old ring
   operations on this slot are still draining. */
static void take_over(DbPool *pool, DbConnection *db) {
    if (db->recv_busy || db->send_busy) return;
    PGconn *conn = db->connection;
    if (conn->inStart != conn->inEnd || conn->outCount) {
        disconnect_database(pool, db, "unexpected database data", true);
        return;
    }
    if (db->registered_fd >= 0) epoll_ctl(pool->epoll_fd, EPOLL_CTL_DEL, db->registered_fd, NULL);
    db->registered_fd = -1;
    db->fd = PQsocket(conn);
    db->in.len = db->out.len = db->sending.len = db->sent = 0;
    db->state = DB_READY;
    if (!arm_recv(db)) disconnect_database(pool, db, "database receive failed", true);
}

static void start_preparing(DbPool *pool, DbConnection *db) {
    if (PQsetnonblocking(db->connection, 1) != 0 || !PQenterPipelineMode(db->connection)) {
        disconnect_database(pool, db, PQerrorMessage(db->connection), true);
        return;
    }
    db->state = DB_PREPARING;
    db->prepared = 0;
    db->synced = false;
    for (unsigned i = 0; i < 5; i++) {
        Oid types[2] = { 20, i == POST_CREATE ? 25 : 20 };
        if (!PQsendPrepare(db->connection, statement_names[i], statement_sql[i], statement_parameters[i], types)) {
            disconnect_database(pool, db, PQerrorMessage(db->connection), true);
            return;
        }
    }
    if (!PQpipelineSync(db->connection)) {
        disconnect_database(pool, db, PQerrorMessage(db->connection), true);
        return;
    }
    flush_libpq(pool, db);
}

static void poll_connection(DbPool *pool, DbConnection *db) {
    PostgresPollingStatusType result = PQconnectPoll(db->connection);
    if (result == PGRES_POLLING_OK) {
        start_preparing(pool, db);
    } else if (result == PGRES_POLLING_READING || result == PGRES_POLLING_WRITING) {
        if (!watch_database(pool, db, result == PGRES_POLLING_READING ? EPOLLIN : EPOLLOUT))
            disconnect_database(pool, db, "database socket registration failed", true);
    } else {
        disconnect_database(pool, db, PQerrorMessage(db->connection), true);
    }
}

static void start_connection(DbPool *pool, DbConnection *db) {
    db->generation = (db->generation + 1) & 0x3fffff;
    if (!db->generation) db->generation = 1;
    const char *keywords[] = { "dbname", "options", "fallback_application_name", "sslmode", NULL };
    const char *values[] = { pool->url, "-c timezone=UTC -c datestyle=ISO,YMD", "compact-feed", "disable", NULL };
    db->connection = PQconnectStartParams(keywords, values, 1);
    db->connect_ms = pool->now_ms;
    db->state = DB_CONNECTING;
    if (!db->connection) {
        disconnect_database(pool, db, "database connection allocation failed", true);
        return;
    }
    if (PQstatus(db->connection) == CONNECTION_BAD) {
        disconnect_database(pool, db, PQerrorMessage(db->connection), true);
        return;
    }
    poll_connection(pool, db);
}

static void prepare_results(DbPool *pool, DbConnection *db) {
    while (db->state == DB_PREPARING) {
        if (PQstatus(db->connection) == CONNECTION_BAD) {
            disconnect_database(pool, db, PQerrorMessage(db->connection), true);
            return;
        }
        if (PQisBusy(db->connection)) return;
        PGresult *result = PQgetResult(db->connection);
        if (!result) continue;
        ExecStatusType status = PQresultStatus(result);
        if (status == PGRES_COMMAND_OK) {
            db->prepared++;
        } else if (status == PGRES_PIPELINE_SYNC && db->prepared == 5) {
            PQclear(result);
            db->synced = true;
            take_over(pool, db);
            return;
        } else {
            char message[512];
            snprintf(message, sizeof(message), "%s", PQresultErrorMessage(result));
            PQclear(result);
            disconnect_database(pool, db, *message ? message : "database prepare failed", true);
            return;
        }
        PQclear(result);
    }
}

DbPool *db_create(int epoll_fd, const char *url, unsigned reads, unsigned writes) {
    if (!url || !reads || reads > 512 || writes > 512 || reads + writes > 512) return NULL;
    DbPool *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pool->epoll_fd = epoll_fd;
    pool->reads = reads;
    pool->writes = writes;
    pool->total = reads + writes;
    pool->url = strdup(url);
    pool->connections = calloc(pool->total, sizeof(*pool->connections));
    if (!pool->url || !pool->connections) {
        free(pool->url);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    pool->now_ms = monotonic_ms();
    const char *batch = getenv("DB_BATCH");
    pool->batch = batch && atoi(batch) > 0 ? (unsigned)atoi(batch) : 64;
    const char *flush = getenv("DB_FLUSH_US");
    pool->flush_ns = (flush && atoi(flush) >= 0 ? (uint64_t)atoi(flush) : 200) * 1000;
    pool->sticky[0] = pool->sticky[1] = UINT32_MAX;
    for (unsigned i = 0; i < pool->total; i++) {
        DbConnection *db = &pool->connections[i];
        db->index = i;
        db->registered_fd = db->fd = -1;
        start_connection(pool, db);
    }
    return pool;
}

static unsigned request_group(DbPool *pool, Request *request) {
    return pool->writes && (request->route == POST_CREATE || request->route == POST_LIKE);
}

/* Reads: each tick fills one connection with up to `batch` queries before moving to the least
   loaded one, so a batch costs one send and one backend wakeup.
   Writes: group commit. A write group goes only to an idle connection; while every connection
   is committing, writes wait and the next idle connection takes them all as one transaction. */
static DbConnection *select_connection(DbPool *pool, unsigned group, bool *available) {
    unsigned start = group ? pool->reads : 0, end = group ? pool->total : pool->reads;
    unsigned limit = group ? PIPELINE_LIMIT : pool->batch;
    DbConnection *sticky = pool->sticky[group] < pool->total ? &pool->connections[pool->sticky[group]] : NULL;
    if (sticky && sticky->state == DB_READY && sticky->count < PIPELINE_LIMIT && sticky->batch < limit &&
        (!group || sticky->group_open)) {
        *available = true;
        return sticky;
    }
    DbConnection *selected = NULL;
    *available = false;
    for (unsigned i = start; i < end; i++) {
        DbConnection *db = &pool->connections[i];
        if (db->state != DB_READY) continue;
        *available = true;
        if (group && db->count) continue;
        if (db->count < PIPELINE_LIMIT && (!selected || db->count < selected->count)) selected = db;
    }
    if (selected) {
        pool->sticky[group] = selected->index;
        selected->batch = 0;
    }
    return selected;
}

static void close_group(DbConnection *db);
static char *reserve(Buffer *b, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 16384;
        while (cap < b->len + n) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return NULL;
        b->data = p;
        b->cap = cap;
    }
    char *p = b->data + b->len;
    b->len += n;
    return p;
}

static void send_request(DbPool *pool, DbConnection *db, Request *request) {
    (void)pool;
    if (request->route == POST_CREATE && memchr(request->body, '\0', request->body_len)) {
        request_error(request, 500, "internal server error");
        app_complete(request);
        return;
    }
    const char *name = statement_names[request->route];
    size_t name_len = strlen(name) + 1;
    unsigned params = (unsigned)statement_parameters[request->route];
    size_t param_bytes = params == 0 ? 0 : params == 1 ? 12 : request->route == POST_CREATE ? 16 + request->body_len : 24;
    size_t bind_len = 4 + 1 + name_len + 2 + 2 + 2 + param_bytes + 2 + 2;
    char *p = reserve(&db->out, 1 + bind_len + 10);
    if (!p) {
        request_error(request, 500, "internal server error");
        app_complete(request);
        return;
    }
    *p++ = 'B'; put32(p, (uint32_t)bind_len); p += 4;
    *p++ = 0;
    memcpy(p, name, name_len); p += name_len;
    put16(p, 1); put16(p + 2, 1); put16(p + 4, params); p += 6;
    if (request->route == POST_GET) {
        put32(p, 8); put64(p + 4, request->id); p += 12;
    } else if (params == 2) {
        put32(p, 8); put64(p + 4, request->user_id); p += 12;
        if (request->route == POST_CREATE) {
            put32(p, (uint32_t)request->body_len); memcpy(p + 4, request->body, request->body_len); p += 4 + request->body_len;
        } else {
            put32(p, 8); put64(p + 4, request->id); p += 12;
        }
    }
    put16(p, 1); put16(p + 2, 1); p += 4;
    *p++ = 'E'; put32(p, 9); p[4] = 0; put32(p + 5, 0);
    unsigned index = (db->head + db->count) % PIPELINE_LIMIT;
    request->status = 200;
    db->pending[index] = (Pending){ .request = request, .sent_ms = pool->now_ms };
    db->count++;
    db->batch++;
    if (!db->group_open) db->open_ns = pool->now_ns;
    db->group_open = true;
    /* A retried request runs in its own transaction, so one failing write cannot keep rolling
       back the others. */
    if (request->retries) close_group(db);
}

/* Ends the connection's current group with one Sync: Postgres runs the group as one implicit
   transaction, so a batch of writes shares a single commit. */
static void close_group(DbConnection *db) {
    if (!db->group_open) return;
    char *p = reserve(&db->out, 5);
    if (!p) return;
    p[0] = 'S'; put32(p + 1, 4);
    db->pending[(db->head + db->count - 1) % PIPELINE_LIMIT].group_end = true;
    db->group_open = false;
}

static void post_fields(const char *row, uint32_t len, const char **values, int32_t *lengths, bool *ok) {
    *ok = false;
    if (len < 2 || (unsigned char)row[0] != 0 || row[1] != 5) return;
    uint32_t at = 2;
    for (unsigned i = 0; i < 5; i++) {
        if (at + 4 > len) return;
        int32_t n = (int32_t)get32(row + at);
        at += 4;
        if (n < 0 || (uint32_t)n > len - at) return;
        values[i] = row + at;
        lengths[i] = n;
        at += (uint32_t)n;
    }
    *ok = lengths[0] == 8 && lengths[2] == 8 && lengths[4] == 4;
}

static bool data_row(Pending *pending, const char *row, uint32_t len) {
    Request *request = pending->request;
    Buffer *out = &request->response;
    const char *values[5];
    int32_t lengths[5];
    bool ok;
    switch (request->route) {
        case HEALTH:
            pending->rows++;
            return true;
        case FEED:
            post_fields(row, len, values, lengths, &ok);
            if (!ok) return false;
            if (pending->rows++ ? !buffer_append(out, ",", 1) : !buffer_append(out, "{\"posts\":[", 10)) return false;
            return buffer_post_row(out, values, lengths);
        case POST_GET:
            post_fields(row, len, values, lengths, &ok);
            if (!ok || pending->rows++) return false;
            return buffer_text(out, "{\"post\":") && buffer_post_row(out, values, lengths) && buffer_text(out, "}");
        case POST_CREATE: {
            if (pending->rows++ || len < 26 || row[0] != 0 || row[1] != 2 || get32(row + 2) != 8 || get32(row + 14) != 8) return false;
            request->status = 201;
            return buffer_text(out, "{\"post\":{\"id\":") && buffer_integer(out, get64(row + 6)) &&
                buffer_text(out, ",\"body\":") && buffer_json(out, request->body, request->body_len) &&
                buffer_text(out, ",\"created_at\":") && buffer_timestamp(out, row + 18, 8) &&
                buffer_text(out, ",\"author\":") && buffer_json(out, request->username, request->username_len) &&
                buffer_text(out, ",\"like_count\":0}}");
        }
        case POST_LIKE:
            return false;
    }
    return false;
}

static bool command_complete(Pending *pending, const char *tag, uint32_t len) {
    Request *request = pending->request;
    Buffer *out = &request->response;
    switch (request->route) {
        case HEALTH:
            return pending->rows == 1 && buffer_text(out, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":") &&
                buffer_integer(out, (int64_t)app_uptime()) && buffer_text(out, "}");
        case FEED:
            if (!pending->rows && !buffer_text(out, "{\"posts\":[")) return false;
            return buffer_text(out, "]}");
        case POST_GET:
            if (!pending->rows) request_error(request, 404, "post not found");
            return true;
        case POST_CREATE:
            return pending->rows == 1;
        case POST_LIKE: {
            bool inserted = len == 10 && !memcmp(tag, "INSERT 0 1", 10);
            if (!inserted && !(len == 10 && !memcmp(tag, "INSERT 0 0", 10))) return false;
            request->status = inserted ? 201 : 200;
            return buffer_text(out, inserted ?
                "{\"liked\":true,\"already_liked\":false,\"post_id\":" :
                "{\"liked\":true,\"already_liked\":true,\"post_id\":") &&
                buffer_integer(out, request->id) && buffer_text(out, "}");
        }
    }
    return false;
}

static void error_response(Pending *pending, const char *body, uint32_t len) {
    pending->failed = true;
    const char *state = NULL, *message = NULL;
    for (uint32_t at = 0; at < len && body[at];) {
        char type = body[at++];
        const char *value = body + at;
        const char *end = memchr(value, 0, len - at);
        if (!end) break;
        if (type == 'C') state = value;
        else if (type == 'M') message = value;
        at = (uint32_t)(end - body) + 1;
    }
    Request *request = pending->request;
    request->response.len = 0;
    /* Serialization failures and deadlocks are transient: run the request again. */
    if (state && !strncmp(state, "40", 2)) { pending->failed = false; pending->retry = true; return; }
    if (request->route == POST_LIKE && state && !strcmp(state, "23503"))
        request_error(request, 404, "post not found");
    else
        database_error(request, message && *message ? message : "database query failed");
}

static void requeue_request(DbPool *pool, unsigned group, Request *request);
static unsigned request_group(DbPool *pool, Request *request);

/* ReadyForQuery ends a group. After an error Postgres skipped the rest of the group and, for
   writes, rolled back the requests before it: those run again in a later group. */
static void finish_group(DbPool *pool, DbConnection *db) {
    bool error = db->group_error, writes = db->index >= pool->reads;
    while (db->count) {
        Pending *pending = &db->pending[db->head];
        Request *request = pending->request;
        bool end = pending->group_end, done = pending->done, failed = pending->failed, retry = pending->retry;
        memset(pending, 0, sizeof(*pending));
        db->head = (db->head + 1) % PIPELINE_LIMIT;
        db->count--;
        bool again = retry || (error && (!done || (writes && !failed)));
        if (again && request->retries < 8) {
            request->retries++;
            request->response.len = 0;
            requeue_request(pool, request_group(pool, request), request);
        } else {
            if (!done || again) request_error(request, 500, "internal server error");
            app_complete(request);
        }
        if (end) break;
    }
    db->cursor = 0;
    db->group_error = false;
}

/* Parses complete backend messages; returns false when the stream is unusable. */
static bool parse_input(DbPool *pool, DbConnection *db) {
    size_t at = 0;
    while (db->in.len - at >= 5) {
        const char *message = db->in.data + at;
        uint32_t len = get32(message + 1);
        if (len < 4) return false;
        if (db->in.len - at < 1 + (size_t)len) break;
        const char *body = message + 5;
        uint32_t body_len = len - 4;
        at += 1 + len;
        char type = message[0];
        if (type == 'N' || type == 'S' || type == 'A' || type == '2') continue;
        if (type == 'Z') {
            if (!db->count) return false;
            finish_group(pool, db);
            continue;
        }
        if (db->cursor >= db->count) return false;
        Pending *pending = &db->pending[(db->head + db->cursor) % PIPELINE_LIMIT];
        switch (type) {
            case 'D':
                if (pending->done || !data_row(pending, body, body_len)) {
                    request_error(pending->request, 500, "internal server error");
                    pending->done = true;
                }
                break;
            case 'C':
                if (!pending->done && !command_complete(pending, body, body_len ? body_len - 1 : 0))
                    request_error(pending->request, 500, "internal server error");
                pending->done = true;
                db->cursor++;
                break;
            case 'E':
                if (!pending->done) error_response(pending, body, body_len);
                pending->done = true;
                db->group_error = true;
                db->cursor = db->count;
                break;
            default:
                return false;
        }
    }
    size_t left = db->in.len - at;
    if (left && at) memmove(db->in.data, db->in.data + at, left);
    db->in.len = left;
    (void)pool;
    return true;
}

void db_completion(DbPool *pool, uint32_t kind, uint32_t tag, int res) {
    unsigned index = tag & 511;
    if (index >= pool->total) return;
    DbConnection *db = &pool->connections[index];
    bool current = db->state == DB_READY && db->generation == tag >> 9;
    if (kind == DB_KIND_RECV) {
        db->recv_busy = false;
        if (!current) {
            if (db->state == DB_PREPARING && db->synced && !db->send_busy) take_over(pool, db);
            return;
        }
        if (res <= 0) { disconnect_database(pool, db, res ? strerror(-res) : "database closed the connection", true); return; }
        db->in.len += (size_t)res;
        if (!parse_input(pool, db)) { disconnect_database(pool, db, "unexpected database message", true); return; }
        if (db->state == DB_READY && !arm_recv(db)) disconnect_database(pool, db, "database receive failed", true);
    } else {
        db->send_busy = false;
        if (!current) {
            if (db->state == DB_PREPARING && db->synced && !db->recv_busy) take_over(pool, db);
            return;
        }
        if (res <= 0) { disconnect_database(pool, db, res ? strerror(-res) : "database send failed", true); return; }
        db->sent += (size_t)res;
        submit_send(db);
    }
}

static void queue_request(DbPool *pool, unsigned group, Request *request) {
    if (pool->waiting_count[group] == WAITING_LIMIT) {
        database_error(request, "database waiting queue full");
        app_complete(request);
        return;
    }
    request->next = NULL;
    if (pool->waiting_tail[group]) pool->waiting_tail[group]->next = request;
    else pool->waiting_head[group] = request;
    pool->waiting_tail[group] = request;
    pool->waiting_count[group]++;
}

static void requeue_request(DbPool *pool, unsigned group, Request *request) {
    request->next = pool->waiting_head[group];
    pool->waiting_head[group] = request;
    if (!pool->waiting_tail[group]) pool->waiting_tail[group] = request;
    pool->waiting_count[group]++;
}

static Request *dequeue_request(DbPool *pool, unsigned group) {
    Request *request = pool->waiting_head[group];
    pool->waiting_head[group] = request->next;
    if (!pool->waiting_head[group]) pool->waiting_tail[group] = NULL;
    pool->waiting_count[group]--;
    request->next = NULL;
    return request;
}

static void drain_waiting(DbPool *pool) {
    for (unsigned group = 0; group < 2; group++) {
        while (pool->waiting_head[group]) {
            bool available;
            DbConnection *db = select_connection(pool, group, &available);
            if (!db && available) break;
            Request *request = dequeue_request(pool, group);
            if (db) send_request(pool, db, request);
            else {
                database_error(request, "database pool unavailable");
                app_complete(request);
            }
        }
    }
}

void db_submit(DbPool *pool, Request *request) {
    unsigned group = request_group(pool, request);
    bool available;
    DbConnection *db = select_connection(pool, group, &available);
    if (!available) {
        database_error(request, "database pool unavailable");
        app_complete(request);
    } else if (pool->waiting_head[group] || !db) {
        queue_request(pool, group, request);
    } else {
        send_request(pool, db, request);
    }
}

void db_event(DbPool *pool, unsigned token, uint32_t events) {
    unsigned index = token & 511;
    if (index >= pool->total) return;
    DbConnection *db = &pool->connections[index];
    if (db->state == DB_DOWN || db->state == DB_READY || db->generation != token >> 9) return;
    if (db->state == DB_CONNECTING) {
        poll_connection(pool, db);
        return;
    }
    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        if (!PQconsumeInput(db->connection)) {
            disconnect_database(pool, db, PQerrorMessage(db->connection), true);
            return;
        }
    }
    if ((events & EPOLLOUT) && !flush_libpq(pool, db)) return;
    prepare_results(pool, db);
}

/* Flushes a connection's batch once it is full or has waited flush_ns: fewer, larger sends
   mean fewer backend wakeups and receives per query. Returns how long the caller may sleep. */
uint64_t db_tick(DbPool *pool, uint64_t now_ns) {
    uint64_t now_ms = now_ns / 1000000, wait = UINT64_MAX;
    pool->now_ms = now_ms;
    pool->now_ns = now_ns;
    drain_waiting(pool);
    for (unsigned i = 0; i < pool->total; i++) {
        DbConnection *db = &pool->connections[i];
        if (db->group_open) {
            uint64_t due = db->open_ns + pool->flush_ns;
            if (i < pool->reads && db->batch < pool->batch && now_ns < due && db->out.len < 65536) {
                if (due - now_ns < wait) wait = due - now_ns;
                continue;
            }
            close_group(db);
            db->batch = 0;
            for (unsigned g = 0; g < 2; g++) if (pool->sticky[g] == i) pool->sticky[g] = UINT32_MAX;
        }
        if (db->out.len && !db->send_busy) submit_send(db);
    }
    if (now_ms < pool->tick_ms) return wait;
    pool->tick_ms = now_ms + 250;
    for (unsigned i = 0; i < pool->total; i++) {
        DbConnection *db = &pool->connections[i];
        if (db->state == DB_DOWN && now_ms >= db->retry_ms && !db->recv_busy && !db->send_busy)
            start_connection(pool, db);
        else if (db->state == DB_PREPARING && db->synced)
            take_over(pool, db);
        else if ((db->state == DB_CONNECTING || db->state == DB_PREPARING) &&
            now_ms - db->connect_ms >= CONNECT_TIMEOUT)
            disconnect_database(pool, db, "database connection timeout", true);
        else if (db->count && now_ms - db->pending[db->head].sent_ms >= REQUEST_TIMEOUT)
            disconnect_database(pool, db, "database request timeout", true);
    }
    return wait;
}

void db_destroy(DbPool *pool) {
    if (!pool) return;
    for (unsigned group = 0; group < 2; group++)
        while (pool->waiting_head[group]) request_destroy(dequeue_request(pool, group));
    for (unsigned i = 0; i < pool->total; i++)
        disconnect_database(pool, &pool->connections[i], "server shutting down", false);
    free(pool->connections);
    free(pool->url);
    free(pool);
}
