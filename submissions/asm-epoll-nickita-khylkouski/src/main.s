# main.s: process entry. Reads the environment, prepares the shared state (JWT key, Postgres config, the fd-indexed
# connection table, the listening sockets of every worker) and starts one worker thread per CPU the process may
# run on; the main thread then runs worker 0 itself. Nothing is pinned: the kernel places the threads, and its
# SO_REUSEPORT hash spreads new connections over the workers' listeners.
#
#   DATABASE_URL   postgres://user:pass@host:port/db (required)
#   JWT_SECRET     HS256 key (default million-users-challenge)
#   HOST           IPv4 literal to bind (default 0.0.0.0)
#   PORT           first port (default 80), EXTRA_PORT second port (default none)
#   WORKERS        worker threads (default and maximum: one per CPU in the process's affinity mask)
#   PG_CONNS       Postgres connections in total (default 64, at least 2 per worker)
#   DEFER_ACCEPT   TCP_DEFER_ACCEPT seconds on the listeners (default 1, 0 = off)
#   QUICKACK       0 (default) puts accepted sockets in delayed-ACK (pingpong) mode from the start, so the ACK of
#                  a connection's first request rides on the response instead of going out alone
#   QUICKACK_MS    a response this late (default 25 ms, measured from the earliest the request can have arrived)
#                  may have come after the kernel's 40 ms delayed-ACK timer, which dropped the socket out of
#                  pingpong mode; the server then sets TCP_QUICKACK=0 on it again so the next request does not
#                  cost a pure ACK (0 = off)
#   BATCH_US       pause after a light loop iteration so the next one batches more (default 2000, 0 = off)
#   BATCH_MIN      iterations with fewer queued statements than this pause (default 32)

.include "defs.inc"
.include "app.inc"

.section .rodata
DEFSTR e_dburl, "DATABASE_URL"
DEFSTR e_secret, "JWT_SECRET"
DEFSTR e_host, "HOST"
DEFSTR e_port, "PORT"
DEFSTR e_xport, "EXTRA_PORT"
DEFSTR e_workers, "WORKERS"
DEFSTR e_pgconns, "PG_CONNS"
DEFSTR e_defer, "DEFER_ACCEPT"
DEFSTR e_quickack, "QUICKACK"
DEFSTR e_quickackms, "QUICKACK_MS"
DEFSTR e_batchus, "BATCH_US"
DEFSTR e_batchmin, "BATCH_MIN"
DEFSTR d_secret, "million-users-challenge"
DEFSTR d_host, "0.0.0.0"
DEFSTR m_nodb, "DATABASE_URL is required\n"
DEFSTR m_baddb, "DATABASE_URL: cannot parse\n"
DEFSTR m_badhost, "HOST must be an IPv4 literal\n"
DEFSTR m_badport, "PORT/EXTRA_PORT must be 1..65535\n"
DEFSTR m_sock, "socket() failed\n"
DEFSTR m_bind, "bind() failed (port in use, or no CAP_NET_BIND_SERVICE for ports < 1024)\n"
DEFSTR m_listen, "listen() failed\n"
DEFSTR m_workers, "workers: "
DEFSTR m_pgper, "postgres connections per worker: "
DEFSTR m_pgwr, "postgres connections per worker for writes: "

.bss
.align 64
.globl g_hmac
g_hmac:         .skip HMAC_CTX_SIZE     # HMAC-SHA256 context of the JWT secret
.globl g_pgcfg
g_pgcfg:        .skip PGC_SIZE          # parsed DATABASE_URL
.globl g_conns
g_conns:        .skip 8                 # connection slots, C_SIZE each, indexed by fd
.globl g_maxfd
g_maxfd:        .skip 8                 # slots in g_conns
.globl g_nworkers
g_nworkers:     .skip 8
.globl g_workers
g_workers:      .skip 8                 # array of g_nworkers worker structs (W_SIZE each)
.globl g_pg_per
g_pg_per:       .skip 8                 # Postgres connections per worker
.globl g_pg_wr
g_pg_wr:        .skip 8                 # of which the last g_pg_wr take the writes
.globl g_start_mono
g_start_mono:   .skip 8                 # monotonic ms at start (uptime_s)
.globl g_batch_us
g_batch_us:     .skip 8
.globl g_batch_min
g_batch_min:    .skip 8
.globl g_host_addr
g_host_addr:    .skip 4                 # bind address, network order
.globl g_ports
g_ports:        .skip 8                 # two words: PORT, EXTRA_PORT (0 = none)
g_envp:         .skip 8
g_ncpus:        .skip 8                 # CPUs in the affinity mask, at most MAX_WORKERS
g_defer:        .skip 8
g_quickack:     .skip 8
.globl g_quickack_ms
g_quickack_ms:  .skip 8
.align 16
g_affmask:      .skip 128

.text

# ---------------------------------------------------------------- env_get(name, len) -> rax = value ptr | 0, rdx = len
env_get:
    mov r8, [rip + g_envp]
.Leg_next:
    mov r9, [r8]
    test r9, r9
    jz .Leg_none
    add r8, 8
    # compare name + '='
    xor ecx, ecx
.Leg_cmp:
    cmp rcx, rsi
    jae .Leg_eq
    movzx eax, byte ptr [r9 + rcx]
    cmp al, [rdi + rcx]
    jne .Leg_next
    inc rcx
    jmp .Leg_cmp
.Leg_eq:
    cmp byte ptr [r9 + rsi], 0x3d       # '='
    jne .Leg_next
    lea rax, [r9 + rsi + 1]
    mov rdx, rax
.Leg_len:
    cmp byte ptr [rdx], 0
    je .Leg_done
    inc rdx
    jmp .Leg_len
.Leg_done:
    sub rdx, rax
    ret
.Leg_none:
    xor eax, eax
    xor edx, edx
    ret

# env_num(name, len, default) -> rax: a decimal env value or the default
env_num:
    push rdx
    call env_get
    test rax, rax
    jz .Len_def
    test rdx, rdx
    jz .Len_def
    mov rdi, rax
    mov rsi, rdx
    call rt_parse_u64
    test edx, edx
    jz .Len_def
    pop rdx
    ret
.Len_def:
    pop rax
    ret

# parse_ipv4(ptr, len) -> rax = address in network order, rdx = 1 ok / 0 bad
parse_ipv4:
    lea r8, [rdi + rsi]
    xor r9d, r9d                        # result (built byte by byte)
    xor r10d, r10d                      # octets done
.Lip_octet:
    xor eax, eax
    xor ecx, ecx                        # digits
.Lip_digit:
    cmp rdi, r8
    jae .Lip_end
    movzx edx, byte ptr [rdi]
    cmp dl, 0x2e                        # '.'
    je .Lip_end
    sub edx, 0x30
    cmp edx, 9
    ja .Lip_bad
    imul eax, eax, 10
    add eax, edx
    cmp eax, 255
    ja .Lip_bad
    inc ecx
    inc rdi
    jmp .Lip_digit
.Lip_end:
    test ecx, ecx
    jz .Lip_bad
    shl r9d, 8
    or r9d, eax
    inc r10d
    cmp rdi, r8
    jae .Lip_fin
    inc rdi                             # skip '.'
    cmp r10d, 4
    jae .Lip_bad
    jmp .Lip_octet
.Lip_fin:
    cmp r10d, 4
    jne .Lip_bad
    bswap r9d
    mov eax, r9d
    mov edx, 1
    ret
.Lip_bad:
    xor eax, eax
    xor edx, edx
    ret

# ---------------------------------------------------------------- listen_on(port) -> fd (dies on failure)
listen_on:
    push rbx
    push r12
    sub rsp, 40
    mov ebx, edi
    mov edi, AF_INET
    mov esi, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC
    xor edx, edx
    SYS SYS_socket
    test rax, rax
    js .Llo_sock
    mov r12, rax
    mov dword ptr [rsp + 32], 1
    # SO_REUSEADDR, SO_REUSEPORT
    mov rdi, r12
    mov esi, SOL_SOCKET
    mov edx, SO_REUSEADDR
    lea r10, [rsp + 32]
    mov r8d, 4
    SYS SYS_setsockopt
    mov rdi, r12
    mov esi, SOL_SOCKET
    mov edx, SO_REUSEPORT
    lea r10, [rsp + 32]
    mov r8d, 4
    SYS SYS_setsockopt
    # TCP_NODELAY (inherited by accepted sockets)
    mov rdi, r12
    mov esi, IPPROTO_TCP
    mov edx, TCP_NODELAY
    lea r10, [rsp + 32]
    mov r8d, 4
    SYS SYS_setsockopt
    # TCP_THIN_LINEAR_TIMEOUTS (inherited as well, checked on 6.8): while fewer than four segments are in flight
    # (a response is one or two), the retransmission timeout does not double for the first six retries, so a
    # response lost three times is sent again after 600 ms instead of 800 ms, and after 800 ms instead of 1.6 s
    # when lost four times. Best effort, the result is not checked.
    mov rdi, r12
    mov esi, IPPROTO_TCP
    mov edx, TCP_THIN_LINEAR_TIMEOUTS
    lea r10, [rsp + 32]
    mov r8d, 4
    SYS SYS_setsockopt
    # TCP_DEFER_ACCEPT
    mov rax, [rip + g_defer]
    test rax, rax
    jz 1f
    mov [rsp + 32], eax
    mov rdi, r12
    mov esi, IPPROTO_TCP
    mov edx, TCP_DEFER_ACCEPT
    lea r10, [rsp + 32]
    mov r8d, 4
    SYS SYS_setsockopt
1:  # bind
    mov word ptr [rsp], AF_INET
    mov eax, ebx
    xchg al, ah
    mov [rsp + 2], ax
    mov eax, [rip + g_host_addr]
    mov [rsp + 4], eax
    mov qword ptr [rsp + 8], 0
    mov rdi, r12
    mov rsi, rsp
    mov edx, 16
    SYS SYS_bind
    test rax, rax
    js .Llo_bind
    mov rdi, r12
    mov esi, 65535
    SYS SYS_listen
    test rax, rax
    js .Llo_listen
    # TCP_QUICKACK 0: accepted sockets start in pingpong (delayed ACK) mode, so the ACK of a connection's first
    # request rides on the response. This must come AFTER listen(): inet_csk_listen_start() zeroes the listener's
    # ACK state, so an option set before listen() never reaches the children (verified on 6.8 with tcpdump).
    mov rax, [rip + g_quickack]
    test rax, rax
    jnz 2f
    mov dword ptr [rsp + 32], 0
    mov rdi, r12
    mov esi, IPPROTO_TCP
    mov edx, TCP_QUICKACK
    lea r10, [rsp + 32]
    mov r8d, 4
    SYS SYS_setsockopt
2:  mov rax, r12
    add rsp, 40
    pop r12
    pop rbx
    ret
.Llo_sock:
    lea rdi, [rip + m_sock]
    mov esi, m_sock_len
    jmp rt_die
.Llo_bind:
    lea rdi, [rip + m_bind]
    mov esi, m_bind_len
    jmp rt_die
.Llo_listen:
    lea rdi, [rip + m_listen]
    mov esi, m_listen_len
    jmp rt_die

# ---------------------------------------------------------------- _start
.globl _start
_start:
    and rsp, -16
    mov rax, [rsp]                      # argc (rsp was 16-aligned at entry and still points at argc)
    lea rdi, [rsp + 8 + rax*8 + 8]      # envp
    mov [rip + g_envp], rdi
    sub rsp, 64

    call cpu_detect
    call crypto_init
    call rt_mono_ms
    mov [rip + g_start_mono], rax

    # SIGPIPE -> ignore
    mov qword ptr [rsp], SIG_IGN
    mov qword ptr [rsp + 8], 0
    mov qword ptr [rsp + 16], 0
    mov qword ptr [rsp + 24], 0
    mov edi, SIGPIPE
    mov rsi, rsp
    xor edx, edx
    mov r10d, 8
    SYS SYS_rt_sigaction

    # JWT secret -> HMAC context
    lea rdi, [rip + e_secret]
    mov esi, e_secret_len
    call env_get
    test rax, rax
    jz 1f
    test rdx, rdx
    jnz 2f
1:  lea rax, [rip + d_secret]
    mov edx, d_secret_len
2:  mov rdi, rax
    mov rsi, rdx
    lea rdx, [rip + g_hmac]
    call crypto_hmac_init

    # DATABASE_URL
    lea rdi, [rip + e_dburl]
    mov esi, e_dburl_len
    call env_get
    test rax, rax
    jz .Lno_db
    test rdx, rdx
    jz .Lno_db
    mov rdi, rax
    mov rsi, rdx
    lea rdx, [rip + g_pgcfg]
    call pg_url_parse
    test rax, rax
    jnz .Lbad_db

    # HOST
    lea rdi, [rip + e_host]
    mov esi, e_host_len
    call env_get
    test rax, rax
    jz 3f
    test rdx, rdx
    jnz 4f
3:  lea rax, [rip + d_host]
    mov edx, d_host_len
4:  mov rdi, rax
    mov rsi, rdx
    call parse_ipv4
    test edx, edx
    jz .Lbad_host
    mov [rip + g_host_addr], eax

    # PORT, EXTRA_PORT
    lea rdi, [rip + e_port]
    mov esi, e_port_len
    mov edx, 80
    call env_num
    test rax, rax
    jz .Lbad_port
    cmp rax, 65535
    ja .Lbad_port
    mov [rip + g_ports], eax
    lea rdi, [rip + e_xport]
    mov esi, e_xport_len
    xor edx, edx
    call env_num
    cmp rax, 65535
    ja .Lbad_port
    mov [rip + g_ports + 4], eax

    # tunables
    lea rdi, [rip + e_defer]
    mov esi, e_defer_len
    mov edx, 1
    call env_num
    mov [rip + g_defer], rax
    lea rdi, [rip + e_quickack]
    mov esi, e_quickack_len
    xor edx, edx
    call env_num
    mov [rip + g_quickack], rax
    lea rdi, [rip + e_quickackms]
    mov esi, e_quickackms_len
    mov edx, 25
    call env_num
    mov [rip + g_quickack_ms], rax
    lea rdi, [rip + e_batchus]
    mov esi, e_batchus_len
    mov edx, 2000
    call env_num
    mov [rip + g_batch_us], rax
    lea rdi, [rip + e_batchmin]
    mov esi, e_batchmin_len
    mov edx, 32
    call env_num
    mov [rip + g_batch_min], rax

    # fd limit -> connection table
    xor edi, edi
    mov esi, RLIMIT_NOFILE
    xor edx, edx
    mov r10, rsp
    SYS SYS_prlimit64
    mov rax, [rsp]                      # soft limit
    test rax, rax
    js 5f
    cmp rax, 16777216
    jbe 6f
5:  mov eax, 16777216
6:  cmp rax, 1024
    jae 7f
    mov eax, 1024
7:  mov [rip + g_maxfd], rax
    shl rax, 5                          # C_SIZE
    mov rdi, rax
    call rt_mmap                        # MAP_NORESERVE: untouched slots cost nothing
    mov [rip + g_conns], rax

    # CPUs: one worker per CPU in the process's affinity mask (what the box gives the service). Nothing is pinned
    # (rule 13): the kernel places the threads.
    xor edi, edi
    mov esi, 128
    lea rdx, [rip + g_affmask]
    SYS SYS_sched_getaffinity
    lea rdi, [rip + g_affmask]
    xor ecx, ecx                        # count
    xor edx, edx                        # cpu
.Lcpu_scan:
    cmp edx, 1024
    jae .Lcpu_done
    cmp ecx, MAX_WORKERS
    jae .Lcpu_done
    mov eax, edx
    shr eax, 3
    movzx eax, byte ptr [rdi + rax]
    mov r9d, edx
    and r9d, 7
    bt eax, r9d
    jnc 8f
    inc ecx
8:  inc edx
    jmp .Lcpu_scan
.Lcpu_done:
    test ecx, ecx
    jnz 9f
    mov ecx, 1                          # no affinity info: one worker
9:  mov [rip + g_ncpus], rcx
    mov [rip + g_nworkers], rcx
    mov rdx, rcx                        # WORKERS may only lower it
    lea rdi, [rip + e_workers]
    mov esi, e_workers_len
    call env_num
    test rax, rax
    jz 10f
    cmp rax, [rip + g_ncpus]
    jae 10f
    mov [rip + g_nworkers], rax
10: # Postgres connections per worker
    lea rdi, [rip + e_pgconns]
    mov esi, e_pgconns_len
    mov edx, 64
    call env_num
    xor edx, edx
    div qword ptr [rip + g_nworkers]
    cmp rax, 2
    jae 11f
    mov eax, 2
11: cmp rax, MAX_PG
    jbe 12f
    mov eax, MAX_PG
12: mov [rip + g_pg_per], rax
    shr rax, 2                          # the last quarter of them, at least one, take the writes
    jnz 13f
    mov eax, 1
13: mov [rip + g_pg_wr], rax

    # log the choices
    lea rdi, [rip + m_workers]
    mov esi, m_workers_len
    mov rdx, [rip + g_nworkers]
    call rt_log_num
    lea rdi, [rip + m_pgper]
    mov esi, m_pgper_len
    mov rdx, [rip + g_pg_per]
    call rt_log_num
    lea rdi, [rip + m_pgwr]
    mov esi, m_pgwr_len
    mov rdx, [rip + g_pg_wr]
    call rt_log_num

    # worker structs
    mov rdi, [rip + g_nworkers]
    shl rdi, 10                         # W_SIZE
    call rt_mmap
    mov [rip + g_workers], rax

    # listeners: one per worker and port, each port's listeners forming one SO_REUSEPORT group
    call render_init
    xor ebx, ebx                        # port slot 0/1
.Lport_loop:
    lea rax, [rip + g_ports]
    mov r12d, [rax + rbx*4]
    test r12d, r12d
    jz .Lport_next
    xor r13d, r13d                      # worker i
.Lport_w:
    cmp r13, [rip + g_nworkers]
    jae .Lport_next
    mov edi, r12d
    call listen_on
    mov rdx, r13
    shl rdx, 10
    add rdx, [rip + g_workers]
    mov [rdx + W_LFD0 + rbx*8], rax
    inc r13
    jmp .Lport_w
.Lport_next:
    inc ebx
    cmp ebx, 2
    jb .Lport_loop
    # no EXTRA_PORT: mark the second listener slot
    cmp dword ptr [rip + g_ports + 4], 0
    jne 13f
    mov rdx, [rip + g_workers]
    mov rcx, [rip + g_nworkers]
.Lnox:
    mov qword ptr [rdx + W_LFD1], -1
    add rdx, W_SIZE
    dec rcx
    jnz .Lnox
13:
    # fill ids, spawn workers 1..n-1, run worker 0 here
    mov rdx, [rip + g_workers]
    mov rcx, [rip + g_nworkers]
    xor eax, eax
.Lw_fill:
    mov [rdx + W_ID], rax
    add rdx, W_SIZE
    inc rax
    cmp rax, rcx
    jb .Lw_fill
    mov r13d, 1
.Lw_spawn:
    cmp r13, [rip + g_nworkers]
    jae .Lw_run0
    lea rdi, [rip + worker_main]
    mov rsi, r13
    shl rsi, 10
    add rsi, [rip + g_workers]
    call rt_spawn
    inc r13
    jmp .Lw_spawn
.Lw_run0:
    mov rdi, [rip + g_workers]
    call worker_main                    # never returns
    xor edi, edi
    SYS SYS_exit_group

.Lno_db:
    lea rdi, [rip + m_nodb]
    mov esi, m_nodb_len
    jmp rt_die
.Lbad_db:
    lea rdi, [rip + m_baddb]
    mov esi, m_baddb_len
    jmp rt_die
.Lbad_host:
    lea rdi, [rip + m_badhost]
    mov esi, m_badhost_len
    jmp rt_die
.Lbad_port:
    lea rdi, [rip + m_badport]
    mov esi, m_badport_len
    jmp rt_die

.section .note.GNU-stack,"",@progbits
