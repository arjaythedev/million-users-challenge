// postgres/pool_cli.cpp
// usage: pool_cli <qname> [params...]
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>

static const char* SOCK_PATH = "/tmp/feed_pool.sock";

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <qname> [params...]\n", argv[0]); return 2; }
    std::string qname = argv[1];
    int n = argc - 2;
    std::string line = qname + "\t" + std::to_string(n);
    for (int i = 2; i < argc; ++i) {
        std::string p = argv[i];
        line += "\t" + std::to_string(p.size()) + ":" + p;
    }
    line += "\n";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{}; sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof sa.sun_path - 1);
    if (connect(fd, (sockaddr*)&sa, sizeof sa) < 0) { perror("connect"); return 1; }
    (void)!write(fd, line.data(), line.size());

    std::string resp;
    char tmp[8192];
    for (;;) {
        ssize_t r = read(fd, tmp, sizeof tmp);
        if (r <= 0) break;
        resp.append(tmp, (size_t)r);
        if (!resp.empty() && resp.back() == '\n') break;
    }
    close(fd);
    if (!resp.empty() && resp.back() == '\n') resp.pop_back();

    if (resp.rfind("OK\t", 0) == 0) {
        fwrite(resp.data() + 3, 1, resp.size() - 3, stdout);
        fputc('\n', stdout);
        return 0;
    }
    if (resp.rfind("ERR\t", 0) == 0) {
        fprintf(stderr, "%s\n", resp.c_str() + 4);
        return 1;
    }
    fprintf(stderr, "bad response: %s\n", resp.c_str());
    return 1;
}