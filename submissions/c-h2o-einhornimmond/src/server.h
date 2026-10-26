#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

/** Serves on @p host:@p port and, when it is not 0, @p extra_port too, with @p threads event
 *  loops (0: one per core) over @p pool_size database connections. Does not return unless
 *  starting fails. */
int server_run(const char *host, uint16_t port, uint16_t extra_port, const char *database_url,
               int threads, int pool_size);

#endif
