# SHA-256, HMAC, base64 and JWT HS256.
.include "defs.inc"

.section .rodata
.balign 64
sha_k:
.long 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
.long 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
.long 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
.long 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
.long 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
.long 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070
.long 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
.long 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
sha_init:
.long 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
b64url_a: .ascii "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
.globl b64std_a
b64std_a: .ascii "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
.globl b64bc_a
b64bc_a:  .ascii "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
jwt_head: .ascii "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
.set jwt_head_len,.-jwt_head
k_alg: .ascii "alg"
k_sub: .ascii "sub"
k_username: .ascii "username"
k_exp: .ascii "exp"
.bss
.balign 64
g_jwt_hmac: .skip 64
.globl rev_url
rev_url: .skip 256
.globl rev_std
rev_std: .skip 256
.globl rev_bc
rev_bc:  .skip 256
.text

.set i,0
.macro SHA_ROUND a,b,c,d,e,f,g,h
  .if i>=16
    mov r12d,[rsp+((i+1)&15)*4]
    mov r13d,r12d
    mov r14d,r12d
    ror r12d,7
    ror r13d,18
    shr r14d,3
    xor r12d,r13d
    xor r12d,r14d
    mov r13d,[rsp+((i+14)&15)*4]
    mov r14d,r13d
    mov r15d,r13d
    ror r13d,17
    ror r14d,19
    shr r15d,10
    xor r13d,r14d
    xor r13d,r15d
    add r12d,r13d
    add r12d,[rsp+((i+9)&15)*4]
    add [rsp+(i&15)*4],r12d
  .endif
  mov r12d,\e
  mov r13d,\e
  mov r14d,\e
  ror r12d,6
  ror r13d,11
  ror r14d,25
  xor r12d,r13d
  xor r12d,r14d
  mov r13d,\f
  xor r13d,\g
  and r13d,\e
  xor r13d,\g
  add r12d,r13d
  add r12d,[rsp+(i&15)*4]
  add r12d,dword ptr [sha_k+i*4]
  add \h,r12d
  add \d,\h
  mov r12d,\a
  mov r13d,\a
  mov r14d,\a
  ror r12d,2
  ror r13d,13
  ror r14d,22
  xor r12d,r13d
  xor r12d,r14d
  mov r13d,\a
  or r13d,\b
  and r13d,\c
  mov r14d,\a
  and r14d,\b
  or r13d,r14d
  add r12d,r13d
  add \h,r12d
  .set i,i+1
.endm

# sha256_block rdi state, rsi 64-byte block. Preserves rbx, rbp, r12-r15, rdi, rsi.
.globl sha256_block
sha256_block:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  push rdi
  push rsi
  sub rsp,64
  .set n,0
  .rept 16
    mov eax,[rsi+n*4]
    bswap eax
    mov [rsp+n*4],eax
    .set n,n+1
  .endr
  mov eax,[rdi]
  mov ebx,[rdi+4]
  mov ecx,[rdi+8]
  mov edx,[rdi+12]
  mov r8d,[rdi+16]
  mov r9d,[rdi+20]
  mov r10d,[rdi+24]
  mov r11d,[rdi+28]
  .set i,0
  .rept 8
    SHA_ROUND eax,ebx,ecx,edx,r8d,r9d,r10d,r11d
    SHA_ROUND r11d,eax,ebx,ecx,edx,r8d,r9d,r10d
    SHA_ROUND r10d,r11d,eax,ebx,ecx,edx,r8d,r9d
    SHA_ROUND r9d,r10d,r11d,eax,ebx,ecx,edx,r8d
    SHA_ROUND r8d,r9d,r10d,r11d,eax,ebx,ecx,edx
    SHA_ROUND edx,r8d,r9d,r10d,r11d,eax,ebx,ecx
    SHA_ROUND ecx,edx,r8d,r9d,r10d,r11d,eax,ebx
    SHA_ROUND ebx,ecx,edx,r8d,r9d,r10d,r11d,eax
  .endr
  add rsp,64
  pop rsi
  pop rdi
  add [rdi],eax
  add [rdi+4],ebx
  add [rdi+8],ecx
  add [rdi+12],edx
  add [rdi+16],r8d
  add [rdi+20],r9d
  add [rdi+24],r10d
  add [rdi+28],r11d
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

# sha_finish rdi state, rsi data, rdx len, rcx bytes already absorbed (multiple of 64), r8 out(32).
# Absorbs data with padding and writes the digest. Used for one-shot and midstate hashing.
.globl sha_finish
sha_finish:
  push rbx
  push r12
  push r13
  push r14
  sub rsp,128
  mov r12,rdi
  mov rbx,rsi
  mov r13,rdx
  lea r14,[rcx+rdx]     # total
  push r8
1:cmp r13,64
  jb 2f
  mov rdi,r12
  mov rsi,rbx
  call sha256_block
  add rbx,64
  sub r13,64
  jmp 1b
2:lea rdi,[rsp+8]
  mov rsi,rbx
  mov ecx,r13d
  call mcpy
  mov byte ptr [rdi],0x80
  inc rdi
  # zero to the end of two blocks
  lea rcx,[rsp+8+128]
  sub rcx,rdi
  xor eax,eax
  rep stosb
  shl r14,3
  bswap r14
  cmp r13,56
  jb 3f
  mov [rsp+8+120],r14
  mov rdi,r12
  lea rsi,[rsp+8]
  call sha256_block
  mov rdi,r12
  lea rsi,[rsp+8+64]
  call sha256_block
  jmp 4f
3:mov [rsp+8+56],r14
  mov rdi,r12
  lea rsi,[rsp+8]
  call sha256_block
4:pop r8
  xor ecx,ecx
5:mov eax,[r12+rcx*4]
  bswap eax
  mov [r8+rcx*4],eax
  inc ecx
  cmp ecx,8
  jne 5b
  add rsp,128
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

# sha256 rdi data, rsi len, rdx out
.globl sha256
sha256:
  sub rsp,32
  movdqu xmm0,[rip+sha_init]
  movdqu xmm1,[rip+sha_init+16]
  movdqu [rsp],xmm0
  movdqu [rsp+16],xmm1
  mov r8,rdx
  mov rdx,rsi
  mov rsi,rdi
  mov rdi,rsp
  xor ecx,ecx
  call sha_finish
  add rsp,32
  ret

# hmac_init rdi ctx(64: inner state, outer state), rsi key, rdx keylen
.globl hmac_init
hmac_init:
  push rbx
  push r12
  sub rsp,64+32
  mov rbx,rdi
  cmp rdx,64
  jbe 1f
  mov rdi,rsi
  mov rsi,rdx
  lea rdx,[rsp+64]
  call sha256
  lea rsi,[rsp+64]
  mov edx,32
1:mov r12,rdx
  mov rdi,rsp
  mov ecx,64
  xor eax,eax
  push rdi
  rep stosb
  pop rdi
  mov rcx,r12
  rep movsb
  # ipad
  xor ecx,ecx
2:xor byte ptr [rsp+rcx],0x36
  inc ecx
  cmp ecx,64
  jne 2b
  movdqu xmm0,[rip+sha_init]
  movdqu xmm1,[rip+sha_init+16]
  movdqu [rbx],xmm0
  movdqu [rbx+16],xmm1
  mov rdi,rbx
  mov rsi,rsp
  call sha256_block
  xor ecx,ecx
3:xor byte ptr [rsp+rcx],0x36^0x5c
  inc ecx
  cmp ecx,64
  jne 3b
  movdqu xmm0,[rip+sha_init]
  movdqu xmm1,[rip+sha_init+16]
  movdqu [rbx+32],xmm0
  movdqu [rbx+48],xmm1
  lea rdi,[rbx+32]
  mov rsi,rsp
  call sha256_block
  add rsp,64+32
  pop r12
  pop rbx
  ret

# hmac rdi ctx, rsi data, rdx len, rcx out(32). out may overlap data.
.globl hmac
hmac:
  push rbx
  push r12
  sub rsp,40
  mov rbx,rdi
  mov r12,rcx
  movdqu xmm0,[rbx]
  movdqu xmm1,[rbx+16]
  movdqu [rsp],xmm0
  movdqu [rsp+16],xmm1
  mov rdi,rsp
  mov ecx,64
  mov r8,r12
  call sha_finish
  movdqu xmm0,[rbx+32]
  movdqu xmm1,[rbx+48]
  movdqu [rsp],xmm0
  movdqu [rsp+16],xmm1
  mov rdi,rsp
  mov rsi,r12
  mov edx,32
  mov ecx,64
  mov r8,r12
  call sha_finish
  add rsp,40
  pop r12
  pop rbx
  ret

# b64_enc rdi dst, rsi src, rdx len, rcx alphabet, r8 pad(1/0) -> rdi advanced. Preserves rbx, rbp, r12-r15.
.globl b64_enc
b64_enc:
1:cmp rdx,3
  jb 2f
  movzx eax,byte ptr [rsi]
  shl eax,8
  movzx r9d,byte ptr [rsi+1]
  or eax,r9d
  shl eax,8
  movzx r9d,byte ptr [rsi+2]
  or eax,r9d
  mov r9d,eax
  shr r9d,18
  mov r9b,[rcx+r9]
  mov [rdi],r9b
  mov r9d,eax
  shr r9d,12
  and r9d,63
  mov r9b,[rcx+r9]
  mov [rdi+1],r9b
  mov r9d,eax
  shr r9d,6
  and r9d,63
  mov r9b,[rcx+r9]
  mov [rdi+2],r9b
  and eax,63
  mov al,[rcx+rax]
  mov [rdi+3],al
  add rsi,3
  add rdi,4
  sub rdx,3
  jmp 1b
2:test rdx,rdx
  jz 9f
  movzx eax,byte ptr [rsi]
  shl eax,16
  cmp rdx,2
  jne 3f
  movzx r9d,byte ptr [rsi+1]
  shl r9d,8
  or eax,r9d
3:mov r9d,eax
  shr r9d,18
  mov r9b,[rcx+r9]
  mov [rdi],r9b
  mov r9d,eax
  shr r9d,12
  and r9d,63
  mov r9b,[rcx+r9]
  mov [rdi+1],r9b
  add rdi,2
  cmp rdx,2
  jne 4f
  shr eax,6
  and eax,63
  mov al,[rcx+rax]
  mov [rdi],al
  inc rdi
4:test r8,r8
  jz 9f
  mov r9,3
  sub r9,rdx
5:mov byte ptr [rdi],'='
  inc rdi
  dec r9
  jnz 5b
9:ret

# b64_dec rdi dst, rsi src, rdx len, rcx reverse table -> rax decoded length, or -1 on a bad char.
# Stops at '='.
.globl b64_dec
b64_dec:
  push rdi
  xor r8d,r8d          # accumulator
  xor r9d,r9d          # bits
1:test rdx,rdx
  jz 8f
  movzx eax,byte ptr [rsi]
  inc rsi
  dec rdx
  cmp al,'='
  je 8f
  movzx eax,byte ptr [rcx+rax]
  cmp al,0xff
  je 7f
  shl r8d,6
  or r8d,eax
  add r9d,6
  cmp r9d,8
  jb 1b
  sub r9d,8
  mov eax,r8d
  push rcx
  mov ecx,r9d
  shr eax,cl
  pop rcx
  mov [rdi],al
  inc rdi
  jmp 1b
7:pop rdi
  mov rax,-1
  ret
8:mov rax,rdi
  pop rdi
  sub rax,rdi
  ret

# b64_rev rdi table(256), rsi alphabet
.globl b64_rev
b64_rev:
  mov ecx,256
  mov al,0xff
  push rdi
  rep stosb
  pop rdi
  xor ecx,ecx
1:movzx eax,byte ptr [rsi+rcx]
  mov [rdi+rax],cl
  inc ecx
  cmp ecx,64
  jne 1b
  ret

# jwt_init rdi secret, rsi len
.globl jwt_init
jwt_init:
  mov rdx,rsi
  mov rsi,rdi
  lea rdi,[rip+g_jwt_hmac]
  call hmac_init
  lea rdi,[rip+rev_url]
  lea rsi,[rip+b64url_a]
  call b64_rev
  lea rdi,[rip+rev_std]
  lea rsi,[rip+b64std_a]
  call b64_rev
  lea rdi,[rip+rev_bc]
  lea rsi,[rip+b64bc_a]
  jmp b64_rev

# jwt_verify rdi token, rsi len, rdx now(s), rcx username out -> rax user id > 0, 0 invalid, -1 bad payload; rdx username length.
.globl jwt_verify
jwt_verify:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp,1192
  mov rbx,rdi
  mov r12,rsi
  mov [rsp+1184],rdx
  mov [rsp+1176],rcx
  cmp r12,1024
  ja .Lbad
  # find the two dots
  mov al,'.'
  mov rcx,r12
  mov rdi,rbx
  repne scasb
  jne .Lbad
  mov r13,rdi            # payload start
  mov rax,rcx
  test rax,rax
  jz .Lbad
  mov al,'.'
  repne scasb
  jne .Lbad
  mov r14,rdi            # signature start
  lea r15,[rbx+r12]
  sub r15,r14            # signature length
  cmp r15,43
  jne .Lbad
  # signature
  lea rdi,[rip+g_jwt_hmac]
  mov rsi,rbx
  mov rdx,r14
  sub rdx,rbx
  dec rdx
  mov rcx,rsp
  call hmac
  lea rdi,[rsp+32]
  mov rsi,r14
  mov rdx,r15
  lea rcx,[rip+rev_url]
  call b64_dec
  cmp rax,32
  jne .Lbad
  mov rax,[rsp]
  xor rax,[rsp+32]
  mov rcx,[rsp+8]
  xor rcx,[rsp+40]
  or rax,rcx
  mov rcx,[rsp+16]
  xor rcx,[rsp+48]
  or rax,rcx
  mov rcx,[rsp+24]
  xor rcx,[rsp+56]
  or rax,rcx
  jnz .Lbad
  # header: alg must be HS256
  lea rdi,[rsp+64]
  mov rsi,rbx
  mov rdx,r13
  sub rdx,rbx
  dec rdx
  cmp rdx,256
  ja .Lbad
  lea rcx,[rip+rev_url]
  call b64_dec
  cmp rax,0
  jle .Lbad
  lea rdi,[rsp+64]
  lea rsi,[rdi+rax]
  mov rbp,rsi
  call json_valid
  test rax,rax
  jz .Lbad
  lea rdi,[rsp+64]
  mov rsi,rbp
  lea rdx,[rip+k_alg]
  mov ecx,3
  call json_field
  test rax,rax
  jz .Lbad
  mov rcx,rdx
  sub rcx,rax
  cmp rcx,7
  jne .Lbad
  cmp byte ptr [rax+1],'H'
  jne .Lbad
  cmp byte ptr [rax+2],'S'
  jne .Lbad
  cmp byte ptr [rax+3],'2'
  jne .Lbad
  cmp byte ptr [rax+4],'5'
  jne .Lbad
  cmp byte ptr [rax+5],'6'
  jne .Lbad
  # payload
  lea rdi,[rsp+64]
  mov rsi,r13
  mov rdx,r14
  sub rdx,r13
  dec rdx
  cmp rdx,768
  ja .Lbad
  lea rcx,[rip+rev_url]
  call b64_dec
  cmp rax,0
  jle .Lbad
  lea rdi,[rsp+64]
  lea rbp,[rdi+rax]
  mov rsi,rbp
  call json_valid
  test rax,rax
  jz .Lbad
  lea rdi,[rsp+64]
  mov rsi,rbp
  lea rdx,[rip+k_exp]
  mov ecx,3
  call json_field
  test rax,rax
  jz .Lbad
  mov rdi,rax
  mov rsi,rdx
  sub rsi,rax
  # exp may be a float: take the integer part
  xor ecx,ecx
1:cmp rcx,rsi
  je 2f
  cmp byte ptr [rdi+rcx],'.'
  je 2f
  inc rcx
  jmp 1b
2:mov rsi,rcx
  call parse_u64
  test rdx,rdx
  jz .Lbad
  cmp rax,[rsp+1184]
  jbe .Lbad
  lea rdi,[rsp+64]
  mov rsi,rbp
  lea rdx,[rip+k_sub]
  mov ecx,3
  call json_field
  test rax,rax
  jz .Lpayload
  cmp byte ptr [rax],'"'
  jne .Lpayload
  lea rdi,[rax+1]
  lea rsi,[rdx-2]
  sub rsi,rax
  call parse_u64
  test rdx,rdx
  jz .Lpayload
  test rax,rax
  jz .Lpayload
  mov r12,rax
  lea rdi,[rsp+64]
  mov rsi,rbp
  lea rdx,[rip+k_username]
  mov ecx,8
  call json_field
  test rax,rax
  jz .Lpayload
  cmp byte ptr [rax],'"'
  jne .Lpayload
  lea rsi,[rax+1]
  dec rdx                # at the closing quote
  mov rdi,[rsp+1176]
  call json_unescape
  test eax,eax
  jz .Lpayload
  mov rdx,rdi
  sub rdx,[rsp+1176]
  mov rax,r12
  jmp .Ldone
.Lpayload:
  mov rax,-1
  jmp .Ldone
.Lbad:
  xor eax,eax
.Ldone:
  add rsp,1192
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

# jwt_make rdi out, rsi user id, rdx now(s) -> rdi advanced. Preserves rbx, rbp, r12-r15.
.globl jwt_make
jwt_make:
  push rbx
  push r12
  push r13
  push r14
  sub rsp,256
  mov rbx,rdi           # token start
  mov r12,rsi
  mov r13,rdx
  lea rsi,[rip+jwt_head]
  mov ecx,jwt_head_len
  call mcpy
  mov r14,rdi
  # claims in the stack buffer
  mov rdi,rsp
  LIT "{\"sub\":\""
  mov rsi,r12
  call put_u64
  LIT "\",\"iat\":"
  mov rsi,r13
  call put_u64
  LIT ",\"exp\":"
  lea rsi,[r13+2592000]
  call put_u64
  mov byte ptr [rdi],'}'
  inc rdi
  mov rdx,rdi
  sub rdx,rsp
  mov rsi,rsp
  mov rdi,r14
  lea rcx,[rip+b64url_a]
  xor r8d,r8d
  call b64_enc
  mov r14,rdi
  # signature over head.payload
  mov byte ptr [r14],'.'
  lea rdi,[rip+g_jwt_hmac]
  mov rsi,rbx
  mov rdx,r14
  sub rdx,rbx
  mov rcx,rsp
  call hmac
  lea rdi,[r14+1]
  mov rsi,rsp
  mov edx,32
  lea rcx,[rip+b64url_a]
  xor r8d,r8d
  call b64_enc
  add rsp,256
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
