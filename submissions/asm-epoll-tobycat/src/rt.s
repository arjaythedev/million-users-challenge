# Runtime: memory copy, allocator, locks, threads, time, integer formatting, env.
.include "defs.inc"
.text

# mcpy: rdi dst, rsi src, ecx len. Returns rdi advanced. Clobbers rax, rcx, rsi, xmm0.
.globl mcpy
mcpy:
  mov eax,ecx
  cmp rcx,16
  jb 2f
1:movdqu xmm0,[rsi]
  movdqu [rdi],xmm0
  add rsi,16
  add rdi,16
  sub rcx,16
  cmp rcx,16
  jae 1b
  # tail: last 16 bytes, overlapping what was written
  test rcx,rcx
  jz 9f
  movdqu xmm0,[rsi+rcx-16]
  movdqu [rdi+rcx-16],xmm0
  add rdi,rcx
9:ret
2:test ecx,ecx
  jz 9b
  mov rax,rcx
  rep movsb
  ret

# spin_lock rdi: preserves everything but rax, rcx, r11.
.globl spin_lock
spin_lock:
  mov eax,1
  xchg eax,[rdi]
  test eax,eax
  jnz 1f
  ret
1:mov ecx,2000
2:pause
  cmp dword ptr [rdi],0
  je spin_lock
  dec ecx
  jnz 2b
  # Still held, perhaps by a preempted low-priority thread: spinning or yielding at higher priority would keep it off
  # the CPU, so sleep. Unlocking is a plain store with no wake, hence the 1 ms timeout.
  push rdi
  push rsi
  push rdx
  push r10
  push r8
  push r9
  sub rsp,16
  mov qword ptr [rsp],0
  mov qword ptr [rsp+8],1000000
  mov esi,128            # FUTEX_WAIT|PRIVATE
  mov edx,1              # only while it still reads as held
  mov r10,rsp
  xor r8d,r8d
  xor r9d,r9d
  SYS SYS_FUTEX
  add rsp,16
  pop r9
  pop r8
  pop r10
  pop rdx
  pop rsi
  pop rdi
  jmp spin_lock

# ---- allocator: power-of-two classes with a 16-byte header, one lock per class.
.set A_MINORD,6
.set A_MAXORD,34
.set A_CHUNK,0x4000000
.set A_DIRECT,24      # orders at or above this get their own mapping
.bss
.balign 64
a_class:  .skip 64*(A_MAXORD+1)     # [0]=lock, [8]=freelist head
a_bumplk: .skip 64
a_bump:   .skip 8
a_bumpend:.skip 8
.text

# mmap_anon rdi size -> rax. Exits on failure. Clobbers rcx, r11, rsi, rdx, r8-r10.
.globl mmap_anon
mmap_anon:
  mov rsi,rdi
  xor edi,edi
  mov edx,3
  mov r10d,0x4022      # PRIVATE|ANONYMOUS|NORESERVE
  mov r8,-1
  xor r9d,r9d
  SYS SYS_MMAP
  cmp rax,-4096
  ja fatal_oom
  ret
fatal_oom:
  lea rdi,[rip+oom_msg]
  jmp die
.section .rodata
oom_msg: .asciz "out of memory"
.text

# alloc rdi size -> rax. Preserves rbx, rbp, r12-r15, rdi not preserved.
.globl alloc
alloc:
  lea rcx,[rdi+15]
  bsr rcx,rcx
  inc ecx                 # 2^ecx >= size+16
  cmp ecx,A_MINORD
  jae 1f
  mov ecx,A_MINORD
1:mov edx,ecx             # order
  imul r8d,edx,64
  lea r9,[rip+a_class]
  add r9,r8               # class
  mov rdi,r9
  push rdx
  call spin_lock
  pop rdx
  mov rax,[r9+8]
  test rax,rax
  jz 2f
  mov rcx,[rax+16]        # next, stored in the free block's payload
  mov [r9+8],rcx
  mov dword ptr [r9],0
  add rax,16
  ret
2:mov dword ptr [r9],0
  cmp edx,A_DIRECT
  jae 4f
  lea rdi,[rip+a_bumplk]
  push rdx
  call spin_lock
  pop rdx
  mov ecx,edx
  mov esi,1
  shl rsi,cl
  mov rax,[rip+a_bump]
  lea r8,[rax+rsi]
  test rax,rax
  jz 3f
  cmp r8,[rip+a_bumpend]
  jbe 5f
3:push rdx
  mov rdi,A_CHUNK
  call mmap_anon
  pop rdx
  mov ecx,edx
  mov esi,1
  shl rsi,cl
  mov [rip+a_bumpend],rax
  mov r8,A_CHUNK
  add [rip+a_bumpend],r8
  lea r8,[rax+rsi]
5:mov [rip+a_bump],r8
  mov dword ptr [rip+a_bumplk],0
  mov [rax],rdx
  add rax,16
  ret
4:push rdx
  mov ecx,edx
  mov edi,1
  shl rdi,cl
  call mmap_anon
  pop rdx
  mov [rax],rdx
  add rax,16
  ret

# free rdi ptr (0 is a no-op). Preserves rbx, rbp, r12-r15.
.globl free
free:
  test rdi,rdi
  jz 1f
  lea rsi,[rdi-16]
  mov rdx,[rsi]
  imul edx,edx,64
  lea rdi,[rip+a_class]
  add rdi,rdx
  call spin_lock
  mov rax,[rdi+8]
  mov [rsi+16],rax
  mov [rdi+8],rsi
  mov dword ptr [rdi],0
1:ret

# spawn rdi fn, rsi arg: starts a thread on a fresh stack; fn receives arg in rdi.
.globl spawn
spawn:
  push rdi
  push rsi
  mov rdi,0x200000
  call mmap_anon
  pop rsi
  pop rdx
  lea r8,[rax+0x200000-16]
  mov [r8],rdx
  mov [r8+8],rsi
  mov edi,0x50F00      # VM|FS|FILES|SIGHAND|THREAD|SYSVSEM
  mov rsi,r8
  xor edx,edx
  xor r10d,r10d
  xor r8d,r8d
  SYS SYS_CLONE
  test rax,rax
  jz 1f
  js spawn_fail
  ret
1:pop rax
  pop rdi
  call rax
  xor edi,edi
  SYS SYS_EXIT
spawn_fail:
  lea rdi,[rip+clone_msg]
  jmp die
.section .rodata
clone_msg: .asciz "clone failed"
.text

# futex_wait rdi addr, esi expected, rdx timeout-ms (or -1)
.globl futex_wait
futex_wait:
  sub rsp,16
  xor r10d,r10d
  cmp rdx,0
  jl 1f
  mov rax,rdx
  xor edx,edx
  mov ecx,1000
  div rcx
  mov [rsp],rax
  imul rdx,rdx,1000000
  mov [rsp+8],rdx
  mov r10,rsp
1:mov edx,esi
  mov esi,128          # FUTEX_WAIT|PRIVATE
  xor r8d,r8d
  xor r9d,r9d
  SYS SYS_FUTEX
  add rsp,16
  ret

# futex_wake rdi addr, esi count
.globl futex_wake
futex_wake:
  mov edx,esi
  mov esi,129
  xor r10d,r10d
  SYS SYS_FUTEX
  ret

# now_ms -> rax monotonic ms
.globl now_ms
now_ms:
  sub rsp,16
  mov edi,1
  mov rsi,rsp
  SYS SYS_CLOCK_GETTIME
  mov rax,[rsp]
  imul rax,rax,1000
  mov rcx,rax
  mov rax,[rsp+8]
  xor edx,edx
  mov esi,1000000
  div rsi
  add rax,rcx
  add rsp,16
  ret

# now_s -> rax realtime seconds
.globl now_s
now_s:
  sub rsp,16
  xor edi,edi
  mov rsi,rsp
  SYS SYS_CLOCK_GETTIME
  mov rax,[rsp]
  add rsp,16
  ret

# put_u64 rdi out, rsi value -> rdi advanced. Clobbers rax, rcx, rdx, rsi, r8.
.globl put_u64
put_u64:
  mov rax,rsi
  mov r8,rsp
  mov rcx,10
1:xor edx,edx
  div rcx
  add dl,'0'
  dec rsp
  mov [rsp],dl
  test rax,rax
  jnz 1b
2:mov al,[rsp]
  inc rsp
  mov [rdi],al
  inc rdi
  cmp rsp,r8
  jne 2b
  ret

# parse_u64 rdi ptr, rsi len -> rax value, rdx 1 ok / 0 bad (empty, non-digit, or over 2^63).
.globl parse_u64
parse_u64:
  xor eax,eax
  xor edx,edx
  test rsi,rsi
  jz 3f
  cmp rsi,18
  ja 3f
1:movzx ecx,byte ptr [rdi]
  sub ecx,'0'
  cmp ecx,9
  ja 3f
  imul rax,rax,10
  add rax,rcx
  inc rdi
  dec rsi
  jnz 1b
  mov edx,1
3:ret

# getenv rdi name (NUL-terminated) -> rax value ptr or 0.
.globl getenv
getenv:
  mov r8,[rip+g_envp]
1:mov r9,[r8]
  test r9,r9
  jz 8f
  add r8,8
  mov rsi,rdi
2:mov al,[rsi]
  test al,al
  jz 3f
  cmp al,[r9]
  jne 1b
  inc rsi
  inc r9
  jmp 2b
3:cmp byte ptr [r9],'='
  jne 1b
  lea rax,[r9+1]
  ret
8:xor eax,eax
  ret

# env_int rdi name, rsi default -> rax
.globl env_int
env_int:
  push rsi
  call getenv
  pop rdx
  test rax,rax
  jz 2f
  mov r8,rax
  xor eax,eax
1:movzx ecx,byte ptr [r8]
  sub ecx,'0'
  cmp ecx,9
  ja 3f
  imul rax,rax,10
  add rax,rcx
  inc r8
  jmp 1b
2:mov rax,rdx
3:ret

# strlen rdi -> rax. Preserves rdi.
.globl strlen
strlen:
  xor eax,eax
1:cmp byte ptr [rdi+rax],0
  je 2f
  inc rax
  jmp 1b
2:ret

# die rdi msg: prints and exits the process.
.globl die
die:
  mov rbx,rdi
  call strlen
  mov rdx,rax
  mov rsi,rbx
  mov edi,2
  SYS SYS_WRITE
  mov edi,2
  lea rsi,[rip+nl]
  mov edx,1
  SYS SYS_WRITE
  mov edi,1
  SYS SYS_EXIT_GROUP
.section .rodata
nl: .ascii "\n"
.bss
.globl g_envp
g_envp: .skip 8
