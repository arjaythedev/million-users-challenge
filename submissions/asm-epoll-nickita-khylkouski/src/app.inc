# app.inc: struct layouts and macros shared by the app files (main.s server.s api.s pgio.s render.s sql.s rt.s).
# Included after defs.inc.
#
# Register convention inside worker threads: r15 always holds the current worker struct (W_*). App code never
# changes r15 after worker_main sets it, and every module preserves it (callee-saved), so it works like a
# thread pointer.

.equ SYS_prlimit64, 302
.equ SYS_sched_setaffinity, 203
.equ SYS_getrlimit, 97
.equ RLIMIT_NOFILE, 7
.equ SIGPIPE, 13
.equ SIG_IGN, 1
.equ MSG_NOSIGNAL, 0x4000
.equ MSG_DONTWAIT, 0x40
.equ SO_ATTACH_REUSEPORT_CBPF, 51
.equ SO_INCOMING_CPU, 49
.equ SO_DETACH_REUSEPORT_BPF, 68
.equ TCP_DEFER_ACCEPT, 9
.equ TCP_QUICKACK, 12
.equ TCP_INFO, 11
.equ TCPI_LAST_DATA_RECV, 52            # offsetof(struct tcp_info, tcpi_last_data_recv), u32 ms
.equ EPIPE, 32
.equ ECONNRESET, 104
.equ EMFILE, 24
.equ ENFILE, 23
.equ ENOBUFS, 105

# ---------------------------------------------------------------- limits and tunables
.equ MAX_WORKERS, 64
.equ MAX_PG, 64                         # Postgres connections per worker (upper bound)
.equ EV_BATCH, 1024                     # epoll_wait events per call
.equ RBUF_SIZE, 65536                   # per-worker read buffer
.equ MAXREQ, 65536                      # a request (head + body) larger than this drops the connection
.equ ARENA_SIZE, 262144                 # per-worker response arena (body rendered here)
.equ HEAD_ROOM, 192                     # bytes reserved before the body for the HTTP head (HTTP_HEAD_SPACE <= 160)
.equ GZMIN, 1024                        # bodies this long or longer are gzipped when the client accepts it
.equ PACK, 64                           # read statements sharing one Sync
.equ OPS_CAP, 4096                      # op ring entries per Postgres connection (power of two)
.equ PG_INCAP, 262144                   # per-connection input buffer
.equ PG_OUTCAP, 262144                  # per-connection output buffer (Bind/Execute messages)
.equ ACCEPT_BURST, 256
.equ NCLASS, 32                         # allocator size classes (class k = 2^k bytes)

# ---------------------------------------------------------------- epoll tags (epoll_data.u64)
# Clients: the fd itself (bit 63 clear). Listeners: TAG_LISTEN | fd. Postgres: TAG_PG | index.
.equ TAG_LISTEN_HI, 0x80000000          # high dword of the tag
.equ TAG_PG_HI, 0x40000000

# ---------------------------------------------------------------- worker (W_*), one per worker thread, 1024 B
.equ W_ID, 0
.equ W_EPFD, 8
.equ W_CPU, 16                          # cpu this worker is pinned to (-1 none)
.equ W_NOW, 24                          # unix seconds, refreshed once per loop iteration
.equ W_MONO, 32                         # monotonic ms, refreshed once per loop iteration
.equ W_EVENTS, 40                       # epoll_event array (EV_BATCH * 12 B)
.equ W_RBUF, 48                         # RBUF_SIZE read buffer
.equ W_ARENA, 56                        # ARENA_SIZE response arena
.equ W_ZBUF, 64                         # gzip output buffer (ARENA_SIZE)
.equ W_GZ, 72                           # gzip context (GZ_CTX_SIZE, 64-byte aligned)
.equ W_LFD0, 80                         # listener fd (PORT)
.equ W_LFD1, 88                         # listener fd (EXTRA_PORT), -1 if none
.equ W_NPG, 96                          # Postgres connections
.equ W_NWR, 104                         # of which the last W_NWR take the writes
.equ W_PG, 112                          # array of W_NPG pgconn structs (PC_SIZE each)
.equ W_RDCUR, 120                       # read connection being filled this iteration (-1 none)
.equ W_RETRY, 128                       # monotonic ms before which no reconnect is tried
.equ W_ACCOFF, 136                      # monotonic ms when accepting resumes after EMFILE (0 = accepting)
.equ W_NDEAD, 144                       # Postgres connections currently down
.equ W_NQUEUED, 152                     # statements queued to Postgres this iteration
.equ W_HSCR, 160                        # 192 B scratch for the HTTP head (HEAD_ROOM)
.equ W_REQ, 352                         # parsed request (REQ_SIZE = 80)
.equ W_T0, 432                          # lower bound (monotonic ms) on the arrival of the requests read this iteration
.equ W_CLS, 256 + 192                   # NCLASS free-list heads (rt.s), 448..703
.equ W_ACCEPTS, 704                     # connections accepted so far (the steering guard reads it)
.equ W_KEEPN, 984                       # rt.s: blocks on the class 17..20 free lists (4 dwords, 984..999)
.equ W_SIZE, 1024

# ---------------------------------------------------------------- client connection slot (C_*), 32 B, indexed by fd
.equ C_IN, 0                            # pending input bytes (0 = none); capacity 1 << (6 + low nibble of C_CLS)
.equ C_OUT, 8                           # parked output (0 = none); capacity 1 << (6 + high nibble of C_CLS)
.equ C_INLEN, 16                        # dword
.equ C_OUTLEN, 20                       # dword: bytes in C_OUT
.equ C_OUTOFF, 24                       # dword: bytes of C_OUT already sent
.equ C_GEN, 28                          # word: bumped on close, checked before a late response is delivered
.equ C_FLAGS, 30                        # byte
.equ C_CLS, 31                          # byte: size classes of the two buffers
.equ C_SIZE, 32
.equ CF_OPEN, 1
.equ CF_BUSY, 2                         # a statement is in flight for this connection
.equ CF_CLOSING, 4                      # close once the parked output is sent
.equ CF_WOUT, 8                         # registered for EPOLLOUT
.equ CF_CONT, 16                        # "100 Continue" sent for the request being read

# ---------------------------------------------------------------- Postgres connection (PC_*), 256 B
.equ PC_FD, 0                           # dword (-1 = down)
.equ PC_FLAGS, 4                        # dword PCF_*
.equ PC_OUT, 8                          # pgbuf {ptr, len, cap} at 8/16/24
.equ PC_OUTOFF, 32                      # bytes of PC_OUT already sent
.equ PC_IN, 40                          # input buffer
.equ PC_INLEN, 48
.equ PC_INPOS, 56                       # first message of the head op (kept until the op is answered)
.equ PC_SCAN, 64                        # parse cursor (>= PC_INPOS)
.equ PC_INCAP, 72
.equ PC_RING, 80                        # op ring (OPS_CAP * OP_SIZE)
.equ PC_HEAD, 88                        # dword: ring head index
.equ PC_N, 92                           # dword: ops in the ring
.equ PC_INBATCH, 96                     # dword: read statements queued since the last Sync
.equ PC_IDX, 100                        # dword: index in the worker's array
.equ PC_NROWS, 104                      # dword: DataRows seen for the head op
.equ PC_ERRST, 108                      # dword: 1 when the head op got an ErrorResponse (write ops, kept until Z)
.equ PC_SQLST, 112                      # SQLSTATE of that error (5 bytes LE)
.equ PC_SIZE, 256
.equ PCF_WRITE, 1                       # this connection takes the writes
.equ PCF_DIRTY, 2                       # PC_OUT has bytes to flush
.equ PCF_UNSYNCED, 4                    # read statements queued since the last Sync
.equ PCF_EOUT, 8                        # registered for EPOLLOUT (partial flush)

# ---------------------------------------------------------------- op (OP_*), 32 B: a statement (or Sync) in flight
.equ OP_FD, 0                           # dword client fd
.equ OP_GEN, 4                          # word
.equ OP_KIND, 6                         # byte K_*
.equ OP_FL, 7                           # byte OPF_*
.equ OP_ID, 8                           # post id (K_LIKE) / request time (unused otherwise)
.equ OP_AUX, 16                         # K_INS: block with the username and body (AUX_* layout), else 0
.equ OP_T0, 24                          # W_MONO when the statement was queued (slow-response detection)
.equ OP_SIZE, 32
.equ K_FEED, 1
.equ K_POST, 2
.equ K_INS, 3
.equ K_LIKE, 4
.equ K_HEALTH, 5
.equ K_SYNC, 6
.equ OPF_CLOSE, 1                       # answer with Connection: close and close
.equ OPF_GZ, 2                          # client accepts gzip

# aux block of a K_INS op (allocated with rt_alloc)
.equ AUX_CAP, 0                         # dword: block capacity (for rt_free)
.equ AUX_ULEN, 4                        # dword: username length
.equ AUX_BLEN, 8                        # dword: body length
.equ AUX_DATA, 16                       # username bytes, then body bytes

# ---------------------------------------------------------------- error responses (render.s builds them once)
.equ E_NOTFOUND, 1                      # 404 not found
.equ E_BADID, 2                         # 400 invalid post id
.equ E_POSTNF, 3                        # 404 post not found
.equ E_NOBEARER, 4                      # 401 missing bearer token
.equ E_BADTOKEN, 5                      # 401 invalid or expired token
.equ E_PAYLOAD, 6                       # 401 invalid token payload
.equ E_MALFORMED, 7                     # 400 malformed JSON body
.equ E_BODYREQ, 8                       # 400 body is required
.equ E_TOOLONG, 9                       # 400 body must be at most 500 characters
.equ E_INTERNAL, 10                     # 500 internal server error
.equ E_DEGRADED, 11                     # 503 degraded health
.equ NERR, 12

# ---------------------------------------------------------------- macros
# SLOT reg, fd32: reg = &g_conns[fd]. Clobbers rax.
.macro SLOT reg, fd
    mov \reg, [rip + g_conns]
    mov eax, \fd
    shl rax, 5
    add \reg, rax
.endm

# DEFSTR name, "text": defines `name` and `name_len`.
.macro DEFSTR name, text
\name: .ascii "\text"
.equ \name\()_len, . - \name
.endm

# PUTL lit: append the rodata literal `lit` (length `lit_len`) at rdi; rdi = new end. Clobbers rax rcx rsi rdx.
.macro PUTL lit
    lea rsi, [rip + \lit]
    mov edx, \lit\()_len
    call rt_memcpy
    mov rdi, rax
.endm

# PUTC byte: append one byte at rdi.
.macro PUTC c
    mov byte ptr [rdi], \c
    inc rdi
.endm

# ENDLIT name: after `name: .ascii "..."`, defines name_len.
.macro ENDLIT name
.equ \name\()_len, . - \name
.endm
