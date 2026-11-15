# api.s: request routing, JWT authentication and the POST /posts body rules; hands statements to pgio.s.
#
#   api_handle(fd, req)   req = parsed request (REQ_*). Answers directly (errors) or queues a statement and
#                         marks the connection busy.
#
# Scratch: the worker arena is free while a request is being routed (responses are rendered there only when
# the Postgres reply arrives), so decoded token parts and the request body are decoded into it.

.include "defs.inc"
.include "app.inc"

.equ SCR_HDR, 0                         # decoded JWT header (<= 512 B)
.equ SCR_PL, 2048                       # decoded JWT payload (<= 2048 B)
.equ SCR_MAC, 6144                      # HMAC (32 B)
.equ SCR_SIG, 6208                      # decoded signature (32 B + slack)
.equ SCR_SUB, 6400                      # decoded sub (<= 128 B)
.equ SCR_UNAME, 8192                    # decoded username (<= 2 KB)
.equ SCR_BODY, 16384                    # decoded post body (<= MAXREQ)

.section .rodata
DEFSTR s_feed, "/feed"
DEFSTR s_health, "/health"
DEFSTR s_posts, "/posts"
DEFSTR s_posts_sl, "/posts/"
DEFSTR s_like, "/like"
DEFSTR s_bearer, "Bearer "
DEFSTR s_alg, "alg"
DEFSTR s_hs256, "HS256"
DEFSTR s_exp, "exp"
DEFSTR s_nbf, "nbf"
DEFSTR s_sub, "sub"
DEFSTR s_username, "username"
DEFSTR s_body, "body"

.text

# ---------------------------------------------------------------- parse_id(ptr, len) -> rax = id (> 0) | -1
# Digits only, no sign. Values past 10^17 saturate (a huge id simply finds no row).
parse_id:
    test rsi, rsi
    jz .Lpid_bad
    lea r8, [rdi + rsi]
    xor eax, eax
    mov r9, 100000000000000000
.Lpid_loop:
    cmp rdi, r8
    jae .Lpid_end
    movzx ecx, byte ptr [rdi]
    sub ecx, 0x30
    cmp ecx, 9
    ja .Lpid_bad
    inc rdi
    cmp rax, r9
    jae .Lpid_sat
    imul rax, rax, 10
    add rax, rcx
    jmp .Lpid_loop
.Lpid_sat:
    mov rax, 0x7fffffffffffffff
    jmp .Lpid_loop
.Lpid_end:
    test rax, rax
    jz .Lpid_bad
    ret
.Lpid_bad:
    mov rax, -1
    ret

# ---------------------------------------------------------------- jsws_at(p, end) -> rax = bytes of JS whitespace at p (0 none)
jsws_at:
    movzx eax, byte ptr [rdi]
    cmp al, 0x20
    je .Lws1
    cmp al, 9
    jb .Lws_more
    cmp al, 13
    jbe .Lws1
.Lws_more:
    mov rdx, rsi
    sub rdx, rdi                        # bytes left
    cmp rdx, 2
    jb .Lws0
    cmp al, 0xc2
    jne 1f
    cmp byte ptr [rdi + 1], 0xa0
    je .Lws2
    jmp .Lws0
1:  cmp rdx, 3
    jb .Lws0
    movzx ecx, byte ptr [rdi + 1]
    movzx edx, byte ptr [rdi + 2]
    shl eax, 16
    shl ecx, 8
    or eax, ecx
    or eax, edx
    cmp eax, 0xe19a80
    je .Lws3
    cmp eax, 0xe38080
    je .Lws3
    cmp eax, 0xefbbbf
    je .Lws3
    cmp eax, 0xe280a8
    je .Lws3
    cmp eax, 0xe280a9
    je .Lws3
    cmp eax, 0xe280af
    je .Lws3
    cmp eax, 0xe2819f
    je .Lws3
    cmp eax, 0xe28080
    jb .Lws0
    cmp eax, 0xe2808a
    jbe .Lws3
.Lws0:
    xor eax, eax
    ret
.Lws1:
    mov eax, 1
    ret
.Lws2:
    mov eax, 2
    ret
.Lws3:
    mov eax, 3
    ret

# jsws_before(start, end) -> rax = bytes of JS whitespace ending at end (0 none)
jsws_before:
    movzx eax, byte ptr [rsi - 1]
    cmp al, 0x20
    je .Lwb1
    cmp al, 9
    jb .Lwb_more
    cmp al, 13
    jbe .Lwb1
.Lwb_more:
    mov rdx, rsi
    sub rdx, rdi
    cmp rdx, 2
    jb .Lwb0
    cmp byte ptr [rsi - 2], 0xc2
    jne 1f
    cmp al, 0xa0
    je .Lwb2
1:  cmp rdx, 3
    jb .Lwb0
    push rsi
    lea rdi, [rsi - 3]
    call jsws_at
    pop rsi
    cmp eax, 3
    je .Lwb3
.Lwb0:
    xor eax, eax
    ret
.Lwb1:
    mov eax, 1
    ret
.Lwb2:
    mov eax, 2
    ret
.Lwb3:
    mov eax, 3
    ret

# ---------------------------------------------------------------- number_check(ptr, len, now, is_exp) -> rax = 1 ok / 0 bad
# exp: ok when now < value; nbf: ok when now >= value. Integers with an optional fraction; anything else is bad.
number_check:
    lea r8, [rdi + rsi]
    xor r9d, r9d                        # negative
    xor eax, eax
    cmp rdi, r8
    jae .Lnc_bad
    cmp byte ptr [rdi], 0x2d
    jne 1f
    mov r9d, 1
    inc rdi
1:  xor r10d, r10d                      # digits
    mov rsi, 1000000000000000000        # saturation limit
.Lnc_int:
    cmp rdi, r8
    jae .Lnc_num
    movzx r11d, byte ptr [rdi]
    cmp r11b, 0x2e
    je .Lnc_frac
    sub r11d, 0x30
    cmp r11d, 9
    ja .Lnc_bad
    inc r10d
    cmp rax, rsi
    jae 2f
    imul rax, rax, 10
    add rax, r11
2:  inc rdi
    jmp .Lnc_int
.Lnc_frac:
    inc rdi
    xor r11d, r11d                      # nonzero fraction seen
.Lnc_fr:
    cmp rdi, r8
    jae .Lnc_num2
    movzx esi, byte ptr [rdi]
    sub esi, 0x30
    cmp esi, 9
    ja .Lnc_bad
    or r11d, esi
    inc rdi
    jmp .Lnc_fr
.Lnc_num2:
    test r11d, r11d
    jz .Lnc_num
    # value = int + fraction: exp ok iff now < int + f  <=> now <= int; nbf ok iff now >= int + f <=> now > int
    test r9d, r9d
    jnz .Lnc_negf
    test ecx, ecx
    jz .Lnc_nbf_f
    cmp rdx, rax                        # exp: now <= int
    jbe .Lnc_ok
    jmp .Lnc_bad
.Lnc_nbf_f:
    cmp rdx, rax                        # nbf: now > int
    ja .Lnc_ok
    jmp .Lnc_bad
.Lnc_negf:
    # negative with fraction: value < 0 <= now: exp bad, nbf ok
    test ecx, ecx
    jnz .Lnc_bad
    jmp .Lnc_ok
.Lnc_num:
    test r10d, r10d
    jz .Lnc_bad
    test r9d, r9d
    jnz .Lnc_neg
    test ecx, ecx
    jz .Lnc_nbf
    cmp rdx, rax                        # exp: now < value
    jb .Lnc_ok
    jmp .Lnc_bad
.Lnc_nbf:
    cmp rdx, rax                        # nbf: now >= value
    jae .Lnc_ok
    jmp .Lnc_bad
.Lnc_neg:                               # value <= 0 <= now
    test ecx, ecx
    jnz .Lnc_bad
.Lnc_ok:
    mov eax, 1
    ret
.Lnc_bad:
    xor eax, eax
    ret

# ---------------------------------------------------------------- api_auth(ptr, len) -> rax = 0 ok | E_NOBEARER | E_BADTOKEN | E_PAYLOAD
#   on success rdx = user id, rcx = username length (bytes at arena + SCR_UNAME)
api_auth:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp
    sub rsp, 24
    mov rbp, r15                        # worker
    mov r15, [r15 + W_ARENA]            # scratch base
    test rdi, rdi
    jz .Laa_nobearer
    cmp rsi, 7
    jb .Laa_nobearer
    mov rbx, rdi
    mov r12, rsi
    mov esi, 7
    lea rdx, [rip + s_bearer]
    mov ecx, 7
    call rt_memeq
    test eax, eax
    jz .Laa_nobearer
    add rbx, 7                          # token
    sub r12, 7
    # dots
    mov rdi, rbx
    mov rsi, r12
    mov edx, 0x2e
    call rt_memchr                      # rax = ptr | 0
    test rax, rax
    jz .Laa_bad
    mov r13, rax                        # d1
    lea rdi, [rax + 1]
    lea rsi, [rbx + r12]
    sub rsi, rdi
    mov edx, 0x2e
    call rt_memchr
    test rax, rax
    jz .Laa_bad
    mov r14, rax                        # d2
    lea rdi, [rax + 1]
    lea rsi, [rbx + r12]
    sub rsi, rdi
    mov edx, 0x2e
    call rt_memchr
    test rax, rax
    jnz .Laa_bad
    # signature: 43 chars -> 32 bytes
    lea rax, [rbx + r12]
    sub rax, r14
    dec rax
    cmp rax, 43
    jne .Laa_bad
    lea rdi, [r14 + 1]
    mov esi, 43
    lea rdx, [r15 + SCR_SIG]
    call crypto_b64url_decode
    cmp rax, 32
    jne .Laa_bad
    lea rdi, [rip + g_hmac]
    mov rsi, rbx
    mov rdx, r14
    sub rdx, rbx
    lea rcx, [r15 + SCR_MAC]
    call crypto_hmac_ctx
    lea rdi, [r15 + SCR_MAC]
    mov esi, 32
    lea rdx, [r15 + SCR_SIG]
    mov ecx, 32
    call rt_memeq
    test eax, eax
    jz .Laa_bad
    # header
    mov rdx, r13
    sub rdx, rbx
    cmp rdx, 680
    ja .Laa_bad
    mov rdi, rbx
    mov rsi, rdx
    lea rdx, [r15 + SCR_HDR]
    call crypto_b64url_decode
    test rax, rax
    js .Laa_bad
    mov [rsp], rax                      # header length
    mov rdx, r14
    sub rdx, r13
    dec rdx
    cmp rdx, 2700
    ja .Laa_bad
    lea rdi, [r13 + 1]
    mov rsi, rdx
    lea rdx, [r15 + SCR_PL]
    call crypto_b64url_decode
    test rax, rax
    js .Laa_bad
    mov [rsp + 8], rax                  # payload length
    # header must be a JSON object with "alg": "HS256"
    lea rdi, [r15 + SCR_HDR]
    mov rsi, [rsp]
    call json_validate
    test eax, eax
    jz .Laa_bad
    lea rdi, [r15 + SCR_HDR]
    mov rsi, [rsp]
    lea rdx, [rip + s_alg]
    mov ecx, s_alg_len
    call json_find
    test rax, rax
    jz .Laa_bad
    cmp ecx, JT_STRING
    jne .Laa_bad
    cmp rdx, 64
    ja .Laa_bad
    mov rdi, rax
    mov rsi, rdx
    lea rdx, [r15 + SCR_SUB]            # scratch
    call json_str_decode
    cmp rax, 5
    jne .Laa_bad
    lea rdi, [r15 + SCR_SUB]
    mov esi, 5
    lea rdx, [rip + s_hs256]
    mov ecx, 5
    call rt_memeq
    test eax, eax
    jz .Laa_bad
    # payload: object
    lea rdi, [r15 + SCR_PL]
    mov rsi, [rsp + 8]
    call json_validate
    test eax, eax
    jz .Laa_bad
    # exp (required, number, now < exp)
    lea rdi, [r15 + SCR_PL]
    mov rsi, [rsp + 8]
    lea rdx, [rip + s_exp]
    mov ecx, s_exp_len
    call json_find
    test rax, rax
    jz .Laa_bad
    cmp ecx, JT_NUMBER
    jne .Laa_bad
    mov rdi, rax
    mov rsi, rdx
    mov rdx, [rbp + W_NOW]
    mov ecx, 1
    call number_check
    test eax, eax
    jz .Laa_bad
    # nbf (optional, number, now >= nbf)
    lea rdi, [r15 + SCR_PL]
    mov rsi, [rsp + 8]
    lea rdx, [rip + s_nbf]
    mov ecx, s_nbf_len
    call json_find
    test rax, rax
    jz 1f
    cmp ecx, JT_NUMBER
    jne .Laa_bad
    mov rdi, rax
    mov rsi, rdx
    mov rdx, [rbp + W_NOW]
    xor ecx, ecx
    call number_check
    test eax, eax
    jz .Laa_bad
1:  # sub: string of 1..15 digits, > 0
    lea rdi, [r15 + SCR_PL]
    mov rsi, [rsp + 8]
    lea rdx, [rip + s_sub]
    mov ecx, s_sub_len
    call json_find
    test rax, rax
    jz .Laa_payload
    cmp ecx, JT_STRING
    jne .Laa_payload
    cmp rdx, 100
    ja .Laa_payload
    mov rdi, rax
    mov rsi, rdx
    lea rdx, [r15 + SCR_SUB]
    call json_str_decode
    test rax, rax
    jle .Laa_payload
    cmp rax, 15
    ja .Laa_payload
    lea rdi, [r15 + SCR_SUB]
    mov rsi, rax
    call parse_id                       # digits only, > 0 (no saturation possible at <= 15 digits)
    test rax, rax
    js .Laa_payload
    mov r13, rax                        # user id
    # username: string
    lea rdi, [r15 + SCR_PL]
    mov rsi, [rsp + 8]
    lea rdx, [rip + s_username]
    mov ecx, s_username_len
    call json_find
    test rax, rax
    jz .Laa_payload
    cmp ecx, JT_STRING
    jne .Laa_payload
    mov rdi, rax
    mov rsi, rdx
    lea rdx, [r15 + SCR_UNAME]
    call json_str_decode
    test rax, rax
    js .Laa_payload
    cmp rax, 1024
    ja .Laa_payload
    mov rcx, rax
    mov rdx, r13
    xor eax, eax
    jmp .Laa_ret
.Laa_nobearer:
    mov eax, E_NOBEARER
    jmp .Laa_ret
.Laa_bad:
    mov eax, E_BADTOKEN
    jmp .Laa_ret
.Laa_payload:
    mov eax, E_PAYLOAD
.Laa_ret:
    mov r15, rbp
    add rsp, 24
    pop rbp
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# ---------------------------------------------------------------- api_handle(fd, req)
.globl api_handle
api_handle:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    sub rsp, 32
    mov r12d, edi
    mov rbx, rsi
    # op flags from the request
    mov eax, [rbx + REQ_FLAGS]
    xor r13d, r13d
    test eax, REQF_CLOSE
    jz 1f
    or r13d, OPF_CLOSE
1:  test eax, REQF_GZIP
    jz 2f
    or r13d, OPF_GZ
2:  mov rax, [rbx + REQ_METHOD]
    mov rsi, [rbx + REQ_PATH]
    mov rdx, [rbx + REQ_PATH_LEN]
    cmp rax, M_GET
    jne .Lah_not_get
    # GET /feed
    cmp rdx, 5
    jne 3f
    mov rdi, rsi
    mov esi, 5
    lea rdx, [rip + s_feed]
    mov ecx, 5
    call rt_memeq
    test eax, eax
    jz .Lah_404
    mov edi, K_FEED
    jmp .Lah_submit_noid
3:  cmp rdx, 7
    jne .Lah_posts_get
    mov rdi, rsi
    mov esi, 7
    lea rdx, [rip + s_health]
    mov ecx, 7
    call rt_memeq
    test eax, eax
    jz .Lah_posts_get
    mov edi, K_HEALTH
    jmp .Lah_submit_noid
.Lah_posts_get:
    # GET /posts/<id>  (no further slash)
    mov rsi, [rbx + REQ_PATH]
    mov rdx, [rbx + REQ_PATH_LEN]
    cmp rdx, 7
    jbe .Lah_404
    call .Lis_posts_prefix
    test eax, eax
    jz .Lah_404
    mov rdi, [rbx + REQ_PATH]
    add rdi, 7
    mov rsi, [rbx + REQ_PATH_LEN]
    sub rsi, 7
    mov r14, rdi
    mov rbp, rsi
    mov edx, 0x2f
    call rt_memchr
    test rax, rax
    jnz .Lah_404                        # /posts/<id>/something with GET
    mov rdi, r14
    mov rsi, rbp
    call parse_id
    test rax, rax
    js .Lah_badid
    mov rbp, rax                        # id
    mov edi, K_POST
    xor r8d, r8d                        # uid
    xor r9d, r9d                        # aux
    jmp .Lah_submit
.Lah_not_get:
    cmp rax, M_POST
    jne .Lah_404
    # POST /posts
    cmp rdx, 6
    jne .Lah_post_like
    mov rdi, rsi
    mov esi, 6
    lea rdx, [rip + s_posts]
    mov ecx, 6
    call rt_memeq
    test eax, eax
    jz .Lah_404
    jmp .Lah_create
.Lah_post_like:
    cmp rdx, 7
    jbe .Lah_404
    call .Lis_posts_prefix
    test eax, eax
    jz .Lah_404
    mov rdi, [rbx + REQ_PATH]
    add rdi, 7
    mov rsi, [rbx + REQ_PATH_LEN]
    sub rsi, 7
    mov r14, rdi                        # seg
    mov rbp, rsi                        # seg + rest length
    mov edx, 0x2f
    call rt_memchr
    test rax, rax
    jz .Lah_404
    # rest must be "/like"
    lea rdx, [r14 + rbp]
    sub rdx, rax
    cmp rdx, 5
    jne .Lah_404
    mov [rsp], rax                      # slash position
    mov rdi, rax
    mov esi, 5
    lea rdx, [rip + s_like]
    mov ecx, 5
    call rt_memeq
    test eax, eax
    jz .Lah_404
    # auth first, then the id
    mov rdi, [rbx + REQ_AUTH]
    mov rsi, [rbx + REQ_AUTH_LEN]
    call api_auth
    test eax, eax
    jnz .Lah_err                        # eax = error id
    mov [rsp + 8], rdx                  # uid
    mov rdi, r14
    mov rsi, [rsp]
    sub rsi, r14
    call parse_id
    test rax, rax
    js .Lah_badid
    mov rbp, rax                        # post id
    mov r8, [rsp + 8]
    xor r9d, r9d
    mov edi, K_LIKE
    jmp .Lah_submit
.Lah_create:
    mov rdi, [rbx + REQ_AUTH]
    mov rsi, [rbx + REQ_AUTH_LEN]
    call api_auth
    test eax, eax
    jnz .Lah_err
    mov [rsp + 8], rdx                  # uid
    mov [rsp + 16], rcx                 # username length (arena + SCR_UNAME)
    # body: strict JSON object with a string "body"
    mov rdi, [rbx + REQ_BODY]
    mov rsi, [rbx + REQ_BODY_LEN]
    call json_validate
    test eax, eax
    jz .Lah_malformed
    mov rdi, [rbx + REQ_BODY]
    mov rsi, [rbx + REQ_BODY_LEN]
    lea rdx, [rip + s_body]
    mov ecx, s_body_len
    call json_find
    test rax, rax
    jz .Lah_bodyreq
    cmp ecx, JT_STRING
    jne .Lah_bodyreq
    mov rdi, rax
    mov rsi, rdx
    mov rdx, [r15 + W_ARENA]
    add rdx, SCR_BODY
    call json_str_decode
    test rax, rax
    js .Lah_malformed
    # trim JS whitespace
    mov r14, [r15 + W_ARENA]
    add r14, SCR_BODY                   # s
    lea rbp, [r14 + rax]                # e
.Lah_ltrim:
    cmp r14, rbp
    jae .Lah_bodyreq
    mov rdi, r14
    mov rsi, rbp
    call jsws_at
    test eax, eax
    jz .Lah_rtrim
    add r14, rax
    jmp .Lah_ltrim
.Lah_rtrim:
    cmp rbp, r14
    jbe .Lah_bodyreq
    mov rdi, r14
    mov rsi, rbp
    call jsws_before
    test eax, eax
    jz .Lah_trimmed
    sub rbp, rax
    jmp .Lah_rtrim
.Lah_trimmed:
    # code points <= 500
    mov rdi, r14
    xor ecx, ecx
4:  cmp rdi, rbp
    jae 5f
    movzx eax, byte ptr [rdi]
    and eax, 0xc0
    cmp eax, 0x80
    je 6f
    inc ecx
6:  inc rdi
    jmp 4b
5:  cmp ecx, 500
    ja .Lah_toolong
    # aux block: username + body
    mov rdx, rbp
    sub rdx, r14                        # body length
    mov [rsp + 24], rdx
    lea rdi, [rdx + AUX_DATA]
    add rdi, [rsp + 16]
    call rt_alloc                       # rax = block, rdx = cap
    mov r9, rax
    mov [rax + AUX_CAP], edx
    mov rcx, [rsp + 16]
    mov [rax + AUX_ULEN], ecx
    mov rdx, [rsp + 24]
    mov [rax + AUX_BLEN], edx
    lea rdi, [rax + AUX_DATA]
    mov rsi, [r15 + W_ARENA]
    add rsi, SCR_UNAME
    mov rdx, rcx
    push r9
    push r9
    call rt_memcpy
    mov rdi, rax
    mov rsi, r14
    mov rdx, [rsp + 16 + 24]
    call rt_memcpy
    pop r9
    pop r9
    mov r8, [rsp + 8]                   # uid
    xor ebp, ebp                        # id
    mov edi, K_INS
    jmp .Lah_submit
.Lah_submit_noid:
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
.Lah_submit:
    # pgio_submit(kind, fd, flags, id, uid, aux) -> 1 queued / 0 no connection (aux freed)
    mov r14d, edi
    mov esi, r12d
    mov edx, r13d
    mov rcx, rbp
    call pgio_submit
    test eax, eax
    jnz .Lah_ret
    # no database connection: 500, or the degraded health answer
    cmp r14d, K_HEALTH
    je .Lah_degraded
    mov eax, E_INTERNAL
    jmp .Lah_err
.Lah_degraded:
    mov eax, E_DEGRADED
    jmp .Lah_err
.Lah_404:
    mov eax, E_NOTFOUND
    jmp .Lah_err
.Lah_badid:
    mov eax, E_BADID
    jmp .Lah_err
.Lah_malformed:
    mov eax, E_MALFORMED
    jmp .Lah_err
.Lah_bodyreq:
    mov eax, E_BODYREQ
    jmp .Lah_err
.Lah_toolong:
    mov eax, E_TOOLONG
.Lah_err:
    mov edi, r12d
    mov esi, eax
    mov edx, r13d
    and edx, OPF_CLOSE
    call resp_error
.Lah_ret:
    add rsp, 32
    pop rbp
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

# path starts with "/posts/" (rbx = req) -> eax
.Lis_posts_prefix:
    mov rdi, [rbx + REQ_PATH]
    mov esi, 7
    lea rdx, [rip + s_posts_sl]
    mov ecx, 7
    jmp rt_memeq

.section .note.GNU-stack,"",@progbits
