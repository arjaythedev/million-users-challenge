//
// Created by dmitry on 27.10.2026.
//

// listener/server.cpp
// C++20, libevent. Два порта, keep-alive, fork+execv launcher на запрос.
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* LAUNCHER = "/app/launcher/launcher";
static constexpr int KEEPALIVE_IDLE_SEC = 75;

struct Conn {
    bufferevent* bev = nullptr;
    std::string req;
    bool busy = false;      // waiting for response from launcher
    pid_t child = -1;
    int out_fd = -1;        // are reading stdout of child
    event* ev_child_out = nullptr;
    int idle_timer = -1;
};

static event_base* g_base = nullptr;

// ---- helpers ----
static bool find_request_end(const std::string& s, size_t& header_end) {
    auto p = s.find("\r\n\r\n");
    if (p == std::string::npos) return false;
    header_end = p + 4;
    return true;
}

static size_t content_length_of(const std::string& headers) {
    // ищем Content-Length: N (регистронезависимо, грубо)
    std::string low = headers;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    auto p = low.find("content-length:");
    if (p == std::string::npos) return 0;
    p += 15;
    while (p < low.size() && (low[p] == ' ' || low[p] == '\t')) ++p;
    size_t v = 0;
    while (p < low.size() && isdigit((unsigned char)low[p])) { v = v * 10 + (low[p] - '0'); ++p; }
    return v;
}

static void on_child_stdout(evutil_socket_t fd, short, void* arg);
static void finish_request(Conn* c, const std::string& response);

static void spawn_launcher(Conn* c, const std::string& raw) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) { finish_request(c, ""); return; }

    pid_t pid = fork();
    if (pid < 0) { finish_request(c, ""); return; }

    if (pid == 0) {
        // child: stdin <- in_pipe[0], stdout -> out_pipe[1]
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        // stderr оставляем как есть (в лог)
        char* argv[] = { (char*)LAUNCHER, nullptr };
        char* envp[] = { nullptr };
        execve(LAUNCHER, argv, environ);
        _exit(127);
    }

    // parent
    close(in_pipe[0]); close(out_pipe[1]);
    // записать запрос в stdin ребёнка
    size_t off = 0;
    while (off < raw.size()) {
        ssize_t n = write(in_pipe[1], raw.data() + off, raw.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(in_pipe[1]); // EOF для cmake -> handler дочитает stdin

    c->busy = true;
    c->child = pid;
    c->out_fd = out_pipe[0];
    fcntl(c->out_fd, F_SETFL, O_NONBLOCK);

    c->ev_child_out = event_new(g_base, c->out_fd, EV_READ | EV_PERSIST,
                               on_child_stdout, c);
    event_add(c->ev_child_out, nullptr);
}

static void finish_request(Conn* c, const std::string& response) {
    if (c->ev_child_out) { event_free(c->ev_child_out); c->ev_child_out = nullptr; }
    if (c->out_fd >= 0) { close(c->out_fd); c->out_fd = -1; }
    if (c->child > 0) { waitpid(c->child, nullptr, 0); c->child = -1; }

    c->busy = false;
    if (!response.empty()) {
        bufferevent_write(c->bev, response.data(), response.size());
    }
    // если response пуст — закрываем соединение (ошибка)
    if (response.empty()) {
        bufferevent_free(c->bev);
        delete c;
    }
}

static void on_child_stdout(evutil_socket_t fd, short, void* arg) {
    Conn* c = (Conn*)arg;
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) { c->req.append(buf, (size_t)n); continue; }
        if (n == 0) {
            // EOF: ответ готов (мы пишем в c->req для простоты)
            std::string resp = c->req;
            c->req.clear();
            finish_request(c, resp);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        finish_request(c, "");
        return;
    }
}

// ---- чтение от клиента: ищем границу одного запроса ----
static void on_client_read(bufferevent* bev, void* arg) {
    Conn* c = (Conn*)arg;
    if (c->busy) {
        // пока заняты, буферизуем (клиент может пайплайнить)
        return;
    }
    evbuffer* in = bufferevent_get_input(bev);
    size_t len = evbuffer_get_length(in);
    if (len == 0) return;

    char* tmp = (char*)malloc(len);
    evbuffer_copyout(in, tmp, len);
    c->req.append(tmp, len);
    free(tmp);
    evbuffer_drain(in, len);

    size_t header_end = 0;
    if (!find_request_end(c->req, header_end)) return; // ждём ещё

    size_t clen = content_length_of(c->req.substr(0, header_end));
    if (c->req.size() < header_end + clen) return; // ждём тело

    // извлекаем ровно один запрос, остаток оставляем на следующий цикл
    std::string raw = c->req.substr(0, header_end + clen);
    c->req.erase(0, header_end + clen);

    spawn_launcher(c, raw);
}

static void on_client_event(bufferevent* bev, short events, void* arg) {
    Conn* c = (Conn*)arg;
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        if (c->ev_child_out) { event_free(c->ev_child_out); c->ev_child_out = nullptr; }
        if (c->out_fd >= 0) { close(c->out_fd); c->out_fd = -1; }
        if (c->child > 0) { kill(c->child, SIGKILL); waitpid(c->child, nullptr, 0); c->child = -1; }
        bufferevent_free(bev);
        delete c;
    }
}

// ---- accept ----
static void on_accept(evconnlistener*, evutil_socket_t fd, sockaddr*, int, void*) {
    event_base* base = g_base;
    bufferevent* bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) { close(fd); return; }

    timeval tv{ KEEPALIVE_IDLE_SEC, 0 };
    bufferevent_set_timeouts(bev, &tv, &tv);

    Conn* c = new Conn();
    c->bev = bev;
    bufferevent_setcb(bev, on_client_read, nullptr, on_client_event, c);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    const char* host = getenv("HOST"); if (!host) host = "0.0.0.0";
    const char* port = getenv("PORT"); if (!port) port = "80";
    const char* xport = getenv("EXTRA_PORT"); if (!xport) xport = "81";

    g_base = event_base_new();
    if (!g_base) { fprintf(stderr, "event_base_new failed\n"); return 1; }

    auto listen_on = [&](const char* p) -> evconnlistener* {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)atoi(p));
        if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
            fprintf(stderr, "bad HOST: %s\n", host); exit(1);
        }
        evconnlistener* l = evconnlistener_new_bind(
            g_base, on_accept, nullptr,
            LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
            -1, (sockaddr*)&sin, sizeof sin);
        if (!l) { fprintf(stderr, "listen on %s:%s failed\n", host, p); exit(1); }
        return l;
    };

    evconnlistener* l1 = listen_on(port);
    evconnlistener* l2 = listen_on(xport);

    fprintf(stderr, "listener on %s:%s and %s:%s\n", host, port, host, xport);
    event_base_dispatch(g_base);

    evconnlistener_free(l1);
    evconnlistener_free(l2);
    event_base_free(g_base);
    return 0;
}