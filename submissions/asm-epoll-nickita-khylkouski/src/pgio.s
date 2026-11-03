# pgio.s: asynchronous, pipelined Postgres I/O. Each worker owns W_NPG connections, the last W_NWR of them for the
# writes; statements are appended to a connection's output buffer as Bind/Execute pairs and sent once per
# event-loop iteration behind one Sync (reads) or with their own Sync (writes, so each insert commits on its own
# before the client is answered).
# Results come back in order; an op ring per connection remembers which client each result belongs to.
#
#   pgio_init()                                   connect + prepare (blocking, at worker start)
#   pgio_submit(kind, fd, flags, id, uid, aux)    -> 1 queued | 0 no usable connection (aux freed)
#   pgio_flush()                                  end of iteration: Sync the read batches, send everything
#   pgio_event(idx, events)                       epoll event on connection idx
#   pgio_reconnect_one()                          try to bring one dead connection back
#
# Ring discipline: a read op is popped when its CommandComplete (or ErrorResponse) arrives; a write op stays
# until the ReadyForQuery that follows its Sync, which is when the transaction is committed. At ReadyForQuery
# every op up to and including the K_SYNC marker is popped: ops still there were skipped by the backend after
# an error and get a 500.

.include "defs.inc"
.include "app.inc"

.equ EPOLL_IN_FLAGS, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP
.equ OPS_MASK, OPS_CAP - 1
.equ ROOM, 4096                         # free bytes guaranteed in PC_OUT before a statement is appended
.equ NSTMT, 5

.section .rodata
DEFSTR m_pgconn, "pg: connect failed, errno "
DEFSTR m_pgprep, "pg: prepare failed\n"
DEFSTR m_pgdown, "pg: connection lost, worker "
DEFSTR m_pgup, "pg: reconnected, worker "
.align 8
prep_tab:
    .quad st_feed, sql_feed, sql_feed_len
    .quad st_post, sql_post, sql_post_len
    .quad st_ins, sql_ins, sql_ins_len
    .quad st_like, sql_like, sql_like_len
    .quad st_health, sql_health, sql_health_len

# rax = address of the ring slot after the last op (rbx = pc); PC_N incremented.
.macro PUSHOP
    mov eax, [rbx + PC_HEAD]
    add eax, [rbx + PC_N]
    and eax, OPS_MASK
    shl rax, 5
    add rax, [rbx + PC_RING]
    inc dword ptr [rbx + PC_N]
.endm

# rax = head op (rbx = pc)
.macro HEADOP
    mov eax, [rbx + PC_HEAD]
    shl rax, 5
    add rax, [rbx + PC_RING]
.endm

# drop the head op (rbx = pc); clobbers eax
.macro POPOP
    mov eax, [rbx + PC_HEAD]
    inc eax
    and eax, OPS_MASK
    mov [rbx + PC_HEAD], eax
    dec dword ptr [rbx + PC_N]
.endm

.text

# ---------------------------------------------------------------- pgio_init()
.globl pgio_init
pgio_init:
    push rbx
    push r12
    push r13
    mov rax, [rip + g_pg_per]
    mov [r15 + W_NPG], rax
    mov rcx, [rip + g_pg_wr]
    mov [r15 + W_NWR], rcx
    mov rdi, rax
    shl rdi, 8
    call rt_mmap
    mov [r15 + W_PG], rax
    xor ebx, ebx
.Lpi_loop:
    cmp rbx, [r15 + W_NPG]
    jae .Lpi_done
    mov r12, rbx
    shl r12, 8
    add r12, [r15 + W_PG]
    mov [r12 + PC_IDX], ebx
    mov dword ptr [r12 + PC_FD], -1
    mov dword ptr [r12 + PC_FLAGS], 0
    mov edi, PG_OUTCAP
    call rt_alloc
    mov [r12 + PC_OUT + PGB_PTR], rax
    mov qword ptr [r12 + PC_OUT + PGB_LEN], 0
    mov [r12 + PC_OUT + PGB_CAP], rdx
    mov edi, PG_INCAP
    call rt_mmap
    mov [r12 + PC_IN], rax
    mov qword ptr [r12 + PC_INCAP], PG_INCAP
    mov edi, OPS_CAP * OP_SIZE
    call rt_mmap
    mov [r12 + PC_RING], rax
    mov rdi, r12
    call pg_open
    test eax, eax
    jnz 1f
    inc qword ptr [r15 + W_NDEAD]
1:  inc rbx
    jmp .Lpi_loop
.Lpi_done:
    mov qword ptr [r15 + W_RDCUR], -1
    call rt_mono_ms
    add rax, 1000
    mov [r15 + W_RETRY], rax
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- pg_open(pc) -> 1 | 0
# Blocking connect + the five Parse messages, then non-blocking and into epoll.
pg_open:
    push rbx
    push r12
    push r13
    mov rbx, rdi
    lea rdi, [rip + g_pgcfg]
    call pg_connect
    test rax, rax
    js .Lpo_cfail
    mov r12d, eax
    xor r13d, r13d
.Lpo_prep:
    cmp r13d, NSTMT
    jae .Lpo_prepared
    lea rax, [rip + prep_tab]
    lea rcx, [r13 + r13*2]
    lea rax, [rax + rcx*8]
    mov edi, r12d
    mov rsi, [rax]
    mov rdx, [rax + 8]
    mov rcx, [rax + 16]
    call pg_prepare
    test eax, eax
    jnz .Lpo_pfail
    inc r13d
    jmp .Lpo_prep
.Lpo_prepared:
    mov edi, r12d
    mov esi, F_SETFL
    mov edx, O_NONBLOCK
    SYS SYS_fcntl
    mov qword ptr [rbx + PC_OUT + PGB_LEN], 0
    mov qword ptr [rbx + PC_OUTOFF], 0
    mov qword ptr [rbx + PC_INLEN], 0
    mov qword ptr [rbx + PC_INPOS], 0
    mov qword ptr [rbx + PC_SCAN], 0
    mov dword ptr [rbx + PC_HEAD], 0
    mov dword ptr [rbx + PC_N], 0
    mov dword ptr [rbx + PC_INBATCH], 0
    mov dword ptr [rbx + PC_FLAGS], 0
    mov [rbx + PC_FD], r12d
    mov edi, EPOLL_CTL_ADD
    mov esi, r12d
    mov edx, EPOLL_IN_FLAGS
    mov ecx, [rbx + PC_IDX]
    mov rax, TAG_PG_HI << 32
    or rcx, rax
    call ep_ctl
    mov eax, 1
    jmp .Lpo_ret
.Lpo_pfail:
    mov edi, r12d
    SYS SYS_close
    lea rdi, [rip + m_pgprep]
    mov esi, m_pgprep_len
    call rt_log
    xor eax, eax
    jmp .Lpo_ret
.Lpo_cfail:
    neg rax
    lea rdi, [rip + m_pgconn]
    mov esi, m_pgconn_len
    mov rdx, rax
    call rt_log_num
    xor eax, eax
.Lpo_ret:
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- pg_down(pc): the connection is lost
# Every op in flight is answered with a 500 (health: 503), the socket closed, a reconnect scheduled.
pg_down:
    push rbx
    push r12
    push r13
    mov rbx, rdi
    mov edi, [rbx + PC_FD]
    test edi, edi
    js .Lpd_ret
    SYS SYS_close
    mov dword ptr [rbx + PC_FD], -1
    inc qword ptr [r15 + W_NDEAD]
    mov eax, [rbx + PC_IDX]
    cmp rax, [r15 + W_RDCUR]
    jne 1f
    mov qword ptr [r15 + W_RDCUR], -1
1:  lea rdi, [rip + m_pgdown]
    mov esi, m_pgdown_len
    mov rdx, [r15 + W_ID]
    call rt_log_num
.Lpd_loop:
    cmp dword ptr [rbx + PC_N], 0
    je .Lpd_clean
    HEADOP
    mov r12, rax
    POPOP
    cmp byte ptr [r12 + OP_KIND], K_SYNC
    je .Lpd_loop
    mov rdi, r12
    mov esi, E_INTERNAL
    call render_fail
    jmp .Lpd_loop
.Lpd_clean:
    mov dword ptr [rbx + PC_HEAD], 0
    mov dword ptr [rbx + PC_N], 0
    mov qword ptr [rbx + PC_OUT + PGB_LEN], 0
    mov qword ptr [rbx + PC_OUTOFF], 0
    mov qword ptr [rbx + PC_INLEN], 0
    mov qword ptr [rbx + PC_INPOS], 0
    mov qword ptr [rbx + PC_SCAN], 0
    mov dword ptr [rbx + PC_INBATCH], 0
    mov dword ptr [rbx + PC_FLAGS], 0
    mov rax, [r15 + W_MONO]
    add rax, 1000
    mov [r15 + W_RETRY], rax
.Lpd_ret:
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- pick(write) -> rax = connection to append to | 0
# The last W_NWR connections take the writes and the others the reads, so a slow commit never holds up reads
# pipelined behind it. Reads keep filling the current connection up to PACK statements per iteration; otherwise,
# and for every write, the live connection of their group with the fewest ops in flight. When the group has no
# live connection, any live connection does.
pick:
    test edi, edi
    jnz .Lpk_write
    mov rax, [r15 + W_RDCUR]
    test rax, rax
    js .Lpk_read
    shl rax, 8
    add rax, [r15 + W_PG]
    cmp dword ptr [rax + PC_FD], 0
    jl .Lpk_read
    cmp dword ptr [rax + PC_INBATCH], PACK
    jae .Lpk_read
    cmp dword ptr [rax + PC_N], OPS_CAP - 2
    ja .Lpk_read
    ret
.Lpk_read:
    xor esi, esi
    mov rdx, [r15 + W_NPG]
    sub rdx, [r15 + W_NWR]
    call pick_min
    test rax, rax
    jnz 1f
    xor esi, esi
    mov rdx, [r15 + W_NPG]
    call pick_min
    test rax, rax
    jz 2f
1:  mov [r15 + W_RDCUR], r10
    mov dword ptr [rax + PC_INBATCH], 0
2:  ret
.Lpk_write:
    mov rsi, [r15 + W_NPG]
    mov rdx, rsi
    sub rsi, [r15 + W_NWR]
    call pick_min
    test rax, rax
    jnz 1f
    xor esi, esi
    mov rdx, [r15 + W_NPG]
    call pick_min
1:  ret

# pick_min(first, end) -> rax = the live connection in [first, end) with the fewest ops in flight and room for
# two more | 0, r10 = its index.
pick_min:
    xor eax, eax
    mov r9d, 0x7fffffff
    mov rcx, rsi
    shl rsi, 8
    add rsi, [r15 + W_PG]
.Lpm_loop:
    cmp rcx, rdx
    jae .Lpm_done
    cmp dword ptr [rsi + PC_FD], 0
    jl .Lpm_next
    mov r8d, [rsi + PC_N]
    cmp r8d, OPS_CAP - 2
    ja .Lpm_next
    cmp r8d, r9d
    jae .Lpm_next
    mov r9d, r8d
    mov rax, rsi
    mov r10, rcx
.Lpm_next:
    inc rcx
    add rsi, PC_SIZE
    jmp .Lpm_loop
.Lpm_done:
    ret

# ---------------------------------------------------------------- pgio_submit(kind, fd, flags, id, uid, aux) -> 1 | 0
.globl pgio_submit
pgio_submit:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    sub rsp, 32
    mov r12d, edi                       # kind
    mov r13d, esi                       # client fd
    mov r14d, edx                       # OPF_*
    mov rbp, rcx                        # post id
    mov [rsp], r8                       # user id
    mov [rsp + 8], r9                   # aux
    lea eax, [r12 - K_INS]              # K_INS and K_LIKE are the writes
    xor edi, edi
    cmp eax, 1
    setbe dil
    call pick
    test rax, rax
    jz .Lps_fail
    mov rbx, rax
    # room in the output buffer
    mov rax, [rbx + PC_OUT + PGB_CAP]
    sub rax, [rbx + PC_OUT + PGB_LEN]
    mov ecx, ROOM
    cmp r12d, K_INS
    jne 1f
    mov rdx, [rsp + 8]
    mov edx, [rdx + AUX_BLEN]
    add rcx, rdx
1:  cmp rax, rcx
    jae 2f
    mov rdi, [rbx + PC_OUT + PGB_PTR]
    mov rsi, [rbx + PC_OUT + PGB_LEN]
    mov rdx, [rbx + PC_OUT + PGB_CAP]
    add rcx, rsi
    call rt_grow
    mov [rbx + PC_OUT + PGB_PTR], rax
    mov [rbx + PC_OUT + PGB_CAP], rdx
2:  lea rdi, [rbx + PC_OUT]
    cmp r12d, K_FEED
    je .Lps_feed
    cmp r12d, K_POST
    je .Lps_post
    cmp r12d, K_LIKE
    je .Lps_like
    cmp r12d, K_INS
    je .Lps_ins
    cmp r12d, K_HEALTH
    je .Lps_health
    jmp .Lps_fail
.Lps_feed:
    lea rsi, [rip + st_feed]
    jmp .Lps_noparam
.Lps_health:
    lea rsi, [rip + st_health]
.Lps_noparam:
    mov edx, 1
    xor ecx, ecx
    call pgw_bind_begin
    mov [rsp + 16], rax
    jmp .Lps_bind_end
.Lps_post:
    lea rsi, [rip + st_post]
    mov edx, 1
    mov ecx, 1
    call pgw_bind_begin
    mov [rsp + 16], rax
    lea rdi, [rbx + PC_OUT]
    mov rsi, rbp
    call pgw_param_int8
    jmp .Lps_bind_end
.Lps_like:
    lea rsi, [rip + st_like]
    mov edx, 1
    mov ecx, 2
    call pgw_bind_begin
    mov [rsp + 16], rax
    lea rdi, [rbx + PC_OUT]
    mov rsi, [rsp]
    call pgw_param_int8
    lea rdi, [rbx + PC_OUT]
    mov rsi, rbp
    call pgw_param_int8
    jmp .Lps_bind_end
.Lps_ins:
    lea rsi, [rip + st_ins]
    mov edx, 1
    mov ecx, 2
    call pgw_bind_begin
    mov [rsp + 16], rax
    lea rdi, [rbx + PC_OUT]
    mov rsi, [rsp]
    call pgw_param_int8
    lea rdi, [rbx + PC_OUT]
    mov rsi, [rsp + 8]
    mov edx, [rsi + AUX_BLEN]
    mov ecx, [rsi + AUX_ULEN]
    lea rsi, [rsi + rcx + AUX_DATA]
    call pgw_param_text
.Lps_bind_end:
    lea rdi, [rbx + PC_OUT]
    mov rsi, [rsp + 16]
    cmp r12d, K_FEED
    je 3f
    cmp r12d, K_POST
    je 3f
    call pgw_bind_end
    jmp 4f
3:  call bind_end_row
4:  lea rdi, [rbx + PC_OUT]
    call pgw_execute
    # the op
    SLOT rdx, r13d
    movzx ecx, word ptr [rdx + C_GEN]
    or byte ptr [rdx + C_FLAGS], CF_BUSY
    PUSHOP
    mov [rax + OP_FD], r13d
    mov [rax + OP_GEN], cx
    mov [rax + OP_KIND], r12b
    mov [rax + OP_FL], r14b
    mov [rax + OP_ID], rbp
    mov rcx, [rsp + 8]
    mov [rax + OP_AUX], rcx
    mov rcx, [r15 + W_T0]
    mov [rax + OP_T0], rcx
    cmp r12d, K_LIKE
    je .Lps_write
    cmp r12d, K_INS
    je .Lps_write
    or dword ptr [rbx + PC_FLAGS], PCF_UNSYNCED
    jmp .Lps_common
.Lps_write:
    lea rdi, [rbx + PC_OUT]
    call pgw_sync
    PUSHOP
    pxor xmm0, xmm0
    movdqu [rax], xmm0
    movdqu [rax + 16], xmm0
    mov byte ptr [rax + OP_KIND], K_SYNC
    and dword ptr [rbx + PC_FLAGS], ~PCF_UNSYNCED
.Lps_common:
    or dword ptr [rbx + PC_FLAGS], PCF_DIRTY
    inc dword ptr [rbx + PC_INBATCH]
    inc qword ptr [r15 + W_NQUEUED]
    mov eax, 1
    jmp .Lps_ret
.Lps_fail:
    mov rdi, [rsp + 8]
    test rdi, rdi
    jz 1f
    mov esi, [rdi + AUX_CAP]
    call rt_free
1:  xor eax, eax
.Lps_ret:
    add rsp, 32
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- bind_end_row(buf, handle)
# Result formats of a post row (id, body, created_at, username, like_count): text, text, binary, text, text. The
# two integers come back as decimal digits, copied into the JSON as they are: no conversion, and on this data a
# few bytes per row fewer than the 8 + 4 of binary. created_at stays an 8-byte timestamp. Then patch the Bind's
# length (as pgw_bind_end).
bind_end_row:
    mov r9, [rdi + PGB_PTR]
    mov rax, [rdi + PGB_LEN]
    mov rcx, 0x0100000000000500         # int16 5, then 0, 0, 1 ...
    mov [r9 + rax], rcx
    mov dword ptr [r9 + rax + 8], 0     # ... 0, 0
    add rax, 12
    mov [rdi + PGB_LEN], rax
    sub rax, rsi
    dec rax
    bswap eax
    mov [r9 + rsi + 1], eax
    ret

# ---------------------------------------------------------------- pgio_flush()
.globl pgio_flush
pgio_flush:
    push rbx
    push r12
    push r13
    xor r12d, r12d
.Lpf_loop:
    cmp r12, [r15 + W_NPG]
    jae .Lpf_done
    mov rbx, r12
    shl rbx, 8
    add rbx, [r15 + W_PG]
    cmp dword ptr [rbx + PC_FD], 0
    jl .Lpf_next
    mov eax, [rbx + PC_FLAGS]
    test eax, PCF_DIRTY
    jz .Lpf_next
    test eax, PCF_UNSYNCED
    jz 1f
    lea rdi, [rbx + PC_OUT]
    call pgw_sync
    PUSHOP
    pxor xmm0, xmm0
    movdqu [rax], xmm0
    movdqu [rax + 16], xmm0
    mov byte ptr [rax + OP_KIND], K_SYNC
    and dword ptr [rbx + PC_FLAGS], ~PCF_UNSYNCED
1:  mov dword ptr [rbx + PC_INBATCH], 0
    test dword ptr [rbx + PC_FLAGS], PCF_EOUT
    jnz .Lpf_next
    mov rdi, rbx
    call pc_write
.Lpf_next:
    inc r12
    jmp .Lpf_loop
.Lpf_done:
    mov qword ptr [r15 + W_RDCUR], -1
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- pc_write(pc): send what is pending
pc_write:
    push rbx
    mov rbx, rdi
.Lpw_again:
    mov rsi, [rbx + PC_OUT + PGB_PTR]
    add rsi, [rbx + PC_OUTOFF]
    mov rdx, [rbx + PC_OUT + PGB_LEN]
    sub rdx, [rbx + PC_OUTOFF]
    jz .Lpw_drained
    mov edi, [rbx + PC_FD]
    mov r10d, MSG_NOSIGNAL | MSG_DONTWAIT
    xor r8d, r8d
    xor r9d, r9d
    SYS SYS_sendto
    test rax, rax
    js .Lpw_err
    add [rbx + PC_OUTOFF], rax
    mov rax, [rbx + PC_OUTOFF]
    cmp rax, [rbx + PC_OUT + PGB_LEN]
    jb .Lpw_eagain
.Lpw_drained:
    mov qword ptr [rbx + PC_OUT + PGB_LEN], 0
    mov qword ptr [rbx + PC_OUTOFF], 0
    and dword ptr [rbx + PC_FLAGS], ~PCF_DIRTY
    test dword ptr [rbx + PC_FLAGS], PCF_EOUT
    jz .Lpw_ret
    and dword ptr [rbx + PC_FLAGS], ~PCF_EOUT
    mov edi, EPOLL_CTL_MOD
    mov esi, [rbx + PC_FD]
    mov edx, EPOLL_IN_FLAGS
    mov ecx, [rbx + PC_IDX]
    mov rax, TAG_PG_HI << 32
    or rcx, rax
    call ep_ctl
    jmp .Lpw_ret
.Lpw_eagain:
    test dword ptr [rbx + PC_FLAGS], PCF_EOUT
    jnz .Lpw_ret
    or dword ptr [rbx + PC_FLAGS], PCF_EOUT
    mov edi, EPOLL_CTL_MOD
    mov esi, [rbx + PC_FD]
    mov edx, EPOLL_IN_FLAGS | EPOLLOUT
    mov ecx, [rbx + PC_IDX]
    mov rax, TAG_PG_HI << 32
    or rcx, rax
    call ep_ctl
    jmp .Lpw_ret
.Lpw_err:
    cmp rax, -EAGAIN
    je .Lpw_eagain
    cmp rax, -EINTR
    je .Lpw_again
    mov rdi, rbx
    call pg_down
.Lpw_ret:
    pop rbx
    ret

# ---------------------------------------------------------------- pgio_event(idx, events)
.globl pgio_event
pgio_event:
    push rbx
    push r12
    push r13
    mov rbx, rdi
    shl rbx, 8
    add rbx, [r15 + W_PG]
    mov r12d, esi
    cmp dword ptr [rbx + PC_FD], 0
    jl .Lpe_ret
    test r12d, EPOLLOUT
    jz 1f
    mov rdi, rbx
    call pc_write
    cmp dword ptr [rbx + PC_FD], 0
    jl .Lpe_ret
1:  test r12d, EPOLL_IN_FLAGS
    jz .Lpe_ret
.Lpe_read:
    mov rdx, [rbx + PC_INCAP]
    sub rdx, [rbx + PC_INLEN]
    jnz 2f
    mov rdi, rbx
    call pc_compact
    mov rdx, [rbx + PC_INCAP]
    sub rdx, [rbx + PC_INLEN]
    jz .Lpe_down                        # one op's results do not fit: give the connection up
2:  mov r13, rdx
    mov edi, [rbx + PC_FD]
    mov rsi, [rbx + PC_IN]
    add rsi, [rbx + PC_INLEN]
    SYS SYS_read
    test rax, rax
    jz .Lpe_down
    js .Lpe_rerr
    add [rbx + PC_INLEN], rax
    mov r12, rax
    mov rdi, rbx
    call pc_parse
    cmp dword ptr [rbx + PC_FD], 0
    jl .Lpe_ret
    cmp r12, r13
    jb .Lpe_tidy
    jmp .Lpe_read
.Lpe_rerr:
    cmp rax, -EAGAIN
    je .Lpe_tidy
    cmp rax, -EINTR
    je .Lpe_read
    jmp .Lpe_down
.Lpe_tidy:
    mov rax, [rbx + PC_SCAN]
    cmp rax, [rbx + PC_INLEN]
    jne 3f
    cmp rax, [rbx + PC_INPOS]
    jne 3f
    xor eax, eax
    mov [rbx + PC_INLEN], rax
    mov [rbx + PC_INPOS], rax
    mov [rbx + PC_SCAN], rax
    jmp .Lpe_ret
3:  mov rax, [rbx + PC_INCAP]
    sub rax, [rbx + PC_INLEN]
    cmp rax, 65536
    jae .Lpe_ret
    mov rdi, rbx
    call pc_compact
    jmp .Lpe_ret
.Lpe_down:
    mov rdi, rbx
    call pg_down
.Lpe_ret:
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- pc_compact(pc): move [INPOS, INLEN) to the start
pc_compact:
    mov rax, [rdi + PC_INPOS]
    test rax, rax
    jz 1f
    mov r8, rdi
    mov rcx, [r8 + PC_INLEN]
    sub rcx, rax
    mov rdi, [r8 + PC_IN]
    lea rsi, [rdi + rax]
    rep movsb
    sub [r8 + PC_INLEN], rax
    sub [r8 + PC_SCAN], rax
    mov qword ptr [r8 + PC_INPOS], 0
1:  ret

# ---------------------------------------------------------------- pc_parse(pc): consume complete backend messages
pc_parse:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    mov rbx, rdi
.Lpp_loop:
    mov r12, [rbx + PC_IN]
    mov rdi, [rbx + PC_SCAN]
    mov rsi, [rbx + PC_INLEN]
    sub rsi, rdi
    add rdi, r12
    call pgr_next
    test rax, rax
    jz .Lpp_done
    js .Lpp_proto
    add [rbx + PC_SCAN], rdx
    cmp eax, 0x43                       # CommandComplete
    je .Lpp_cc
    cmp eax, 0x45                       # ErrorResponse
    je .Lpp_err
    cmp eax, 0x5a                       # ReadyForQuery
    je .Lpp_rfq
    jmp .Lpp_loop                       # DataRow, BindComplete, notices, parameter status
.Lpp_cc:
    cmp dword ptr [rbx + PC_N], 0
    je .Lpp_loop
    HEADOP
    mov rbp, rax
    movzx eax, byte ptr [rbp + OP_KIND]
    cmp eax, K_INS
    je .Lpp_loop                        # writes wait for ReadyForQuery (the commit)
    cmp eax, K_LIKE
    je .Lpp_loop
    cmp eax, K_SYNC
    je .Lpp_loop
    POPOP
    mov rdi, rbp
    call .Lpp_region
    call render_answer
    mov rax, [rbx + PC_SCAN]
    mov [rbx + PC_INPOS], rax
    jmp .Lpp_loop
.Lpp_err:
    cmp dword ptr [rbx + PC_N], 0
    je .Lpp_loop
    HEADOP
    mov rbp, rax
    movzx eax, byte ptr [rbp + OP_KIND]
    cmp eax, K_INS
    je .Lpp_loop
    cmp eax, K_LIKE
    je .Lpp_loop
    cmp eax, K_SYNC
    je .Lpp_loop
    POPOP
    mov rdi, rbp
    mov esi, E_INTERNAL
    call render_fail
    mov rax, [rbx + PC_SCAN]
    mov [rbx + PC_INPOS], rax
    jmp .Lpp_loop
.Lpp_rfq:
    cmp dword ptr [rbx + PC_N], 0
    je .Lpp_rfq_end
    HEADOP
    mov rbp, rax
    POPOP
    movzx eax, byte ptr [rbp + OP_KIND]
    cmp eax, K_SYNC
    je .Lpp_rfq_end
    cmp eax, K_INS
    je .Lpp_rfq_write
    cmp eax, K_LIKE
    je .Lpp_rfq_write
    mov rdi, rbp                        # a read the backend skipped after an error
    mov esi, E_INTERNAL
    call render_fail
    jmp .Lpp_rfq
.Lpp_rfq_write:
    mov rdi, rbp
    call .Lpp_region
    call render_answer
    jmp .Lpp_rfq
.Lpp_rfq_end:
    mov rax, [rbx + PC_SCAN]
    mov [rbx + PC_INPOS], rax
    jmp .Lpp_loop
.Lpp_proto:
    mov rdi, rbx
    call pg_down
.Lpp_done:
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# rsi = start of the head op's messages, rdx = their length (rbx = pc); keeps rdi
.Lpp_region:
    mov rsi, [rbx + PC_IN]
    add rsi, [rbx + PC_INPOS]
    mov rdx, [rbx + PC_SCAN]
    sub rdx, [rbx + PC_INPOS]
    ret

# ---------------------------------------------------------------- pgio_reconnect_one()
.globl pgio_reconnect_one
pgio_reconnect_one:
    push rbx
    push r12
    push r13
    xor r12d, r12d
.Lrc_loop:
    cmp r12, [r15 + W_NPG]
    jae .Lrc_ret
    mov rbx, r12
    shl rbx, 8
    add rbx, [r15 + W_PG]
    cmp dword ptr [rbx + PC_FD], 0
    jge .Lrc_next
    mov rdi, rbx
    call pg_open
    test eax, eax
    jz .Lrc_ret
    dec qword ptr [r15 + W_NDEAD]
    lea rdi, [rip + m_pgup]
    mov esi, m_pgup_len
    mov rdx, [r15 + W_ID]
    call rt_log_num
    jmp .Lrc_ret
.Lrc_next:
    inc r12
    jmp .Lrc_loop
.Lrc_ret:
    pop r13
    pop r12
    pop rbx
    ret

.section .note.GNU-stack,"",@progbits
