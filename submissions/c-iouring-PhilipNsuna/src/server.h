#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "resp.h"
#include "deflate.h"
#include "jwt.h"

enum { K_HEALTH, K_FEED, K_POST, K_CREATE, K_LIKE };

// A request that waits on the database. username/body are malloc'd copies for K_CREATE.
typedef struct {
    int32_t fd;
    uint32_t gen;
    uint8_t kind;
    bool gzip, close;
    int64_t id, user;
    uint8_t *username; uint32_t ulen;
    uint8_t *body; uint32_t blen;
} job_t;

typedef struct {
    char host[256]; int port;
    char user[128], password[256], dbname[128];
    uint8_t addr[16]; int addrlen; // resolved sockaddr
    int family;
} pgcfg_t;

typedef struct {
    pgcfg_t pg;
    char secret[256]; size_t secret_len;
    uint32_t host; int ports[8]; int nports;  // PORT, then EXTRA_PORT (one port or a comma list)
    int loops, db_read, db_write, batch_max, batch_min, batch_micros, gzip_min, stats_secs;
    bool pin, cbpf, uring, pg_sqe;
} config_t;

extern config_t cfg;
extern struct timespec start_time;
int64_t now_sec(void);

struct loop;
// Called by the database layer when a job's response is ready (any thread's loop, its own jobs only).
void loop_finish_job(struct loop *l, job_t *j, int status, const uint8_t *body, size_t n);
void job_free(job_t *j);
