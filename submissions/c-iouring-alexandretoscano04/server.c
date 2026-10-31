#include "app.h"
#include "uring.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "libdeflate.h"

#define CLIENT_BITS 12
#define CLIENT_BLOCK (1U << CLIENT_BITS)
#define MAX_BLOCKS 512
#define NONE UINT32_MAX
#define ACTIVE_LIMIT 32768
#define BUFFER_BUDGET (64U * 1024 * 1024)
#define RING_ENTRIES 4096
#define CQ_ENTRIES 65536
#define RECV_BUFFERS 16384
#define RECV_BUFFER_SIZE 2048
#define WAIT_NS 100000000L

/* io_uring user_data: operation (2 bits) | client generation (30 bits) | slot (32 bits). */
enum { OP_RECV, OP_SEND, OP_ACCEPT, OP_OTHER };
enum { OTHER_IGNORE, OTHER_DB };
enum { RECV_ARMED = 1, SEND_BUSY = 2, CLOSING = 4 };

typedef struct Active Active;
struct Active {
    Buffer input, output;
    size_t sent;
    unsigned header_len, interim_off;
    bool busy, close, eof, interim_pending, interim_done, sending_interim;
    Active *next_free;
};
typedef struct {
    Active *active;
    uint32_t generation, next;
    int fd;
    uint32_t flags;
} Client;
typedef struct {
    Ring ring;
    BufRing bufs;
    int db_epoll, listeners[2];
    Client *blocks[MAX_BLOCKS];
    uint32_t block_count, free_head;
    uint64_t *deferred;
    unsigned deferred_count, deferred_cap;
    Auth *auth;
    DbPool *db;
    struct libdeflate_compressor *compressor;
    Buffer compressed;
    size_t input_capacity, output_capacity;
    unsigned active_count, gzip_min;
    Active *active_pool;
} Worker;

static Worker worker;
static volatile sig_atomic_t stopping;
static uint64_t started_ms;

static uint64_t monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static uint64_t monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
uint64_t app_uptime(void) { return (monotonic_ms() - started_ms) / 1000; }
static void stop_signal(int sig) { (void)sig; stopping = 1; }
static Client *client_at(uint32_t slot) {
    if (slot >> CLIENT_BITS >= worker.block_count) return NULL;
    return &worker.blocks[slot >> CLIENT_BITS][slot & (CLIENT_BLOCK - 1)];
}
static uint64_t user_data(unsigned op, uint32_t slot, uint32_t generation) {
    return ((uint64_t)op << 62) | ((uint64_t)generation << 32) | slot;
}
static struct io_uring_sqe *sqe_for(unsigned op, uint32_t slot, uint32_t generation, int opcode, int fd) {
    struct io_uring_sqe *sqe = ring_sqe(&worker.ring);
    if (!sqe) return NULL;
    sqe->opcode = (uint8_t)opcode;
    sqe->fd = fd;
    sqe->user_data = user_data(op, slot, generation);
    return sqe;
}
struct io_uring_sqe *app_sqe(uint32_t kind, uint32_t tag) {
    struct io_uring_sqe *sqe = ring_sqe(&worker.ring);
    if (sqe) sqe->user_data = user_data(OP_OTHER, tag, kind);
    return sqe;
}
static void active_free(Active *a) {
    if (!a) return;
    worker.input_capacity -= a->input.cap;
    worker.output_capacity -= a->output.cap;
    worker.active_count--;
    buffer_free(&a->input);
    response_give(&a->output);
    a->next_free = worker.active_pool;
    worker.active_pool = a;
}
static void defer(uint32_t slot) {
    if (worker.deferred_count == worker.deferred_cap) {
        unsigned cap = worker.deferred_cap ? worker.deferred_cap * 2 : 1024;
        uint64_t *p = realloc(worker.deferred, cap * sizeof(*p));
        if (!p) return;
        worker.deferred = p;
        worker.deferred_cap = cap;
    }
    worker.deferred[worker.deferred_count++] = ((uint64_t)client_at(slot)->generation << 32) | slot;
}
static bool arm_recv(uint32_t slot) {
    Client *c = client_at(slot);
    struct io_uring_sqe *sqe = sqe_for(OP_RECV, slot, c->generation, IORING_OP_RECV, c->fd);
    if (!sqe) return false;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->flags = IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE;
    sqe->buf_group = 0;
    c->flags |= RECV_ARMED;
    return true;
}
/* Starts closing; the descriptor is released once no receive or send is outstanding. */
static void client_close(uint32_t slot) {
    Client *c = client_at(slot);
    if (!c || c->fd < 0 || (c->flags & CLOSING)) return;
    c->flags |= CLOSING;
    if (c->flags & RECV_ARMED) {
        struct io_uring_sqe *sqe = sqe_for(OP_OTHER, 0, 0, IORING_OP_ASYNC_CANCEL, -1);
        if (sqe) sqe->addr = user_data(OP_RECV, slot, c->generation);
    }
    defer(slot);
}
static void client_release(uint32_t slot) {
    Client *c = client_at(slot);
    if (c->fd < 0 || !(c->flags & CLOSING) || (c->flags & (RECV_ARMED | SEND_BUSY))) return;
    /* c->fd is an index in the ring's registered file table. */
    struct io_uring_sqe *sqe = sqe_for(OP_OTHER, 0, 0, IORING_OP_CLOSE, 0);
    if (sqe) {
        sqe->file_index = (uint32_t)c->fd + 1;
        sqe->flags = IOSQE_CQE_SKIP_SUCCESS;
    }
    c->fd = -1;
    c->flags = 0;
    active_free(c->active);
    c->active = NULL;
    c->next = worker.free_head;
    worker.free_head = slot;
}
static uint32_t client_allocate(int fd) {
    if (worker.free_head == NONE) {
        if (worker.block_count == MAX_BLOCKS) return NONE;
        Client *block = calloc(CLIENT_BLOCK, sizeof(*block));
        if (!block) return NONE;
        uint32_t base = worker.block_count * CLIENT_BLOCK;
        worker.blocks[worker.block_count++] = block;
        for (unsigned i = 0; i < CLIENT_BLOCK; i++) {
            block[i].fd = -1;
            block[i].next = i + 1 == CLIENT_BLOCK ? NONE : base + i + 1;
        }
        worker.free_head = base;
    }
    uint32_t slot = worker.free_head;
    Client *c = client_at(slot);
    worker.free_head = c->next;
    c->generation = (c->generation + 1) & 0x3fffffff;
    if (!c->generation) c->generation = 1;
    c->fd = fd;
    c->flags = 0;
    return slot;
}
static Active *active_get(uint32_t slot) {
    Client *c = client_at(slot);
    if (c->active) return c->active;
    if (worker.active_count >= ACTIVE_LIMIT) return NULL;
    Active *a = worker.active_pool;
    if (a) worker.active_pool = a->next_free;
    else if (!(a = malloc(sizeof(*a)))) return NULL;
    memset(a, 0, sizeof(*a));
    worker.active_count++;
    c->active = a;
    return a;
}
static const char continue_line[] = "HTTP/1.1 100 Continue\r\n\r\n";
/* The response header sits in the buffer's headroom, so header and body go out as one SEND. */
static void submit_send(uint32_t slot) {
    Client *c = client_at(slot);
    Active *a = c->active;
    if ((c->flags & (SEND_BUSY | CLOSING)) || !a) return;
    const char *data;
    size_t len;
    a->sending_interim = a->interim_pending;
    if (a->interim_pending) {
        data = continue_line + a->interim_off;
        len = sizeof(continue_line) - 1 - a->interim_off;
    } else if (!a->header_len) {
        return;
    } else {
        data = a->output.data - a->header_len + a->sent;
        len = a->header_len + a->output.len - a->sent;
    }
    struct io_uring_sqe *sqe = sqe_for(OP_SEND, slot, c->generation, IORING_OP_SEND, c->fd);
    if (!sqe) { client_close(slot); return; }
    sqe->addr = (uint64_t)(uintptr_t)data;
    sqe->len = (uint32_t)len;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->flags = IOSQE_FIXED_FILE;
    c->flags |= SEND_BUSY;
}
static void process_buffered(uint32_t slot);
static void response_finished(uint32_t slot) {
    Client *c = client_at(slot);
    Active *a = c->active;
    worker.output_capacity -= a->output.cap;
    response_give(&a->output);
    a->sent = 0;
    a->header_len = 0;
    a->busy = false;
    if (a->close || a->eof) { client_close(slot); return; }
    if (a->input.len) { process_buffered(slot); return; }
    if (!a->interim_pending) {
        active_free(a);
        c->active = NULL;
    }
}
static void on_send(uint32_t slot, int res) {
    Client *c = client_at(slot);
    Active *a = c->active;
    c->flags &= ~SEND_BUSY;
    if (c->flags & CLOSING) return;
    if (res <= 0) { client_close(slot); return; }
    if (a->sending_interim) {
        a->interim_off += (unsigned)res;
        if (a->interim_off >= sizeof(continue_line) - 1) a->interim_pending = false;
        submit_send(slot);
        return;
    }
    a->sent += (size_t)res;
    if (a->sent == a->header_len + a->output.len) response_finished(slot);
    else submit_send(slot);
}
void app_complete(Request *r) {
    Client *c = client_at(r->slot);
    if (!c || c->fd < 0 || c->generation != r->generation || (c->flags & CLOSING) || !c->active) {
        request_destroy(r);
        return;
    }
    Active *a = c->active;
    if (r->response.cap > BUFFER_BUDGET - worker.output_capacity) {
        uint32_t slot = r->slot;
        request_destroy(r);
        client_close(slot);
        return;
    }
    bool compressed = false;
    if (r->gzip && r->response.len >= worker.gzip_min) {
        size_t capacity = libdeflate_gzip_compress_bound(worker.compressor, r->response.len);
        if (worker.compressed.cap < capacity) {
            char *p = realloc(worker.compressed.data, capacity);
            if (p) { worker.compressed.data = p; worker.compressed.cap = capacity; }
        }
        if (capacity && worker.compressed.cap >= capacity) {
            size_t n = libdeflate_gzip_compress(worker.compressor, r->response.data, r->response.len,
                                               worker.compressed.data, capacity);
            if (n && n < r->response.len) {
                memcpy(r->response.data, worker.compressed.data, n);
                r->response.len = n;
                compressed = true;
            }
        }
    }
    char header[160], *p = header;
    const char *line = r->status == 200 ? "HTTP/1.1 200 OK\r\n" : r->status == 201 ? "HTTP/1.1 201 Created\r\n" :
        r->status == 400 ? "HTTP/1.1 400 Bad Request\r\n" : r->status == 401 ? "HTTP/1.1 401 Unauthorized\r\n" :
        r->status == 404 ? "HTTP/1.1 404 Not Found\r\n" : r->status == 503 ? "HTTP/1.1 503 Service Unavailable\r\n" :
        "HTTP/1.1 500 Internal Server Error\r\n";
    size_t n = strlen(line);
    memcpy(p, line, n); p += n;
    static const char type[] = "Content-Type: application/json\r\nContent-Length: ";
    memcpy(p, type, sizeof(type) - 1); p += sizeof(type) - 1;
    char digits[20], *d = digits + sizeof(digits);
    size_t length = r->response.len;
    do { *--d = (char)('0' + length % 10); length /= 10; } while (length);
    memcpy(p, d, (size_t)(digits + sizeof(digits) - d)); p += digits + sizeof(digits) - d;
    static const char keep[] = "\r\nConnection: keep-alive\r\nVary: Accept-Encoding\r\n";
    static const char close_header[] = "\r\nConnection: close\r\nVary: Accept-Encoding\r\n";
    if (r->close) { memcpy(p, close_header, sizeof(close_header) - 1); p += sizeof(close_header) - 1; }
    else { memcpy(p, keep, sizeof(keep) - 1); p += sizeof(keep) - 1; }
    if (compressed) { memcpy(p, "Content-Encoding: gzip\r\n", 24); p += 24; }
    *p++ = '\r'; *p++ = '\n';
    size_t header_len = (size_t)(p - header);
    if (r->response.head < header_len) {
        uint32_t slot = r->slot;
        request_destroy(r);
        client_close(slot);
        return;
    }
    a->output = r->response;
    worker.output_capacity += a->output.cap;
    r->response = (Buffer){0};
    a->close = r->close;
    memcpy(a->output.data - header_len, header, header_len);
    a->header_len = (unsigned)header_len;
    uint32_t slot = r->slot;
    request_destroy(r);
    submit_send(slot);
}
static bool slice_equal(const char *s, size_t n, const char *text) { return strlen(text) == n && !memcmp(s, text, n); }
static bool slice_case(const char *s, size_t n, const char *text) { return strlen(text) == n && !strncasecmp(s, text, n); }
static bool number(const char *s, size_t n, int64_t *out) {
    if (!n) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9' || value > ((uint64_t)INT64_MAX - (unsigned)(s[i] - '0')) / 10) return false;
        value = value * 10 + (unsigned)(s[i] - '0');
    }
    *out = (int64_t)value;
    return true;
}
static bool header_token(const char *s, size_t n, const char *token) {
    while (n) {
        const char *end = memchr(s, ',', n);
        size_t len = end ? (size_t)(end - s) : n;
        while (len && (*s == ' ' || *s == '\t')) { s++; len--; n--; }
        while (len && (s[len-1] == ' ' || s[len-1] == '\t')) len--;
        if (slice_case(s, len, token)) return true;
        if (!end) break;
        n -= (size_t)(end + 1 - s); s = end + 1;
    }
    return false;
}
static bool accepts_gzip(const char *s, size_t n) {
    bool wildcard = false;
    while (n) {
        const char *end = memchr(s, ',', n);
        size_t item = end ? (size_t)(end - s) : n;
        const char *p = s, *limit = s + item;
        while (p < limit && isspace((unsigned char)*p)) p++;
        const char *name = p;
        while (p < limit && *p != ';' && !isspace((unsigned char)*p)) p++;
        size_t name_len = (size_t)(p - name);
        double q = 1;
        while (p < limit) {
            if (*p++ != ';') continue;
            while (p < limit && isspace((unsigned char)*p)) p++;
            if (limit - p >= 2 && (p[0] == 'q' || p[0] == 'Q') && p[1] == '=') {
                p += 2; q = 0;
                if (p < limit && *p == '1') q = 1;
                else if (p < limit && *p == '0') {
                    p++;
                    if (p < limit && *p == '.') {
                        for (p++; p < limit && isdigit((unsigned char)*p); p++) if (*p != '0') q = 0.5;
                    }
                }
            }
        }
        if (slice_case(name, name_len, "gzip")) return q > 0;
        if (slice_equal(name, name_len, "*")) wildcard = q > 0;
        if (!end) break;
        n -= item + 1; s = end + 1;
    }
    return wildcard;
}
typedef struct {
    const char *path, *auth, *body;
    size_t path_len, auth_len, body_len, total;
    bool post, get, gzip, close, expect;
} Http;
static int parse_http(const char *data, size_t len, Http *h) {
    const char *end = memmem(data, len, "\r\n\r\n", 4);
    if (!end) return len == MAX_REQUEST ? -1 : 0;
    size_t head_len = (size_t)(end + 4 - data);
    const char *line = memmem(data, head_len, "\r\n", 2);
    const char *space = memchr(data, ' ', (size_t)(line - data));
    if (!space) return -1;
    h->get = slice_equal(data, (size_t)(space-data), "GET");
    h->post = slice_equal(data, (size_t)(space-data), "POST");
    h->path = space + 1;
    space = memchr(h->path, ' ', (size_t)(line - h->path));
    if (!space || *h->path != '/') return -1;
    h->path_len = (size_t)(space - h->path);
    const char *query = memchr(h->path, '?', h->path_len);
    if (query) h->path_len = (size_t)(query - h->path);
    bool v10 = slice_equal(space + 1, (size_t)(line - space - 1), "HTTP/1.0");
    if (!v10 && !slice_equal(space + 1, (size_t)(line-space-1), "HTTP/1.1")) return -1;
    h->close = v10;
    bool has_length = false;
    for (const char *p = line + 2; p < end; ) {
        const char *next = memmem(p, (size_t)(end + 2 - p), "\r\n", 2);
        if (!next) return -1;
        const char *colon = memchr(p, ':', (size_t)(next - p));
        if (!colon || colon == p) return -1;
        for (const char *k = p; k < colon; k++) if (!isalnum((unsigned char)*k) && *k != '-') return -1;
        const char *value = colon + 1;
        while (value < next && (*value == ' ' || *value == '\t')) value++;
        const char *last = next;
        while (last > value && (last[-1] == ' ' || last[-1] == '\t')) last--;
        for (const char *k = value; k < last; k++) if ((unsigned char)*k < 32 && *k != '\t') return -1;
        size_t key_len = (size_t)(colon - p), value_len = (size_t)(last - value);
        if (slice_case(p, key_len, "Content-Length")) {
            int64_t n;
            if (has_length || !number(value, value_len, &n) || n > MAX_REQUEST - (int64_t)head_len) return -1;
            h->body_len = (size_t)n; has_length = true;
        } else if (slice_case(p, key_len, "Transfer-Encoding")) return -1;
        else if (slice_case(p, key_len, "Authorization")) {
            if (h->auth) return -1;
            h->auth = value; h->auth_len = value_len;
        } else if (slice_case(p, key_len, "Accept-Encoding")) h->gzip = accepts_gzip(value, value_len);
        else if (slice_case(p, key_len, "Connection")) {
            if (header_token(value, value_len, "close")) h->close = true;
            else if (v10 && header_token(value, value_len, "keep-alive")) h->close = false;
        } else if (slice_case(p, key_len, "Expect")) h->expect = slice_case(value, value_len, "100-continue");
        p = next + 2;
    }
    h->body = data + head_len;
    h->total = head_len + h->body_len;
    return len >= h->total ? 1 : 0;
}
/* Parses one request from data; returns the bytes consumed, or 0 when the request is incomplete. */
static size_t process_request(uint32_t slot, const char *data, size_t len) {
    Client *c = client_at(slot);
    Active *a = c->active;
    Http h = {0};
    int parsed = parse_http(data, len, &h);
    if (!parsed) {
        if (h.expect && !a->interim_done) {
            a->interim_done = a->interim_pending = true;
            a->interim_off = 0;
            submit_send(slot);
        }
        return 0;
    }
    Request *r = request_new();
    if (!r) { client_close(slot); return len; }
    r->slot = slot; r->generation = c->generation; r->gzip = h.gzip; r->close = h.close;
    a->busy = true;
    a->interim_done = false;
    size_t used = len;
    const char *error = NULL;
    int status = 400;
    if (parsed < 0) { error = "malformed HTTP request"; r->close = true; }
    else {
        used = h.total;
        bool authenticated = false;
        const char *id = NULL; size_t id_len = 0;
        if (h.get && slice_equal(h.path, h.path_len, "/health")) r->route = HEALTH;
        else if (h.get && slice_equal(h.path, h.path_len, "/feed")) r->route = FEED;
        else if (h.post && slice_equal(h.path, h.path_len, "/posts")) { r->route = POST_CREATE; authenticated = true; }
        else if (h.path_len > 7 && !memcmp(h.path, "/posts/", 7)) {
            if (h.post && h.path_len > 12 && !memcmp(h.path + h.path_len - 5, "/like", 5) &&
                !memchr(h.path + 7, '/', h.path_len - 12)) {
                r->route = POST_LIKE; authenticated = true; id = h.path + 7; id_len = h.path_len - 12;
            } else if (h.get && !memchr(h.path + 7, '/', h.path_len - 7)) { r->route = POST_GET; id = h.path + 7; id_len = h.path_len - 7; }
            else { error = "not found"; status = 404; }
        } else { error = "not found"; status = 404; }
        if (!error && authenticated) {
            status = 401;
            if (!h.auth || h.auth_len < 7 || memcmp(h.auth, "Bearer ", 7)) error = "missing bearer token";
            else error = auth_verify(worker.auth, h.auth + 7, h.auth_len - 7, r);
        }
        if (!error && id && (!number(id, id_len, &r->id) || r->id <= 0)) { error = "invalid post id"; status = 400; }
        if (!error && r->route == POST_CREATE) { error = parse_post_body(h.body, h.body_len, r); status = 400; }
    }
    if (error) {
        if (!strcmp(error, "internal server error")) status = 500;
        request_error(r, status, error);
        app_complete(r);
    } else db_submit(worker.db, r);
    return used;
}
static bool input_append(Active *a, const char *data, size_t len) {
    size_t old_capacity = a->input.cap, capacity = old_capacity ? old_capacity : 256;
    if (a->input.len + len >= MAX_REQUEST + RECV_BUFFER_SIZE) return false;
    while (capacity < a->input.len + len + 1) capacity *= 2;
    if (capacity - old_capacity > BUFFER_BUDGET - worker.input_capacity || !buffer_append(&a->input, data, len)) return false;
    worker.input_capacity += a->input.cap - old_capacity;
    return true;
}
static void process_buffered(uint32_t slot) {
    Client *c = client_at(slot);
    Active *a = c->active;
    size_t len = a->input.len < MAX_REQUEST ? a->input.len : MAX_REQUEST;
    size_t used = process_request(slot, a->input.data, len);
    if (c->flags & CLOSING) return;
    if (!used) {
        if (a->input.len >= MAX_REQUEST) client_close(slot);
        return;
    }
    size_t left = a->input.len - used;
    if (left) memmove(a->input.data, a->input.data + used, left);
    a->input.len = left;
    if (!left) {
        worker.input_capacity -= a->input.cap;
        buffer_free(&a->input);
    }
}
static void client_input(uint32_t slot, const char *data, size_t len) {
    Client *c = client_at(slot);
    Active *a = active_get(slot);
    if (!a) { client_close(slot); return; }
    if (a->busy || a->input.len) {
        if (!input_append(a, data, len)) { client_close(slot); return; }
        if (!a->busy) process_buffered(slot);
        return;
    }
    /* Common case: a whole request in one receive buffer is parsed in place, without copying. */
    size_t used = process_request(slot, data, len);
    if (c->flags & CLOSING) return;
    if (used < len && !input_append(a, data + used, len - used)) { client_close(slot); return; }
    if (!a->busy && !a->input.len && !a->interim_pending) {
        active_free(a);
        c->active = NULL;
    }
}
static void on_recv(uint32_t slot, int res, unsigned flags) {
    Client *c = client_at(slot);
    if (!(flags & IORING_CQE_F_MORE)) c->flags &= ~RECV_ARMED;
    if (res > 0) {
        unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
        if (!(c->flags & CLOSING)) client_input(slot, bufring_data(&worker.bufs, bid), (size_t)res);
        bufring_recycle(&worker.bufs, bid);
        if (!(c->flags & (RECV_ARMED | CLOSING))) defer(slot);
    } else if (res == -ENOBUFS && !(c->flags & CLOSING)) {
        defer(slot);
    } else if (res == 0 && c->active && c->active->busy && !(c->flags & CLOSING)) {
        c->active->eof = true;
    } else {
        client_close(slot);
    }
}
static int socket_buffer;
static int listener(const char *host, const char *port) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV | AI_PASSIVE};
    struct addrinfo *addresses = NULL;
    int error = getaddrinfo(host, port, &hints, &addresses);
    if (error) { fprintf(stderr, "listen address: %s\n", gai_strerror(error)); return -1; }
    int fd = -1, yes = 1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        /* Accepted sockets inherit TCP_NODELAY, saving a setsockopt per connection. */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        const char *congestion = getenv("TCP_CC");
        if (congestion) setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, congestion, (socklen_t)strlen(congestion));
        if (socket_buffer) {
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer));
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer, sizeof(socket_buffer));
        }
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) == 0 && bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 65535) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}
static void arm_accept(unsigned index) {
    struct io_uring_sqe *sqe = sqe_for(OP_ACCEPT, index, 0, IORING_OP_ACCEPT, worker.listeners[index]);
    if (!sqe) return;
    sqe->ioprio = IORING_ACCEPT_MULTISHOT;
    /* Direct descriptors have no fd-table entry, so O_CLOEXEC does not apply (and is rejected). */
    sqe->accept_flags = SOCK_NONBLOCK;
    sqe->file_index = IORING_FILE_INDEX_ALLOC;
}
static void on_accept(unsigned index, int res, unsigned flags) {
    if (!(flags & IORING_CQE_F_MORE)) arm_accept(index);
    if (res < 0) return;
    uint32_t slot = client_allocate(res);
    if (slot == NONE) {
        struct io_uring_sqe *sqe = sqe_for(OP_OTHER, 0, 0, IORING_OP_CLOSE, 0);
        if (sqe) { sqe->file_index = (uint32_t)res + 1; sqe->flags = IOSQE_CQE_SKIP_SUCCESS; }
        return;
    }
    if (!arm_recv(slot)) { client_close(slot); }
}
static void arm_db_poll(void) {
    struct io_uring_sqe *sqe = sqe_for(OP_OTHER, OTHER_DB, 0, IORING_OP_POLL_ADD, worker.db_epoll);
    if (!sqe) return;
    sqe->poll32_events = POLLIN;
    sqe->len = IORING_POLL_ADD_MULTI;
}
static void drain_db(void) {
    struct epoll_event events[256];
    int n;
    do {
        n = epoll_wait(worker.db_epoll, events, 256, 0);
        for (int i = 0; i < n; i++)
            db_event(worker.db, (unsigned)(events[i].data.u64 & ~DB_EVENT), events[i].events);
    } while (n == 256);
}
static void run_deferred(void) {
    for (unsigned i = 0; i < worker.deferred_count; i++) {
        uint64_t key = worker.deferred[i];
        uint32_t slot = (uint32_t)key;
        Client *c = client_at(slot);
        if (!c || c->fd < 0 || c->generation != key >> 32) continue;
        if (c->flags & CLOSING) client_release(slot);
        else if (!(c->flags & RECV_ARMED) && !arm_recv(slot)) client_close(slot);
    }
    worker.deferred_count = 0;
}
static int worker_run(const char *host, const char *ports[2], const char *url, const char *secret, unsigned reads, unsigned writes, unsigned gzip_min) {
    worker.free_head = NONE;
    worker.gzip_min = gzip_min;
    worker.db_epoll = epoll_create1(EPOLL_CLOEXEC);
    worker.auth = auth_create(secret);
    worker.compressor = libdeflate_alloc_compressor(1);
    if (worker.db_epoll < 0 || !worker.auth || !worker.compressor) return 1;
    struct rlimit limit;
    unsigned files = 1U << 20;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < files) files = (unsigned)limit.rlim_cur;
    int error = ring_setup(&worker.ring, RING_ENTRIES, CQ_ENTRIES);
    if (!error) error = bufring_setup(&worker.ring, &worker.bufs, RECV_BUFFERS, RECV_BUFFER_SIZE, 0);
    if (!error) error = ring_register_files(&worker.ring, files);
    const char *buffer = getenv("SOCKET_BUFFER");
    socket_buffer = buffer ? atoi(buffer) : 0;
    if (error) { fprintf(stderr, "io_uring setup: %s\n", strerror(-error)); return 1; }
    for (unsigned i = 0; i < 2; i++) {
        worker.listeners[i] = listener(host, ports[i]);
        if (worker.listeners[i] < 0) { perror("listen"); return 1; }
        arm_accept(i);
    }
    worker.db = db_create(worker.db_epoll, url, reads, writes);
    if (!worker.db) return 1;
    arm_db_poll();
    fprintf(stderr, "worker=%ld reads=%u writes=%u gzip_min=%u idle_slot=%zu io=uring\n", (long)getpid(), reads, writes, gzip_min, sizeof(Client));
    Ring *ring = &worker.ring;
    long wait_ns = WAIT_NS;
    while (!stopping) {
        int ret = ring_enter(ring, 1, wait_ns);
        if (ret < 0 && ret != -ETIME && ret != -EINTR && ret != -EBUSY && ret != -EAGAIN) {
            fprintf(stderr, "io_uring_enter: %s\n", strerror(-ret));
            break;
        }
        unsigned head = ring_cq_head(ring);
        bool db_ready = false;
        struct io_uring_cqe *cqe;
        while ((cqe = ring_peek(ring, head))) {
            uint64_t key = cqe->user_data;
            int res = cqe->res;
            unsigned flags = cqe->flags;
            ring_cq_advance(ring, ++head);
            unsigned op = (unsigned)(key >> 62);
            uint32_t slot = (uint32_t)key;
            if (op == OP_ACCEPT) on_accept(slot & 1, res, flags);
            else if (op == OP_OTHER) {
                uint32_t kind = (uint32_t)(key >> 32) & 0x3fffffff;
                if (kind == DB_KIND_RECV || kind == DB_KIND_SEND) db_completion(worker.db, kind, slot, res);
                else if (kind == 0 && slot == OTHER_DB) {
                    db_ready = true;
                    if (!(flags & IORING_CQE_F_MORE)) arm_db_poll();
                }
            } else {
                Client *c = client_at(slot);
                if (!c || c->fd < 0 || c->generation != ((key >> 32) & 0x3fffffff)) {
                    if (op == OP_RECV && res > 0) bufring_recycle(&worker.bufs, flags >> IORING_CQE_BUFFER_SHIFT);
                    continue;
                }
                if (op == OP_RECV) on_recv(slot, res, flags);
                else on_send(slot, res);
                client_release(slot);
            }
        }
        if (db_ready) drain_db();
        bufring_publish(&worker.bufs);
        uint64_t hint = db_tick(worker.db, monotonic_ns());
        wait_ns = hint < (uint64_t)WAIT_NS ? (long)hint : WAIT_NS;
        run_deferred();
    }
    db_destroy(worker.db);
    libdeflate_free_compressor(worker.compressor);
    buffer_free(&worker.compressed);
    return stopping ? 0 : 1;
}
static unsigned positive_env(const char *name, unsigned fallback, unsigned max) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    int64_t n;
    if (!number(value, strlen(value), &n) || n < 1 || n > max) { fprintf(stderr, "invalid %s\n", name); exit(1); }
    return (unsigned)n;
}
static unsigned cpu_count(void) {
    cpu_set_t cpus;
    unsigned count = sched_getaffinity(0, sizeof(cpus), &cpus) == 0 ? (unsigned)CPU_COUNT(&cpus) : 1;
    char path[PATH_MAX] = "/sys/fs/cgroup", line[PATH_MAX];
    FILE *groups = fopen("/proc/self/cgroup", "r");
    if (groups) {
        while (fgets(line, sizeof(line), groups)) {
            if (strncmp(line, "0::/", 4)) continue;
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line + 3) + strlen(path) + 1 < sizeof(path)) strcat(path, line + 3);
            break;
        }
        fclose(groups);
    }
    for (;;) {
        char filename[PATH_MAX + 9];
        snprintf(filename, sizeof(filename), "%s/cpu.max", path);
        FILE *f = fopen(filename, "r");
        if (f) {
            unsigned long long quota, period;
            if (fscanf(f, "%llu %llu", &quota, &period) == 2 && period && quota) {
                unsigned long long limit = quota / period + (quota % period != 0);
                if (limit < count) count = (unsigned)limit;
            }
            fclose(f);
        }
        if (!strcmp(path, "/sys/fs/cgroup")) break;
        char *slash = strrchr(path, '/');
        if (!slash || slash < path + strlen("/sys/fs/cgroup")) break;
        *slash = 0;
    }
    return count > 16 ? 16 : count ? count : 1;
}
int main(void) {
    const char *url = getenv("DATABASE_URL"), *secret = getenv("JWT_SECRET");
    const char *host = getenv("HOST"), *ports[2] = {getenv("PORT"), getenv("EXTRA_PORT")};
    if (!url || !secret) { fprintf(stderr, "DATABASE_URL and JWT_SECRET are required\n"); return 1; }
    if (!host) host = "0.0.0.0";
    if (!ports[0]) ports[0] = "80";
    if (!ports[1]) ports[1] = "81";
    unsigned count = positive_env("WORKERS", cpu_count(), 16);
    /* Per worker: 16 read and 8 write connections, within the 512-connection limit. */
    unsigned per_read = count * 24 <= 512 ? 16 : 320 / count, per_write = count * 24 <= 512 ? 8 : 160 / count;
    unsigned reads = positive_env("DB_READ_CONNECTIONS", count * per_read, 496), writes = positive_env("DB_WRITE_CONNECTIONS", count * per_write, 496);
    /* Compression costs far more CPU than it saves; GZIP_MIN opts in for bandwidth-bound links. */
    unsigned gzip_min = positive_env("GZIP_MIN", UINT_MAX, UINT_MAX);
    if (reads < count || writes < count || reads + writes > 512) { fprintf(stderr, "database pools require at least one connection per worker and at most 512 total\n"); return 1; }
    started_ms = monotonic_ms();
    struct sigaction action = {.sa_handler = stop_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (count == 1) return worker_run(host, ports, url, secret, reads, writes, gzip_min);
    pid_t children[16] = {0};
    int status = 0;
    for (unsigned i = 0; i < count; i++) {
        pid_t child = fork();
        if (!child) {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() == 1) return 1;
            return worker_run(host, ports, url, secret, reads/count + (i < reads%count), writes/count + (i < writes%count), gzip_min);
        }
        if (child < 0) { stopping = 1; status = 1; break; }
        children[i] = child;
    }
    if (!stopping) {
        pid_t exited;
        do { exited = waitpid(-1, &status, 0); } while (exited < 0 && errno == EINTR && !stopping);
        if (!stopping) { fprintf(stderr, "worker exited unexpectedly\n"); status = 1; }
        else status = 0;
    }
    for (unsigned i = 0; i < count; i++) if (children[i]) kill(children[i], SIGTERM);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
    return status ? 1 : 0;
}
