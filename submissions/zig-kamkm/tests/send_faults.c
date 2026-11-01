// Interpose only nonblocking socket sends to exercise real partial-write paths.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/socket.h>
#include <unistd.h>

static ssize_t (*original_send)(int, const void *, size_t, int);
static atomic_uint sends;

__attribute__((constructor)) static void initialize(void) {
    original_send = dlsym(RTLD_NEXT, "send");
}

ssize_t send(int fd, const void *data, size_t size, int flags) {
    if ((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0) {
        if (atomic_fetch_add(&sends, 1) % 7 == 6) {
            errno = EAGAIN;
            return -1;
        }
        if (size > 1) size = size / 2;
        if (size > 97) size = 97;
    }
    return original_send(fd, data, size, flags);
}
