# main.s: process entry. Reads the environment, prepares the shared state (JWT key, Postgres config, the fd-indexed
# connection table, the listening sockets of every worker), picks the worker CPUs and starts one worker thread
# per CPU; the main thread then runs worker 0 itself.
#
#   DATABASE_URL   postgres://user:pass@host:port/db (required)
#   JWT_SECRET     HS256 key (default million-users-challenge)
#   HOST           IPv4 literal to bind (default 0.0.0.0)
#   PORT           first port (default 80), EXTRA_PORT second port (default none)
#   WORKERS        worker threads (default: one per allowed CPU, minus the NIC interrupt CPU when there are >= 4)
#   PG_CONNS       Postgres connections in total (default 64, at least 2 per worker)
#   SKIP_IRQ_CPU   1 (default) leaves the CPU that runs the NIC's NET_RX softirq without a worker
#   STEER          1 (default) attaches a reuseport BPF program: a connection goes to the worker of the CPU that
#                  processed its SYN, so a flow's TCP work and its worker share a core
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
DEFSTR e_skipirq, "SKIP_IRQ_CPU"
DEFSTR e_steer, "STEER"
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
DEFSTR m_irq, "net_rx cpu: "
DEFSTR m_pgper, "postgres connections per worker: "
DEFSTR m_steer, "cpu steering program (0 = attached, else -errno): "
DEFSTR m_softirqs, "/proc/softirqs"
DEFSTR m_interrupts, "/proc/interrupts"
DEFSTR m_input, "-input."
DEFSTR m_procirq, "/proc/irq/"
DEFSTR m_effaff, "/effective_affinity_list"
DEFSTR m_rxirq, "nic rx interrupt (-1 = none found, -2 = several queues): "
DEFSTR m_netrx, "NET_RX:"
DEFSTR m_cpu, "CPU"

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
g_cpus:         .skip MAX_WORKERS * 4   # worker i runs on cpu g_cpus[i]
g_ncpus:        .skip 8
g_defer:        .skip 8
g_quickack:     .skip 8
.globl g_quickack_ms
g_quickack_ms:  .skip 8
g_steer:        .skip 8
.globl g_steered
g_steered:      .skip 8                 # bit g: port group g has the steering program attached
g_skipirq:      .skip 8
g_irqcpu:       .skip 8
g_rxirq:        .skip 8                 # the NIC's receive interrupt number, -1 none found, -2 several
.align 16
g_affmask:      .skip 128
g_bpf:          .skip 8 * (MAX_WORKERS * 2 + 4)   # the steering program
g_fprog:        .skip 16
.align 64
g_softirqs:     .skip 65536
g_softirqs_cols: .skip 1024 * 4         # column -> cpu id of /proc/softirqs

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

# ---------------------------------------------------------------- slurp(path, buf, cap) -> rax = bytes read | -1
# path is NUL-terminated; reads until EOF or cap.
slurp:
    push rbx
    push r12
    push r13
    push r14
    sub rsp, 8
    mov r12, rsi
    mov r13, rdx
    xor esi, esi                        # O_RDONLY
    xor edx, edx
    SYS SYS_open
    test rax, rax
    js .Lsl_fail
    mov rbx, rax
    xor r14d, r14d
.Lsl_read:
    mov rdx, r13
    sub rdx, r14
    jbe .Lsl_done
    mov rdi, rbx
    lea rsi, [r12 + r14]
    SYS SYS_read
    test rax, rax
    jle .Lsl_done
    add r14, rax
    jmp .Lsl_read
.Lsl_done:
    mov rdi, rbx
    SYS SYS_close
    mov rax, r14
    jmp .Lsl_ret
.Lsl_fail:
    mov rax, -1
.Lsl_ret:
    add rsp, 8
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- cpu_cols(buf, len, cols) -> rax = columns, rdx = offset
# Header line of /proc/interrupts or /proc/softirqs: cols[i] = n for the i-th "CPUn" token. rdx = offset of the
# line after it. rax = 0 if there is no complete header line.
cpu_cols:
    xor eax, eax                        # columns
    xor ecx, ecx                        # offset
.Lcc_loop:
    cmp rcx, rsi
    jae .Lcc_none
    movzx r8d, byte ptr [rdi + rcx]
    cmp r8b, 10
    je .Lcc_nl
    cmp r8b, 0x43                       # 'C'
    jne .Lcc_next
    lea r9, [rcx + 3]
    cmp r9, rsi
    jae .Lcc_next
    cmp byte ptr [rdi + rcx + 1], 0x50  # 'P'
    jne .Lcc_next
    cmp byte ptr [rdi + rcx + 2], 0x55  # 'U'
    jne .Lcc_next
    xor r10d, r10d                      # value
    mov r11, r9                         # first digit
.Lcc_dig:
    cmp r9, rsi
    jae .Lcc_num
    movzx r8d, byte ptr [rdi + r9]
    sub r8d, 0x30
    cmp r8d, 9
    ja .Lcc_num
    imul r10d, r10d, 10
    add r10d, r8d
    inc r9
    jmp .Lcc_dig
.Lcc_num:
    cmp r9, r11
    je .Lcc_next                        # "CPU" without a number
    cmp eax, 1024
    jae 1f
    mov [rdx + rax*4], r10d
    inc eax
1:  mov rcx, r9
    jmp .Lcc_loop
.Lcc_next:
    inc rcx
    jmp .Lcc_loop
.Lcc_nl:
    lea rdx, [rcx + 1]
    ret
.Lcc_none:
    xor eax, eax
    mov rdx, rsi
    ret

# ---------------------------------------------------------------- find_rx_irq_cpu() -> rax = cpu | -1 | -2
# The CPU that takes the NIC's receive interrupt. /proc/interrupts names virtio-net's receive queues
# "virtioN-input.Q"; with exactly one such line, /proc/irq/<n>/effective_affinity_list says where that interrupt is
# delivered now (else the line's busiest column, i.e. where it went so far). -1: several receive queues, so no
# single interrupt core (skip nothing). -2: no such line; the caller falls back to /proc/softirqs.
# This is more direct than NET_RX softirq counts: with RPS every CPU runs NET_RX for its backlog, and virtio's
# transmit completions run as NET_RX on the transmit interrupt's CPU, so those counts can have two big columns.
find_rx_irq_cpu:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp
    sub rsp, 136                        # [0,64) path, [64,128) effective_affinity_list contents
    mov qword ptr [rip + g_rxirq], -1
    lea rdi, [rsp]
    lea rsi, [rip + m_interrupts]
    mov edx, m_interrupts_len
    call rt_memcpy
    mov byte ptr [rax], 0
    mov rdi, rsp
    lea rsi, [rip + g_softirqs]
    mov edx, 65536
    call slurp
    test rax, rax
    jle .Lri_unknown
    lea rbx, [rip + g_softirqs]
    mov rdi, rbx
    mov rsi, rax
    lea r13, [rbx + rax]                # end of the text
    lea rdx, [rip + g_softirqs_cols]
    call cpu_cols
    test eax, eax
    jz .Lri_unknown
    mov [rsp + 128], eax                # columns
    lea r12, [rbx + rdx]                # current line
    xor r14d, r14d                      # receive lines found
    mov r15, -1                         # its interrupt number
    mov rbp, -1                         # its busiest column's cpu
.Lri_line:
    cmp r12, r13
    jae .Lri_scanned
    mov rdi, r12
    mov rsi, r13
    sub rsi, r12
    mov edx, 10
    call rt_memchr
    test rax, rax
    jnz 1f
    mov rax, r13
1:  mov rbx, rax                        # end of this line
    # does the line name a receive queue?
    mov rcx, r12
.Lri_find:
    lea rax, [rcx + m_input_len]
    cmp rax, rbx
    ja .Lri_next
    cmp byte ptr [rcx], 0x2d            # '-'
    jne .Lri_find_next
    push rcx
    mov rdi, rcx
    mov esi, m_input_len
    lea rdx, [rip + m_input]
    mov ecx, m_input_len
    call rt_memeq
    pop rcx
    test eax, eax
    jnz .Lri_hit
.Lri_find_next:
    inc rcx
    jmp .Lri_find
.Lri_hit:
    # "<spaces><irq>:" then one count per column
    mov rcx, r12
.Lri_sp0:
    cmp rcx, rbx
    jae .Lri_next
    cmp byte ptr [rcx], 0x20
    jne 2f
    inc rcx
    jmp .Lri_sp0
2:  xor eax, eax
    mov rdx, rcx
.Lri_irqdig:
    cmp rcx, rbx
    jae .Lri_next
    movzx r8d, byte ptr [rcx]
    sub r8d, 0x30
    cmp r8d, 9
    ja 3f
    imul rax, rax, 10
    add rax, r8
    inc rcx
    jmp .Lri_irqdig
3:  cmp rcx, rdx
    je .Lri_next                        # no number (NMI:, LOC:, ...)
    cmp byte ptr [rcx], 0x3a            # ':'
    jne .Lri_next
    inc rcx
    inc r14
    cmp r14, 1
    jne .Lri_next                       # only the first one's details matter
    mov r15, rax
    # busiest column
    xor r9d, r9d                        # column
    xor r10d, r10d                      # best count
    mov rbp, -1
.Lri_col:
    cmp r9d, [rsp + 128]
    jae .Lri_next
.Lri_sp1:
    cmp rcx, rbx
    jae .Lri_next
    cmp byte ptr [rcx], 0x20
    jne 4f
    inc rcx
    jmp .Lri_sp1
4:  xor eax, eax
    mov rdx, rcx
.Lri_cdig:
    cmp rcx, rbx
    jae 5f
    movzx r8d, byte ptr [rcx]
    sub r8d, 0x30
    cmp r8d, 9
    ja 5f
    imul rax, rax, 10
    add rax, r8
    inc rcx
    jmp .Lri_cdig
5:  cmp rcx, rdx
    je .Lri_next                        # not a count: past the CPU columns
    cmp rax, r10
    jbe 6f
    mov r10, rax
    lea r8, [rip + g_softirqs_cols]
    mov ebp, [r8 + r9*4]
6:  inc r9d
    jmp .Lri_col
.Lri_next:
    lea r12, [rbx + 1]
    jmp .Lri_line
.Lri_scanned:
    test r14, r14
    jz .Lri_unknown
    cmp r14, 1
    je 7f
    mov qword ptr [rip + g_rxirq], -2
    mov rax, -1                         # several receive queues
    jmp .Lri_ret
7:  mov [rip + g_rxirq], r15
    # /proc/irq/<n>/effective_affinity_list
    lea rdi, [rsp]
    lea rsi, [rip + m_procirq]
    mov edx, m_procirq_len
    call rt_memcpy
    mov rdi, rax
    mov rsi, r15
    call rt_utoa
    mov rdi, rax
    lea rsi, [rip + m_effaff]
    mov edx, m_effaff_len
    call rt_memcpy
    mov byte ptr [rax], 0
    mov rdi, rsp
    lea rsi, [rsp + 64]
    mov edx, 63
    call slurp
    test rax, rax
    jle .Lri_col_best
    lea rcx, [rsp + 64]
    lea rdx, [rcx + rax]                # end
    mov rsi, rcx
    xor eax, eax
.Lri_adig:
    cmp rcx, rdx
    jae 8f
    movzx r8d, byte ptr [rcx]
    sub r8d, 0x30
    cmp r8d, 9
    ja 8f
    imul rax, rax, 10
    add rax, r8
    inc rcx
    jmp .Lri_adig
8:  cmp rcx, rsi
    je .Lri_col_best                    # no number
    cmp rcx, rdx
    je .Lri_ret                         # "N" alone
    cmp byte ptr [rcx], 10
    je .Lri_ret                         # "N\n": a single cpu
.Lri_col_best:
    mov rax, rbp                        # a list or range: where it has gone so far
    test rax, rax
    jns .Lri_ret
.Lri_unknown:
    mov rax, -2
.Lri_ret:
    add rsp, 136
    pop rbp
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- find_irq_cpu() -> rax = cpu with the dominant NET_RX count, or -1
find_irq_cpu:
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 16
    # copy the path to the stack with a NUL
    sub rsp, 32
    lea rdi, [rsp]
    lea rsi, [rip + m_softirqs]
    mov edx, m_softirqs_len
    call rt_memcpy
    mov byte ptr [rax], 0
    mov rdi, rsp
    xor esi, esi                        # O_RDONLY
    xor edx, edx
    SYS SYS_open
    add rsp, 32
    test rax, rax
    js .Lirq_none
    mov r12, rax                        # fd
    lea rbx, [rip + g_softirqs]
    xor r13d, r13d                      # bytes read
.Lirq_read:
    mov rdi, r12
    lea rsi, [rbx + r13]
    mov edx, 65535
    sub edx, r13d
    jbe .Lirq_full
    SYS SYS_read
    test rax, rax
    jle .Lirq_full
    add r13, rax
    jmp .Lirq_read
.Lirq_full:
    mov rdi, r12
    SYS SYS_close
    test r13, r13
    jz .Lirq_none
    # header line: tokens "CPUn" -> column -> cpu
    lea r14, [rip + g_softirqs_cols]
    xor ecx, ecx                        # column count
    xor edx, edx                        # offset
.Lirq_hdr:
    cmp rdx, r13
    jae .Lirq_none
    movzx eax, byte ptr [rbx + rdx]
    cmp al, 10
    je .Lirq_hdr_done
    cmp al, 0x43                        # 'C'
    jne .Lirq_hdr_next
    lea rsi, [rdx + 3]
    cmp rsi, r13
    jae .Lirq_none
    cmp byte ptr [rbx + rdx + 1], 0x50  # 'P'
    jne .Lirq_hdr_next
    cmp byte ptr [rbx + rdx + 2], 0x55  # 'U'
    jne .Lirq_hdr_next
    # parse digits at rbx + rdx + 3
    xor eax, eax
    mov rsi, rdx
    add rsi, 3
.Lirq_hdr_dig:
    cmp rsi, r13
    jae .Lirq_hdr_num
    movzx edi, byte ptr [rbx + rsi]
    sub edi, 0x30
    cmp edi, 9
    ja .Lirq_hdr_num
    imul eax, eax, 10
    add eax, edi
    inc rsi
    jmp .Lirq_hdr_dig
.Lirq_hdr_num:
    cmp ecx, 1024
    jae .Lirq_none
    mov [r14 + rcx*4], eax
    inc ecx
    mov rdx, rsi
    jmp .Lirq_hdr
.Lirq_hdr_next:
    inc rdx
    jmp .Lirq_hdr
.Lirq_hdr_done:
    test ecx, ecx
    jz .Lirq_none
    mov r15d, ecx                       # columns
    # find "NET_RX:"
    lea rdi, [rbx + rdx]
    mov rsi, r13
    sub rsi, rdx
.Lirq_find:
    cmp rsi, m_netrx_len
    jb .Lirq_none
    push rdi
    push rsi
    mov esi, m_netrx_len
    lea rdx, [rip + m_netrx]
    mov ecx, m_netrx_len
    call rt_memeq
    pop rsi
    pop rdi
    test eax, eax
    jnz .Lirq_found
    inc rdi
    dec rsi
    jmp .Lirq_find
.Lirq_found:
    add rdi, m_netrx_len
    lea r13, [rbx + r13]                # end pointer
    xor ecx, ecx                        # column
    xor r8d, r8d                        # best value (counts compare unsigned, so start at 0, not -1)
    xor r9d, r9d                        # second best
    mov r10d, -1                        # best column
.Lirq_num:
    cmp ecx, r15d
    jae .Lirq_eval
.Lirq_sp:
    cmp rdi, r13
    jae .Lirq_eval
    movzx eax, byte ptr [rdi]
    cmp al, 0x20
    je .Lirq_sp1
    cmp al, 10
    je .Lirq_eval
    jmp .Lirq_digits
.Lirq_sp1:
    inc rdi
    jmp .Lirq_sp
.Lirq_digits:
    xor eax, eax
.Lirq_dg:
    cmp rdi, r13
    jae .Lirq_val
    movzx edx, byte ptr [rdi]
    sub edx, 0x30
    cmp edx, 9
    ja .Lirq_val
    imul rax, rax, 10
    add rax, rdx
    inc rdi
    jmp .Lirq_dg
.Lirq_val:
    cmp rax, r8
    jbe .Lirq_notbest
    mov r9, r8
    mov r8, rax
    mov r10d, ecx
    jmp .Lirq_nextcol
.Lirq_notbest:
    cmp rax, r9
    jbe .Lirq_nextcol
    mov r9, rax
.Lirq_nextcol:
    inc ecx
    jmp .Lirq_num
.Lirq_eval:
    cmp r10d, -1
    je .Lirq_none
    # dominant: best > 4 * second (and > 0). With one queue the interrupt CPU counts every poll, the others only
    # the RPS backlogs sent to them; a 4-queue NIC's uneven queues gave 2.4 : 1 on a CI runner, which must not count
    test r8, r8
    jz .Lirq_none
    lea rax, [r9*4]
    cmp r8, rax
    jbe .Lirq_none
    mov eax, [r14 + r10*4]
    jmp .Lirq_ret
.Lirq_none:
    mov rax, -1
.Lirq_ret:
    add rsp, 16
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- listen_on(port, cpu_hint) -> fd (dies on failure)
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

# ---------------------------------------------------------------- attach_steer(fd): reuseport program cpu -> worker index
# ld #cpu; for each worker i: jeq #cpu_i -> ret #i; otherwise ret #0xffffffff. An index outside the group makes the
# kernel pick a listener by its own 4-tuple hash (run_bpf_filter returns NULL, reuseport_select_sock_by_hash), so a
# SYN handled on a CPU without a worker (the NET_RX core) goes to a random worker instead of always to worker 0,
# which would give worker 0 twice the connections of the others.
attach_steer:
    push rbx
    sub rsp, 16
    mov rbx, rdi
    lea rdi, [rip + g_bpf]
    mov rcx, [rip + g_nworkers]
    # [0] ld abs #SKF_AD_CPU
    mov word ptr [rdi], 0x20
    mov word ptr [rdi + 2], 0
    mov dword ptr [rdi + 4], 0xfffff024  # SKF_AD_OFF (-0x1000) + SKF_AD_CPU (36)
    add rdi, 8
    xor edx, edx                        # i
.Las_jeq:
    cmp rdx, rcx
    jae .Las_def
    mov word ptr [rdi], 0x15            # jeq #k, jt, jf
    mov byte ptr [rdi + 2], cl          # jt = n: jeq i sits at index 1 + i, ret #i at n + 2 + i
    mov byte ptr [rdi + 3], 0
    lea r8, [rip + g_cpus]
    mov eax, [r8 + rdx*4]
    mov [rdi + 4], eax
    add rdi, 8
    inc rdx
    jmp .Las_jeq
.Las_def:
    mov word ptr [rdi], 0x06            # ret #0xffffffff (default: the kernel's hash)
    mov word ptr [rdi + 2], 0
    mov dword ptr [rdi + 4], -1
    add rdi, 8
    xor edx, edx
.Las_ret:
    cmp rdx, rcx
    jae .Las_done
    mov word ptr [rdi], 0x06
    mov word ptr [rdi + 2], 0
    mov [rdi + 4], edx
    add rdi, 8
    inc rdx
    jmp .Las_ret
.Las_done:
    lea rax, [rcx + rcx + 2]            # instructions: 1 + n + 1 + n
    lea r8, [rip + g_fprog]
    mov [r8], ax
    lea rax, [rip + g_bpf]
    mov [r8 + 8], rax
    mov rdi, rbx
    mov esi, SOL_SOCKET
    mov edx, SO_ATTACH_REUSEPORT_CBPF
    mov r10, r8
    mov r8d, 16
    SYS SYS_setsockopt
    add rsp, 16
    pop rbx
    ret

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
    lea rdi, [rip + e_steer]
    mov esi, e_steer_len
    mov edx, 1
    call env_num
    mov [rip + g_steer], rax
    lea rdi, [rip + e_skipirq]
    mov esi, e_skipirq_len
    mov edx, 1
    call env_num
    mov [rip + g_skipirq], rax
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

    # CPUs: allowed set, minus the NIC's NET_RX cpu
    xor edi, edi
    mov esi, 128
    lea rdx, [rip + g_affmask]
    SYS SYS_sched_getaffinity
    lea rsi, [rip + g_cpus]
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
    mov [rsi + rcx*4], edx
    inc ecx
8:  inc edx
    jmp .Lcpu_scan
.Lcpu_done:
    mov [rip + g_ncpus], rcx
    test ecx, ecx
    jnz 9f
    mov dword ptr [rip + g_cpus], -1    # no affinity info: one unpinned worker
    mov qword ptr [rip + g_ncpus], 1
9:  mov qword ptr [rip + g_irqcpu], -1
    cmp qword ptr [rip + g_skipirq], 0
    je .Lcpu_sel
    cmp qword ptr [rip + g_ncpus], 4
    jb .Lcpu_sel
    call find_rx_irq_cpu
    cmp rax, -2
    jne 1f
    call find_irq_cpu
1:  mov [rip + g_irqcpu], rax
    test rax, rax
    js .Lcpu_sel
    # remove it from g_cpus
    lea rsi, [rip + g_cpus]
    mov rcx, [rip + g_ncpus]
    xor edx, edx
.Lcpu_rm:
    cmp rdx, rcx
    jae .Lcpu_sel
    cmp [rsi + rdx*4], eax
    je .Lcpu_rm_hit
    inc rdx
    jmp .Lcpu_rm
.Lcpu_rm_hit:
    lea rdi, [rsi + rdx*4]
    lea r8, [rsi + rdx*4 + 4]
    mov r9, rcx
    sub r9, rdx
    dec r9
    shl r9, 2
    push rcx
    mov rsi, r8
    mov rdx, r9
    call rt_memcpy                      # forward copy is fine (dst < src)
    pop rcx
    dec rcx
    mov [rip + g_ncpus], rcx
.Lcpu_sel:
    mov rdx, [rip + g_ncpus]
    mov [rip + g_nworkers], rdx
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

    # log the choices
    lea rdi, [rip + m_workers]
    mov esi, m_workers_len
    mov rdx, [rip + g_nworkers]
    call rt_log_num
    lea rdi, [rip + m_irq]
    mov esi, m_irq_len
    mov rdx, [rip + g_irqcpu]
    call rt_log_num
    cmp qword ptr [rip + g_rxirq], 0    # only when /proc/interrupts was consulted
    je 1f
    lea rdi, [rip + m_rxirq]
    mov esi, m_rxirq_len
    mov rdx, [rip + g_rxirq]
    call rt_log_num
1:
    lea rdi, [rip + m_pgper]
    mov esi, m_pgper_len
    mov rdx, [rip + g_pg_per]
    call rt_log_num

    # worker structs
    mov rdi, [rip + g_nworkers]
    shl rdi, 10                         # W_SIZE
    call rt_mmap
    mov [rip + g_workers], rax

    # listeners: worker order per port, so the steering program's index matches
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
    jae .Lport_steer
    mov edi, r12d
    call listen_on
    mov rdx, r13
    shl rdx, 10
    add rdx, [rip + g_workers]
    mov [rdx + W_LFD0 + rbx*8], rax
    inc r13
    jmp .Lport_w
.Lport_steer:
    cmp qword ptr [rip + g_steer], 0
    je .Lport_next
    cmp qword ptr [rip + g_nworkers], 1
    jbe .Lport_next
    mov rdx, [rip + g_workers]
    mov rdi, [rdx + W_LFD0 + rbx*8]     # first socket of the group
    call attach_steer
    test rax, rax
    jnz 1f
    lea rcx, [rbx + 1]                  # group 0 -> bit 0, group 1 -> bit 1
    or [rip + g_steered], rcx
1:  test ebx, ebx                       # log the first group's result (a failure only costs the steering)
    jnz .Lport_next
    lea rdi, [rip + m_steer]
    mov esi, m_steer_len
    mov rdx, rax
    call rt_log_num
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
    # fill ids and cpus, spawn workers 1..n-1, run worker 0 here
    mov rdx, [rip + g_workers]
    mov rcx, [rip + g_nworkers]
    lea rsi, [rip + g_cpus]
    xor eax, eax
.Lw_fill:
    mov [rdx + W_ID], rax
    movsxd rdi, dword ptr [rsi + rax*4]
    mov [rdx + W_CPU], rdi
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
