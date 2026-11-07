# JSON: validation, field lookup, string decoding, and the escaped writer.
.include "defs.inc"
.section .rodata
.balign 16
x_quote: .fill 16,1,'"'
x_bslash: .fill 16,1,0x5c
x_1f: .fill 16,1,0x1f
hexd: .ascii "0123456789abcdef"
.text

.globl skip_ws,skip_value
# skip_ws: rdi cursor, rsi end -> rdi at the first non-whitespace (may equal end)
skip_ws:
1:cmp rdi,rsi
  jae 2f
  mov al,[rdi]
  cmp al,' '
  je 3f
  cmp al,10
  je 3f
  cmp al,13
  je 3f
  cmp al,9
  jne 2f
3:inc rdi
  jmp 1b
2:ret

# skip_value: rdi cursor, rsi end, edx depth -> rdi past the value, CF set when invalid.
# Clobbers rax, rcx, r8.
skip_value:
  call skip_ws
  cmp rdi,rsi
  jae .Lbadv
  movzx eax,byte ptr [rdi]
  cmp al,'"'
  je skip_string
  cmp al,'{'
  je .Lobj
  cmp al,'['
  je .Larr
  cmp al,'t'
  je .Ltrue
  cmp al,'f'
  je .Lfalse
  cmp al,'n'
  je .Lnull
  jmp .Lnum
.Ltrue:
  lea rax,[rdi+4]
  cmp rax,rsi
  ja .Lbadv
  cmp dword ptr [rdi],0x65757274
  jne .Lbadv
  mov rdi,rax
  clc
  ret
.Lnull:
  lea rax,[rdi+4]
  cmp rax,rsi
  ja .Lbadv
  cmp dword ptr [rdi],0x6c6c756e
  jne .Lbadv
  mov rdi,rax
  clc
  ret
.Lfalse:
  lea rax,[rdi+5]
  cmp rax,rsi
  ja .Lbadv
  cmp dword ptr [rdi+1],0x65736c61
  jne .Lbadv
  mov rdi,rax
  clc
  ret
.Lnum:
  cmp al,'-'
  jne 1f
  inc rdi
1:mov r8,rdi
2:cmp rdi,rsi
  jae 3f
  movzx eax,byte ptr [rdi]
  lea ecx,[rax-'0']
  cmp ecx,9
  jbe 4f
  cmp al,'.'
  je 4f
  cmp al,'e'
  je 4f
  cmp al,'E'
  je 4f
  cmp al,'+'
  je 4f
  cmp al,'-'
  jne 3f
4:inc rdi
  jmp 2b
3:cmp rdi,r8
  je .Lbadv
  clc
  ret
.Lobj:
  cmp edx,64
  jae .Lbadv
  inc edx
  inc rdi
  call skip_ws
  cmp rdi,rsi
  jae .Lbadv
  cmp byte ptr [rdi],'}'
  je .Lclose
1:call skip_ws
  cmp rdi,rsi
  jae .Lbadv
  cmp byte ptr [rdi],'"'
  jne .Lbadv
  call skip_string
  jc .Lbadv
  call skip_ws
  cmp rdi,rsi
  jae .Lbadv
  cmp byte ptr [rdi],':'
  jne .Lbadv
  inc rdi
  call skip_value
  jc .Lbadv
  call skip_ws
  cmp rdi,rsi
  jae .Lbadv
  mov al,[rdi]
  inc rdi
  cmp al,','
  je 1b
  dec edx
  cmp al,'}'
  jne .Lbadv
  clc
  ret
.Larr:
  cmp edx,64
  jae .Lbadv
  inc edx
  inc rdi
  call skip_ws
  cmp rdi,rsi
  jae .Lbadv
  cmp byte ptr [rdi],']'
  je .Lclose
1:call skip_value
  jc .Lbadv
  call skip_ws
  cmp rdi,rsi
  jae .Lbadv
  mov al,[rdi]
  inc rdi
  cmp al,','
  je 1b
  dec edx
  cmp al,']'
  jne .Lbadv
  clc
  ret
.Lclose:
  dec edx
  inc rdi
  clc
  ret
.Lbadv:
  stc
  ret

# rdi at the opening quote -> past the closing quote.
skip_string:
  inc rdi
1:cmp rdi,rsi
  jae .Lbadv
  movzx eax,byte ptr [rdi]
  inc rdi
  cmp al,'"'
  je 2f
  cmp al,0x20
  jb .Lbadv
  cmp al,0x5c
  jne 1b
  cmp rdi,rsi
  jae .Lbadv
  inc rdi
  jmp 1b
2:clc
  ret

# json_valid rdi start, rsi end -> rax 1 when exactly one JSON value fills the range.
.globl json_valid
json_valid:
  xor edx,edx
  call skip_value
  jc 1f
  call skip_ws
  cmp rdi,rsi
  jne 1f
  mov eax,1
  ret
1:xor eax,eax
  ret

# json_field rdi object start, rsi end, rdx key, rcx keylen -> rax value start, rdx value end
# (rax 0 when absent or the value is not an object). The range has passed json_valid.
.globl json_field
json_field:
  push rbx
  push r12
  mov rbx,rdx
  mov r12,rcx
  call skip_ws
  cmp rdi,rsi
  jae 8f
  cmp byte ptr [rdi],'{'
  jne 8f
  inc rdi
1:call skip_ws
  cmp byte ptr [rdi],'"'
  jne 8f
  lea r9,[rdi+1]
  call skip_string
  lea r10,[rdi-1]
  sub r10,r9
  call skip_ws
  inc rdi                # ':'
  call skip_ws
  mov r11,rdi            # value start
  push r9
  push r10
  push r11
  xor edx,edx
  call skip_value
  pop r11
  pop r10
  pop r9
  cmp r10,r12
  jne 3f
  xor ecx,ecx
2:cmp rcx,r12
  je 7f
  mov al,[r9+rcx]
  cmp al,[rbx+rcx]
  jne 3f
  inc rcx
  jmp 2b
3:call skip_ws
  cmp byte ptr [rdi],','
  jne 8f
  inc rdi
  jmp 1b
7:mov rax,r11
  mov rdx,rdi
  pop r12
  pop rbx
  ret
8:xor eax,eax
  xor edx,edx
  pop r12
  pop rbx
  ret

hex4:  # rsi -> 4 hex digits at rsi: eax value, CF on bad. Advances rsi.
  xor eax,eax
  mov ecx,4
1:movzx edx,byte ptr [rsi]
  inc rsi
  shl eax,4
  cmp dl,'9'
  ja 2f
  sub dl,'0'
  jb 9f
  jmp 3f
2:or dl,0x20
  sub dl,'a'
  cmp dl,5
  ja 9f
  add dl,10
3:or eax,edx
  dec ecx
  jnz 1b
  clc
  ret
9:stc
  ret

# json_unescape rdi dst, rsi src (just inside the quotes), rdx src end (at the closing quote)
# -> rdi advanced; rax 1 ok, 0 for a bad escape or an embedded NUL.
.globl json_unescape
json_unescape:
  mov r8,rdx
1:cmp rsi,r8
  jae 8f
  movzx eax,byte ptr [rsi]
  inc rsi
  cmp al,0x5c
  je 2f
  mov [rdi],al
  inc rdi
  jmp 1b
2:movzx eax,byte ptr [rsi]
  inc rsi
  cmp al,'n'
  je .Ln
  cmp al,'t'
  je .Lt
  cmp al,'r'
  je .Lr
  cmp al,'b'
  je .Lb
  cmp al,'f'
  je .Lf
  cmp al,'u'
  je .Lu
  cmp al,'"'
  je 3f
  cmp al,0x5c
  je 3f
  cmp al,'/'
  jne 9f
3:mov [rdi],al
  inc rdi
  jmp 1b
.Ln: mov al,10
  jmp 3b
.Lt: mov al,9
  jmp 3b
.Lr: mov al,13
  jmp 3b
.Lb: mov al,8
  jmp 3b
.Lf: mov al,12
  jmp 3b
.Lu:
  lea rax,[rsi+4]
  cmp rax,r8
  ja 9f
  call hex4
  jc 9f
  test eax,eax
  jz 9f
  mov r9d,eax
  and r9d,0xfc00
  cmp r9d,0xdc00
  je 9f                  # lone low surrogate
  cmp r9d,0xd800
  jne 4f
  lea rax,[rsi+6]
  cmp rax,r8
  ja 9f
  cmp word ptr [rsi],0x755c   # \u
  jne 9f
  mov r10d,eax           # high surrogate
  add rsi,2
  call hex4
  jc 9f
  mov ecx,eax
  and ecx,0xfc00
  cmp ecx,0xdc00
  jne 9f
  sub r10d,0xd800
  shl r10d,10
  sub eax,0xdc00
  lea eax,[rax+r10+0x10000]
4:cmp eax,0x80
  jae 5f
  mov [rdi],al
  inc rdi
  jmp 1b
5:cmp eax,0x800
  jae 6f
  mov ecx,eax
  shr ecx,6
  or cl,0xc0
  mov [rdi],cl
  and al,0x3f
  or al,0x80
  mov [rdi+1],al
  add rdi,2
  jmp 1b
6:cmp eax,0x10000
  jae 7f
  mov ecx,eax
  shr ecx,12
  or cl,0xe0
  mov [rdi],cl
  mov ecx,eax
  shr ecx,6
  and cl,0x3f
  or cl,0x80
  mov [rdi+1],cl
  and al,0x3f
  or al,0x80
  mov [rdi+2],al
  add rdi,3
  jmp 1b
7:mov ecx,eax
  shr ecx,18
  or cl,0xf0
  mov [rdi],cl
  mov ecx,eax
  shr ecx,12
  and cl,0x3f
  or cl,0x80
  mov [rdi+1],cl
  mov ecx,eax
  shr ecx,6
  and cl,0x3f
  or cl,0x80
  mov [rdi+2],cl
  and al,0x3f
  or al,0x80
  mov [rdi+3],al
  add rdi,4
  jmp 1b
8:mov eax,1
  ret
9:xor eax,eax
  ret

# json_esc rdi out, rsi src, rcx len: writes "..." with escapes -> rdi advanced.
# Clobbers rax, rcx, rdx, rsi, xmm0-3. Preserves everything else.
.globl json_esc
json_esc:
  mov byte ptr [rdi],'"'
  inc rdi
  movdqa xmm1,[rip+x_quote]
  movdqa xmm2,[rip+x_bslash]
  movdqa xmm3,[rip+x_1f]
1:cmp rcx,16
  jb 3f
  movdqu xmm0,[rsi]
  movdqu [rdi],xmm0        # optimistic copy
  movdqa xmm4,xmm0
  pcmpeqb xmm4,xmm1
  movdqa xmm5,xmm0
  pcmpeqb xmm5,xmm2
  por xmm4,xmm5
  movdqa xmm5,xmm0
  pminub xmm5,xmm3
  pcmpeqb xmm5,xmm0
  por xmm4,xmm5
  pmovmskb eax,xmm4
  test eax,eax
  jnz 2f
  add rsi,16
  add rdi,16
  sub rcx,16
  jmp 1b
2:bsf eax,eax             # copy the clean prefix, then one escaped byte
  add rsi,rax
  add rdi,rax
  sub rcx,rax
  call esc1
  jmp 1b
3:test rcx,rcx
  jz 4f
  call esc1
  jmp 3b
4:mov byte ptr [rdi],'"'
  inc rdi
  ret
# one byte from rsi to rdi, escaped when needed
esc1:
  movzx eax,byte ptr [rsi]
  inc rsi
  dec rcx
  cmp al,'"'
  je 5f
  cmp al,0x5c
  je 5f
  cmp al,0x20
  jb 6f
  mov [rdi],al
  inc rdi
  ret
5:mov byte ptr [rdi],0x5c
  mov [rdi+1],al
  add rdi,2
  ret
6:mov dword ptr [rdi],0x3030755c   # \u00
  mov edx,eax
  shr edx,4
  lea r8,[rip+hexd]
  mov dl,[r8+rdx]
  mov [rdi+4],dl
  and eax,15
  mov al,[r8+rax]
  mov [rdi+5],al
  add rdi,6
  ret
