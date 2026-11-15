# Host names without libc: a dotted quad, then /etc/hosts, then an A query to each nameserver in /etc/resolv.conf.
.include "defs.inc"
.set SYS_OPEN,2
.set BUF,0x100000
.section .rodata
p_hosts: .asciz "/etc/hosts"
p_resolv: .asciz "/etc/resolv.conf"
s_ns: .ascii "nameserver"
s_search: .ascii "search"
s_domain: .ascii "domain"
s_localhost: .asciz "localhost"
m_host: .asciz "cannot resolve the host in DATABASE_URL"
.text

# ipv4_parse rdi NUL-terminated -> eax address in network order, edx 1 ok / 0 not a dotted quad. Keeps rdi.
.globl ipv4_parse
ipv4_parse:
  mov rsi,rdi
  xor r8d,r8d            # octets done
  xor r9d,r9d            # address
1:xor eax,eax
  xor r10d,r10d          # digits
2:movzx ecx,byte ptr [rsi]
  sub ecx,'0'
  cmp ecx,9
  ja 3f
  imul eax,eax,10
  add eax,ecx
  cmp eax,255
  ja 8f
  inc rsi
  inc r10d
  jmp 2b
3:test r10d,r10d
  jz 8f
  mov ecx,r8d
  shl ecx,3
  shl eax,cl
  or r9d,eax
  inc r8d
  movzx eax,byte ptr [rsi]
  cmp r8d,4
  je 4f
  cmp al,'.'
  jne 8f
  inc rsi
  jmp 1b
4:test al,al
  jnz 8f
  mov eax,r9d
  mov edx,1
  ret
8:xor eax,eax
  xor edx,edx
  ret

# read_file rdi path, rsi buffer -> rax bytes (0 when unreadable), NUL-terminated.
read_file:
  push rbx
  push r12
  mov r12,rsi
  xor esi,esi
  xor edx,edx
  SYS SYS_OPEN
  test rax,rax
  js 8f
  mov rbx,rax
  mov rdi,rbx
  mov rsi,r12
  mov edx,BUF-1
  SYS SYS_READ
  push rax
  mov rdi,rbx
  SYS SYS_CLOSE
  pop rax
  test rax,rax
  jns 9f
8:xor eax,eax
9:mov byte ptr [r12+rax],0
  pop r12
  pop rbx
  ret

# tok rsi cursor -> rax token start, rdx token length, rsi past it. Stays on the line; length 0 at its end or a comment.
tok:
1:mov al,[rsi]
  cmp al,' '
  je 2f
  cmp al,9
  je 2f
  cmp al,13
  je 2f
  jmp 3f
2:inc rsi
  jmp 1b
3:mov rax,rsi
4:mov cl,[rsi]
  test cl,cl
  jz 5f
  cmp cl,10
  je 5f
  cmp cl,13
  je 5f
  cmp cl,' '
  je 5f
  cmp cl,9
  je 5f
  cmp cl,'#'
  je 5f
  inc rsi
  jmp 4b
5:mov rdx,rsi
  sub rdx,rax
  ret

# next_line rsi -> rsi at the start of the next line, or at the NUL.
next_line:
1:mov al,[rsi]
  test al,al
  jz 2f
  inc rsi
  cmp al,10
  jne 1b
2:ret

# name_eq rax token, rdx length, rdi NUL-terminated name -> ZF set when equal, ignoring case. Keeps rax, rdx, rdi, rsi.
name_eq:
  push rsi
  xor ecx,ecx
1:cmp rcx,rdx
  je 3f
  movzx esi,byte ptr [rax+rcx]
  movzx r8d,byte ptr [rdi+rcx]
  test r8d,r8d
  jz 5f
  or esi,0x20
  or r8d,0x20
  cmp esi,r8d
  jne 4f
  inc rcx
  jmp 1b
3:cmp byte ptr [rdi+rcx],0
4:pop rsi
  ret
5:test esp,esp           # the name ended first: not equal
  pop rsi
  ret

# hosts_lookup rdi name, rsi buffer -> eax address, edx 1 found / 0 not.
hosts_lookup:
  push rbx
  push r12
  push r13
  sub rsp,32
  mov r12,rdi
  mov r13,rsi
  lea rdi,[rip+p_hosts]
  call read_file
  mov rsi,r13
.Lh_line:
  cmp byte ptr [rsi],0
  je .Lh_no
  call tok
  test rdx,rdx
  jz .Lh_skip
  cmp rdx,15
  ja .Lh_skip
  # the address, copied so it can be terminated
  xor ecx,ecx
1:mov r8b,[rax+rcx]
  mov [rsp+rcx],r8b
  inc rcx
  cmp rcx,rdx
  jb 1b
  mov byte ptr [rsp+rcx],0
  mov rdi,rsp
  push rsi
  call ipv4_parse
  pop rsi
  test edx,edx
  jz .Lh_skip
  mov ebx,eax
2:call tok
  test rdx,rdx
  jz .Lh_skip
  mov rdi,r12
  call name_eq
  jne 2b
  mov eax,ebx
  mov edx,1
  jmp .Lh_end
.Lh_skip:
  call next_line
  jmp .Lh_line
.Lh_no:
  xor eax,eax
  xor edx,edx
.Lh_end:
  add rsp,32
  pop r13
  pop r12
  pop rbx
  ret

# skip_dns_name rsi cursor, rdi end -> rsi past the name; CF set when it runs out.
skip_dns_name:
1:cmp rsi,rdi
  jae 8f
  movzx eax,byte ptr [rsi]
  cmp eax,0xc0
  jae 2f
  inc rsi
  test eax,eax
  jz 3f
  add rsi,rax
  jmp 1b
2:add rsi,2
3:cmp rdi,rsi
  ret                    # CF set if the name ran past the end
8:stc
  ret

# dns_ask edi server address, rsi name -> eax address, edx 1 found / 0 not.
dns_ask:
  push rbx
  push r12
  push r13
  push r14
  sub rsp,2048
  mov r12d,edi
  mov r13,rsi
  # the question: [rsp+16..]
  lea rdi,[rsp+16]
  mov esi,2
  xor edx,edx
  SYS SYS_GETRANDOM
  lea rdi,[rsp+16]
  mov dword ptr [rdi+2],0x01000001   # recursion desired; one question
  mov byte ptr [rdi+6],0
  mov dword ptr [rdi+7],0
  mov byte ptr [rdi+11],0
  lea rdi,[rsp+16+12]
  mov rsi,r13
1:lea r8,[rdi+1]          # one label
  xor ecx,ecx
2:mov al,[rsi]
  test al,al
  jz 3f
  cmp al,'.'
  je 3f
  mov [r8+rcx],al
  inc rsi
  inc ecx
  cmp ecx,63
  ja .Ld_no0
  jmp 2b
3:test ecx,ecx
  jz .Ld_no0
  mov [rdi],cl
  lea rdi,[r8+rcx]
  lea rax,[rsp+16+255]
  cmp rdi,rax
  ja .Ld_no0
  cmp byte ptr [rsi],0
  je 4f
  inc rsi
  cmp byte ptr [rsi],0   # a trailing dot
  jne 1b
4:mov byte ptr [rdi],0
  mov dword ptr [rdi+1],0x01000100   # type A, class IN
  add rdi,5
  mov r14,rdi
  lea rax,[rsp+16]
  sub r14,rax            # question length
  # the socket
  mov edi,2
  mov esi,2
  xor edx,edx
  SYS SYS_SOCKET
  test rax,rax
  js .Ld_no0
  mov ebx,eax
  mov qword ptr [rsp],2
  mov qword ptr [rsp+8],0
  mov edi,ebx
  mov esi,1
  mov edx,20             # SO_RCVTIMEO
  mov r10,rsp
  mov r8d,16
  SYS SYS_SETSOCKOPT
  mov word ptr [rsp],2
  mov word ptr [rsp+2],0x3500
  mov [rsp+4],r12d
  mov qword ptr [rsp+8],0
  mov edi,ebx
  mov rsi,rsp
  mov edx,16
  SYS SYS_CONNECT
  test rax,rax
  js .Ld_no
  mov r13d,2             # tries
.Ld_send:
  mov edi,ebx
  lea rsi,[rsp+16]
  mov rdx,r14
  SYS SYS_WRITE
  test rax,rax
  js .Ld_no
.Ld_recv:
  mov edi,ebx
  lea rsi,[rsp+512]
  mov edx,1500
  SYS SYS_READ
  cmp rax,12
  jge 5f
  dec r13d
  jnz .Ld_send
  jmp .Ld_no
5:lea rsi,[rsp+512]
  lea rdi,[rsi+rax]
  mov ax,[rsi]
  cmp ax,[rsp+16]
  jne .Ld_recv           # someone else's answer
  test byte ptr [rsi+2],0x80
  jz .Ld_recv
  test byte ptr [rsi+3],0x0f
  jnz .Ld_no             # an error, such as no such name
  movzx r8d,word ptr [rsi+4]
  rol r8w,8
  movzx r9d,word ptr [rsi+6]
  rol r9w,8
  add rsi,12
6:test r8d,r8d
  jz 7f
  call skip_dns_name
  jc .Ld_no
  add rsi,4
  dec r8d
  jmp 6b
7:test r9d,r9d
  jz .Ld_no
  call skip_dns_name
  jc .Ld_no
  lea rax,[rsi+10]
  cmp rax,rdi
  ja .Ld_no
  movzx ecx,word ptr [rsi+8]
  rol cx,8
  add rax,rcx
  cmp rax,rdi
  ja .Ld_no
  # type A, class IN, four bytes
  cmp dword ptr [rsi],0x01000100
  jne 8f
  cmp ecx,4
  jne 8f
  mov eax,[rsi+10]
  mov r12d,eax
  mov edi,ebx
  SYS SYS_CLOSE
  mov eax,r12d
  mov edx,1
  jmp .Ld_end
8:lea rsi,[rsi+rcx+10]   # a CNAME or similar: the address follows
  dec r9d
  jmp 7b
.Ld_no:
  mov edi,ebx
  SYS SYS_CLOSE
.Ld_no0:
  xor eax,eax
  xor edx,edx
.Ld_end:
  add rsp,2048
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

# dns_lookup rdi name, rsi buffer -> eax address, edx 1 found / 0 not. Tries each nameserver.
dns_lookup:
  push rbx
  push r12
  push r13
  sub rsp,32
  mov r12,rdi
  mov r13,rsi
  lea rdi,[rip+p_resolv]
  call read_file
  mov rsi,r13
.Ldl_line:
  cmp byte ptr [rsi],0
  je .Ldl_no
  call tok
  cmp rdx,10
  jne .Ldl_skip
  lea rdi,[rip+s_ns]
  mov ecx,10
  xor r8d,r8d
1:mov r9b,[rax+r8]
  cmp r9b,[rdi+r8]
  jne .Ldl_skip
  inc r8
  cmp r8,rcx
  jb 1b
  call tok
  test rdx,rdx
  jz .Ldl_skip
  cmp rdx,15
  ja .Ldl_skip
  xor ecx,ecx
2:mov r8b,[rax+rcx]
  mov [rsp+rcx],r8b
  inc rcx
  cmp rcx,rdx
  jb 2b
  mov byte ptr [rsp+rcx],0
  mov rdi,rsp
  push rsi
  call ipv4_parse
  pop rsi
  test edx,edx
  jz .Ldl_skip
  mov rbx,rsi
  mov edi,eax
  mov rsi,r12
  call dns_ask
  mov rsi,rbx
  test edx,edx
  jnz .Ldl_end
.Ldl_skip:
  call next_line
  jmp .Ldl_line
.Ldl_no:
  xor eax,eax
  xor edx,edx
.Ldl_end:
  add rsp,32
  pop r13
  pop r12
  pop rbx
  ret

# pg_resolve_host rdi name: sets the address in g_pg_addr, or ends the process.
.globl pg_resolve_host
pg_resolve_host:
  push rbx
  push r12
  mov rbx,rdi
  call ipv4_parse
  test edx,edx
  jnz .Lr_ok
  mov edi,BUF
  call mmap_anon
  mov r12,rax
  mov rdi,rbx
  mov rsi,r12
  call hosts_lookup
  test edx,edx
  jnz .Lr_ok
  # localhost works even without an /etc/hosts entry
  mov rdi,rbx
  call strlen
  mov rdx,rax
  mov rax,rbx
  lea rdi,[rip+s_localhost]
  call name_eq
  mov eax,0x0100007f
  je .Lr_ok
  mov rdi,rbx
  mov rsi,r12
  call dns_lookup
  test edx,edx
  jnz .Lr_ok
  lea rdi,[rip+m_host]
  jmp die
.Lr_ok:
  mov [rip+g_pg_addr+4],eax
  pop r12
  pop rbx
  ret
