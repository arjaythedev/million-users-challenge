# gzip: greedy LZ77 over a 4096-entry hash of 4-byte runs, one fixed-Huffman deflate block, CRC-32 by slicing-by-8.
# Fast rather than tight: it exists to spend little CPU on JSON when the network is the bottleneck.
.intel_syntax noprefix
.section .rodata
.balign 64
.include "gz_tables.inc"
.text
.set GZ_HASH,16384

# Append the entry in eax (bits | nbits<<24) to the bit buffer r13, r14d bits held; flushes whole 32-bit words to r10.
.macro GZ_PUT
  mov edx,eax
  shr edx,24
  and eax,0xffffff
  mov ecx,r14d
  shl rax,cl
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

# gz_compress rdi src, rsi len, rdx dst, rcx cap -> rax bytes written, or 0 when it would not fit in cap.
.globl gz_compress
gz_compress:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp,GZ_HASH+8
  mov r8,rdi
  mov r9,rsi
  mov r10,rdx
  mov r15,rdx            # dst start
  lea r11,[rdx+rcx-32]   # room for the last word, the trailer and one match
  cmp rcx,64
  jb .Lgz_fail
  mov rbx,rdi            # clear the hash
  mov rdi,rsp
  mov ecx,GZ_HASH/8
  xor eax,eax
  rep stosq
  mov rdi,rbx
  mov eax,0x00088b1f
  mov [r10],rax          # 1f 8b 08 00, mtime 0
  mov word ptr [r10+8],0xff00
  add r10,10
  mov r13d,3             # BFINAL=1, BTYPE=01
  mov r14d,3
  mov r12,r8
  lea rbp,[r8+r9]        # end of input
  lea rbx,[rbp-4]        # last position a 4-byte run can start at
.Lgz_loop:
  cmp r10,r11
  jae .Lgz_fail
  cmp r12,rbx
  ja .Lgz_tail
  mov eax,[r12]
  imul eax,eax,0x9E3779B1
  shr eax,20
  mov edx,[rsp+rax*4]
  mov rcx,r12
  sub rcx,r8
  inc ecx                # positions are stored +1; 0 means empty
  mov [rsp+rax*4],ecx
  test edx,edx
  jz .Lgz_lit
  lea rsi,[r8+rdx-1]     # candidate
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
  lea rax,[rip+gz_len]
  mov eax,[rax+rdx*4]
  GZ_PUT
  lea rax,[rdi-1]
  cmp rdi,256
  ja 6f
  lea rdx,[rip+gz_dsmall]
  movzx eax,byte ptr [rdx+rax]
  jmp 7f
6:shr eax,7
  lea rdx,[rip+gz_dlarge]
  movzx eax,byte ptr [rdx+rax]
7:lea rdx,[rip+gz_dist]
  mov esi,[rdx+rax*4]
  dec edi
  mov eax,esi
  shr eax,16
  sub edi,eax            # extra bits' value
  movzx edx,sil          # the 5-bit code
  shl edi,5
  or edi,edx
  shr esi,8
  and esi,0xff
  add esi,5              # bits in all
  shl esi,24
  mov eax,edi
  or eax,esi
  GZ_PUT
  jmp .Lgz_loop
.Lgz_lit:
  movzx eax,byte ptr [r12]
  inc r12
  lea rdx,[rip+gz_lit]
  mov eax,[rdx+rax*4]
  GZ_PUT
  jmp .Lgz_loop
.Lgz_tail:
  cmp r12,rbp
  jae .Lgz_end
  cmp r10,r11
  jae .Lgz_fail
  movzx eax,byte ptr [r12]
  inc r12
  lea rdx,[rip+gz_lit]
  mov eax,[rdx+rax*4]
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
2:mov rdi,r8
  mov rsi,r9
  call gz_crc32
  mov [r10],eax
  mov [r10+4],r9d
  add r10,8
  mov rax,r10
  sub rax,r15
  jmp .Lgz_ret
.Lgz_fail:
  xor eax,eax
.Lgz_ret:
  add rsp,GZ_HASH+8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

# gz_crc32 rdi data, rsi len -> eax. Preserves r8-r15.
gz_crc32:
  lea rcx,[rip+gz_crc]
  mov eax,0xffffffff
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
