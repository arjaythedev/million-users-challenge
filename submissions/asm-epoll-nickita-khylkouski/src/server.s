# server.s: the worker thread. One epoll per worker, level-triggered; clients are indexed by fd in the shared
# connection table (32 bytes each). A request is read straight into the worker's read buffer and parsed there;
# only a partial request, or input that arrives while a statement is in flight, is copied into a per-connection
# buffer. A response is one send(); whatever the socket does not take is parked and finished on EPOLLOUT.
#
# Exports: worker_main(w), conn_send(fd, ptr, len, close), conn_close(fd), conn_resume(fd)

.include "defs.inc"
.include "app.inc"

.equ SYS_sched_setaffinity_, 203
.equ EPOLL_IN_FLAGS, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP

.section .rodata
DEFSTR m_epoll, "epoll_create1 failed\n"
DEFSTR m_ready, "worker ready: "
DEFSTR m_unsteer, "cpu steering detached, busiest worker's share of new connections (%): "

.bss
g_acc_snap:     .skip MAX_WORKERS * 8   # steering guard: W_ACCEPTS of each worker at the start of the window
g_guard_next:   .skip 8                 # monotonic ms of the next check

.text

# ---------------------------------------------------------------- slot helpers

# ep_ctl(op, fd, events, data) : epoll_ctl on this worker's epoll
.globl ep_ctl
ep_ctl:
    sub rsp, 24
    mov [rsp], edx                      # events
    mov [rsp + 4], rcx                  # data (unaligned u64 at +4: the packed epoll_event layout)
    mov edx, esi                        # fd
    mov esi, edi                        # op
    mov rdi, [r15 + W_EPFD]
    mov r10, rsp
    SYS SYS_epoll_ctl
    add rsp, 24
    ret

# ---------------------------------------------------------------- worker_main(w): never returns
.globl worker_main
worker_main:
    mov r15, rdi
    and rsp, -16
    sub rsp, 160
    # pin
    mov rax, [r15 + W_CPU]
    test rax, rax
    js 1f
    # 128-byte mask on the stack
    xor ecx, ecx
2:  mov qword ptr [rsp + rcx*8], 0
    inc ecx
    cmp ecx, 16
    jb 2b
    mov rcx, rax
    shr rcx, 3
    and eax, 7
    bts dword ptr [rsp + rcx], eax
    xor edi, edi
    mov esi, 128
    mov rdx, rsp
    SYS SYS_sched_setaffinity_
1:  # epoll
    xor edi, edi
    SYS SYS_epoll_create1
    test rax, rax
    js .Lwm_noepoll
    mov [r15 + W_EPFD], rax
    # buffers
    mov edi, EV_BATCH * 12
    call rt_mmap
    mov [r15 + W_EVENTS], rax
    mov edi, RBUF_SIZE
    call rt_mmap
    mov [r15 + W_RBUF], rax
    mov edi, ARENA_SIZE
    call rt_mmap
    mov [r15 + W_ARENA], rax
    mov edi, ARENA_SIZE
    call rt_mmap
    mov [r15 + W_ZBUF], rax
    mov edi, GZ_CTX_SIZE + 4096
    call rt_mmap
    add rax, 63
    and rax, -64
    mov [r15 + W_GZ], rax
    mov rdi, rax
    call gzip_init
    mov qword ptr [r15 + W_RDCUR], -1
    # Postgres
    call pgio_init
    # listeners
    mov rsi, [r15 + W_LFD0]
    call .Ladd_listener
    mov rsi, [r15 + W_LFD1]
    test rsi, rsi
    js 3f
    call .Ladd_listener
3:  lea rdi, [rip + m_ready]
    mov esi, m_ready_len
    mov rdx, [r15 + W_ID]
    call rt_log_num

# ---------------------------------------------------------------- the loop
.Lloop:
    mov rdi, [r15 + W_EPFD]
    mov rsi, [r15 + W_EVENTS]
    mov edx, EV_BATCH
    mov r10d, 1000
    SYS SYS_epoll_wait
    test rax, rax
    jns 4f
    xor eax, eax                        # EINTR etc.
4:  mov rbx, rax                        # n events
    mov r12, rax
    call rt_now_sec
    mov [r15 + W_NOW], rax
    call rt_mono_ms
    mov [r15 + W_MONO], rax
    mov [r15 + W_T0], rax
    # W_T0: when did the requests of this batch arrive? epoll reports ready fds in the order they became ready,
    # so the first client event is the oldest; TCP_INFO on it says how long ago its data was received
    # (tcpi_last_data_recv), whether we were busy, asleep or not scheduled meanwhile. One getsockopt per batch.
    # render.s compares a response's time against this bound to tell when the kernel's 40 ms delayed-ACK
    # timer has already fired for the request (op_rearm).
    mov r13, [r15 + W_EVENTS]
    mov rcx, rbx
.Lt0_scan:
    test rcx, rcx
    jz .Lt0_done
    dec rcx
    test dword ptr [r13], EPOLLIN
    jz .Lt0_next
    cmp dword ptr [r13 + 8], 0          # high dword of the tag: 0 = client fd
    jne .Lt0_next
    mov edi, [r13 + 4]
    sub rsp, 272
    mov dword ptr [rsp + 256], 256
    mov esi, IPPROTO_TCP
    mov edx, TCP_INFO
    mov r10, rsp
    lea r8, [rsp + 256]
    SYS SYS_getsockopt
    mov ecx, [rsp + TCPI_LAST_DATA_RECV]
    mov edx, [rsp + 256]
    add rsp, 272
    test rax, rax
    jnz .Lt0_done
    cmp edx, TCPI_LAST_DATA_RECV + 4
    jb .Lt0_done
    cmp ecx, 1000                       # a stale value (no data on this fd, e.g. a FIN): bound the pessimism
    jbe 5f
    mov ecx, 1000
5:  mov rax, [r15 + W_MONO]
    sub rax, rcx
    mov [r15 + W_T0], rax
    jmp .Lt0_done
.Lt0_next:
    add r13, 12
    jmp .Lt0_scan
.Lt0_done:
    mov qword ptr [r15 + W_NQUEUED], 0
    mov r13, [r15 + W_EVENTS]
.Lev_next:
    test rbx, rbx
    jz .Lev_done
    dec rbx
    mov r14d, [r13]                     # events
    mov rax, [r13 + 4]                  # data
    add r13, 12
    mov rdx, rax
    shr rdx, 32
    test edx, TAG_LISTEN_HI
    jnz .Lev_listen
    test edx, TAG_PG_HI
    jnz .Lev_pg
    # client
    mov ebp, eax                        # fd
    test r14d, EPOLL_IN_FLAGS
    jz 5f
    mov edi, ebp
    call on_readable
5:  test r14d, EPOLLOUT
    jz .Lev_next
    mov edi, ebp
    call on_writable
    jmp .Lev_next
.Lev_listen:
    mov edi, eax                        # listener fd
    call accept_burst
    jmp .Lev_next
.Lev_pg:
    mov edi, eax                        # pg index
    mov esi, r14d
    call pgio_event
    jmp .Lev_next
.Lev_done:
    call pgio_flush
    # accept back on after EMFILE
    mov rax, [r15 + W_ACCOFF]
    test rax, rax
    jz 6f
    cmp rax, [r15 + W_MONO]
    ja 6f
    mov qword ptr [r15 + W_ACCOFF], 0
    mov rsi, [r15 + W_LFD0]
    call .Ladd_listener
    mov rsi, [r15 + W_LFD1]
    test rsi, rsi
    js 6f
    call .Ladd_listener
6:  # reconnect lost Postgres connections, one a second
    cmp qword ptr [r15 + W_NDEAD], 0
    je 7f
    mov rax, [r15 + W_MONO]
    cmp rax, [r15 + W_RETRY]
    jb 7f
    add rax, 1000
    mov [r15 + W_RETRY], rax
    call pgio_reconnect_one
7:  # worker 0 checks once a second that the CPU steering spreads new connections evenly
    cmp qword ptr [rip + g_steered], 0
    je 8f
    cmp qword ptr [r15 + W_ID], 0
    jne 8f
    mov rax, [r15 + W_MONO]
    cmp rax, [rip + g_guard_next]
    jb 8f
    add rax, 1000
    mov [rip + g_guard_next], rax
    call steer_guard
8:  # light iteration: pause so the next one batches more
    test r12, r12
    jz .Lloop
    mov rax, [rip + g_batch_us]
    test rax, rax
    jz .Lloop
    cmp r12, [rip + g_batch_min]
    jae .Lloop
    imul rax, rax, 1000
    mov qword ptr [rsp], 0
    mov [rsp + 8], rax
    mov rdi, rsp
    xor esi, esi
    SYS SYS_nanosleep
    jmp .Lloop

.Ladd_listener:                         # rsi = listener fd
    mov edi, EPOLL_CTL_ADD
    mov edx, EPOLLIN
    mov rcx, rsi
    mov rax, TAG_LISTEN_HI
    shl rax, 32
    or rcx, rax
    jmp ep_ctl
.Lwm_noepoll:
    lea rdi, [rip + m_epoll]
    mov esi, m_epoll_len
    jmp rt_die

# ---------------------------------------------------------------- steer_guard: worker 0, once a second
# The steering program hands a connection to the worker on the CPU that processed its SYN. That spreads
# connections evenly only while the kernel spreads receive processing over the CPUs (RPS) and the NET_RX core
# found at start has no worker. If one worker took more than twice the even share of a window of new
# connections (64 per worker), the program is detached from both groups and the kernel's 4-tuple hash
# spreads new connections from then on. Workers only ever write their own W_ACCEPTS.
steer_guard:
    push rbx
    push r12
    push r13
    push r14
    sub rsp, 8
    mov r12, [rip + g_nworkers]
    xor r13d, r13d                      # new connections in this window
    xor r14d, r14d                      # of which the busiest worker took
    mov rdx, [rip + g_workers]
    lea rsi, [rip + g_acc_snap]
    xor ecx, ecx
1:  mov rax, [rdx + W_ACCEPTS]
    sub rax, [rsi + rcx*8]
    add r13, rax
    cmp rax, r14
    jbe 2f
    mov r14, rax
2:  add rdx, W_SIZE
    inc rcx
    cmp rcx, r12
    jb 1b
    mov rax, r12
    shl rax, 6
    cmp r13, rax
    jb .Lsg_ret                         # window not full yet
    mov rdx, [rip + g_workers]          # next window starts here
    xor ecx, ecx
3:  mov rax, [rdx + W_ACCEPTS]
    mov [rsi + rcx*8], rax
    add rdx, W_SIZE
    inc rcx
    cmp rcx, r12
    jb 3b
    mov rax, r14
    imul rax, r12
    lea rdx, [r13 + r13]
    cmp rax, rdx
    jbe .Lsg_ret                        # busiest <= 2 x even share
    mov dword ptr [rsp], 0
    mov rbx, [rip + g_workers]
    test qword ptr [rip + g_steered], 1
    jz 4f
    mov rdi, [rbx + W_LFD0]
    mov esi, SOL_SOCKET
    mov edx, SO_DETACH_REUSEPORT_BPF
    mov r10, rsp
    mov r8d, 4
    SYS SYS_setsockopt
4:  test qword ptr [rip + g_steered], 2
    jz 5f
    mov rdi, [rbx + W_LFD1]
    mov esi, SOL_SOCKET
    mov edx, SO_DETACH_REUSEPORT_BPF
    mov r10, rsp
    mov r8d, 4
    SYS SYS_setsockopt
5:  mov qword ptr [rip + g_steered], 0
    imul rax, r14, 100
    xor edx, edx
    div r13
    lea rdi, [rip + m_unsteer]
    mov esi, m_unsteer_len
    mov rdx, rax
    call rt_log_num
.Lsg_ret:
    add rsp, 8
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- accept_burst(lfd)
accept_burst:
    push rbx
    push r12
    push r13
    mov r12d, edi
    cmp qword ptr [r15 + W_ACCOFF], 0
    jne .Lab_ret
    mov r13d, ACCEPT_BURST
.Lab_next:
    mov edi, r12d
    xor esi, esi
    xor edx, edx
    mov r10d, SOCK_NONBLOCK | SOCK_CLOEXEC
    SYS SYS_accept4
    test rax, rax
    js .Lab_err
    mov ebx, eax
    cmp rax, [rip + g_maxfd]
    jae .Lab_toobig
    SLOT rdx, ebx
    mov qword ptr [rdx + C_IN], 0
    mov qword ptr [rdx + C_OUT], 0
    mov qword ptr [rdx + C_INLEN], 0    # inlen + outlen
    mov dword ptr [rdx + C_OUTOFF], 0
    mov byte ptr [rdx + C_FLAGS], CF_OPEN
    mov byte ptr [rdx + C_CLS], 0
    inc qword ptr [r15 + W_ACCEPTS]
    mov edi, EPOLL_CTL_ADD
    mov esi, ebx
    mov edx, EPOLL_IN_FLAGS
    mov ecx, ebx
    call ep_ctl
    dec r13d
    jnz .Lab_next
    jmp .Lab_ret
.Lab_toobig:
    mov edi, ebx
    SYS SYS_close
    dec r13d
    jnz .Lab_next
    jmp .Lab_ret
.Lab_err:
    cmp eax, -EAGAIN
    je .Lab_ret
    cmp eax, -EMFILE
    je .Lab_pause
    cmp eax, -ENFILE
    je .Lab_pause
    cmp eax, -ENOBUFS
    je .Lab_pause
    cmp eax, -ENOMEM
    je .Lab_pause
    dec r13d                            # ECONNABORTED and friends: keep going
    jnz .Lab_next
    jmp .Lab_ret
.Lab_pause:
    # out of descriptors: stop accepting for a second (level-triggered listeners would spin otherwise)
    mov rax, [r15 + W_MONO]
    add rax, 1000
    mov [r15 + W_ACCOFF], rax
    mov edi, EPOLL_CTL_DEL
    mov rsi, [r15 + W_LFD0]
    xor edx, edx
    xor ecx, ecx
    call ep_ctl
    mov rsi, [r15 + W_LFD1]
    test rsi, rsi
    js .Lab_ret
    mov edi, EPOLL_CTL_DEL
    xor edx, edx
    xor ecx, ecx
    call ep_ctl
.Lab_ret:
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- conn_close(fd)
.globl conn_close
conn_close:
    push rbx
    push r12
    SLOT rbx, edi
    mov r12d, edi
    test byte ptr [rbx + C_FLAGS], CF_OPEN
    jz .Lcc_ret
    mov edi, r12d
    SYS SYS_close                       # epoll forgets the fd with it
    mov rdi, [rbx + C_IN]
    test rdi, rdi
    jz 1f
    movzx ecx, byte ptr [rbx + C_CLS]
    and ecx, 15
    add ecx, 6
    mov esi, 1
    shl rsi, cl
    call rt_free
1:  mov rdi, [rbx + C_OUT]
    test rdi, rdi
    jz 2f
    movzx ecx, byte ptr [rbx + C_CLS]
    shr ecx, 4
    add ecx, 6
    mov esi, 1
    shl rsi, cl
    call rt_free
2:  mov qword ptr [rbx + C_IN], 0
    mov qword ptr [rbx + C_OUT], 0
    mov qword ptr [rbx + C_INLEN], 0
    mov dword ptr [rbx + C_OUTOFF], 0
    inc word ptr [rbx + C_GEN]
    mov word ptr [rbx + C_FLAGS], 0     # flags + cls
.Lcc_ret:
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- conn_send(fd, ptr, len, close)
# Sends a complete response. Keeps order behind parked output; parks what the socket does not take.
.globl conn_send
conn_send:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    SLOT rbx, edi
    mov ebp, edi
    mov r12, rsi
    mov r13, rdx
    mov r14d, ecx
    test byte ptr [rbx + C_FLAGS], CF_OPEN
    jz .Lcs_ret
    cmp qword ptr [rbx + C_OUT], 0
    jne .Lcs_append
    mov edi, ebp
    mov rsi, r12
    mov rdx, r13
    mov r10d, MSG_NOSIGNAL | MSG_DONTWAIT
    xor r8d, r8d
    xor r9d, r9d
    SYS SYS_sendto
    cmp rax, r13
    je .Lcs_all
    test rax, rax
    jns .Lcs_partial
    cmp eax, -EAGAIN
    jne .Lcs_fail
    xor eax, eax
.Lcs_partial:
    add r12, rax                        # remaining bytes -> park
    sub r13, rax
    mov rdi, r13
    call rt_alloc
    mov [rbx + C_OUT], rax
    bsr rcx, rdx
    sub ecx, 6
    shl ecx, 4
    movzx eax, byte ptr [rbx + C_CLS]
    and eax, 15
    or eax, ecx
    mov [rbx + C_CLS], al
    mov rdi, [rbx + C_OUT]
    mov rsi, r12
    mov rdx, r13
    call rt_memcpy
    mov [rbx + C_OUTLEN], r13d
    mov dword ptr [rbx + C_OUTOFF], 0
    or byte ptr [rbx + C_FLAGS], CF_WOUT
    test r14d, r14d
    jz 1f
    or byte ptr [rbx + C_FLAGS], CF_CLOSING
1:  mov edi, EPOLL_CTL_MOD
    mov esi, ebp
    mov edx, EPOLL_IN_FLAGS | EPOLLOUT
    mov ecx, ebp
    call ep_ctl
    jmp .Lcs_ret
.Lcs_all:
    test r14d, r14d
    jz .Lcs_ret
    mov edi, ebp
    call conn_close
    jmp .Lcs_ret
.Lcs_fail:
    mov edi, ebp
    call conn_close
    jmp .Lcs_ret
.Lcs_append:
    # behind parked output: append (grow the block if needed)
    mov rdi, [rbx + C_OUT]
    mov esi, [rbx + C_OUTLEN]
    movzx ecx, byte ptr [rbx + C_CLS]
    shr ecx, 4
    add ecx, 6
    mov edx, 1
    shl rdx, cl
    lea rcx, [rsi + r13]
    call rt_grow
    mov [rbx + C_OUT], rax
    bsr rcx, rdx
    sub ecx, 6
    shl ecx, 4
    movzx edx, byte ptr [rbx + C_CLS]
    and edx, 15
    or edx, ecx
    mov [rbx + C_CLS], dl
    mov edi, [rbx + C_OUTLEN]
    add rdi, rax
    mov rsi, r12
    mov rdx, r13
    call rt_memcpy
    add [rbx + C_OUTLEN], r13d
    test r14d, r14d
    jz .Lcs_ret
    or byte ptr [rbx + C_FLAGS], CF_CLOSING
.Lcs_ret:
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- on_writable(fd)
on_writable:
    push rbx
    push r12
    SLOT rbx, edi
    mov r12d, edi
    mov rsi, [rbx + C_OUT]
    test rsi, rsi
    jz .Low_ret
    mov eax, [rbx + C_OUTOFF]
    add rsi, rax
    mov edx, [rbx + C_OUTLEN]
    sub edx, eax
    mov edi, r12d
    mov r10d, MSG_NOSIGNAL | MSG_DONTWAIT
    xor r8d, r8d
    xor r9d, r9d
    SYS SYS_sendto
    test rax, rax
    js .Low_err
    add [rbx + C_OUTOFF], eax
    mov eax, [rbx + C_OUTOFF]
    cmp eax, [rbx + C_OUTLEN]
    jb .Low_ret
    # drained
    mov rdi, [rbx + C_OUT]
    movzx ecx, byte ptr [rbx + C_CLS]
    shr ecx, 4
    add ecx, 6
    mov esi, 1
    shl rsi, cl
    call rt_free
    mov qword ptr [rbx + C_OUT], 0
    mov dword ptr [rbx + C_OUTLEN], 0
    mov dword ptr [rbx + C_OUTOFF], 0
    test byte ptr [rbx + C_FLAGS], CF_CLOSING
    jz 1f
    mov edi, r12d
    call conn_close
    jmp .Low_ret
1:  and byte ptr [rbx + C_FLAGS], ~CF_WOUT
    mov edi, EPOLL_CTL_MOD
    mov esi, r12d
    mov edx, EPOLL_IN_FLAGS
    mov ecx, r12d
    call ep_ctl
    mov edi, r12d
    call conn_resume
    jmp .Low_ret
.Low_err:
    cmp eax, -EAGAIN
    je .Low_ret
    mov edi, r12d
    call conn_close
.Low_ret:
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- conn_resume(fd): after a statement finished or
# the parked output drained, serve the pipelined requests waiting in C_IN (if any).
.globl conn_resume
conn_resume:
    push rbx
    SLOT rbx, edi
    movzx eax, byte ptr [rbx + C_FLAGS]
    test eax, CF_OPEN
    jz .Lcr_ret
    test eax, CF_BUSY | CF_CLOSING
    jnz .Lcr_ret
    cmp qword ptr [rbx + C_OUT], 0
    jne .Lcr_ret
    cmp dword ptr [rbx + C_INLEN], 0
    je .Lcr_ret
    mov rsi, [rbx + C_IN]
    mov edx, [rbx + C_INLEN]
    call process
.Lcr_ret:
    pop rbx
    ret

# ---------------------------------------------------------------- on_readable(fd)
on_readable:
    push rbx
    push r12
    push r13
    SLOT rbx, edi
    mov r12d, edi
    movzx eax, byte ptr [rbx + C_FLAGS]
    test eax, CF_OPEN
    jz .Lor_ret
    test eax, CF_BUSY
    jnz .Lor_carry
    cmp dword ptr [rbx + C_INLEN], 0
    jne .Lor_carry
    # fast path: read into the worker buffer
    mov edi, r12d
    mov rsi, [r15 + W_RBUF]
    mov edx, RBUF_SIZE
    SYS SYS_read
    test rax, rax
    jle .Lor_eof
    mov edi, r12d
    mov rsi, [r15 + W_RBUF]
    mov rdx, rax
    call process
    jmp .Lor_ret
.Lor_carry:
    # append to the carried bytes
    mov rdi, [rbx + C_IN]
    mov esi, [rbx + C_INLEN]
    test rdi, rdi
    jnz 1f
    xor edx, edx                        # no buffer yet
    jmp 2f
1:  movzx ecx, byte ptr [rbx + C_CLS]
    and ecx, 15
    add ecx, 6
    mov edx, 1
    shl rdx, cl
2:  lea rcx, [rsi + 4096]
    cmp rdx, rcx
    jae 3f
    cmp rdx, 4 * MAXREQ
    ja .Lor_close                       # a client that keeps sending while a statement is in flight
    test rdi, rdi
    jnz 4f
    mov rdi, RBUF_SIZE
    call rt_alloc                       # rax, rdx
    mov [rbx + C_IN], rax
    jmp 5f
4:  call rt_grow
    mov [rbx + C_IN], rax
5:  bsr rcx, rdx
    sub ecx, 6
    movzx eax, byte ptr [rbx + C_CLS]
    and eax, 0xf0
    or eax, ecx
    mov [rbx + C_CLS], al
    mov rdi, [rbx + C_IN]
    mov esi, [rbx + C_INLEN]
3:  # rdi = buffer, rsi = inlen, rdx = cap
    add rdi, rsi
    sub rdx, rsi
    mov rsi, rdi
    mov edi, r12d
    SYS SYS_read
    test rax, rax
    jle .Lor_eof
    add [rbx + C_INLEN], eax
    test byte ptr [rbx + C_FLAGS], CF_BUSY
    jnz .Lor_ret
    mov edi, r12d
    mov rsi, [rbx + C_IN]
    mov edx, [rbx + C_INLEN]
    call process
    jmp .Lor_ret
.Lor_eof:
    cmp eax, -EAGAIN
    je .Lor_ret
.Lor_close:
    mov edi, r12d
    call conn_close
.Lor_ret:
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- process(fd, buf, n): serve, then carry the rest
process:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    SLOT rbx, edi
    mov ebp, edi
    mov r12, rsi
    mov r13, rdx
    call serve                          # rax = used | -1
    test byte ptr [rbx + C_FLAGS], CF_OPEN
    jz .Lpr_ret
    test rax, rax
    js .Lpr_drop
    mov r14, rax
    test byte ptr [rbx + C_FLAGS], CF_CLOSING
    jnz .Lpr_none
    mov rdx, r13
    sub rdx, r14                        # left
    jz .Lpr_none
    cmp r12, [rbx + C_IN]
    je .Lpr_inplace
    # from the worker buffer: copy the rest into a fresh block
    push rdx
    lea rdi, [rdx + RBUF_SIZE]
    call rt_alloc
    mov [rbx + C_IN], rax
    bsr rcx, rdx
    sub ecx, 6
    movzx eax, byte ptr [rbx + C_CLS]
    and eax, 0xf0
    or eax, ecx
    mov [rbx + C_CLS], al
    pop rdx
    mov rdi, [rbx + C_IN]
    lea rsi, [r12 + r14]
    mov [rbx + C_INLEN], edx
    call rt_memcpy
    jmp .Lpr_ret
.Lpr_inplace:
    mov [rbx + C_INLEN], edx
    test r14, r14
    jz .Lpr_ret
    # memmove down (overlapping, dst < src: byte copy forward is safe)
    mov rdi, [rbx + C_IN]
    lea rsi, [rdi + r14]
    mov rcx, rdx
    rep movsb
    jmp .Lpr_ret
.Lpr_none:
    mov rdi, [rbx + C_IN]
    test rdi, rdi
    jz .Lpr_ret
    movzx ecx, byte ptr [rbx + C_CLS]
    and ecx, 15
    add ecx, 6
    mov esi, 1
    shl rsi, cl
    call rt_free
    mov qword ptr [rbx + C_IN], 0
    mov dword ptr [rbx + C_INLEN], 0
    jmp .Lpr_ret
.Lpr_drop:
    mov edi, ebp
    call conn_close
.Lpr_ret:
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- serve(fd, buf, n) -> used | -1
# Serves complete requests until one waits for Postgres (or the connection closes / parks output).
serve:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    sub rsp, 16
    SLOT rbx, edi
    mov ebp, edi
    mov r12, rsi                        # buf
    mov r13, rdx                        # n
    xor r14d, r14d                      # used
.Lsv_loop:
    cmp r14, r13
    jae .Lsv_done
    movzx eax, byte ptr [rbx + C_FLAGS]
    test eax, CF_OPEN
    jz .Lsv_done
    test eax, CF_BUSY | CF_CLOSING
    jnz .Lsv_done
    cmp qword ptr [rbx + C_OUT], 0
    jne .Lsv_done
    lea rdi, [r12 + r14]
    mov rsi, r13
    sub rsi, r14
    lea rdx, [r15 + W_REQ]
    call http_parse
    test rax, rax
    jz .Lsv_partial
    js .Lsv_bad
    add r14, rax
    and byte ptr [rbx + C_FLAGS], ~CF_CONT
    mov edi, ebp
    lea rsi, [r15 + W_REQ]
    call api_handle
    jmp .Lsv_loop
.Lsv_partial:
    mov rax, r13
    sub rax, r14
    cmp rax, MAXREQ
    ja .Lsv_bad
    mov eax, [r15 + W_REQ + REQ_FLAGS]
    and eax, REQF_HEAD_DONE | REQF_EXPECT
    cmp eax, REQF_HEAD_DONE | REQF_EXPECT
    jne .Lsv_done
    test byte ptr [rbx + C_FLAGS], CF_CONT
    jnz .Lsv_done
    or byte ptr [rbx + C_FLAGS], CF_CONT
    lea rdi, [r15 + W_HSCR]
    call http_write_continue
    mov edi, ebp
    lea rsi, [r15 + W_HSCR]
    mov edx, 25
    xor ecx, ecx
    call conn_send
    jmp .Lsv_done
.Lsv_bad:
    mov rax, -1
    jmp .Lsv_ret
.Lsv_done:
    mov rax, r14
.Lsv_ret:
    add rsp, 16
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.section .note.GNU-stack,"",@progbits
