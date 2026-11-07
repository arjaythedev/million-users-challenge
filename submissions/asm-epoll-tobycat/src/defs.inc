# Shared constants, struct layouts and macros.
.intel_syntax noprefix

.set SYS_READ,0
.set SYS_WRITE,1
.set SYS_CLOSE,3
.set SYS_MMAP,9
.set SYS_MADVISE,28
.set SYS_NANOSLEEP,35
.set SYS_SOCKET,41
.set SYS_CONNECT,42
.set SYS_BIND,49
.set SYS_LISTEN,50
.set SYS_SETSOCKOPT,54
.set SYS_CLONE,56
.set SYS_SCHED_YIELD,24
.set SYS_FUTEX,202
.set SYS_SETPRIORITY,141
.set SYS_SCHED_SETSCHED,144
.set SYS_SETRLIMIT,160
.set SYS_GETRLIMIT,97
.set SYS_RT_SIGACTION,13
.set SYS_EPOLL_WAIT,232
.set SYS_EPOLL_CTL,233
.set SYS_EPOLL_CREATE1,291
.set SYS_ACCEPT4,288
.set SYS_EVENTFD2,290
.set SYS_CLOCK_GETTIME,228
.set SYS_GETRANDOM,318
.set SYS_UNSHARE,272
.set SYS_EXIT,60
.set SYS_EXIT_GROUP,231
.set SYS_GETTID,186

.macro SYS n
  mov eax,\n
  syscall
.endm

# ---- job: one in-flight request. Strings live in the same allocation, after the struct.
.set J_NEXT,0
.set J_KIND,8        # K_* (u32)
.set J_WID,12        # owning HTTP worker
.set J_FD,16
.set J_GEN,20
.set J_VIEWER,24
.set J_A,32          # limit, comment id
.set J_B,40          # offset
.set J_SUB,48        # K_LIST_*: sub-kind k passed to SQL
.set J_STATUS,52     # HTTP status to answer with
.set J_T0,56         # enqueue time, ms
.set J_S0,64         # 6 (ptr,len) string slots; ptr==0 means SQL NULL/absent
.set J_S1,80
.set J_S2,96
.set J_S3,112
.set J_S4,128
.set J_S5,144
.set J_TOK,160       # presented token (ptr,len)
.set J_ROW,176       # first result row for this job
.set J_COUNT,184     # articlesCount
.set J_EXTRA,192     # owned allocation freed with the job
.set J_RESP,200      # response allocation (ptr)
.set J_RESPOFF,208   # offset of the first response byte
.set J_RESPLEN,216
.set J_ORD,224
.set J_HASH,232      # 64 bytes: bcrypt hash text
.set J_GZ,296        # client accepts gzip
.set J_SIZE,304

# Slots by meaning
.set J_USERNAME,J_S0
.set J_EMAIL,J_S1
.set J_PWHASH,J_S2
.set J_BIO,J_S3
.set J_IMAGE,J_S4
.set J_PASSWORD,J_S5
.set J_KEY,J_S0
.set J_TITLE,J_S1
.set J_DESC,J_S2
.set J_BODY,J_S3
.set J_TAGS,J_S4

# job kinds. Reads:
.set K_LIST,0        # list/article, one statement (family F_MEGA)
.set K_FEED,1
.set K_COMMENTS,2
.set K_PROFILE,3
.set K_USER,4
.set K_LOGIN,5
.set K_TAGS,6
.set NFAM,7
# Writes
.set K_REG,16
.set K_UPDUSER,17
.set K_FOLLOW,18
.set K_UNFOLLOW,19
.set K_FAV,20
.set K_UNFAV,21
.set K_ARTNEW,22
.set K_ARTUPD,23
.set K_ARTDEL,24
.set K_CMTNEW,25
.set K_CMTDEL,26
# bcrypt stages: run bcrypt, then continue as the kind in J_SUB
.set K_BC_HASH,32
.set K_BC_VERIFY,33

# J_SUB values for K_LIST
.set SUB_ALL,0
.set SUB_TAG,1
.set SUB_AUTHOR,2
.set SUB_FAVORITED,3
.set SUB_SLUG,4

.set MAX_BATCH,512
.set MAX_WORKERS,64

# worker: per HTTP thread
.set W_INBOX,0       # lock-free stack of finished jobs
.set W_NOTIFIED,8
.set W_EVFD,12
.set W_EPFD,16
.set W_LISTEN,20
.set W_CONNS,24
.set W_RBUF,32
.set W_ID,40
.set W_NOW_S,48
.set W_NOW_MS,56
.set T_GZBUF,128
.set T_OUT,136
.set T_NP,144
.set T_NR,152
.set T_P,160
.set T_CURR,168
.set T_WNEXT,176
.set T_L1,184
.set T_L2,188
.set T_NDOWN,192
.set T_NEXTTRY,200
.set T_NDOWN,192
.set T_NEXTTRY,200
.set W_SIZE,256

# conn: per fd, in a per-worker table
.set C_GEN,0
.set C_BUSY,4
.set C_IN,8          # pending input (alloc), or 0
.set C_INLEN,16
.set C_INCAP,20
.set C_OUT,24        # unsent output (alloc), or 0
.set C_OUTOFF,32
.set C_OUTLEN,40
.set C_CLOSING,48
.set C_SIZE,64
.set MAX_FDS,4194304
.set RBUF_SIZE,65536
.set MAX_BODY,2097152

# pg connection context (one per DB thread)
.set PG_FD,0
.set PG_RECV,8       # receive buffer
.set PG_END,16       # bytes valid
.set PG_SEND,24
.set PG_SCRATCH,32
.set PG_ARTS,40      # article row table
.set PG_ERR,48       # 5 bytes SQLSTATE of the first error
.set PG_JOBS,56      # job pointer array
.set PG_MASK,64     # workers to notify
.set PG_POS,72
.set P_RING,128
.set P_HEAD,136
.set P_TAIL,144
.set P_SLEN,152
.set P_SOFF,160
.set P_BATCH,168
.set P_SKIP,176
.set P_WOUT,184
.set P_IDX,192
.set P_CUR,200
.set P_NROW,208
.set P_DEAD,216
.set P_DOWN,224
.set P_RECOV,232
.set P_DEAD,216
.set P_DOWN,224
.set P_RECOV,232
.set PG_SIZE,256
.set RECV_CAP,0x800000
.set SEND_CAP,0x2000000
.set SCRATCH_CAP,0x400000

# Append a literal to the buffer at rdi (advances rdi; clobbers rax, rcx, rsi, xmm0).
.macro LIT s:vararg
  .pushsection .rodata
  771: .ascii \s
  772:
  .popsection
  lea rsi,[rip+771b]
  mov ecx,offset 772b-771b
  call mcpy
.endm
