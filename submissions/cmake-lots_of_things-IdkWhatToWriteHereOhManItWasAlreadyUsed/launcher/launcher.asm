
; NASM, x86_64 System V ABI (Linux)
; Вызывает system("cmake -P /app/server/handler.cmake")

        bits 64
        default rel

        global  _start

        extern  system
        extern  exit

section .rodata
cmd:    db "cmake -P /app/server/handler.cmake", 0

section .text
_start:
        and     rsp, -16

        lea     rdi, [cmd]
        call    system wrt ..plt    ; int system(const char*)

        mov     edi, eax
        call    exit wrt ..plt

        hlt