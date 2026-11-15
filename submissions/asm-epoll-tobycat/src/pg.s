# Postgres wire protocol over a blocking socket: SCRAM-SHA-256 login, prepare, one round trip at a time.
.include "defs.inc"
.set RING_N,1048576
.bss
.globl g_pg_addr
g_pg_addr: .skip 16         # sockaddr_in
g_pg_user: .skip 8
g_pg_pass: .skip 8
g_pg_db:   .skip 8
.section .rodata
m_url: .asciz "bad DATABASE_URL (need postgres://user:pass@host:port/db)"
m_conn: .asciz "cannot connect to postgres"
m_auth: .asciz "postgres auth failed (only SCRAM-SHA-256 and trust are supported)"
m_io:  .asciz "postgres connection lost"
m_prep: .asciz "postgres rejected a statement: "
m_big: .asciz "postgres result too large"
s_session: .asciz "SET jit=off; SET plan_cache_mode=force_generic_plan; SET max_parallel_workers_per_gather=0"
.text

# pg_parse_url rdi url (writable; split in place): postgres://user[:password]@host[:port]/db[?params].
# User, password and database are percent-decoded. The host may be a name.
.globl pg_parse_url
pg_parse_url:
  push rbx
  push r12
  push r13
  push r14
  mov rbx,rdi
1:mov al,[rbx]            # past the scheme
  test al,al
  jz .Lurlbad
  inc rbx
  cmp al,':'
  jne 1b
  cmp word ptr [rbx],0x2f2f
  jne .Lurlbad
  add rbx,2
  mov r12,rbx             # authority
  xor r14d,r14d           # its last '@'
2:mov al,[rbx]
  test al,al
  jz .Lurlbad
  cmp al,'/'
  je 3f
  cmp al,'@'
  cmove r14,rbx
  inc rbx
  jmp 2b
3:mov r13,rbx             # the '/'
  test r14,r14
  jz .Lurlbad
  mov byte ptr [r14],0
  mov [rip+g_pg_user],r12
  lea rax,[r14]           # no password: the empty string at the '@' just cleared
  mov [rip+g_pg_pass],rax
  mov rdi,r12
4:mov al,[rdi]
  test al,al
  jz 5f
  cmp al,':'
  je 41f
  inc rdi
  jmp 4b
41:mov byte ptr [rdi],0
  inc rdi
  mov [rip+g_pg_pass],rdi
  call pct_decode
5:mov rdi,[rip+g_pg_user]
  cmp byte ptr [rdi],0
  je .Lurlbad
  call pct_decode
  # database, without any query string
  lea rdi,[r13+1]
  mov [rip+g_pg_db],rdi
  cmp byte ptr [rdi],0
  je .Lurlbad
6:mov al,[rdi]
  test al,al
  jz 7f
  cmp al,'?'
  je 61f
  inc rdi
  jmp 6b
61:mov byte ptr [rdi],0
7:mov rdi,[rip+g_pg_db]
  call pct_decode
  # port
  mov byte ptr [r13],0
  lea rbx,[r14+1]         # host
  mov word ptr [rip+g_pg_addr],2
  mov eax,5432
  mov rdi,rbx
8:cmp byte ptr [rdi],0
  je 11f
  cmp byte ptr [rdi],':'
  je 9f
  inc rdi
  jmp 8b
9:mov byte ptr [rdi],0
  inc rdi
  xor eax,eax
  xor edx,edx             # digits
10:movzx ecx,byte ptr [rdi]
  test ecx,ecx
  jz 101f
  sub ecx,'0'
  cmp ecx,9
  ja .Lurlbad
  imul eax,eax,10
  add eax,ecx
  cmp eax,65535
  ja .Lurlbad
  inc rdi
  inc edx
  jmp 10b
101:test edx,edx
  jz .Lurlbad
11:xchg al,ah
  mov [rip+g_pg_addr+2],ax
  cmp byte ptr [rbx],0
  je .Lurlbad
  mov rdi,rbx
  call pg_resolve_host
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
.Lurlbad:
  lea rdi,[rip+m_url]
  jmp die

# pct_decode rdi NUL-terminated, in place. A '%' not followed by two hex digits stays as it is.
pct_decode:
  mov rsi,rdi
1:movzx eax,byte ptr [rsi]
  cmp al,'%'
  je 3f
2:mov [rdi],al
  inc rsi
  inc rdi
  test al,al
  jnz 1b
  ret
3:movzx ecx,byte ptr [rsi+1]
  call hexval
  js 4f
  mov edx,ecx
  shl edx,4
  movzx ecx,byte ptr [rsi+2]
  call hexval
  js 4f
  or ecx,edx
  mov [rdi],cl
  add rsi,3
  inc rdi
  jmp 1b
4:mov eax,'%'
  jmp 2b
# hexval ecx character -> ecx value, SF set when not a hex digit. Keeps rax, rdx, rsi, rdi.
hexval:
  cmp ecx,'9'
  ja 1f
  sub ecx,'0'
  jmp 2f
1:or ecx,0x20
  sub ecx,'a'-10
  cmp ecx,15
  ja 3f
  cmp ecx,10
  jb 3f
  test ecx,ecx
  ret
2:cmp ecx,9
  ja 3f
  test ecx,ecx
  ret
3:mov ecx,-1
  test ecx,ecx
  ret

# A failure during a reconnect goes back to pg_try; at startup it ends the process.
pg_die:
  cmp qword ptr [rbp+P_RECOV],0
  je die
  mov rsp,[rbp+P_RECOV]
  jmp pg_try_fail

# send_all rbp ctx, rdi buf, rsi len
send_all:
  mov rdx,rsi
  mov rsi,rdi
1:test rdx,rdx
  jz 2f
  mov rdi,[rbp+PG_FD]
  push rsi
  push rdx
  SYS SYS_WRITE
  pop rdx
  pop rsi
  cmp rax,-4
  je 1b
  test rax,rax
  jle pg_lost
  add rsi,rax
  sub rdx,rax
  jmp 1b
2:ret
pg_lost:
  lea rdi,[rip+m_io]
  jmp pg_die

# recv_more rbp ctx: one read onto the end of the receive buffer
recv_more:
  mov rax,[rbp+PG_END]
  mov rsi,[rbp+PG_RECV]
  add rsi,rax
  mov rdx,RECV_CAP
  sub rdx,rax
  cmp rdx,1048576
  jb 2f
  mov edx,1048576
  mov rdi,[rbp+PG_FD]
  SYS SYS_READ
  cmp rax,-4
  je recv_more
  test rax,rax
  jle pg_lost
  add [rbp+PG_END],rax
  ret
2:lea rdi,[rip+m_big]
  jmp pg_die

# next_msg rbp ctx -> rax message start (type byte), rdx payload length. Consumes it.
next_msg:
1:mov rcx,[rbp+PG_POS]
  mov rax,[rbp+PG_END]
  sub rax,rcx
  cmp rax,5
  jb 2f
  mov rsi,[rbp+PG_RECV]
  add rsi,rcx
  mov edx,[rsi+1]
  bswap edx
  lea r8,[rdx+1]
  cmp rax,r8
  jb 2f
  add [rbp+PG_POS],r8
  mov rax,rsi
  sub edx,4
  ret
2:call recv_more
  jmp 1b

# pg_roundtrip rbp ctx, rdi send length -> rax 1 clean, 0 when the server reported an error.
# PG_RECV..PG_END then holds every reply message. The SQLSTATE of the first error is at PG_ERR.
.globl pg_roundtrip
pg_roundtrip:
  push rbx
  mov rsi,rdi
  mov rdi,[rbp+PG_SEND]
  call send_all
  xor eax,eax
  mov [rbp+PG_END],rax
  mov [rbp+PG_POS],rax
  mov ebx,1
1:call next_msg
  mov cl,[rax]
  cmp cl,'Z'
  je 3f
  cmp cl,'E'
  jne 1b
  test ebx,ebx
  jz 1b
  xor ebx,ebx
  # find the C field
  lea rsi,[rax+5]
2:mov cl,[rsi]
  test cl,cl
  jz 1b
  inc rsi
  cmp cl,'C'
  je 4f
5:cmp byte ptr [rsi],0
  je 6f
  inc rsi
  jmp 5b
6:inc rsi
  jmp 2b
4:mov rax,[rsi]
  mov [rbp+PG_ERR],rax
  jmp 1b
3:mov eax,ebx
  pop rbx
  ret

# put_be32 / length patching helpers for building messages at rdi
.macro MSG_BEGIN type
  mov byte ptr [rdi],\type
  lea rdx,[rdi+1]
  push rdx
  add rdi,5
.endm
.macro MSG_END
  pop rdx
  mov rax,rdi
  sub rax,rdx
  bswap eax
  mov [rdx],eax
.endm

# put_cstr rdi out, rsi NUL-terminated -> rdi past its NUL
put_cstr:
1:mov al,[rsi]
  mov [rdi],al
  inc rsi
  inc rdi
  test al,al
  jnz 1b
  ret

# scram_login rbp ctx: runs the auth exchange up to and including AuthenticationOk.
scram_login:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp,1024
  # [rsp]: nonce 24 chars; [rsp+64..]: scratch
  lea rdi,[rsp+64]
  mov esi,18
  xor edx,edx
  SYS SYS_GETRANDOM
  mov rdi,rsp
  lea rsi,[rsp+64]
  mov edx,18
  lea rcx,[rip+b64std_a]
  xor r8d,r8d
  call b64_enc
.Lauth:
  call next_msg
  mov cl,[rax]
  cmp cl,'E'
  je .Lauthbad
  cmp cl,'R'
  jne .Lauth
  mov ebx,[rax+5]
  bswap ebx
  test ebx,ebx
  jz .Lauthok
  cmp ebx,10
  je .Lsasl
  cmp ebx,11
  je .Lcont
  cmp ebx,12
  je .Lauth
.Lauthbad:
  lea rdi,[rip+m_auth]
  jmp pg_die
.Lauthok:
  add rsp,1024
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
.Lsasl:
  mov rdi,[rbp+PG_SEND]
  MSG_BEGIN 'p'
  LIT "SCRAM-SHA-256\0"
  mov dword ptr [rdi],0x20000000     # length 32, big-endian: "n,,n=,r=" + 24
  add rdi,4
  LIT "n,,n=,r="
  mov rsi,rsp
  add rsi,8                           # the MSG_BEGIN push moved rsp
  mov ecx,24
  call mcpy
  MSG_END
  mov rsi,rdi
  sub rsi,[rbp+PG_SEND]
  mov rdi,[rbp+PG_SEND]
  call send_all
  jmp .Lauth
.Lcont:
  # server-first at rax+9, length rdx-4
  lea r12,[rax+9]
  lea r13,[rdx-4]
  # parse r=,s=,i=
  mov r14,r12              # r= value
  add r14,2
  mov rdi,r14
1:cmp byte ptr [rdi],','
  je 2f
  inc rdi
  jmp 1b
2:mov r15,rdi
  sub r15,r14              # r length
  lea rsi,[rdi+3]          # s= value
  mov rbx,rsi
3:cmp byte ptr [rbx],','
  je 4f
  inc rbx
  jmp 3b
4:mov rdx,rbx
  sub rdx,rsi
  lea rdi,[rsp+64]         # salt
  lea rcx,[rip+rev_std]
  call b64_dec
  mov [rsp+40],rax         # salt length
  lea rdi,[rbx+3]          # i= value
  lea rsi,[r12+r13]
  sub rsi,rdi
  call parse_u64
  mov [rsp+48],rax         # iterations
  # password HMAC key
  lea rbx,[rsp+256]        # hmac ctx (64)
  mov rdi,[rip+g_pg_pass]
  call strlen
  mov rdx,rax
  mov rsi,rdi
  mov rdi,rbx
  call hmac_init
  # U1 = HMAC(salt || 00000001)
  mov rax,[rsp+40]
  mov dword ptr [rsp+64+rax],0x01000000
  mov rdi,rbx
  lea rsi,[rsp+64]
  lea rdx,[rax+4]
  lea rcx,[rsp+320]        # U
  call hmac
  movdqu xmm0,[rsp+320]
  movdqu xmm1,[rsp+336]
  movdqu [rsp+352],xmm0    # SaltedPassword
  movdqu [rsp+368],xmm1
  mov rax,[rsp+48]
  dec rax
  mov [rsp+48],rax
5:cmp qword ptr [rsp+48],0
  je 6f
  mov rdi,rbx
  lea rsi,[rsp+320]
  mov edx,32
  lea rcx,[rsp+320]
  call hmac
  movdqu xmm0,[rsp+320]
  movdqu xmm1,[rsp+336]
  movdqu xmm2,[rsp+352]
  movdqu xmm3,[rsp+368]
  pxor xmm2,xmm0
  pxor xmm3,xmm1
  movdqu [rsp+352],xmm2
  movdqu [rsp+368],xmm3
  dec qword ptr [rsp+48]
  jmp 5b
6:mov rdi,rbx
  lea rsi,[rsp+352]
  mov edx,32
  call hmac_init
  mov rdi,rbx
  lea rsi,[rip+s_ck]
  mov edx,10
  lea rcx,[rsp+384]        # ClientKey
  call hmac
  lea rdi,[rsp+384]
  mov esi,32
  lea rdx,[rsp+416]        # StoredKey
  call sha256
  mov rdi,rbx
  lea rsi,[rsp+416]
  mov edx,32
  call hmac_init
  # AuthMessage in the scratch buffer
  mov rdi,[rbp+PG_SCRATCH]
  LIT "n=,r="
  mov rsi,rsp
  mov ecx,24
  call mcpy
  mov byte ptr [rdi],','
  inc rdi
  mov rsi,r12
  mov rcx,r13
  call mcpy
  LIT ",c=biws,r="
  mov rsi,r14
  mov rcx,r15
  call mcpy
  mov rdx,rdi
  mov rsi,[rbp+PG_SCRATCH]
  sub rdx,rsi
  mov rdi,rbx
  lea rcx,[rsp+448]        # ClientSignature
  call hmac
  movdqu xmm0,[rsp+384]
  movdqu xmm1,[rsp+400]
  movdqu xmm2,[rsp+448]
  movdqu xmm3,[rsp+464]
  pxor xmm0,xmm2
  pxor xmm1,xmm3
  movdqu [rsp+448],xmm0
  movdqu [rsp+464],xmm1
  mov rdi,[rbp+PG_SEND]
  MSG_BEGIN 'p'
  LIT "c=biws,r="
  mov rsi,r14
  mov rcx,r15
  call mcpy
  LIT ",p="
  lea rsi,[rsp+448+8]
  mov edx,32
  lea rcx,[rip+b64std_a]
  mov r8d,1
  call b64_enc
  MSG_END
  mov rsi,rdi
  sub rsi,[rbp+PG_SEND]
  mov rdi,[rbp+PG_SEND]
  call send_all
  jmp .Lauth
.section .rodata
s_ck: .ascii "Client Key"
.text

# pg_prepare rbp ctx, rdi name (NUL-terminated), rsi sql
.globl pg_prepare
pg_prepare:
  push rbx
  mov rbx,rsi
  mov rsi,rdi
  mov rdi,[rbp+PG_SEND]
  MSG_BEGIN 'P'
  call put_cstr
  mov rsi,rbx
  call put_cstr
  mov word ptr [rdi],0
  add rdi,2
  MSG_END
  mov dword ptr [rdi],0x00000053
  mov dword ptr [rdi+1],0x04000000
  add rdi,5
  sub rdi,[rbp+PG_SEND]
  call pg_roundtrip
  test eax,eax
  jz 1f
  pop rbx
  ret
1:cmp qword ptr [rbp+P_RECOV],0
  jne 2f
  # at startup: print the server's message then die
  mov rsi,[rbp+PG_RECV]
  mov rdx,[rbp+PG_END]
  mov edi,2
  SYS SYS_WRITE
2:lea rdi,[rip+m_prep]
  jmp pg_die

# pg_connect rbp ctx: socket, startup, auth, session settings.
.globl pg_connect
pg_connect:
  push rbx
  mov edi,2
  mov esi,1
  xor edx,edx
  SYS SYS_SOCKET
  test rax,rax
  js .Lnoconn
  mov [rbp+PG_FD],rax
  # 3 s limit on connecting and on each step of the login
  sub rsp,16
  mov qword ptr [rsp],3
  mov qword ptr [rsp+8],0
  .irp opt,20,21         # SO_RCVTIMEO, SO_SNDTIMEO
  mov rdi,[rbp+PG_FD]
  mov esi,1
  mov edx,\opt
  mov r10,rsp
  mov r8d,16
  SYS SYS_SETSOCKOPT
  .endr
  add rsp,16
  mov rdi,[rbp+PG_FD]
  lea rsi,[rip+g_pg_addr]
  mov edx,16
  SYS SYS_CONNECT
  test rax,rax
  js .Lnoconn
  # TCP_NODELAY
  sub rsp,8
  mov dword ptr [rsp],1
  mov rdi,[rbp+PG_FD]
  mov esi,6
  mov edx,1
  mov r10,rsp
  mov r8d,4
  SYS SYS_SETSOCKOPT
  # TCP_RTO_MIN_US: a lost query is resent after ~RTT+30 ms, not 200 ms (ignored before Linux 6.15)
  mov rax,[rip+g_rto_us]
  test rax,rax
  jz 1f
  mov [rsp],eax
  mov rdi,[rbp+PG_FD]
  mov esi,6
  mov edx,45
  mov r10,rsp
  mov r8d,4
  SYS SYS_SETSOCKOPT
1:add rsp,8
  # startup
  mov rdi,[rbp+PG_SEND]
  add rdi,4
  mov dword ptr [rdi],0x00000300
  add rdi,4
  LIT "user\0"
  mov rsi,[rip+g_pg_user]
  call put_cstr
  LIT "database\0"
  mov rsi,[rip+g_pg_db]
  call put_cstr
  LIT "client_encoding\0UTF8\0\0"
  mov rsi,rdi
  sub rsi,[rbp+PG_SEND]
  mov eax,esi
  bswap eax
  mov rdx,[rbp+PG_SEND]
  mov [rdx],eax
  mov rdi,rdx
  call send_all
  xor eax,eax
  mov [rbp+PG_END],rax
  mov [rbp+PG_POS],rax
  call scram_login
1:call next_msg
  cmp byte ptr [rax],'Z'
  jne 1b
  # session settings
  mov rdi,[rbp+PG_SEND]
  MSG_BEGIN 'Q'
  lea rsi,[rip+s_session]
  call put_cstr
  MSG_END
  sub rdi,[rbp+PG_SEND]
  call pg_roundtrip
  pop rbx
  ret
.Lnoconn:
  lea rdi,[rip+m_conn]
  jmp pg_die

# bind_begin rdi out, rsi statement name -> rdi at the parameter count; pushes nothing. Helpers for db.s:
# bind_begin writes: 'B' len "" name fmt(1:text) nparams(from dx)
.globl bind_begin
bind_begin:
  mov byte ptr [rdi],'B'
  add rdi,5
  mov byte ptr [rdi],0
  inc rdi
  call put_cstr
  mov dword ptr [rdi],0x00000100     # one param format code: text
  add rdi,4
  ret

# ctx allocation: new_ctx -> rax
.globl new_ctx
new_ctx:
  push rbx
  mov rdi,PG_SIZE
  call alloc
  mov rbx,rax
  mov rdi,RECV_CAP
  call mmap_anon
  mov [rbx+PG_RECV],rax
  mov rdi,SEND_CAP
  call mmap_anon
  mov [rbx+PG_SEND],rax
  mov rdi,SCRATCH_CAP
  call mmap_anon
  mov [rbx+PG_SCRATCH],rax
  mov rdi,RING_N*32
  call mmap_anon
  mov [rbx+P_RING],rax
  xor eax,eax
  .irp f,P_HEAD,P_TAIL,P_SLEN,P_SOFF,P_BATCH,P_SKIP,P_WOUT,P_NROW,PG_END,PG_POS,P_DEAD,P_DOWN,P_RECOV,P_NDONE,P_INFL,P_RXT,P_WAIT,P_STALL
  mov [rbx+\f],rax
  .endr
  mov rax,rbx
  pop rbx
  ret
