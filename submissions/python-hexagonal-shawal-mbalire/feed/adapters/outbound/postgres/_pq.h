/* What _pq.c and _fastloop.c, linked into one extension, share. */
#ifndef FEED_PQ_H
#define FEED_PQ_H
#include <Python.h>
#include <libpq-fe.h>
#include <stddef.h>

/* The compressors (_igzip, _deflate) export this through a capsule, so the C path calls them directly.
 * -> compressed size, 0 on failure. `cap` is at least n + n/8 + 1024. */
typedef size_t (*compress_fn)(const void *in, size_t n, void *out, size_t cap);
#define COMPRESS_CAPSULE "feed.compress_fn"

PGconn *pq_pgconn(PyObject *conn);  /* borrowed; NULL with an exception set if it is not an open Conn */

extern PyMethodDef fastloop_functions[];
int fastloop_register(PyObject *module);
char *fastloop_put_time(char *w, long long micros);
#endif
