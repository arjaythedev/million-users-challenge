# gzip: greedy LZ77 over a 4096-entry hash of 4-byte runs, one fixed-Huffman deflate block, CRC-32 by carry-less multiply folding.
# Fast rather than tight: it exists to spend little CPU on JSON when the network is the bottleneck.
.intel_syntax noprefix
.section .rodata
.balign 64
# CRC-32 (reflected 0xEDB88320) folding constants, as in Linux's crc32-pclmul
crc_k12:   .quad 0x154442bd4,0x1c6e41596
crc_k34:   .quad 0x1751997d0,0x0ccaa009e
crc_k5:    .quad 0x163cd6124,0
crc_mask32:.quad 0xffffffff,0
crc_pu:    .quad 0x1db710641,0x1f7011641
# 5-bit codes bit-reversed, as deflate writes Huffman codes
gz_rev5: .byte 0,16,8,24,4,20,12,28,2,18,10,26,6,22,14,30,1,17,9,25,5,21,13,29,3,19,11,27,7,23,15,31
.balign 64
.include "gz_tables.inc"
.text
.set GZ_HASH,16384

# Append the entry in eax (bits | nbits<<24) to the bit buffer r13, r14d bits held; flushes whole 32-bit words to r10.
.macro GZ_PUT
  mov edx,eax
  shr edx,24
  and eax,0xffffff
  shlx rax,rax,r14       # BMI2: one uop (shl by cl is three on Haswell)
  or r13,rax
  add r14d,edx
  cmp r14d,32
  jb 771f
  mov [r10],r13d
  add r10,4
  shr r13,32
  sub r14d,32
771:
.endm

# gz_compress rdi src, rsi len, rdx dst, rcx cap, r8 hash table (GZ_HASH bytes, then a u32 base; zeroed at first)
# -> rax bytes written, or 0 when it would not fit in cap. The table is never cleared: an entry holds its position
# + base + 1, the base moves past every position of this call, so an entry from an earlier call points before src
# and is rejected like an empty one.
.globl gz_compress
gz_compress:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp,24
  mov [rsp],rdi          # src
  mov [rsp+8],rsi        # len
  mov r9,r8              # table
  mov r10,rdx
  mov r15,rdx            # dst start
  lea r11,[rdx+rcx-32]   # room for the last word, the trailer and one match
  cmp rcx,64
  jb .Lgz_fail
  mov eax,[r9+GZ_HASH]   # base
  lea rcx,[rax+rsi+1]
  cmp rcx,0x70000000
  jb 1f
  mov rbx,rdi            # the base would wrap: start over from a cleared table
  mov rdi,r9
  mov ecx,GZ_HASH/8
  xor eax,eax
  rep stosq
  mov rdi,rbx
  lea rcx,[rsi+1]
1:mov [r9+GZ_HASH],ecx
  mov r8,rdi
  sub r8,rax
  dec r8                 # r8 = src - base - 1: position p is stored as p - r8
  mov eax,0x00088b1f
  mov [r10],rax          # 1f 8b 08 00, mtime 0
  mov word ptr [r10+8],0xff00
  add r10,10
  mov r13d,3             # BFINAL=1, BTYPE=01
  mov r14d,3
  mov r12,rdi
  lea rbp,[rdi+rsi]      # end of input
  lea rbx,[rbp-4]        # last position a 4-byte run can start at
.Lgz_loop:
  cmp r10,r11
  jae .Lgz_fail
  cmp r12,rbx
  ja .Lgz_tail
  mov eax,[r12]
  imul eax,eax,0x9E3779B1
  shr eax,20
  mov edx,[r9+rax*4]
  mov rcx,r12
  sub rcx,r8
  mov [r9+rax*4],ecx
  lea rsi,[r8+rdx]       # candidate
  cmp rsi,[rsp]
  jb .Lgz_lit            # empty, or from an earlier call
  mov rdi,r12
  sub rdi,rsi            # distance
  cmp rdi,32768
  ja .Lgz_lit
  mov eax,[rsi]
  cmp eax,[r12]
  jne .Lgz_lit
  # how long; rdx = length, rcx = its limit
  mov edx,4
  mov rcx,rbp
  sub rcx,r12
  cmp rcx,258
  jbe 1f
  mov ecx,258
1:lea rax,[rdx+8]
  cmp rax,rcx
  ja 3f
  mov rax,[r12+rdx]
  xor rax,[rsi+rdx]
  jnz 2f
  add rdx,8
  jmp 1b
2:bsf rax,rax
  shr eax,3
  add rdx,rax
  jmp 5f
3:cmp rdx,rcx
  jae 5f
  mov al,[r12+rdx]
  cmp al,[rsi+rdx]
  jne 5f
  inc rdx
  jmp 3b
5:add r12,rdx
  # rdi distance survives GZ_PUT: it uses only rax, rcx, rdx
  mov eax,[gz_len+rdx*4]
  GZ_PUT
  # distance code from the top bit of d-1 (deflate: codes 0-3 are d 1-4, then two codes per power of two)
  lea eax,[rdi-1]
  cmp eax,4
  jae 6f
  movzx eax,byte ptr [gz_rev5+rax]
  or eax,5<<24
  jmp 7f
6:bsr ecx,eax
  lea esi,[rcx-1]        # extra bits
  shrx edx,eax,esi       # the top two bits: 2 or 3
  lea edx,[rdx+rcx*2-2]  # the code
  movzx edx,byte ptr [gz_rev5+rdx]
  bzhi eax,eax,esi       # the extra bits' value
  shl eax,5
  or eax,edx
  add esi,5
  shl esi,24
  or eax,esi
7:
  GZ_PUT
  jmp .Lgz_loop
.Lgz_lit:
  movzx eax,byte ptr [r12]
  inc r12
  mov eax,[gz_lit+rax*4]
  GZ_PUT
  jmp .Lgz_loop
.Lgz_tail:
  cmp r12,rbp
  jae .Lgz_end
  cmp r10,r11
  jae .Lgz_fail
  movzx eax,byte ptr [r12]
  inc r12
  mov eax,[gz_lit+rax*4]
  GZ_PUT
  jmp .Lgz_tail
.Lgz_end:
  lea rax,[rip+gz_lit]
  mov eax,[rax+256*4]
  GZ_PUT
1:test r14d,r14d
  jz 2f
  mov [r10],r13b
  inc r10
  shr r13,8
  cmp r14d,8
  jbe 3f
  sub r14d,8
  jmp 1b
3:xor r14d,r14d
2:mov rdi,[rsp]
  mov rsi,[rsp+8]
  call gz_crc32
  mov [r10],eax
  mov rsi,[rsp+8]
  mov [r10+4],esi
  add r10,8
  mov rax,r10
  sub rax,r15
  jmp .Lgz_ret
.Lgz_fail:
  xor eax,eax
.Lgz_ret:
  add rsp,24
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

# gz_crc32 rdi data, rsi len -> eax. Preserves r8-r15; clobbers rax, rcx, rdx, rsi, rdi, xmm0-xmm8.
# 64 bytes or more: carry-less multiply folding (PCLMULQDQ, as in Linux's crc32-pclmul), 64 bytes a step, then
# 16 bytes a step, then a Barrett reduction; what is left (under 16 bytes) goes through the slicing-by-8 table.
gz_crc32:
  mov eax,0xffffffff
  cmp rsi,64
  jb .Lcrc_tab
  movdqu xmm1,[rdi]
  movdqu xmm2,[rdi+16]
  movdqu xmm3,[rdi+32]
  movdqu xmm4,[rdi+48]
  movd xmm0,eax
  pxor xmm1,xmm0
  add rdi,64
  sub rsi,64
  cmp rsi,64
  jb 2f
  movdqa xmm0,[rip+crc_k12]
1:movdqa xmm5,xmm1
  movdqa xmm6,xmm2
  movdqa xmm7,xmm3
  movdqa xmm8,xmm4
  pclmulqdq xmm1,xmm0,0x00
  pclmulqdq xmm2,xmm0,0x00
  pclmulqdq xmm3,xmm0,0x00
  pclmulqdq xmm4,xmm0,0x00
  pclmulqdq xmm5,xmm0,0x11
  pclmulqdq xmm6,xmm0,0x11
  pclmulqdq xmm7,xmm0,0x11
  pclmulqdq xmm8,xmm0,0x11
  pxor xmm1,xmm5
  pxor xmm2,xmm6
  pxor xmm3,xmm7
  pxor xmm4,xmm8
  movdqu xmm5,[rdi]
  movdqu xmm6,[rdi+16]
  movdqu xmm7,[rdi+32]
  movdqu xmm8,[rdi+48]
  pxor xmm1,xmm5
  pxor xmm2,xmm6
  pxor xmm3,xmm7
  pxor xmm4,xmm8
  add rdi,64
  sub rsi,64
  cmp rsi,64
  jae 1b
  # four lanes into one
2:movdqa xmm0,[rip+crc_k34]
  .irp r,2,3,4
  movdqa xmm5,xmm1
  pclmulqdq xmm1,xmm0,0x00
  pclmulqdq xmm5,xmm0,0x11
  pxor xmm1,xmm5
  pxor xmm1,xmm\r
  .endr
  cmp rsi,16
  jb 4f
3:movdqa xmm5,xmm1
  pclmulqdq xmm1,xmm0,0x00
  pclmulqdq xmm5,xmm0,0x11
  pxor xmm1,xmm5
  movdqu xmm5,[rdi]
  pxor xmm1,xmm5
  add rdi,16
  sub rsi,16
  cmp rsi,16
  jae 3b
  # 128 -> 64 bits, also appending 32 zero bits
4:pclmulqdq xmm0,xmm1,0x01
  psrldq xmm1,8
  pxor xmm1,xmm0
  # 64 -> 32 bits
  movdqa xmm0,[rip+crc_k5]
  movdqa xmm3,[rip+crc_mask32]
  movdqa xmm2,xmm1
  pand xmm1,xmm3
  psrldq xmm2,4
  pclmulqdq xmm1,xmm0,0x00
  pxor xmm1,xmm2
  # bit-reflected Barrett reduction
  movdqa xmm0,[rip+crc_pu]
  movdqa xmm2,xmm1
  pand xmm1,xmm3
  pclmulqdq xmm1,xmm0,0x10
  pand xmm1,xmm3
  pclmulqdq xmm1,xmm0,0x00
  pxor xmm1,xmm2
  pextrd eax,xmm1,1
.Lcrc_tab:
  lea rcx,[rip+gz_crc]
1:cmp rsi,8
  jb 2f
  mov rdx,[rdi]
  add rdi,8
  sub rsi,8
  xor rdx,rax            # the running CRC folds into the low word
  push rbx
  push rbp
  mov ebp,edx
  shr rdx,32
  movzx ebx,dl
  mov eax,[rcx+3*1024+rbx*4]
  movzx ebx,dh
  xor eax,[rcx+2*1024+rbx*4]
  shr edx,16
  movzx ebx,dl
  xor eax,[rcx+1*1024+rbx*4]
  movzx ebx,dh
  xor eax,[rcx+rbx*4]
  movzx ebx,bpl
  xor eax,[rcx+7*1024+rbx*4]
  mov ebx,ebp
  movzx ebx,bh
  xor eax,[rcx+6*1024+rbx*4]
  shr ebp,16
  movzx ebx,bpl
  xor eax,[rcx+5*1024+rbx*4]
  mov ebx,ebp
  movzx ebx,bh
  xor eax,[rcx+4*1024+rbx*4]
  pop rbp
  pop rbx
  jmp 1b
2:test rsi,rsi
  jz 3f
  movzx edx,byte ptr [rdi]
  inc rdi
  dec rsi
  xor dl,al
  shr eax,8
  xor eax,[rcx+rdx*4]
  jmp 2b
3:not eax
  ret
