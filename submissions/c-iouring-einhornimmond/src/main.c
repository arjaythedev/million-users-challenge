/* The million users challenge on gradido-unicorn's fast-path stack -- libpq, libsodium and arnm,
 * built with zig -- on an io_uring loop. See README.md for the design. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "jwt.h"
#include "server.h"

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0' ? atoi(value) : fallback;
}

int main(void)
{
    const char *host = getenv("HOST"), *secret = getenv("JWT_SECRET"), *url = getenv("DATABASE_URL");
    struct rlimit limit;

    if (url == NULL || secret == NULL) {
        fprintf(stderr, "DATABASE_URL and JWT_SECRET are required\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    /* Every keep-alive connection is a descriptor: take all the service is allowed. */
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = limit.rlim_max;
        (void)setrlimit(RLIMIT_NOFILE, &limit);
    }

    jwt_init(secret);
    return server_run(host != NULL ? host : "0.0.0.0", (uint16_t)env_int("PORT", 3000),
                      (uint16_t)env_int("EXTRA_PORT", 0), url, env_int("SERVER_THREADS", 0),
                      env_int("DB_POOL_SIZE", 512));
}
