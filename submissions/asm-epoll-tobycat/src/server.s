# One epoll loop per CPU. Each owns its listeners, its private fd table and its Postgres connections, so nothing on
# the request path crosses threads. Reads pipeline into one Sync per batch; the writes of a pass share one Sync,
# so one transaction and one commit.
.include "defs.inc"
.set RING_N,1048576
.set R_METHOD,0
.set R_PATH,8
.set R_PATHLEN,16
.set R_QUERY,24
.set R_QLEN,32
.set R_AUTH,40
.set R_AUTHLEN,48
.set R_BODY,56
.set R_BODYLEN,64
.set R_CLOSE,72
.set R_TOTAL,104
.set R_FD,112
.set R_GZ,232
.set R_USER,256        # decoded token username
.set R_SIZE,1400
.set M_GET,1
.set M_POST,2
.set M_PUT,3
.set M_DEL,4
# ring entry
.set E_FD,0
.set E_GEN,4
.set E_KIND,8
.set E_GZ,9
.set E_LAST,10
.set E_RES,11          # like: 1 when its insert added the row
.set E_SOLO,12         # a write resent in a transaction of its own
.set E_DROP,13         # a read sent again elsewhere by pg_hedge: its reply here is discarded
.set E_EXTRA,16
.set E_A,24
.set KF,0              # feed
.set KP,1              # post
.set KC,2              # create
.set KL,3              # like
.set KH,4              # health
.set HEAD,512          # room for response headers before a body
.set TAG_CLIENT,0
.set TAG_LISTEN,1
.set TAG_PG,2
.set SYS_FCNTL,72
.set SYS_AFFINITY,204

.macro D2
  xor edx,edx
  mov ecx,10
  div ecx
  add al,'0'
  add dl,'0'
  mov [rdi],al
  mov [rdi+1],dl
  add rdi,2
.endm

.macro CUR_ENTRY reg   # the entry the next result belongs to: past the executed writes awaiting their commit
  mov \reg,[rbp+P_HEAD]
  add \reg,[rbp+P_NDONE]
  and \reg,RING_N-1
  shl \reg,5
  add \reg,[rbp+P_RING]
.endm

.macro HEAD_ENTRY reg
  mov \reg,[rbp+P_HEAD]
  and \reg,RING_N-1
  shl \reg,5
  add \reg,[rbp+P_RING]
.endm

.section .rodata
e_db: .asciz "DATABASE_URL"
e_jwt:.asciz "JWT_SECRET"
e_host:.asciz "HOST"
e_port:.asciz "PORT"
e_port2:.asciz "EXTRA_PORT"
e_thr:.asciz "THREADS"
e_gz:.asciz "HTTP_GZIP"
e_win:.asciz "BATCH_US"
e_pgc:.asciz "PG_CONNS"
e_hedge:.asciz "HEDGE_MS"
e_idle:.asciz "SCHED_IDLE"
e_rto:.asciz "RTO_MIN_US"
m_hedged:.ascii "hedged reads: "
m_env:.asciz "DATABASE_URL and JWT_SECRET are required"
m_listen:.asciz "cannot listen on HOST:PORT"
n_f:.asciz "f"
n_g:.asciz "g"
n_c:.asciz "c"
n_l:.asciz "l"
n_h:.asciz "h"
sql_f:.ascii "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id"
.asciz " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
sql_g:.asciz "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1"
sql_c:.asciz "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
sql_l:.asciz "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
sql_h:.asciz "SELECT 1"
k_body:.ascii "body"
.bss
.balign 8
g_addr: .skip 16
g_port1:.skip 8
g_port2:.skip 8
g_threads:.skip 8
g_gz:.skip 8
g_win_ns:.skip 8
g_conns:.skip 8
g_hedge_ns:.skip 8
g_idle:.skip 8
.globl g_rto_us
g_rto_us:.skip 8
g_start:.skip 8
.text

.globl _start
_start:
  mov rax,[rsp]
  lea rcx,[rsp+rax*8+16]
  mov [rip+g_envp],rcx
  and rsp,-16
  sub rsp,256
  # SIGPIPE ignored
  mov qword ptr [rsp],1
  xor eax,eax
  mov [rsp+8],rax
  mov [rsp+16],rax
  mov [rsp+24],rax
  mov edi,13
  mov rsi,rsp
  xor edx,edx
  mov r10d,8
  SYS SYS_RT_SIGACTION
  # open files up to the hard limit
  mov edi,7
  mov rsi,rsp
  SYS SYS_GETRLIMIT
  mov rax,[rsp+8]
  mov [rsp],rax
  mov edi,7
  mov rsi,rsp
  SYS SYS_SETRLIMIT
  lea rdi,[rip+e_db]
  call getenv
  test rax,rax
  jz .Lnoenv
  mov rdi,rax
  call pg_parse_url
  lea rdi,[rip+e_jwt]
  call getenv
  test rax,rax
  jz .Lnoenv
  mov rdi,rax
  call strlen
  mov rsi,rax
  call jwt_init
  call now_s
  mov [rip+g_start],rax
  # threads: the affinity mask's CPUs
  xor edi,edi
  mov esi,128
  mov rdx,rsp
  SYS SYS_AFFINITY
  xor ecx,ecx
  xor ebx,ebx
1:cmp rcx,rax
  jae 2f
  popcnt rdx,[rsp+rcx]
  add rbx,rdx
  add rcx,8
  jmp 1b
2:lea rdi,[rip+e_thr]
  mov rsi,rbx
  call env_int
  test rax,rax
  jnz 3f
  mov eax,1
3:mov [rip+g_threads],rax
  lea rdi,[rip+e_gz]
  mov esi,1
  call env_int
  mov [rip+g_gz],rax
  lea rdi,[rip+e_win]
  mov esi,2000
  call env_int
  imul rax,rax,1000
  mov [rip+g_win_ns],rax
  # Postgres connections per thread: 64 in all, at least 2 each
  mov eax,64
  xor edx,edx
  div qword ptr [rip+g_threads]
  cmp rax,2
  jae 4f
  mov eax,2
4:lea rdi,[rip+e_pgc]
  mov rsi,rax
  call env_int
  mov [rip+g_conns],rax
  lea rdi,[rip+e_hedge]
  mov esi,0              # off by default: a hedged read runs its statement twice (rule 3, see #99)
  call env_int
  imul rax,rax,1000000
  mov [rip+g_hedge_ns],rax
  lea rdi,[rip+e_idle]
  mov esi,1
  call env_int
  mov [rip+g_idle],rax
  lea rdi,[rip+e_rto]
  mov esi,30000
  call env_int
  mov [rip+g_rto_us],rax
  # listen address
  mov word ptr [rip+g_addr],2
  lea rdi,[rip+e_port]
  mov esi,3000
  call env_int
  xchg al,ah
  mov [rip+g_port1],rax
  lea rdi,[rip+e_port2]
  mov esi,0
  call env_int
  xchg al,ah
  mov [rip+g_port2],rax
  lea rdi,[rip+e_host]
  call getenv
  test rax,rax
  jz 9f
  mov rsi,rax
  xor r8d,r8d
7:xor eax,eax
8:movzx ecx,byte ptr [rsi]
  sub ecx,'0'
  cmp ecx,9
  ja 10f
  imul eax,eax,10
  add eax,ecx
  inc rsi
  jmp 8b
10:lea rdx,[rip+g_addr+4]
  mov [rdx+r8],al
  inc r8
  cmp byte ptr [rsi],'.'
  jne 9f
  inc rsi
  cmp r8,4
  jb 7b
9:mov r12,[rip+g_threads]
  xor ebx,ebx
5:cmp rbx,r12
  je .Lpark
  mov edi,W_SIZE
  call mmap_anon
  mov [rax+W_ID],rbx
  lea rdi,[rip+worker_main]
  mov rsi,rax
  call spawn
  inc rbx
  jmp 5b
.Lpark:
  mov qword ptr [rsp],86400
  mov qword ptr [rsp+8],0
  mov rdi,rsp
  xor esi,esi
  SYS SYS_NANOSLEEP
  jmp .Lpark
.Lnoenv:
  lea rdi,[rip+m_env]
  jmp die

# open_listener esi port (network order), edx tag -> nothing. r15 thread. Registers it.
open_listener:
  push rbx
  push r12
  sub rsp,32
  mov r12d,esi
  mov edi,2
  mov esi,0x80801        # STREAM|NONBLOCK|CLOEXEC
  xor edx,edx
  SYS SYS_SOCKET
  test rax,rax
  js .Lnolisten
  mov ebx,eax
  mov dword ptr [rsp],1
  .irp opt,2,15          # SO_REUSEADDR, SO_REUSEPORT
  mov edi,ebx
  mov esi,1
  mov edx,\opt
  mov r10,rsp
  mov r8d,4
  SYS SYS_SETSOCKOPT
  .endr
  # IPPROTO_TCP options: TCP_NODELAY and TCP_THIN_LINEAR_TIMEOUTS are inherited by accepted sockets (a reply
  # lost twice waits 400 ms instead of 600); with TCP_DEFER_ACCEPT a connection is accepted once its first
  # request is here, so do_accept serves it in the same pass instead of waking again for EPOLLIN
  .irp opt,1,16,9        # TCP_NODELAY, TCP_THIN_LINEAR_TIMEOUTS, TCP_DEFER_ACCEPT (1 s)
  mov edi,ebx
  mov esi,6
  mov edx,\opt
  mov r10,rsp
  mov r8d,4
  SYS SYS_SETSOCKOPT
  .endr
  # TCP_RTO_MIN_US (Linux 6.15+, inherited by accepted sockets): a lost reply is resent after ~RTT+30 ms
  # instead of the 200 ms minimum RTO; older kernels return ENOPROTOOPT and keep 200 ms. Not lower: clients
  # delay their ACKs, and at 10 ms a 600k-user run had 10x the retransmits and 43x the RTO timeouts
  mov rax,[rip+g_rto_us]
  test rax,rax
  jz 5f
  mov [rsp],eax
  mov edi,ebx
  mov esi,6
  mov edx,45
  mov r10,rsp
  mov r8d,4
  SYS SYS_SETSOCKOPT
5:mov rax,[rip+g_addr]
  mov [rsp],rax
  mov [rsp+2],r12w
  mov edi,ebx
  mov rsi,rsp
  mov edx,16
  SYS SYS_BIND
  test rax,rax
  js .Lnolisten
  mov edi,ebx
  mov esi,65535
  SYS SYS_LISTEN
  test rax,rax
  js .Lnolisten
  mov edi,ebx
  mov esi,1
  mov edx,1
  mov ecx,TAG_LISTEN
  call ep_ctl_tag
  add rsp,32
  pop r12
  pop rbx
  ret
.Lnolisten:
  lea rdi,[rip+m_listen]
  jmp die

# ep_ctl_tag edi fd (or index in data), esi op, edx events, ecx tag; for fds whose data differs use ep_ctl_pg.
ep_ctl_tag:
  sub rsp,16
  mov [rsp],edx
  mov [rsp+4],edi
  mov [rsp+8],ecx
  mov edx,edi
  mov edi,[r15+W_EPFD]
  mov r10,rsp
  SYS SYS_EPOLL_CTL
  add rsp,16
  ret
# ep_ctl_pg rbp conn, esi op, edx events
ep_ctl_pg:
  sub rsp,16
  mov [rsp],edx
  mov rax,[rbp+P_IDX]
  mov [rsp+4],eax
  mov dword ptr [rsp+8],TAG_PG
  mov rdx,[rbp+PG_FD]
  mov edi,[r15+W_EPFD]
  mov r10,rsp
  SYS SYS_EPOLL_CTL
  add rsp,16
  ret

.globl worker_main
worker_main:
  mov r15,rdi
  mov edi,0x400          # CLONE_FILES: a private fd table for this loop
  SYS SYS_UNSHARE
  # SCHED_IDLE: the box's NIC has one queue, so under load receive work moves to ksoftirqd, which would share a
  # core equally with a busy loop and drop packets meanwhile; in the lowest class this thread lets it run first.
  # The thread's own policy only: no affinity, nothing raised.
  cmp qword ptr [rip+g_idle],0
  je 1f
  sub rsp,16
  mov qword ptr [rsp],0  # sched_priority 0
  xor edi,edi
  mov esi,5              # SCHED_IDLE
  mov rdx,rsp
  SYS SYS_SCHED_SETSCHED
  add rsp,16
1:
  mov edi,0x80000
  SYS SYS_EPOLL_CREATE1
  mov [r15+W_EPFD],eax
  mov edi,RBUF_SIZE+64
  call mmap_anon
  mov [r15+W_RBUF],rax
  mov rdi,C_SIZE*MAX_FDS
  call mmap_anon
  mov [r15+W_CONNS],rax
  mov edi,0x400000
  call mmap_anon
  mov [r15+T_GZBUF],rax
  mov edi,0x10000
  call mmap_anon
  mov [r15+T_OUT],rax
  sub rsp,12*1024+R_SIZE+64
  lea r14,[rsp+12*1024]
  # Postgres: a quarter of the connections write, at least one
  mov rbx,[rip+g_conns]
  mov [r15+T_NP],rbx
  mov rax,rbx
  shr rax,2
  jnz 1f
  mov eax,1
1:mov rcx,rbx
  sub rcx,rax
  mov [r15+T_NR],rcx
  mov [r15+T_WNEXT],rcx
  mov qword ptr [r15+T_CURR],0
  mov qword ptr [r15+T_WCUR],0
  lea rdi,[rbx*8]
  call mmap_anon
  mov [r15+T_P],rax
  xor r12d,r12d
2:cmp r12,rbx
  je 3f
  call new_ctx
  mov rbp,rax
  mov [rbp+P_IDX],r12
  mov rax,[r15+T_P]
  mov [rax+r12*8],rbp
  call pg_open
  inc r12
  jmp 2b
3:mov rsi,[rip+g_port1]
  call open_listener
  mov rsi,[rip+g_port2]
  test esi,esi
  jz .Lwloop
  call open_listener
.Lwloop:
  mov edi,[r15+W_EPFD]
  mov rsi,rsp
  mov edx,1024
  mov r10,-1
  cmp qword ptr [r15+T_NDOWN],0
  je 91f
  mov r10d,1000          # a connection is down: wake to retry it
91:SYS SYS_EPOLL_WAIT
  mov r12,rax
  call now_ns
  mov [r15+W_T0NS],rax
  call now_s
  mov [r15+W_NOW_S],rax
  test r12,r12
  jg 92f
  call pg_flush_all
  jmp .Lwloop
92:
  xor r13d,r13d
.Lwev:
  lea rax,[r13+r13*2]
  mov ebx,[rsp+rax*4]    # events
  mov ebp,[rsp+rax*4+4]  # fd, or conn index
  mov ecx,[rsp+rax*4+8]  # tag
  test ecx,ecx
  jnz .Lwspecial
  test ebx,4
  jz 1f
  mov edi,ebp
  call conn_flush
1:test ebx,0x2019       # IN|ERR|HUP|RDHUP
  jz .Lwnext
  mov edi,ebp
  call conn_read
.Lwnext:
  inc r13
  cmp r13,r12
  jne .Lwev
  # every batch built in this pass goes out now
  call pg_flush_all
  # fixed cadence: a pass lasts at least BATCH_US from epoll_wait's return, so whatever arrives meanwhile is
  # served by the next pass (one wake-up, one Sync per batch); only a pass that filled the event array goes on
  cmp r12,1024
  jae .Lwloop
  mov rbx,[rip+g_win_ns]
  test rbx,rbx
  jz .Lwloop
  call now_ns
  sub rax,[r15+W_T0NS]
  sub rbx,rax            # what is left of the window
  cmp rbx,20000
  jl .Lwloop
  mov rax,rbx
  xor edx,edx
  mov ecx,1000000000
  div rcx
  sub rsp,16
  mov [rsp],rax
  mov [rsp+8],rdx
  mov rdi,rsp
  xor esi,esi
  SYS SYS_NANOSLEEP
  add rsp,16
  jmp .Lwloop
.Lwspecial:
  cmp ecx,TAG_LISTEN
  jne 2f
  mov [r15+W_LISTEN],ebp
  call do_accept
  jmp .Lwnext
2:mov rax,[r15+T_P]
  mov rbp,[rax+rbp*8]
  mov rax,[rbp+P_DEAD]
  or rax,[rbp+P_DOWN]
  jnz .Lwnext
  test ebx,4
  jz 3f
  mov qword ptr [rbp+P_WOUT],0
  mov esi,3
  mov edx,1
  call ep_ctl_pg
  call pg_write
3:test ebx,0x19        # IN|ERR|HUP
  jz .Lwnext
  call pg_read
  jmp .Lwnext

# ---- sending to Postgres. rbp conn.
pg_flush_all:
  push rbp
  push rbx
  xor ebx,ebx
1:cmp rbx,[r15+T_NP]
  je 3f
  mov rax,[r15+T_P]
  mov rbp,[rax+rbx*8]
  cmp qword ptr [rbp+P_DOWN],0
  jne 21f
  cmp qword ptr [rbp+P_DEAD],0
  je 11f
  # answering its requests may queue new ones elsewhere, so go round again
  call pg_fail
  xor ebx,ebx
  jmp 1b
11:cmp qword ptr [rbp+P_BATCH],0
  je 2f
  # a write connection with a transaction in flight holds its batch: it goes at that ReadyForQuery, so under load
  # a batch grows with everything that arrived during the previous commit
  cmp qword ptr [rbp+P_INFL],0
  je 12f
  cmp rbx,[r15+T_NR]
  jae 2f
12:call pg_close_batch
2:call pg_write
  cmp qword ptr [rbp+P_DEAD],0
  jne 1b
21:inc rbx
  jmp 1b
3:mov qword ptr [r15+T_CURR],0
  mov qword ptr [r15+T_WCUR],0
  call pg_hedge
  cmp qword ptr [r15+T_NDOWN],0
  je 4f
  call pg_retry
4:pop rbx
  pop rbp
  ret

# ---- hedged reads. A lost packet on a Postgres connection waits for TCP's retransmit (200 ms at least), and a
# read batch shares its connection's round trip, so one loss stalls up to 64 requests. A read connection that owes
# replies and has received nothing for HEDGE_MS, while another read connection of this thread has received since,
# has its reads sent again on the least-loaded other read connection. The copies answer; the originals stay in its
# ring marked E_DROP and their late replies are discarded. It gets no new reads until it receives again. Only
# reads (SPEC's /feed, /posts/:id and /health statements) are ever sent twice; nothing is kept between requests.
pg_hedge:
  mov r11,[rip+g_hedge_ns]
  test r11,r11
  jz 9f
  push rbx
  push rbp
  push r12
  push r13
  mov r13,[r15+W_T0NS]
  mov r12,[r15+T_P]
  # wait at least 4x the usual read latency: when Postgres itself is slow every connection goes quiet for a while,
  # and sending its reads twice would only add to the load
  mov rax,[r15+T_RLAT]
  shl rax,2
  cmp rax,r11
  jbe 0f
  mov r11,rax
0:# the latest receive on any live read connection, and how many are stalled already: at most a quarter (at least
  # one), so a slow Postgres can't have every read sent twice
  xor r8d,r8d
  xor r9d,r9d
  xor ebx,ebx
1:cmp rbx,[r15+T_NR]
  je 2f
  mov rax,[r12+rbx*8]
  inc rbx
  mov rdx,[rax+P_DEAD]
  or rdx,[rax+P_DOWN]
  jnz 1b
  add r9,[rax+P_STALL]
  mov rdx,[rax+P_RXT]
  cmp rdx,r8
  jbe 1b
  mov r8,rdx
  jmp 1b
2:mov rax,[r15+T_NR]
  shr rax,2
  jnz 21f
  mov eax,1
21:cmp r9,rax
  jae 8f
  mov rax,r13
  sub rax,r8
  cmp rax,r11
  jae 8f                 # nothing heard lately: Postgres is slow or idle, not one connection stalled
  xor ebx,ebx
3:cmp rbx,[r15+T_NR]
  je 8f
  mov rbp,[r12+rbx*8]
  inc rbx
  mov rax,[rbp+P_DEAD]
  or rax,[rbp+P_DOWN]
  or rax,[rbp+P_STALL]
  jnz 3b
  mov rax,[rbp+P_TAIL]
  cmp rax,[rbp+P_HEAD]
  je 3b
  mov rax,[rbp+P_WAIT]
  cmp r8,rax
  jbe 3b                 # no other connection has received since it started waiting
  mov rdx,r13
  sub rdx,rax
  cmp rdx,r11
  jb 3b
  call hedge_conn
8:# report the count every 10 s while it changes
  mov rax,[r15+T_HEDGES]
  cmp rax,[r15+T_HLOGGED]
  je 81f
  mov rdx,[r15+W_NOW_S]
  mov rcx,rdx
  sub rcx,[r15+T_HLAST]
  cmp rcx,10
  jb 81f
  mov [r15+T_HLAST],rdx
  mov [r15+T_HLOGGED],rax
  mov rdi,[r15+T_OUT]
  lea rsi,[rip+m_hedged]
  mov ecx,14
  rep movsb
  mov rsi,rax
  call put_u64
  mov byte ptr [rdi],10
  inc rdi
  mov rdx,rdi
  mov rsi,[r15+T_OUT]
  sub rdx,rsi
  mov edi,2
  SYS SYS_WRITE
81:pop r13
  pop r12
  pop rbp
  pop rbx
9:ret

# hedge_conn rbp stalled read connection: its owed reads go again on the least-loaded other live read connection.
hedge_conn:
  push rbx
  push r12
  push r13
  push r14
  mov r12,rbp
  xor r13d,r13d
  mov r9,-1
  xor ecx,ecx
  mov r8,[r15+T_P]
1:cmp rcx,[r15+T_NR]
  je 2f
  mov rax,[r8+rcx*8]
  inc rcx
  cmp rax,r12
  je 1b
  mov rdx,[rax+P_DEAD]
  or rdx,[rax+P_DOWN]
  or rdx,[rax+P_STALL]
  jnz 1b
  mov rdx,[rax+P_TAIL]
  sub rdx,[rax+P_HEAD]
  cmp rdx,r9
  jae 1b
  mov r9,rdx
  mov r13,rax
  jmp 1b
2:test r13,r13
  jz 9f
  mov qword ptr [r12+P_STALL],1
  mov rbx,[r12+P_HEAD]
  mov rbp,r13
  xor r9d,r9d            # copies
3:cmp rbx,[r12+P_TAIL]
  jae 7f
  mov r14,rbx
  and r14,RING_N-1
  shl r14,5
  add r14,[r12+P_RING]
  inc rbx
  cmp byte ptr [r14+E_DROP],0
  jne 3b
  cmp r9,128
  jae 7f                 # at most two batches: what a loss holds up, not a whole backlog
  call has_room
  jz 7f
  mov rax,[rbp+P_TAIL]
  cmp rax,[rbp+P_HEAD]
  jne 4f
  mov rdx,[r15+W_T0NS]
  mov [rbp+P_WAIT],rdx
4:movzx esi,byte ptr [r14+E_KIND]
  mov rdx,[r14+E_A]
  call bind_read
  mov rax,[rbp+P_TAIL]
  and rax,RING_N-1
  shl rax,5
  add rax,[rbp+P_RING]
  movdqu xmm0,[r14]
  movdqu xmm1,[r14+16]
  movdqu [rax],xmm0
  movdqu [rax+16],xmm1
  mov byte ptr [rax+E_LAST],0
  inc qword ptr [rbp+P_TAIL]
  mov byte ptr [r14+E_DROP],1
  inc r9
  inc qword ptr [r15+T_HEDGES]
  inc qword ptr [rbp+P_BATCH]
  cmp qword ptr [rbp+P_BATCH],64
  jb 3b
  call pg_close_batch
  jmp 3b
7:cmp qword ptr [rbp+P_BATCH],0
  je 8f
  call pg_close_batch
8:call pg_write
9:mov rbp,r12
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

# pg_fail rbp: the connection is gone. Everything queued on it gets a 500.
pg_fail:
  mov rdi,[rbp+PG_FD]
  SYS SYS_CLOSE
  xor eax,eax
  .irp f,P_DEAD,P_SLEN,P_SOFF,P_BATCH,P_SKIP,P_WOUT,P_NROW,PG_END,PG_POS,P_NDONE,P_INFL,P_STALL
  mov [rbp+\f],rax
  .endr
  cmp [r15+T_WCUR],rbp
  jne 4f
  mov [r15+T_WCUR],rax
4:  mov qword ptr [rbp+P_DOWN],1
  inc qword ptr [r15+T_NDOWN]
  mov rax,[r15+W_NOW_S]
  inc rax
  mov [r15+T_NEXTTRY],rax
  cmp [r15+T_CURR],rbp
  jne 1f
  mov qword ptr [r15+T_CURR],0
1:mov rax,[rbp+P_HEAD]
  cmp rax,[rbp+P_TAIL]
  jae 3f
  HEAD_ENTRY rax
  cmp byte ptr [rax+E_KIND],KC
  jne 2f
  mov rdi,[rax+E_EXTRA]
  call free
2:call answer_500
  jmp 1b
3:ret

# pg_retry: one try per second per thread; stops at the first failure, since a dead database can take 3 s to say so.
pg_retry:
  mov rax,[r15+W_NOW_S]
  cmp rax,[r15+T_NEXTTRY]
  jb 9f
  inc rax
  mov [r15+T_NEXTTRY],rax
  push rbx
  xor ebx,ebx
1:cmp rbx,[r15+T_NP]
  je 8f
  mov rax,[r15+T_P]
  mov rbp,[rax+rbx*8]
  inc rbx
  cmp qword ptr [rbp+P_DOWN],0
  je 1b
  call pg_try
  test eax,eax
  jz 8f
  mov qword ptr [rbp+P_DOWN],0
  dec qword ptr [r15+T_NDOWN]
  jmp 1b
8:pop rbx
9:ret

# pg_try rbp -> eax 1 connected, 0 not.
pg_try:
  push rbx
  push r12
  push r13
  push r14
  mov qword ptr [rbp+PG_FD],-1
  mov [rbp+P_RECOV],rsp
  call pg_open
  mov qword ptr [rbp+P_RECOV],0
  mov eax,1
  jmp 1f
.globl pg_try_fail
pg_try_fail:
  mov qword ptr [rbp+P_RECOV],0
  mov rdi,[rbp+PG_FD]
  test rdi,rdi
  js 2f
  SYS SYS_CLOSE
2:xor eax,eax
1:pop r14
  pop r13
  pop r12
  pop rbx
  ret

# pg_open rbp: connect, log in, prepare, go non-blocking, join the epoll set.
pg_open:
  xor eax,eax
  mov [rbp+P_SLEN],rax
  call pg_connect
  .irp s,f,g,c,l,h
  lea rdi,[rip+n_\s]
  lea rsi,[rip+sql_\s]
  call pg_prepare
  .endr
  xor eax,eax
  mov [rbp+PG_END],rax
  mov [rbp+PG_POS],rax
  mov rdi,[rbp+PG_FD]
  mov esi,4              # F_SETFL
  mov edx,0x800          # O_NONBLOCK
  SYS SYS_FCNTL
  mov esi,1
  mov edx,1
  jmp ep_ctl_pg

# The batch ends here: Sync, and its last request is marked so an error can skip to it.
pg_close_batch:
  mov rdi,[rbp+PG_SEND]
  mov rax,[rbp+P_SLEN]
  add rax,rdi
  mov dword ptr [rax],0x00000053
  mov dword ptr [rax+1],0x04000000
  add qword ptr [rbp+P_SLEN],5
  mov rax,[rbp+P_TAIL]
  dec rax
  and rax,RING_N-1
  shl rax,5
  add rax,[rbp+P_RING]
  mov byte ptr [rax+E_LAST],1
  mov qword ptr [rbp+P_BATCH],0
  inc qword ptr [rbp+P_INFL]
  ret

pg_write:
  mov rax,[rbp+P_WOUT]
  or rax,[rbp+P_DEAD]
  or rax,[rbp+P_DOWN]
  jnz 9f
1:mov rdx,[rbp+P_SLEN]
  mov rax,[rbp+P_SOFF]
  sub rdx,rax
  jz 8f
  mov rsi,[rbp+PG_SEND]
  add rsi,rax
  mov rdi,[rbp+PG_FD]
  SYS_SEND
  cmp rax,-4
  je 1b
  cmp rax,-11
  je 7f
  test rax,rax
  js pg_gone
  add [rbp+P_SOFF],rax
  jmp 1b
7:mov qword ptr [rbp+P_WOUT],1
  mov esi,3
  mov edx,5              # IN|OUT
  jmp ep_ctl_pg
8:xor eax,eax
  mov [rbp+P_SLEN],rax
  mov [rbp+P_SOFF],rax
9:ret
# Requests may be mid-route here, so this only marks it; pg_flush_all answers them.
pg_gone:
  mov qword ptr [rbp+P_DEAD],1
  ret

# pick_reader -> rbp the connection to add reads to, or rbp 0 when everything is full. Passes over a connection
# whose reads pg_hedge moved away, unless every live one is like that.
pick_reader:
  mov rbp,[r15+T_CURR]
  test rbp,rbp
  jnz 5f
  push rbx
  mov ebx,1
0:mov r8,-1
  xor ecx,ecx
  mov r9,[r15+T_P]
1:cmp rcx,[r15+T_NR]
  je 3f
  mov rax,[r9+rcx*8]
  mov rdx,[rax+P_DEAD]
  or rdx,[rax+P_DOWN]
  jnz 2f
  test ebx,ebx
  jz 11f
  cmp qword ptr [rax+P_STALL],0
  jne 2f
11:mov rdx,[rax+P_TAIL]
  sub rdx,[rax+P_HEAD]
  cmp rdx,r8
  jae 2f
  mov r8,rdx
  mov rbp,rax
2:inc rcx
  jmp 1b
3:test rbp,rbp
  jnz 4f
  test ebx,ebx
  jz 4f
  xor ebx,ebx
  jmp 0b
4:pop rbx
  mov [r15+T_CURR],rbp
5:ret

# has_room rbp -> ZF clear when the connection is up and both its ring and send buffer can take a request.
has_room:
  test rbp,rbp
  jz 1f
  mov rax,[rbp+P_DEAD]
  or rax,[rbp+P_DOWN]
  jnz 1f
  mov rax,[rbp+P_TAIL]
  sub rax,[rbp+P_HEAD]
  cmp rax,RING_N-2
  jae 1f
  mov rax,[rbp+P_SLEN]
  cmp rax,SEND_CAP-0x10000
  jae 1f
  mov eax,1
  test eax,eax
  ret
1:xor eax,eax
  ret

# ring_push rbp, esi kind, rdi extra, rdx a. Marks the connection busy.
ring_push:
  mov rax,[rbp+P_TAIL]
  cmp rax,[rbp+P_HEAD]
  jne 1f
  mov rcx,[r15+W_T0NS]   # owed replies wait from now
  mov [rbp+P_WAIT],rcx
1:and rax,RING_N-1
  shl rax,5
  add rax,[rbp+P_RING]
  mov [rax+E_EXTRA],rdi
  mov [rax+E_A],rdx
  mov [rax+E_KIND],sil
  mov rcx,[r14+R_GZ]
  mov [rax+E_GZ],cl
  mov byte ptr [rax+E_LAST],0
  mov byte ptr [rax+E_SOLO],0
  mov byte ptr [rax+E_DROP],0
  mov rcx,[r14+R_FD]
  mov [rax+E_FD],ecx
  shl rcx,6
  add rcx,[r15+W_CONNS]
  mov edx,[rcx+C_GEN]
  mov [rax+E_GEN],edx
  mov dword ptr [rcx+C_BUSY],1
  inc qword ptr [rbp+P_TAIL]
  ret

# Bind + Execute. rbp conn, rdi statement name byte, then at the parameter section.
# bind_start al name -> rdi at nparams; [rsp] on return is unaffected, length is patched by bind_end.
.macro BIND_START name
  mov rdi,[rbp+PG_SEND]
  add rdi,[rbp+P_SLEN]
  mov r10,rdi
  mov byte ptr [rdi],'B'
  mov byte ptr [rdi+5],0
  mov word ptr [rdi+6],\name
  mov dword ptr [rdi+8],0x01000100   # one format code: binary
  add rdi,12
.endm
.macro BIND_END
  mov dword ptr [rdi],0x01000100     # one result format code: binary
  add rdi,4
  mov rax,rdi
  sub rax,r10
  dec eax
  bswap eax
  mov [r10+1],eax
  mov dword ptr [rdi],0x00000045     # 'E' len 9, portal "", no row limit
  mov dword ptr [rdi+1],0x09000000
  mov byte ptr [rdi+5],0
  mov dword ptr [rdi+6],0
  add rdi,10
  sub rdi,[rbp+PG_SEND]
  mov [rbp+P_SLEN],rdi
.endm

# bind_read rbp, esi kind, rdx post id (KP): Bind + Execute of a read. Clobbers rax, rdi, r10.
bind_read:
  cmp esi,KF
  jne 1f
  BIND_START 'f'
  mov word ptr [rdi],0
  add rdi,2
  jmp 3f
1:cmp esi,KH
  jne 2f
  BIND_START 'h'
  mov word ptr [rdi],0
  add rdi,2
  jmp 3f
2:BIND_START 'g'
  mov word ptr [rdi],0x0100
  mov dword ptr [rdi+2],0x08000000
  mov rax,rdx
  bswap rax
  mov [rdi+6],rax
  add rdi,14
3:BIND_END
  ret

# q_read esi kind, rdx post id (KP) -> reads are batched; the next pass's flush or a full batch sends them.
q_read:
  push rbp
  push rbx
  push r12
  mov ebx,esi
  mov r12,rdx
  call pick_reader
  call has_room
  jz .Lq_full
  mov esi,ebx
  mov rdx,r12
  call bind_read
  mov esi,ebx
  mov rdi,[r15+W_T0NS]   # pushed at, for the latency average
  mov rdx,r12            # the post id, for a hedge
  call ring_push
  inc qword ptr [rbp+P_BATCH]
  cmp qword ptr [rbp+P_BATCH],64
  jb 9f
  call pg_close_batch
  call pg_write
  mov qword ptr [r15+T_CURR],0
9:pop r12
  pop rbx
  pop rbp
  ret
.Lq_full:
  cmp ebx,KH
  pop r12
  pop rbx
  pop rbp
  jne reply_503
  # /health with no usable database connection
  mov rax,[r15+T_OUT]
  lea rdi,[rax+HEAD]
  LIT "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}"
  mov rax,[r15+T_OUT]
  lea rdx,[rax+HEAD]
  mov rcx,rdi
  sub rcx,rdx
  mov rdi,[r14+R_FD]
  mov esi,503
  xor r8d,r8d
  jmp http_send

# pick_writer -> rbp: the next live write connection in turn, passing over one that owes replies and has heard
# nothing for HEDGE_MS (a lost packet: its writes wait for the retransmit, new ones need not); if every live one
# is like that, the next live one.
pick_writer:
  push rbx
  mov rbx,[rip+g_hedge_ns]
0:mov rdx,[r15+T_NP]
  sub rdx,[r15+T_NR]     # tries
  mov rax,[r15+T_WNEXT]
  mov rcx,[r15+T_P]
1:mov rbp,[rcx+rax*8]
  inc rax
  cmp rax,[r15+T_NP]
  jb 2f
  mov rax,[r15+T_NR]
2:mov r8,[rbp+P_DEAD]
  or r8,[rbp+P_DOWN]
  jnz 4f
  test rbx,rbx
  jz 3f
  mov r8,[rbp+P_TAIL]
  cmp r8,[rbp+P_HEAD]
  je 3f
  mov r8,[r15+W_T0NS]
  sub r8,[rbp+P_WAIT]
  cmp r8,rbx
  jb 3f
4:dec rdx
  jnz 1b
  test rbx,rbx
  jz 3f
  xor ebx,ebx            # all stalled or down: take the next live one
  jmp 0b
3:mov [r15+T_WNEXT],rax
  pop rbx
  ret

# pick_wbatch -> rbp: this pass's write connection. Its writes share one Sync, so one transaction and one commit.
pick_wbatch:
  mov rbp,[r15+T_WCUR]
  test rbp,rbp
  jz 1f
  mov rax,[rbp+P_DEAD]
  or rax,[rbp+P_DOWN]
  jnz 1f
  ret
1:call pick_writer
  mov [r15+T_WCUR],rbp
  ret

# wbatch_add rbp: a write joined the open batch; at 256 the batch ends and goes out.
wbatch_add:
  inc qword ptr [rbp+P_BATCH]
  cmp qword ptr [rbp+P_BATCH],256
  jb 9f
  call pg_close_batch
  call pg_write
  mov qword ptr [r15+T_WCUR],0
9:ret

# bind_like rbp, rsi user id, rdx post id: Bind + Execute of "l".
bind_like:
  BIND_START 'l'
  mov word ptr [rdi],0x0200
  mov dword ptr [rdi+2],0x08000000
  mov rax,rsi
  bswap rax
  mov [rdi+6],rax
  mov dword ptr [rdi+14],0x08000000
  mov rax,rdx
  bswap rax
  mov [rdi+18],rax
  add rdi,26
  BIND_END
  ret

# bind_create rbp, rsi extra: Bind + Execute of "c", user id and body from the extra.
bind_create:
  BIND_START 'c'
  mov word ptr [rdi],0x0200
  mov dword ptr [rdi+2],0x08000000
  mov rax,[rsi+16]
  bswap rax
  mov [rdi+6],rax
  add rdi,14
  mov rcx,[rsi+8]
  mov eax,ecx
  bswap eax
  mov [rdi],eax
  add rdi,4
  mov rax,[rsi]
  lea rsi,[rsi+rax+48]
  push r10
  call mcpy
  pop r10
  BIND_END
  ret

# ---- routing. r14 frame.
.macro STATIC_REPLY status,text:vararg
  .pushsection .rodata
  773: .ascii \text
  774:
  .popsection
  mov esi,\status
  lea rdx,[rip+773b]
  mov ecx,offset 774b-773b
  jmp reply_err
.endm
reply_404: STATIC_REPLY 404,"not found"
reply_id:  STATIC_REPLY 400,"invalid post id"
reply_nobearer: STATIC_REPLY 401,"missing bearer token"
reply_badtok: STATIC_REPLY 401,"invalid or expired token"
reply_payload: STATIC_REPLY 401,"invalid token payload"
reply_badjson: STATIC_REPLY 400,"malformed JSON body"
reply_nobody: STATIC_REPLY 400,"body is required"
reply_long: STATIC_REPLY 400,"body must be at most 500 characters"
reply_503: STATIC_REPLY 503,"overloaded"
reply_malformed: STATIC_REPLY 400,"malformed JSON body"

route:
  mov rdi,[r14+R_PATH]
  mov rcx,[r14+R_PATHLEN]
  mov rax,[r14+R_METHOD]
  cmp rax,M_GET
  jne .Lr_post
  cmp rcx,5
  jne 1f
  cmp dword ptr [rdi],0x6565662f   # /fee
  jne reply_404
  cmp byte ptr [rdi+4],'d'
  jne reply_404
  mov esi,KF
  jmp q_read
1:cmp rcx,7
  jne 2f
  mov rax,0x00ffffffffffffff
  and rax,[rdi]
  mov rdx,0x0068746c6165682f       # /health
  cmp rax,rdx
  jne 2f
  mov esi,KH
  jmp q_read
2:cmp rcx,7
  jb reply_404
  mov rax,0x00ffffffffffffff
  and rax,[rdi]
  mov rdx,0x002f7374736f702f       # /posts/
  cmp rax,rdx
  jne reply_404
  add rdi,7
  lea rsi,[rcx-7]
  call parse_id
  test rax,rax
  jz reply_id
  mov rdx,rax
  mov esi,KP
  jmp q_read
.Lr_post:
  cmp rax,M_POST
  jne reply_404
  cmp rcx,6
  jne 1f
  cmp dword ptr [rdi],0x736f702f   # /pos
  jne reply_404
  cmp word ptr [rdi+4],0x7374      # ts
  jne reply_404
  jmp create_post
1:cmp rcx,12
  jb reply_404
  mov rax,0x00ffffffffffffff
  and rax,[rdi]
  mov rdx,0x002f7374736f702f       # /posts/
  cmp rax,rdx
  jne reply_404
  lea rdx,[rdi+rcx-5]
  cmp dword ptr [rdx],0x6b696c2f   # /lik
  jne reply_404
  cmp byte ptr [rdx+4],'e'
  jne reply_404
  jmp like_post

# parse_id rdi, rsi len -> rax id, or 0 when it is not a positive integer.
parse_id:
  call parse_u64
  test rdx,rdx
  jnz 1f
  xor eax,eax
1:ret

# check_auth -> rax user id, rdx username length; on failure answers and returns rax 0.
check_auth:
  mov rax,[r14+R_AUTH]
  test rax,rax
  jz 8f
  mov rcx,[r14+R_AUTHLEN]
  cmp rcx,7
  jb 8f
  mov rdx,0x00ffffffffffffff
  and rdx,[rax]
  mov rsi,0x0020726572616542       # "Bearer "
  cmp rdx,rsi
  jne 8f
  lea rdi,[rax+7]
  lea rsi,[rcx-7]
  mov rdx,[r15+W_NOW_S]
  lea rcx,[r14+R_USER]
  call jwt_verify
  test rax,rax
  jz 7f
  js 6f
  ret
6:call reply_payload
  xor eax,eax
  ret
7:call reply_badtok
  xor eax,eax
  ret
8:call reply_nobearer
  xor eax,eax
  ret

like_post:
  push rbx
  push rbp
  call check_auth
  test rax,rax
  jz 9f
  mov rbx,rax
  mov rdi,[r14+R_PATH]
  add rdi,7
  mov rsi,[r14+R_PATHLEN]
  sub rsi,12
  call parse_id
  test rax,rax
  jnz 1f
  call reply_id
  jmp 9f
1:push rax
  call pick_wbatch
  call has_room
  pop rdx
  jnz 2f
  call reply_503
  jmp 9f
2:mov rsi,rbx
  push rdx
  call bind_like
  pop rdx
  mov esi,KL
  mov rdi,rbx            # the user id, kept for a resend
  call ring_push
  call wbatch_add
9:pop rbp
  pop rbx
  ret

# create_post: auth, then the body. The body and username stay in an allocation until the insert answers.
# extra: [0] username length, [8] body length, [16] user id, [24] id and created_at as Postgres sent them
# (length + value each, filled at CommandComplete), [48..] username then body.
create_post:
  push rbx
  push rbp
  push r12
  push r13
  call check_auth
  test rax,rax
  jz .Lcp_end
  mov rbx,rax            # user id
  mov r12,rdx            # username length
  mov rdi,[r14+R_BODY]
  mov rsi,[r14+R_BODYLEN]
  add rsi,rdi
  push rsi
  call json_valid
  pop rsi
  test rax,rax
  jnz 1f
  call reply_badjson
  jmp .Lcp_end
1:mov rdi,[r14+R_BODY]
  lea rdx,[rip+k_body]
  mov ecx,4
  call json_field
  test rax,rax
  jz .Lcp_nobody
  cmp byte ptr [rax],'"'
  jne .Lcp_nobody
  push rax
  push rdx
  mov rax,rdx
  lea rdi,[r12+rax+96]
  sub rdi,[rsp+8]
  call alloc
  mov r13,rax
  pop rdx
  pop rsi
  mov [r13],r12
  mov [r13+16],rbx
  lea rdi,[r13+48]
  push rsi
  push rdx
  lea rsi,[r14+R_USER]
  mov rcx,r12
  call mcpy
  pop rdx
  pop rsi
  inc rsi
  dec rdx
  push rdi
  call json_unescape
  pop rsi                # start of the decoded body
  test eax,eax
  jz .Lcp_badesc
  # rsi..rdi: trim as JavaScript's String.trim does
.Lcp_l:
  cmp rsi,rdi
  jae .Lcp_empty
  call ws_head
  add rsi,rax
  test rax,rax
  jnz .Lcp_l
.Lcp_r:
  call ws_tail
  sub rdi,rax
  test rax,rax
  jz 8f
  cmp rdi,rsi
  ja .Lcp_r
  jmp .Lcp_empty
8:# length in characters; a code point above U+FFFF counts as two UTF-16 units for JS but one here (char_length)
  mov rcx,rdi
  sub rcx,rsi
  mov [r13+8],rcx
  xor edx,edx
  mov r8,rsi
9:cmp r8,rdi
  jae 10f
  movzx eax,byte ptr [r8]
  and eax,0xc0
  cmp eax,0x80
  je 11f
  inc rdx
11:inc r8
  jmp 9b
10:cmp rdx,500
  ja .Lcp_long
  # move the trimmed body up to right after the username
  lea r9,[r13+r12+48]
  push rdi
  mov rdi,r9
  rep movsb
  pop rdi
  call pick_wbatch
  call has_room
  jz .Lcp_503
  mov rsi,r13
  call bind_create
  mov esi,KC
  mov rdi,r13
  xor edx,edx
  call ring_push
  call wbatch_add
.Lcp_end:
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret
.Lcp_nobody:
  call reply_nobody
  jmp .Lcp_end
.Lcp_badesc:
  mov rdi,r13
  call free
  call reply_badjson
  jmp .Lcp_end
.Lcp_empty:
  mov rdi,r13
  call free
  call reply_nobody
  jmp .Lcp_end
.Lcp_long:
  mov rdi,r13
  call free
  call reply_long
  jmp .Lcp_end
.Lcp_503:
  mov rdi,r13
  call free
  call reply_503
  jmp .Lcp_end

# is_ws3 eax three UTF-8 bytes, first in the low byte -> ZF set for a three-byte JavaScript whitespace character.
is_ws3:
  and eax,0xffffff
  cmp eax,0x809ae1       # U+1680
  je 8f
  cmp eax,0x8080e3       # U+3000
  je 8f
  cmp eax,0xbfbbef       # U+FEFF
  je 8f
  cmp eax,0x9f81e2       # U+205F
  je 8f
  mov ecx,eax
  cmp cx,0x80e2
  jne 7f
  shr ecx,16
  cmp ecx,0xa8           # U+2028
  je 8f
  cmp ecx,0xa9           # U+2029
  je 8f
  cmp ecx,0xaf           # U+202F
  je 8f
  sub ecx,0x80           # U+2000..U+200A
  cmp ecx,0x0a
  ja 7f
  xor ecx,ecx
8:ret
7:test esp,esp           # the stack pointer is never zero: clears ZF
  ret

# ws_head rsi start, rdi end (start < end) -> rax bytes of whitespace at the start, 0 if none.
ws_head:
  movzx eax,byte ptr [rsi]
  cmp al,' '
  je 1f
  sub eax,9
  cmp eax,4
  jbe 1f
  mov rdx,rdi
  sub rdx,rsi
  cmp rdx,2
  jb 9f
  cmp word ptr [rsi],0xa0c2   # U+00A0
  je 2f
  cmp rdx,3
  jb 9f
  mov eax,[rsi]
  call is_ws3
  je 3f
9:xor eax,eax
  ret
1:mov eax,1
  ret
2:mov eax,2
  ret
3:mov eax,3
  ret

# ws_tail rsi start, rdi end (start < end) -> rax bytes of whitespace at the end, 0 if none.
ws_tail:
  movzx eax,byte ptr [rdi-1]
  cmp al,' '
  je 1f
  sub eax,9
  cmp eax,4
  jbe 1f
  mov rdx,rdi
  sub rdx,rsi
  cmp rdx,2
  jb 9f
  cmp word ptr [rdi-2],0xa0c2
  je 2f
  cmp rdx,3
  jb 9f
  movzx eax,word ptr [rdi-3]
  movzx ecx,byte ptr [rdi-1]
  shl ecx,16
  or eax,ecx
  call is_ws3
  je 3f
9:xor eax,eax
  ret
1:mov eax,1
  ret
2:mov eax,2
  ret
3:mov eax,3
  ret

# reply_err esi status, rdx message, ecx length: answers the current request directly.
reply_err:
  push rbx
  push r12
  mov ebx,esi
  mov r12,rdx
  mov rax,[r15+T_OUT]
  lea rdi,[rax+HEAD]
  push rcx
  LIT "{\"error\":\""
  pop rcx
  mov rsi,r12
  call mcpy
  mov dword ptr [rdi],0x007d22
  add rdi,2
  mov rax,[r15+T_OUT]
  lea rdx,[rax+HEAD]
  mov rcx,rdi
  sub rcx,rdx
  mov rdi,[r14+R_FD]
  mov esi,ebx
  xor r8d,r8d
  call http_send
  pop r12
  pop rbx
  ret

# http_send edi fd, esi status, rdx body (HEAD bytes of room before it), rcx length, r8d gzip-encoded.
http_send:
  push rbx
  push r12
  push r13
  push rbp
  sub rsp,256
  mov r12d,edi
  mov rbx,rdx
  mov r13,rcx
  mov ebp,r8d
  mov rdi,rsp
  cmp esi,200
  jne 1f
  LIT "HTTP/1.1 200 OK\r\n"
  jmp 9f
1:cmp esi,201
  jne 2f
  LIT "HTTP/1.1 201 Created\r\n"
  jmp 9f
2:cmp esi,400
  jne 3f
  LIT "HTTP/1.1 400 Bad Request\r\n"
  jmp 9f
3:cmp esi,401
  jne 4f
  LIT "HTTP/1.1 401 Unauthorized\r\n"
  jmp 9f
4:cmp esi,404
  jne 5f
  LIT "HTTP/1.1 404 Not Found\r\n"
  jmp 9f
5:cmp esi,503
  jne 6f
  LIT "HTTP/1.1 503 Service Unavailable\r\n"
  jmp 9f
6:LIT "HTTP/1.1 500 Internal Server Error\r\n"
9:test ebp,ebp
  jz 10f
  LIT "Content-Encoding: gzip\r\n"
10:LIT "Content-Type: application/json\r\nContent-Length: "
  mov rsi,r13
  call put_u64
  mov dword ptr [rdi],0x0a0d0a0d
  add rdi,4
  mov rcx,rdi
  sub rcx,rsp            # header length
  mov rdi,rbx
  sub rdi,rcx
  mov rbx,rdi
  add r13,rcx
  mov rsi,rsp
  call mcpy
  mov edi,r12d
  mov rsi,rbx
  mov rdx,r13
  call conn_send
  add rsp,256
  pop rbp
  pop r13
  pop r12
  pop rbx
  ret

# ---- answers from Postgres. rbp conn.
pg_read:
  push rbx
  push r12
1:mov rsi,[rbp+PG_RECV]
  mov rax,[rbp+PG_END]
  add rsi,rax
  mov rdx,RECV_CAP
  sub rdx,rax
  mov rdi,[rbp+PG_FD]
  SYS_RECV
  cmp rax,-4
  je 1b
  cmp rax,-11
  je 9f
  test rax,rax
  jg 11f
  mov qword ptr [rbp+P_DEAD],1
  jmp 9f
11:add [rbp+PG_END],rax
  mov rax,[r15+W_T0NS]
  mov [rbp+P_RXT],rax
  mov [rbp+P_WAIT],rax
  mov qword ptr [rbp+P_STALL],0
.Lp_next:
  mov rcx,[rbp+PG_POS]
  mov rax,[rbp+PG_END]
  sub rax,rcx
  cmp rax,5
  jb .Lp_short
  mov rbx,[rbp+PG_RECV]
  add rbx,rcx
  mov edx,[rbx+1]
  bswap edx
  lea r8,[rdx+1]
  cmp rax,r8
  jb .Lp_short
  add [rbp+PG_POS],r8
  mov r12,rdx            # message length without the type byte
  movzx eax,byte ptr [rbx]
  mov rcx,[rbp+P_HEAD]
  cmp rcx,[rbp+P_TAIL]
  jae 12f
  cmp al,'D'
  je .Lp_row
  cmp al,'C'
  je .Lp_done
12:cmp al,'E'
  je .Lp_err
  cmp al,'Z'
  je .Lp_ready
  jmp .Lp_next
.Lp_short:
  # slide what is left to the front
  mov rcx,[rbp+PG_POS]
  mov rdx,[rbp+PG_END]
  sub rdx,rcx
  mov [rbp+PG_END],rdx
  mov qword ptr [rbp+PG_POS],0
  test rdx,rdx
  jz 9f
  mov rdi,[rbp+PG_RECV]
  lea rsi,[rdi+rcx]
  mov rcx,rdx
  rep movsb
9:pop r12
  pop rbx
  ret

.Lp_row:
  CUR_ENTRY rax
  movzx eax,byte ptr [rax+E_KIND]
  cmp eax,KH
  je .Lp_next
  add rbx,7              # first column
  cmp eax,KC
  je .Lp_row_c
  mov rdi,[rbp+P_CUR]
  cmp eax,KP
  je .Lp_row_p
  # feed
  cmp qword ptr [rbp+P_NROW],0
  jne 1f
  mov rdi,[rbp+PG_SCRATCH]
  add rdi,HEAD
  LIT "{\"posts\":["
  jmp 2f
1:mov byte ptr [rdi],','
  inc rdi
2:call emit_post
  jmp .Lp_rowend
.Lp_row_p:
  mov rdi,[rbp+PG_SCRATCH]
  add rdi,HEAD
  LIT "{\"post\":"
  call emit_post
  mov byte ptr [rdi],'}'
  inc rdi
.Lp_rowend:
  mov [rbp+P_CUR],rdi
  inc qword ptr [rbp+P_NROW]
  jmp .Lp_next
.Lp_row_c:
  # id and created_at from RETURNING, kept as sent: the reply is built at the commit
  CUR_ENTRY rax
  mov rax,[rax+E_EXTRA]
  movdqu xmm0,[rbx]
  movq xmm1,[rbx+16]
  movdqu [rax+24],xmm0
  movq [rax+40],xmm1
  jmp .Lp_next

# emit_post rbx first column, rdi out -> rdi advanced.
emit_post:
  LIT "{\"id\":"
  mov rsi,[rbx+4]
  bswap rsi
  add rbx,12
  call put_u64
  LIT ",\"body\":"
  call fld
  call json_esc
  LIT ",\"created_at\":"
  call r_ts
  LIT ",\"author\":"
  call fld
  call json_esc
  LIT ",\"like_count\":"
  mov esi,[rbx+4]
  bswap esi
  add rbx,8
  call put_u64
  mov byte ptr [rdi],'}'
  inc rdi
  ret

.Lp_done:
  CUR_ENTRY rax
  movzx edx,byte ptr [rax+E_KIND]
  cmp edx,KF
  jne 1f
  cmp qword ptr [rbp+P_NROW],0
  jne 11f
  mov rdi,[rbp+PG_SCRATCH]
  add rdi,HEAD
  LIT "{\"posts\":["
  jmp 12f
11:mov rdi,[rbp+P_CUR]
12:mov word ptr [rdi],0x7d5d
  add rdi,2
  mov esi,200
  jmp .Lp_finish
1:cmp edx,KP
  jne 2f
  mov esi,200
  mov rdi,[rbp+P_CUR]
  cmp qword ptr [rbp+P_NROW],0
  jne .Lp_finish
  jmp .Lp_nf
2:# a write: it commits at the ReadyForQuery of its Sync, so it is answered there
  cmp edx,KC
  je 21f
  cmp edx,KL
  jne 4f
  # "INSERT 0 n": one row inserted is a first like
  cmp byte ptr [rbx+r12-1],'1'
  sete byte ptr [rax+E_RES]
21:inc qword ptr [rbp+P_NDONE]
  jmp .Lp_next
4:# health
  mov rdi,[rbp+PG_SCRATCH]
  add rdi,HEAD
  LIT "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"
  mov rsi,[r15+W_NOW_S]
  sub rsi,[rip+g_start]
  call put_u64
  mov byte ptr [rdi],'}'
  inc rdi
  mov esi,200
  jmp .Lp_finish
.Lp_nf:
  # post not found
  mov rdi,[rbp+PG_SCRATCH]
  add rdi,HEAD
  LIT "{\"error\":\"post not found\"}"
  mov esi,404
.Lp_finish:
  call pg_finish
  jmp .Lp_next

.Lp_err:
  # a FATAL with nothing waiting (shutdown, admin kill): the read of 0 that follows handles the connection
  mov rax,[rbp+P_HEAD]
  cmp rax,[rbp+P_TAIL]
  jae .Lp_next
  # find the SQLSTATE
  lea rsi,[rbx+5]
1:mov cl,[rsi]
  test cl,cl
  jz 4f
  inc rsi
  cmp cl,'C'
  je 3f
2:cmp byte ptr [rsi],0
  je 21f
  inc rsi
  jmp 2b
21:inc rsi
  jmp 1b
3:mov eax,[rsi]
  jmp 5f
4:xor eax,eax
5:mov rcx,[rbp+P_IDX]
  cmp rcx,[r15+T_NR]
  jae .Lp_werr
  HEAD_ENTRY rdx
  cmp byte ptr [rdx+E_LAST],0
  jne 6f
  mov qword ptr [rbp+P_SKIP],1
6:movzx ecx,byte ptr [rdx+E_KIND]
  cmp ecx,KC
  jne 7f
  push rax
  mov rdi,[rdx+E_EXTRA]
  call free
  pop rax
  jmp 8f
7:cmp ecx,KL
  jne 8f
  cmp eax,0x30353332     # "2350", then '3'
  jne 8f
  cmp byte ptr [rsi+4],'3'
  jne 8f
  jmp .Lp_nf
8:call answer_500
  jmp .Lp_next

.Lp_werr:
  # A write transaction rolled back. A statement that failed fails alone when it ran solo or its post is missing
  # (23503); the writes before it (undone) and after it (skipped) committed nothing and are resent, each in a
  # transaction of its own. An error after the last statement (the commit) fails every write in it.
  xor r12d,r12d
  cmp eax,0x30353332     # "2350", then '3'
  jne 1f
  cmp byte ptr [rsi+4],'3'
  jne 1f
  inc r12d
1:cmp qword ptr [rbp+P_BATCH],0
  je 11f
  call pg_close_batch    # bound what Postgres skips: up to here
11:mov rbx,[rbp+P_NDONE]
  mov qword ptr [rbp+P_NDONE],0
  test rbx,rbx
  jz 3f
  mov rax,[rbp+P_HEAD]
  lea rax,[rax+rbx-1]
  and rax,RING_N-1
  shl rax,5
  add rax,[rbp+P_RING]
  cmp byte ptr [rax+E_LAST],0
  je 2f
21:call answer_w500      # the commit failed
  dec rbx
  jnz 21b
  jmp .Lp_next
2:call resend_head
  dec rbx
  jnz 2b
3:HEAD_ENTRY rax         # the statement that failed
  movzx ebx,byte ptr [rax+E_LAST]
  test r12d,r12d
  jnz 31f
  cmp byte ptr [rax+E_SOLO],0
  jne 32f
  call resend_head
  jmp 4f
31:cmp byte ptr [rax+E_KIND],KL
  jne 32f
  mov rdi,[rbp+PG_SCRATCH]
  add rdi,HEAD
  LIT "{\"error\":\"post not found\"}"
  mov esi,404
  call pg_finish
  jmp 4f
32:call answer_w500
4:test ebx,ebx           # the rest of its batch was skipped
  jnz .Lp_next
41:HEAD_ENTRY rax
  movzx ebx,byte ptr [rax+E_LAST]
  call resend_head
  test ebx,ebx
  jz 41b
  jmp .Lp_next

.Lp_ready:
  cmp qword ptr [rbp+P_INFL],0
  je 6f
  dec qword ptr [rbp+P_INFL]
6:# the writes executed since the last ReadyForQuery committed with it: answer them
  mov rbx,[rbp+P_NDONE]
  test rbx,rbx
  jz 7f
  mov qword ptr [rbp+P_NDONE],0
5:call answer_write
  dec rbx
  jnz 5b
7:# a write connection now idle sends the batch it held
  mov rax,[rbp+P_IDX]
  cmp rax,[r15+T_NR]
  jb 4f
  cmp qword ptr [rbp+P_INFL],0
  jne 4f
  cmp qword ptr [rbp+P_BATCH],0
  je 4f
  call pg_close_batch
  call pg_write
4:# after an error, the requests behind it in the batch were skipped
  cmp qword ptr [rbp+P_SKIP],0
  je .Lp_next
1:mov rax,[rbp+P_HEAD]
  cmp rax,[rbp+P_TAIL]
  jae 3f
  HEAD_ENTRY rax
  movzx ebx,byte ptr [rax+E_LAST]
  call answer_500
  test ebx,ebx
  jz 1b
3:mov qword ptr [rbp+P_SKIP],0
  jmp .Lp_next

# answer_write rbp: the head entry is a committed write; answer it.
answer_write:
  push rbx
  HEAD_ENTRY rax
  mov rdi,[rbp+PG_SCRATCH]
  add rdi,HEAD
  cmp byte ptr [rax+E_KIND],KC
  je 2f
  mov r12,[rax+E_A]
  movzx ebx,byte ptr [rax+E_RES]
  LIT "{\"liked\":true,\"already_liked\":"
  test ebx,ebx
  jz 1f
  LIT "false,\"post_id\":"
  jmp 11f
1:LIT "true,\"post_id\":"
11:mov rsi,r12
  call put_u64
  mov byte ptr [rdi],'}'
  inc rdi
  mov esi,200
  test ebx,ebx
  jz 12f
  mov esi,201
12:pop rbx
  jmp pg_finish
2:mov r12,[rax+E_EXTRA]
  LIT "{\"post\":{\"id\":"
  lea rbx,[r12+24]
  mov rsi,[rbx+4]
  bswap rsi
  add rbx,12
  call put_u64
  LIT ",\"body\":"
  mov rax,[r12]
  lea rsi,[r12+rax+48]
  mov rcx,[r12+8]
  call json_esc
  LIT ",\"created_at\":"
  call r_ts
  LIT ",\"author\":"
  lea rsi,[r12+48]
  mov rcx,[r12]
  call json_esc
  LIT ",\"like_count\":0}}"
  push rdi
  mov rdi,r12
  call free
  pop rdi
  mov esi,201
  pop rbx
  jmp pg_finish

# resend_head rbp: the head entry is a write that committed nothing. It moves to the tail in a transaction of its
# own (its one execution), or gets a 500 when the connection has no room.
resend_head:
  push rbx
  HEAD_ENTRY rbx
  mov rax,[rbp+P_TAIL]
  sub rax,[rbp+P_HEAD]
  cmp rax,RING_N-2
  jae 8f
  cmp qword ptr [rbp+P_SLEN],SEND_CAP-0x10000
  jae 8f
  cmp qword ptr [rbp+P_BATCH],0
  je 1f
  call pg_close_batch
1:mov rsi,[rbx+E_EXTRA]
  cmp byte ptr [rbx+E_KIND],KC
  jne 2f
  call bind_create
  jmp 3f
2:mov rdx,[rbx+E_A]
  call bind_like
3:mov rax,[rbp+P_TAIL]
  and rax,RING_N-1
  shl rax,5
  add rax,[rbp+P_RING]
  movdqu xmm0,[rbx]
  movdqu xmm1,[rbx+16]
  movdqu [rax],xmm0
  movdqu [rax+16],xmm1
  mov byte ptr [rax+E_SOLO],1
  inc qword ptr [rbp+P_TAIL]
  inc qword ptr [rbp+P_HEAD]
  call pg_close_batch    # its own Sync; marks it the last of its batch
  pop rbx
  ret
8:pop rbx
# answer_w500 rbp: the head entry is a write that failed.
answer_w500:
  HEAD_ENTRY rax
  cmp byte ptr [rax+E_KIND],KC
  jne answer_500
  mov rdi,[rax+E_EXTRA]
  call free
answer_500:
  mov rdi,[rbp+PG_SCRATCH]
  add rdi,HEAD
  LIT "{\"error\":\"internal server error\"}"
  mov esi,500
# pg_finish rbp, esi status, rdi body end: gzip when asked, send, and pick up anything pipelined behind it.
pg_finish:
  push rbx
  push r12
  push r13
  HEAD_ENTRY rbx
  inc qword ptr [rbp+P_HEAD]
  mov qword ptr [rbp+P_NROW],0
  cmp byte ptr [rbx+E_DROP],0
  jne .Lf_end            # sent again elsewhere by pg_hedge; that copy answers
  cmp byte ptr [rbx+E_KIND],KC
  jae 1f                 # writes: E_EXTRA is theirs
  mov rax,[r15+W_T0NS]
  sub rax,[rbx+E_EXTRA]
  sub rax,[r15+T_RLAT]
  sar rax,4
  add [r15+T_RLAT],rax
1:mov r13d,esi
  mov r12d,[rbx+E_FD]
  mov rax,r12
  shl rax,6
  add rax,[r15+W_CONNS]
  mov ecx,[rbx+E_GEN]
  cmp ecx,[rax+C_GEN]
  jne .Lf_end            # the client left
  mov dword ptr [rax+C_BUSY],0
  mov rdx,[rbp+PG_SCRATCH]
  add rdx,HEAD
  mov rcx,rdi
  sub rcx,rdx
  xor r8d,r8d
  cmp byte ptr [rbx+E_GZ],0
  je 2f
  cmp qword ptr [rip+g_gz],0
  je 2f
  cmp rcx,1024
  jb 2f
  push rdx
  push rcx
  mov rdi,rdx
  mov rsi,rcx
  mov rdx,[r15+T_GZBUF]
  lea r8,[rdx+0x3f8000]  # its hash table, past the 0x3f0000 of output
  add rdx,HEAD
  mov ecx,0x3f0000
  call gz_compress
  pop rcx
  pop rdx
  xor r8d,r8d
  test rax,rax
  jz 2f
  cmp rax,rcx
  jae 2f
  mov rcx,rax
  mov rdx,[r15+T_GZBUF]
  add rdx,HEAD
  mov r8d,1
2:mov edi,r12d
  mov esi,r13d
  mov r13,r12
  shl r13,6
  add r13,[r15+W_CONNS]
  mov eax,[r13+C_GEN]
  push rax
  call http_send
  pop rax
  cmp eax,[r13+C_GEN]
  jne .Lf_end            # the send failed and closed it
  cmp dword ptr [r13+C_CLOSING],0
  je 3f
  cmp qword ptr [r13+C_OUT],0
  jne .Lf_end            # close after the flush
  mov edi,r12d
  call close_conn
  jmp .Lf_end
3:mov edi,r12d
  call conn_resume
.Lf_end:
  pop r13
  pop r12
  pop rbx
  ret

fld:
  mov ecx,[rbx]
  bswap ecx
  lea rsi,[rbx+4]
  add rbx,4
  cmp ecx,-1
  je 1f
  add rbx,rcx
1:ret


# ---- connection plumbing. r15 worker throughout.
# conn_of edi fd -> rax conn
.macro CONN_OF fd,out
  mov \out,\fd
  shl \out,6
  add \out,[r15+W_CONNS]
.endm

close_conn:            # edi fd. Bumps gen so late job completions are dropped.
  push rbx
  push r12
  mov r12d,edi
  CONN_OF r12,rbx
  inc dword ptr [rbx+C_GEN]
  mov dword ptr [rbx+C_BUSY],0
  mov dword ptr [rbx+C_CLOSING],0
  mov dword ptr [rbx+C_EOF],0
  mov rdi,[rbx+C_IN]
  call free
  mov rdi,[rbx+C_OUT]
  call free
  xor eax,eax
  mov [rbx+C_IN],rax
  mov [rbx+C_INLEN],rax
  mov [rbx+C_OUT],rax
  mov edi,r12d
  SYS SYS_CLOSE
  pop r12
  pop rbx
  ret

# conn_send edi fd, rsi data, rdx len. Copies what the socket will not take now.
conn_send:
  push rbx
  push r12
  push r13
  mov r12d,edi
  mov r13,rsi
  mov rbx,rdx
1:mov edi,r12d
  mov rsi,r13
  mov rdx,rbx
  SYS_SEND
  cmp rax,-4
  je 1b
  cmp rax,-11
  je 2f
  test rax,rax
  js 8f
  add r13,rax
  sub rbx,rax
  jz 7f
  jmp 1b
2:# socket full: keep the rest and watch for EPOLLOUT
  lea rdi,[rbx+16]
  call alloc
  mov rdi,rax
  mov rsi,r13
  mov rcx,rbx
  push rax
  call mcpy
  pop rax
  mov rcx,r12
  shl rcx,6
  add rcx,[r15+W_CONNS]
  mov [rcx+C_OUT],rax
  mov qword ptr [rcx+C_OUTOFF],0
  mov [rcx+C_OUTLEN],rbx
  mov esi,3              # EPOLL_CTL_MOD
  mov edx,0x80002005     # IN|OUT|RDHUP|ET
  mov edi,r12d
  call ep_ctl
7:pop r13
  pop r12
  pop rbx
  ret
8:mov edi,r12d
  pop r13
  pop r12
  pop rbx
  jmp close_conn

# ep_ctl edi fd, esi op, edx events
ep_ctl:
  sub rsp,16
  mov [rsp],edx
  mov [rsp+4],edi
  mov dword ptr [rsp+8],0
  mov edx,edi
  mov edi,[r15+W_EPFD]
  mov r10,rsp
  SYS SYS_EPOLL_CTL
  add rsp,16
  ret


do_accept:
  push rbx
  mov ebx,256
1:mov edi,[r15+W_LISTEN]
  xor esi,esi
  xor edx,edx
  mov r10d,0x80800       # NONBLOCK|CLOEXEC
  SYS SYS_ACCEPT4
  test rax,rax
  js 3f
  cmp rax,MAX_FDS
  jae 4f
  mov edi,eax
  mov esi,1              # ADD
  mov edx,0x80002001     # IN|RDHUP|ET: no re-poll of a served socket on every epoll_wait
  push rax
  push rax               # the fd, and the stack stays 16-byte aligned
  call ep_ctl
  pop rdi
  pop rax
  call conn_read         # TCP_DEFER_ACCEPT: the request is usually here already; EAGAIN if not
  dec ebx
  jnz 1b
2:pop rbx
  ret
3:cmp rax,-11
  je 2b
  # out of fds or similar: back off instead of spinning on the level-triggered listener
  sub rsp,16
  mov qword ptr [rsp],0
  mov qword ptr [rsp+8],1000000
  mov rdi,rsp
  xor esi,esi
  SYS SYS_NANOSLEEP
  add rsp,16
  jmp 2b
4:mov edi,eax
  SYS SYS_CLOSE
  jmp 1b

# conn_flush edi fd: EPOLLOUT
conn_flush:
  push rbx
  push r12
  mov r12d,edi
  CONN_OF r12,rbx
  cmp qword ptr [rbx+C_OUT],0
  je 9f
1:mov edi,r12d
  mov rsi,[rbx+C_OUT]
  add rsi,[rbx+C_OUTOFF]
  mov rdx,[rbx+C_OUTLEN]
  SYS_SEND
  cmp rax,-4
  je 1b
  cmp rax,-11
  je 9f
  test rax,rax
  js 8f
  add [rbx+C_OUTOFF],rax
  sub [rbx+C_OUTLEN],rax
  jnz 1b
  mov rdi,[rbx+C_OUT]
  call free
  mov qword ptr [rbx+C_OUT],0
  mov edi,r12d
  mov esi,3
  mov edx,0x80002001
  call ep_ctl
  cmp dword ptr [rbx+C_CLOSING],0
  jne 8f
  mov edi,r12d
  call conn_resume
9:pop r12
  pop rbx
  ret
8:mov edi,r12d
  pop r12
  pop rbx
  jmp close_conn


# conn_read edi fd
conn_read:
  push rbx
  push r12
  mov r12d,edi
  CONN_OF r12,rbx
  cmp qword ptr [rbx+C_IN],0
  jne .Lappend
  # common case: the request sits in the shared read buffer
  mov edi,r12d
  mov rsi,[r15+W_RBUF]
  mov edx,RBUF_SIZE
  SYS_RECV
  cmp rax,-11
  je .Lrd_done
  cmp rax,-4
  je .Lrd_done
  test rax,rax
  js .Lrd_close
  jz .Lrd_eof
  cmp rax,RBUF_SIZE
  jne 7f
  push rax
  push rax
  call conn_rearm        # the buffer filled: there may be more
  pop rax
  pop rax
7:
  mov edi,r12d
  mov rsi,[r15+W_RBUF]
  mov rdx,rax
  call conn_process
.Lrd_done:
  pop r12
  pop rbx
  ret
.Lrd_eof:
  # the peer is done sending; a reply still due (in flight, or unsent) is answered first, then conn_process or
  # conn_resume close it once it's idle
  cmp dword ptr [rbx+C_BUSY],0
  jne 1f
  cmp qword ptr [rbx+C_OUT],0
  je .Lrd_close
1:mov dword ptr [rbx+C_EOF],1
  jmp .Lrd_done
.Lrd_close:
  mov edi,r12d
  pop r12
  pop rbx
  jmp close_conn
.Lappend:
  # grow the pending buffer if a full read might not fit
  mov eax,[rbx+C_INLEN]
  add eax,RBUF_SIZE
  cmp eax,[rbx+C_INCAP]
  jbe 2f
  cmp eax,MAX_BODY+RBUF_SIZE*3
  ja .Lrd_close
  lea edi,[rax+rax]
  mov [rbx+C_INCAP],edi
  call alloc
  push rax
  mov rdi,rax
  mov rsi,[rbx+C_IN]
  mov ecx,[rbx+C_INLEN]
  call mcpy
  mov rdi,[rbx+C_IN]
  call free
  pop rax
  mov [rbx+C_IN],rax
2:mov edi,r12d
  mov rsi,[rbx+C_IN]
  mov eax,[rbx+C_INLEN]
  add rsi,rax
  mov edx,RBUF_SIZE
  SYS_RECV
  cmp rax,-11
  je .Lrd_done
  cmp rax,-4
  je .Lrd_done
  test rax,rax
  js .Lrd_close
  jz .Lrd_eof
  cmp rax,RBUF_SIZE
  jne 7f
  push rax
  push rax
  call conn_rearm        # the buffer filled: there may be more
  pop rax
  pop rax
7:
  add [rbx+C_INLEN],eax
  cmp dword ptr [rbx+C_BUSY],0
  jne .Lrd_done
  cmp qword ptr [rbx+C_OUT],0
  jne .Lrd_done
  mov edi,r12d
  call conn_resume
  jmp .Lrd_done

# conn_rearm r12d fd, rbx conn: client sockets are edge-triggered, so after a read that filled the buffer this
# re-arms the fd (EPOLL_CTL_MOD re-checks readiness): data still queued raises a new event in the next pass.
conn_rearm:
  mov edi,r12d
  mov esi,3
  mov edx,0x80002001
  cmp qword ptr [rbx+C_OUT],0
  je 1f
  mov edx,0x80002005
1:jmp ep_ctl

# conn_resume edi fd: parse whatever is pending
conn_resume:
  mov eax,edi
  shl rax,6
  add rax,[r15+W_CONNS]
  mov rsi,[rax+C_IN]
  test rsi,rsi
  jz 1f
  mov edx,[rax+C_INLEN]
  test edx,edx
  jz 1f
  jmp conn_process
1:cmp dword ptr [rax+C_EOF],0
  je 2f
  cmp dword ptr [rax+C_BUSY],0
  jne 2f
  cmp qword ptr [rax+C_OUT],0
  je close_conn          # edi is still the fd
2:ret

# conn_process edi fd, rsi data, rdx len. data is the read buffer or the conn's own pending buffer.
conn_process:
  push rbx
  push rbp
  push r12
  push r13
  mov r12d,edi
  CONN_OF r12,rbx
  mov r13,rsi            # cursor
  mov rbp,rdx            # bytes left
  mov [r14+R_FD],r12
  mov eax,[rbx+C_GEN]
  push rax
.Lproc:
  test rbp,rbp
  jz .Lleft
  cmp dword ptr [rbx+C_BUSY],0
  jne .Lleft
  cmp qword ptr [rbx+C_OUT],0
  jne .Lleft
  mov rdi,r13
  mov rsi,rbp
  call parse_request     # rax = bytes of this request, 0 when incomplete, -1 when malformed
  test rax,rax
  jz .Lleft
  js .Lproc_bad
  add r13,rax
  sub rbp,rax
  mov [r14+R_TOTAL],rax
  call route
  mov eax,[rsp]
  cmp eax,[rbx+C_GEN]
  jne .Lproc_end         # closed while answering
  cmp qword ptr [r14+R_CLOSE],0
  je .Lproc
  # Connection: close
  mov dword ptr [rbx+C_CLOSING],1
  cmp dword ptr [rbx+C_BUSY],0
  jne .Lleft
  cmp qword ptr [rbx+C_OUT],0
  jne .Lleft
  mov edi,r12d
  call close_conn
  jmp .Lproc_end
.Lproc_bad:
  call reply_malformed
  mov edi,r12d
  call close_conn
  jmp .Lproc_end
.Lleft:
  # keep what was not consumed
  mov rax,[rbx+C_IN]
  test rax,rax
  jz .Lstash
  # already in the pending buffer: slide down
  mov [rbx+C_INLEN],ebp
  test rbp,rbp
  jz .Lfree_in
  cmp rax,r13
  je .Lproc_end
  mov rdi,rax
  mov rsi,r13
  mov rcx,rbp
  rep movsb
  jmp .Lproc_end
.Lfree_in:
  mov rdi,rax
  call free
  mov qword ptr [rbx+C_IN],0
  mov dword ptr [rbx+C_INCAP],0
  jmp .Lproc_end
.Lstash:
  test rbp,rbp
  jz .Lproc_end
  lea rdi,[rbp+RBUF_SIZE*2]
  mov [rbx+C_INCAP],edi
  call alloc
  mov [rbx+C_IN],rax
  mov [rbx+C_INLEN],ebp
  mov rdi,rax
  mov rsi,r13
  mov rcx,rbp
  call mcpy
.Lproc_end:
  cmp dword ptr [rbx+C_EOF],0
  je 1f
  cmp dword ptr [rbx+C_BUSY],0
  jne 1f
  cmp qword ptr [rbx+C_OUT],0
  jne 1f
  mov edi,r12d
  call close_conn        # half-closed and everything it asked for is answered
1:add rsp,8
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret


# ---- request parsing
.section .rodata
.balign 16
x_cr: .fill 16,1,13
.text
# ieq: compare rdi[0..rcx) case-insensitively with lowercase rsi. ZF when equal.
ieq:
  test rcx,rcx
  jz 2f
1:mov al,[rdi]
  cmp al,'A'
  jb 3f
  cmp al,'Z'
  ja 3f
  or al,0x20
3:cmp al,[rsi]
  jne 2f
  inc rdi
  inc rsi
  dec rcx
  jnz 1b
2:ret

# parse_request rdi data, rsi len -> rax total bytes (0 incomplete, -1 malformed). Fills the frame.
parse_request:
  push rbx
  push r12
  push r13
  mov rbx,rdi
  lea r12,[rdi+rsi]      # end
  # find the end of the head
  mov r13,rdi
  movdqa xmm1,[rip+x_cr]
1:lea rax,[r13+16]
  cmp rax,r12
  ja 3f
  movdqu xmm0,[r13]
  pcmpeqb xmm0,xmm1
  pmovmskb eax,xmm0
2:test eax,eax
  jz 5f
  bsf ecx,eax
  btr eax,ecx
  lea rdx,[r13+rcx]
  lea rsi,[rdx+4]
  cmp rsi,r12
  ja 6f
  cmp dword ptr [rdx],0x0a0d0a0d
  je .Lhead
  jmp 2b
5:add r13,16
  jmp 1b
3:# tail, byte by byte
  lea rax,[r13+4]
  cmp rax,r12
  ja 6f
  cmp dword ptr [r13],0x0a0d0a0d
  mov rdx,r13
  je .Lhead
  inc r13
  jmp 3b
6:mov rax,r12
  sub rax,rbx
  cmp rax,65536
  ja .Lmal
  xor eax,eax
  jmp .Lpend
.Lhead:
  mov r13,rdx            # at the final CRLFCRLF
  xor eax,eax
  mov [r14+R_AUTH],rax
  mov [r14+R_AUTHLEN],rax
  mov [r14+R_CLOSE],rax
  mov [r14+R_GZ],rax
  mov [r14+R_BODYLEN],rax
  mov [r14+R_QUERY],rax
  mov [r14+R_QLEN],rax
  # method
  mov eax,[rbx]
  mov ecx,M_GET
  cmp eax,0x20544547
  je 7f
  mov ecx,M_POST
  cmp eax,0x54534f50
  je 7f
  mov ecx,M_PUT
  cmp eax,0x20545550
  je 7f
  mov ecx,M_DEL
  cmp eax,0x454c4544
  je 7f
  xor ecx,ecx
7:mov [r14+R_METHOD],rcx
  # target
  mov rdi,rbx
8:cmp rdi,r13
  jae .Lmal
  cmp byte ptr [rdi],' '
  je 9f
  inc rdi
  jmp 8b
9:inc rdi
  mov [r14+R_PATH],rdi
  mov rsi,rdi
10:cmp rsi,r13
  jae .Lmal
  mov al,[rsi]
  cmp al,' '
  je 11f
  cmp al,'?'
  je 12f
  inc rsi
  jmp 10b
12:mov rax,rsi
  sub rax,rdi
  mov [r14+R_PATHLEN],rax
  inc rsi
  mov [r14+R_QUERY],rsi
  mov rdi,rsi
13:cmp rsi,r13
  jae .Lmal
  cmp byte ptr [rsi],' '
  je 14f
  inc rsi
  jmp 13b
14:mov rax,rsi
  sub rax,rdi
  mov [r14+R_QLEN],rax
  jmp 15f
11:mov rax,rsi
  sub rax,rdi
  mov [r14+R_PATHLEN],rax
15:# version: only 1.0 disables keep-alive by default
  # headers, one per line
  mov rdi,rsi
16:cmp rdi,r13
  jae .Lhdone
  cmp byte ptr [rdi],10
  je 17f
  inc rdi
  jmp 16b
17:inc rdi              # at a header line
  mov r8,rdi
  xor r9d,r9d            # content-length seen below
  # dispatch on the first letter, lowercased
  movzx eax,byte ptr [rdi]
  or al,0x20
  cmp al,'a'
  je .Lh_auth
  cmp al,'c'
  je .Lh_c
  jmp 16b
.Lh_auth:
  mov rdi,r8
  lea rsi,[rip+h_auth]
  mov ecx,14
  call ieq
  mov rdi,r8
  jne .Lh_ae
  lea rdi,[r8+14]
  call hdr_value
  mov [r14+R_AUTH],rax
  mov [r14+R_AUTHLEN],rdx
  jmp 16b
.Lh_ae:
  lea rsi,[rip+h_ae]
  mov ecx,16
  call ieq
  mov rdi,r8
  jne 16b
  lea rdi,[r8+16]
  call hdr_value
  mov rdi,r8
  # the value lists "gzip" somewhere
41:cmp rdx,4
  jb 16b
  cmp dword ptr [rax],0x70697a67
  je 42f
  inc rax
  dec rdx
  jmp 41b
42:mov qword ptr [r14+R_GZ],1
  jmp 16b
.Lh_c:
  mov rdi,r8
  lea rsi,[rip+h_cl]
  mov ecx,15
  call ieq
  mov rdi,r8
  jne .Lh_conn
  lea rdi,[r8+15]
  call hdr_value
  mov rdi,rax
  mov rsi,rdx
  push r8
  call parse_u64
  pop r8
  test rdx,rdx
  jz .Lmal
  cmp rax,MAX_BODY
  ja .Lmal
  mov [r14+R_BODYLEN],rax
  mov rdi,r8
  jmp 16b
.Lh_conn:
  lea rsi,[rip+h_conn]
  mov ecx,11
  call ieq
  mov rdi,r8
  jne 16b
  lea rdi,[r8+11]
  call hdr_value
  cmp rdx,5
  jne 18f
  mov rdi,rax
  lea rsi,[rip+v_close]
  mov ecx,5
  call ieq
  jne 18f
  mov qword ptr [r14+R_CLOSE],1
18:mov rdi,r8
  jmp 16b
.Lhdone:
  jmp .Lhd2
.Lmal:
  mov rax,-1
.Lpend:
  pop r13
  pop r12
  pop rbx
  ret
.Lhd2:
  lea rdx,[r13+4]        # body start
  mov [r14+R_BODY],rdx
  mov rax,[r14+R_BODYLEN]
  lea rcx,[rdx+rax]
  cmp rcx,r12
  ja 6f
  mov rax,rcx
  sub rax,rbx
  jmp .Lpend
6:xor eax,eax
  jmp .Lpend
.section .rodata
h_auth: .ascii "authorization:"
h_ae:   .ascii "accept-encoding:"
h_cl:   .ascii "content-length:"
h_conn: .ascii "connection:"
v_close:.ascii "close"
.text
# hdr_value rdi at the value -> rax start, rdx length (spaces and CRLF trimmed). Preserves r8.
hdr_value:
1:cmp byte ptr [rdi],' '
  jne 2f
  inc rdi
  jmp 1b
2:mov rax,rdi
3:mov cl,[rdi]
  cmp cl,13
  je 4f
  cmp cl,10
  je 4f
  inc rdi
  jmp 3b
4:mov rdx,rdi
  sub rdx,rax
5:test rdx,rdx
  jz 6f
  cmp byte ptr [rax+rdx-1],' '
  jne 6f
  dec rdx
  jmp 5b
6:ret

r_ts:
  call fld
  mov rax,[rsi]
  bswap rax
  jmp fmt_ts

# UDIV d, M, k: rax = rax / d for 0 <= rax < 2^63, by multiplying with M = ceil(2^(64+k) / d), k = floor(log2 d)
# (exact for every rax below 2^63). Clobbers rdx.
.macro UDIV M,k
  mov rdx,\M
  mul rdx
  .if \k
  shr rdx,\k
  .endif
  mov rax,rdx
.endm
# PUT2: the two digits of rax (0-99) at rdi
.macro PUT2
  movzx eax,word ptr [dig2+rax*2]
  mov [rdi],ax
  add rdi,2
.endm

# fmt_ts rdi out, rax Postgres timestamp (microseconds since 2000-01-01) -> rdi past "YYYY-MM-DDTHH:MM:SS.mmmZ".
# Clobbers rax, rcx, rdx, r8-r11. No divide instructions on the usual path (a date from 1970 on).
.globl fmt_ts
fmt_ts:
  push r12
  push r13
  test rax,rax
  js 7f
  mov r8,rax
  UDIV 0x8637bd05af6c69b6,19   # seconds since 2000
  imul rcx,rax,1000000
  sub r8,rcx
  mov r12,r8             # micros
  add rax,946684800      # unix seconds
  jmp 8f
7:mov rcx,1000000        # before 2000: floor division the slow way
  cqo
  idiv rcx
  test rdx,rdx
  jns 1f
  add rdx,rcx
  dec rax
1:mov r12,rdx
  add rax,946684800
  js 9f
8:mov r8,rax
  UDIV 0xc22e450672894ab7,16   # days since 1970
  imul rcx,rax,86400
  sub r8,rcx
  mov r13,r8             # seconds of day
  jmp 2f
9:mov rcx,86400          # before 1970
  cqo
  idiv rcx
  test rdx,rdx
  jns 1f
  add rdx,rcx
  dec rax
1:mov r13,rdx
  # civil from days (Hinnant); dates before 1970 take this path too, so no dates before 0000
2:add rax,719468
  mov r9,rax
  UDIV 0xe5ac1af323e18bb4,17   # era
  mov r8,rax
  imul rcx,rax,146097
  sub r9,rcx             # doe, 0-146096
  mov rax,r9
  UDIV 0xb38cf9b00b38cf9c,10   # doe/1460
  mov r10,r9
  sub r10,rax
  mov rax,r9
  UDIV 0xe5ac81fa000e5ac9,15   # doe/36524
  add r10,rax
  xor eax,eax
  cmp r9,146096
  sete al                # doe/146096
  sub r10,rax            # doe - doe/1460 + doe/36524 - doe/146096
  mov rax,r10
  UDIV 0xb38cf9b00b38cf9c,8    # /365: yoe
  mov r10,rax
  imul r11,r8,400
  add r11,r10            # y
  imul rcx,r10,365
  mov rax,r10
  shr rax,2
  add rcx,rax
  mov rax,r10
  UDIV 0xa3d70a3d70a3d70b,6    # yoe/100
  sub rcx,rax
  sub r9,rcx             # doy
  lea rax,[r9*4+r9+2]
  UDIV 0xd62b80d62b80d62c,7    # (5*doy+2)/153: mp
  mov r10,rax
  imul rax,r10,153
  add rax,2
  UDIV 0xcccccccccccccccd,2    # /5
  sub r9,rax
  inc r9                 # day
  lea r8,[r10+3]
  cmp r8,12
  jbe 3f
  sub r8,12
3:cmp r8,2
  ja 4f
  inc r11
4:mov byte ptr [rdi],'"'
  inc rdi
  mov rax,r11
  mov r10,r11
  UDIV 0xa3d70a3d70a3d70b,6
  imul rcx,rax,100
  sub r10,rcx
  PUT2
  mov rax,r10
  PUT2
  mov byte ptr [rdi],'-'
  inc rdi
  mov rax,r8
  PUT2
  mov byte ptr [rdi],'-'
  inc rdi
  mov rax,r9
  PUT2
  mov byte ptr [rdi],'T'
  inc rdi
  mov rax,r13
  UDIV 0x91a2b3c4d5e6f80a,11   # hours
  imul rcx,rax,3600
  sub r13,rcx
  PUT2
  mov byte ptr [rdi],':'
  inc rdi
  mov rax,r13
  UDIV 0x8888888888888889,5    # minutes
  imul rcx,rax,60
  sub r13,rcx
  PUT2
  mov byte ptr [rdi],':'
  inc rdi
  mov rax,r13
  PUT2
  mov byte ptr [rdi],'.'
  inc rdi
  mov rax,r12
  UDIV 0x83126e978d4fdf3c,9    # ms
  mov r10,rax
  UDIV 0xa3d70a3d70a3d70b,6    # hundreds of ms
  imul rcx,rax,100
  sub r10,rcx
  add al,'0'
  mov [rdi],al
  inc rdi
  mov rax,r10
  PUT2
  mov word ptr [rdi],0x225a   # Z"
  add rdi,2
  pop r13
  pop r12
  ret
