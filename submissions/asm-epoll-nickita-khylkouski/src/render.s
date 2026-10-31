# render.s: HTTP responses.
#   render_init()                       builds every error response once (keep-alive and close variants)
#   resp_error(fd, err, close)          sends a prebuilt error response
#   resp_body(fd, status, len, opflags) sends the body rendered at W_ARENA + HEAD_ROOM (gzip when asked and
#                                       long enough), head and body in one send
#   render_answer(op, msgs, len)        answers an op from its backend messages (DataRow / CommandComplete /
#                                       ErrorResponse) and serves the next pipelined request of that client
#   render_fail(op, err)                answers an op with an error response (health: 503 degraded)

.include "defs.inc"
.include "app.inc"

.equ ERR_SLOT, 320                      # bytes per prebuilt response (head <= 151, body <= 85)

.section .rodata
b_notfound:  .ascii "{\"error\":\"not found\"}"
ENDLIT b_notfound
b_badid:     .ascii "{\"error\":\"invalid post id\"}"
ENDLIT b_badid
b_postnf:    .ascii "{\"error\":\"post not found\"}"
ENDLIT b_postnf
b_nobearer:  .ascii "{\"error\":\"missing bearer token\"}"
ENDLIT b_nobearer
b_badtoken:  .ascii "{\"error\":\"invalid or expired token\"}"
ENDLIT b_badtoken
b_payload:   .ascii "{\"error\":\"invalid token payload\"}"
ENDLIT b_payload
b_malformed: .ascii "{\"error\":\"malformed JSON body\"}"
ENDLIT b_malformed
b_bodyreq:   .ascii "{\"error\":\"body is required\"}"
ENDLIT b_bodyreq
b_toolong:   .ascii "{\"error\":\"body must be at most 500 characters\"}"
ENDLIT b_toolong
b_internal:  .ascii "{\"error\":\"internal server error\"}"
ENDLIT b_internal
b_degraded:  .ascii "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}"
ENDLIT b_degraded

lit_posts_open: .ascii "{\"posts\":["
ENDLIT lit_posts_open
lit_posts_close: .ascii "]}"
ENDLIT lit_posts_close
lit_post_open: .ascii "{\"post\":"
ENDLIT lit_post_open
lit_id: .ascii "{\"id\":"
ENDLIT lit_id
lit_body: .ascii ",\"body\":"
ENDLIT lit_body
lit_created: .ascii ",\"created_at\":"
ENDLIT lit_created
lit_author: .ascii ",\"author\":"
ENDLIT lit_author
lit_likes: .ascii ",\"like_count\":"
ENDLIT lit_likes
lit_likes0: .ascii ",\"like_count\":0}}"
ENDLIT lit_likes0
lit_health: .ascii "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"
ENDLIT lit_health
lit_like_new: .ascii "{\"liked\":true,\"already_liked\":false,\"post_id\":"
ENDLIT lit_like_new
lit_like_old: .ascii "{\"liked\":true,\"already_liked\":true,\"post_id\":"
ENDLIT lit_like_old

.align 8
err_def:                                # body, length, status (index = E_*)
    .quad 0, 0, 0
    .quad b_notfound, b_notfound_len, 404
    .quad b_badid, b_badid_len, 400
    .quad b_postnf, b_postnf_len, 404
    .quad b_nobearer, b_nobearer_len, 401
    .quad b_badtoken, b_badtoken_len, 401
    .quad b_payload, b_payload_len, 401
    .quad b_malformed, b_malformed_len, 400
    .quad b_bodyreq, b_bodyreq_len, 400
    .quad b_toolong, b_toolong_len, 400
    .quad b_internal, b_internal_len, 500
    .quad b_degraded, b_degraded_len, 503

.bss
.align 16
g_errbuf: .skip NERR * 2 * ERR_SLOT
g_errtab: .skip NERR * 2 * 16           # {ptr, len} per (err, close)

.text

# ---------------------------------------------------------------- render_init()
.globl render_init
render_init:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    mov ebx, 1
.Lri_err:
    cmp ebx, NERR
    jae .Lri_done
    xor r12d, r12d                      # close variant
.Lri_var:
    lea eax, [rbx + rbx]
    add eax, r12d
    imul eax, eax, ERR_SLOT
    lea r13, [rip + g_errbuf]
    add r13, rax
    lea rax, [rip + err_def]
    lea rcx, [rbx + rbx*2]
    lea r14, [rax + rcx*8]
    mov rdi, r13
    mov rsi, [r14 + 16]
    mov rdx, [r14 + 8]
    mov ecx, r12d                       # HW_CLOSE = 1
    call http_write_head
    mov rdi, rax
    mov rsi, [r14]
    mov rdx, [r14 + 8]
    call rt_memcpy
    sub rax, r13
    lea ecx, [rbx + rbx]
    add ecx, r12d
    shl rcx, 4
    lea rdx, [rip + g_errtab]
    mov [rdx + rcx], r13
    mov [rdx + rcx + 8], rax
    inc r12d
    cmp r12d, 2
    jb .Lri_var
    inc ebx
    jmp .Lri_err
.Lri_done:
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- resp_error(fd, err, close)
.globl resp_error
resp_error:
    lea eax, [rsi + rsi]
    test edx, edx
    jz 1f
    inc eax
1:  shl rax, 4
    lea r8, [rip + g_errtab]
    add r8, rax
    mov ecx, edx
    mov rsi, [r8]
    mov rdx, [r8 + 8]
    jmp conn_send

# ---------------------------------------------------------------- resp_body(fd, status, len, opflags)
.globl resp_body
resp_body:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    mov ebx, edi
    mov r12d, esi
    mov r13, rdx
    mov r14d, ecx
    mov rbp, [r15 + W_ARENA]
    add rbp, HEAD_ROOM
    test r14d, OPF_GZ
    jz .Lrb_head
    cmp r13, GZMIN
    jb .Lrb_head
    mov rdi, [r15 + W_GZ]
    mov rsi, rbp
    mov rdx, r13
    mov rcx, [r15 + W_ZBUF]
    add rcx, HEAD_ROOM
    mov r8d, ARENA_SIZE - HEAD_ROOM
    call gzip_compress
    test rax, rax
    jz .Lrb_head
    mov r13, rax
    mov rbp, [r15 + W_ZBUF]
    add rbp, HEAD_ROOM
    or r14d, 0x100
.Lrb_head:
    mov ecx, r14d
    and ecx, OPF_CLOSE                  # = HW_CLOSE
    test r14d, 0x100
    jz 1f
    or ecx, HW_GZIP
1:  lea rdi, [r15 + W_HSCR]
    mov esi, r12d
    mov rdx, r13
    call http_write_head
    lea rsi, [r15 + W_HSCR]
    mov r12, rax
    sub r12, rsi                        # head length
    mov rdi, rbp
    sub rdi, r12
    mov rdx, r12
    call rt_memcpy
    mov edi, ebx
    mov rsi, rbp
    sub rsi, r12
    lea rdx, [r13 + r12]
    mov ecx, r14d
    and ecx, OPF_CLOSE
    call conn_send
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- op helpers
# op_begin(op) -> 1 when the client is still the one that asked (then CF_BUSY is cleared), else 0
op_begin:
    mov eax, [rdi + OP_FD]
    SLOT rdx, eax
    test byte ptr [rdx + C_FLAGS], CF_OPEN
    jz 1f
    movzx eax, word ptr [rdx + C_GEN]
    cmp ax, [rdi + OP_GEN]
    jne 1f
    and byte ptr [rdx + C_FLAGS], ~CF_BUSY
    mov eax, 1
    ret
1:  xor eax, eax
    ret

# op_rearm(op): called after the response went out. A response that left QUICKACK_MS or more after its request
# was read came after the kernel's 40 ms delayed-ACK timer, which ACKed the request on its own and took the
# socket out of pingpong mode; the next request would then be ACKed at once, in a packet of its own, instead of
# riding on its response. TCP_QUICKACK=0 puts the socket back in pingpong mode. Nothing to do for closing clients.
op_rearm:
    mov rax, [rip + g_quickack_ms]
    test rax, rax
    jz 1f
    test byte ptr [rdi + OP_FL], OPF_CLOSE
    jnz 1f
    mov rcx, [r15 + W_MONO]
    sub rcx, [rdi + OP_T0]
    cmp rcx, rax
    jb 1f
    sub rsp, 24
    mov dword ptr [rsp], 0
    mov edi, [rdi + OP_FD]
    mov esi, IPPROTO_TCP
    mov edx, TCP_QUICKACK
    mov r10, rsp
    mov r8d, 4
    SYS SYS_setsockopt
    add rsp, 24
1:  ret

# op_free_aux(op)
op_free_aux:
    mov rdi, [rdi + OP_AUX]
    test rdi, rdi
    jz 1f
    mov esi, [rdi + AUX_CAP]
    jmp rt_free
1:  ret

# find_msg(msgs, len, type) -> rax = message | 0, rdx = its length
find_msg:
    lea r8, [rdi + rsi]
1:  cmp rdi, r8
    jae 2f
    mov ecx, [rdi + 1]
    bswap ecx
    inc rcx
    cmp byte ptr [rdi], dl
    je 3f
    add rdi, rcx
    jmp 1b
2:  xor eax, eax
    ret
3:  mov rax, rdi
    mov rdx, rcx
    ret

# put_post_row(dst, datarow) -> rax = new cursor | 0 (bad row or no room)
put_post_row:
    push rbx
    push r12
    sub rsp, 88
    mov rbx, rdi
    mov rdi, rsi
    mov rsi, rsp
    mov edx, 5
    call pgr_datarow
    cmp eax, 5
    jne .Lppr_bad
    mov rax, [rsp + 24]                 # body length
    add rax, [rsp + 56]                 # username length
    imul rax, rax, 6
    add rax, 160
    mov rcx, [r15 + W_ARENA]
    add rcx, ARENA_SIZE
    sub rcx, rbx
    cmp rax, rcx
    ja .Lppr_bad
    mov rdi, rbx
    PUTL lit_id
    mov rsi, [rsp]
    mov rsi, [rsi]
    bswap rsi
    call jw_int
    mov rdi, rax
    PUTL lit_body
    mov rsi, [rsp + 16]
    mov rdx, [rsp + 24]
    call jw_str
    mov rdi, rax
    PUTL lit_created
    mov rsi, [rsp + 32]
    mov rsi, [rsi]
    bswap rsi
    call jw_ts
    mov rdi, rax
    PUTL lit_author
    mov rsi, [rsp + 48]
    mov rdx, [rsp + 56]
    call jw_str
    mov rdi, rax
    PUTL lit_likes
    mov rsi, [rsp + 64]
    mov esi, [rsi]
    bswap esi
    movsxd rsi, esi
    call jw_int
    mov rdi, rax
    PUTC 0x7d
    mov rax, rdi
    jmp .Lppr_ret
.Lppr_bad:
    xor eax, eax
.Lppr_ret:
    add rsp, 88
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- render_fail(op, err)
.globl render_fail
render_fail:
    push rbx
    push r12
    push r13
    sub rsp, 32
    movdqu xmm0, [rdi]
    movdqu xmm1, [rdi + 16]
    movdqu [rsp], xmm0                  # the ring slot may be reused while the client is served
    movdqu [rsp + 16], xmm1
    mov rbx, rsp
    mov r12d, esi
    cmp byte ptr [rbx + OP_KIND], K_HEALTH
    jne 1f
    cmp r12d, E_INTERNAL
    jne 1f
    mov r12d, E_DEGRADED
1:  mov rdi, rbx
    call op_begin
    test eax, eax
    jz .Lrf_free
    mov edi, [rbx + OP_FD]
    mov esi, r12d
    movzx edx, byte ptr [rbx + OP_FL]
    and edx, OPF_CLOSE
    call resp_error
    mov rdi, rbx
    call op_rearm
    mov edi, [rbx + OP_FD]
    call conn_resume
.Lrf_free:
    mov rdi, rbx
    call op_free_aux
    add rsp, 32
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- render_answer(op, msgs, len)
.globl render_answer
render_answer:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    sub rsp, 96                         # [0,32) op copy, [32] msgs, [40] len, [48] next, [56] status, [64,96) columns
    movdqu xmm0, [rdi]
    movdqu xmm1, [rdi + 16]
    movdqu [rsp], xmm0
    movdqu [rsp + 16], xmm1
    mov [rsp + 32], rsi                 # messages
    mov [rsp + 40], rdx
    mov rbx, rsp
    mov rdi, rbx
    call op_begin
    test eax, eax
    jz .Lra_free
    mov r12d, [rbx + OP_FD]
    movzx r13d, byte ptr [rbx + OP_FL]
    mov rbp, [r15 + W_ARENA]
    add rbp, HEAD_ROOM                  # body start
    movzx eax, byte ptr [rbx + OP_KIND]
    cmp eax, K_FEED
    je .Lra_feed
    cmp eax, K_POST
    je .Lra_post
    cmp eax, K_HEALTH
    je .Lra_health
    cmp eax, K_INS
    je .Lra_ins
    cmp eax, K_LIKE
    je .Lra_like
    jmp .Lra_500

.Lra_feed:
    mov rdi, rbp
    PUTL lit_posts_open
    mov r14, [rsp + 32]
.Lra_feed_loop:
    mov rax, [rsp + 32]
    add rax, [rsp + 40]
    cmp r14, rax
    jae .Lra_feed_end
    mov edx, [r14 + 1]
    bswap edx
    inc rdx
    add rdx, r14
    mov [rsp + 48], rdx                 # next message
    cmp byte ptr [r14], 0x44
    jne .Lra_feed_next
    test r13d, 0x100
    jz 1f
    PUTC 0x2c
1:  or r13d, 0x100
    mov rsi, r14
    call put_post_row
    test rax, rax
    jz .Lra_500
    mov rdi, rax
.Lra_feed_next:
    mov r14, [rsp + 48]
    jmp .Lra_feed_loop
.Lra_feed_end:
    PUTL lit_posts_close
    mov esi, 200
    jmp .Lra_send

.Lra_post:
    mov rdi, [rsp + 32]
    mov rsi, [rsp + 40]
    mov edx, 0x44
    call find_msg
    test rax, rax
    jz .Lra_postnf
    mov r14, rax
    mov rdi, rbp
    PUTL lit_post_open
    mov rsi, r14
    call put_post_row
    test rax, rax
    jz .Lra_500
    mov rdi, rax
    PUTC 0x7d
    mov esi, 200
    jmp .Lra_send

.Lra_health:
    mov rdi, rbp
    PUTL lit_health
    mov rax, [r15 + W_MONO]
    sub rax, [rip + g_start_mono]
    xor edx, edx
    mov ecx, 1000
    div rcx
    mov rsi, rax
    call jw_int
    mov rdi, rax
    PUTC 0x7d
    mov esi, 200
    jmp .Lra_send

.Lra_ins:
    mov rdi, [rsp + 32]
    mov rsi, [rsp + 40]
    mov edx, 0x44
    call find_msg
    test rax, rax
    jz .Lra_500
    mov rdi, rax
    lea rsi, [rsp + 64]                 # id, created_at
    mov edx, 2
    call pgr_datarow
    cmp eax, 2
    jne .Lra_500
    mov r14, [rbx + OP_AUX]
    mov eax, [r14 + AUX_ULEN]
    add eax, [r14 + AUX_BLEN]
    imul eax, eax, 6
    cmp eax, ARENA_SIZE - 1024
    ja .Lra_500
    mov rdi, rbp
    PUTL lit_post_open
    PUTL lit_id
    mov rsi, [rsp + 64]
    mov rsi, [rsi]
    bswap rsi
    call jw_int
    mov rdi, rax
    PUTL lit_body
    mov ecx, [r14 + AUX_ULEN]
    lea rsi, [r14 + rcx + AUX_DATA]
    mov edx, [r14 + AUX_BLEN]
    call jw_str
    mov rdi, rax
    PUTL lit_created
    mov rsi, [rsp + 80]
    mov rsi, [rsi]
    bswap rsi
    call jw_ts
    mov rdi, rax
    PUTL lit_author
    lea rsi, [r14 + AUX_DATA]
    mov edx, [r14 + AUX_ULEN]
    call jw_str
    mov rdi, rax
    PUTL lit_likes0
    mov esi, 201
    jmp .Lra_send

.Lra_like:
    mov rdi, [rsp + 32]
    mov rsi, [rsp + 40]
    mov edx, 0x43                       # CommandComplete "INSERT 0 n"
    call find_msg
    test rax, rax
    jz .Lra_like_err
    mov rdi, rbp
    cmp byte ptr [rax + rdx - 2], 0x31
    je 1f
    PUTL lit_like_old
    mov esi, 200
    jmp 2f
1:  PUTL lit_like_new
    mov esi, 201
2:  mov [rsp + 56], rsi
    mov rsi, [rbx + OP_ID]
    call jw_int
    mov rdi, rax
    PUTC 0x7d
    mov rsi, [rsp + 56]
    jmp .Lra_send
.Lra_like_err:
    mov rdi, [rsp + 32]
    mov rsi, [rsp + 40]
    mov edx, 0x45
    call find_msg
    test rax, rax
    jz .Lra_500
    mov rdi, rax
    call pgr_error_sqlstate
    mov rcx, SQLSTATE_FK_VIOLATION
    cmp rax, rcx
    jne .Lra_500
    jmp .Lra_postnf

.Lra_send:                              # rdi = end of body, esi = status
    mov rdx, rdi
    sub rdx, rbp
    mov edi, r12d
    mov ecx, r13d
    and ecx, OPF_CLOSE | OPF_GZ
    call resp_body
    mov rdi, rbx
    call op_rearm
    mov edi, r12d
    call conn_resume
    jmp .Lra_free
.Lra_postnf:
    mov esi, E_POSTNF
    jmp .Lra_err
.Lra_500:
    mov esi, E_INTERNAL
.Lra_err:
    mov edi, r12d
    mov edx, r13d
    and edx, OPF_CLOSE
    call resp_error
    mov rdi, rbx
    call op_rearm
    mov edi, r12d
    call conn_resume
.Lra_free:
    mov rdi, rbx
    call op_free_aux
    add rsp, 96
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.section .note.GNU-stack,"",@progbits
