//! x86-64 assembly main loop of the gzip encoder (see deflate.zig for the algorithm and the Zig reference version it must
//! match byte for byte).
//!
//! Why assembly: the compiled loop keeps its state in stack slots (too many live values for the registers it allots),
//! re-checks conditions that cannot fail here, and uses 32-bit flushes with a branch on every symbol. This version keeps
//! everything in registers and drops what is provably redundant for inputs up to 32 KiB: a table entry that is at least
//! `base` is always an earlier position of this call, and its distance is below 32768.
//!
//! Register map:  rsi input   rdi table   r8d base   r9 i   r11 output pointer   r12 bit accumulator   r13 bit count
//!                r14 literal table   r15 best match length   rdx best match pointer   rbp match length   rcx max length
//!                r10 pointer to the current input position   rax rbx scratch      [rsp] n   [rsp+8] state   [rsp+16] limit
const std = @import("std");
const builtin = @import("builtin");
const tabs = @import("deflate_tabs.zig");

/// True when the assembly routine can be used (needs BMI1/BMI2: tzcnt, shlx, shrx, bzhi).
pub const available = builtin.cpu.arch == .x86_64 and std.Target.x86.featureSetHasAll(builtin.cpu.features, .{ .bmi, .bmi2 });

/// State passed to and returned from the routine. Field offsets are used by the assembly.
pub const Core = extern struct {
    in: [*]const u8, // 0
    n: usize, // 8
    tab: [*]u32, // 16
    base: u32, // 24
    out: [*]u8, // 32: next output byte
    acc: u64, // 40
    nb: u64, // 48
    i: usize, // 56: first input position not yet coded
};

/// Codes every position below `n - 8`. Requires `n >= 8`, `n <= 32768 + 8` and a table of `2 << 12` entries.
pub extern fn mu_deflate_core(c: *Core) callconv(.c) void;

comptime {
    if (available) {
        @export(&tabs.lit_tab, .{ .name = "mu_lit_tab" });
        @export(&tabs.len_tab, .{ .name = "mu_len_tab" });
        @export(&tabs.dist_code5, .{ .name = "mu_dist5" });
        asm (std.fmt.comptimePrint(source, .{ .shift = 32 - tabs.hash_bits, .cap = tabs.interior_cap }));
    }
}

const source =
    \\.intel_syntax noprefix
    \\.text
    \\.globl mu_deflate_core
    \\.type mu_deflate_core,@function
    \\.p2align 6
    \\
    \\.macro EXTEND cp, id
    \\    mov rcx, qword ptr [rsp]
    \\    sub rcx, r9
    \\    mov eax, 258
    \\    cmp rcx, rax
    \\    cmova rcx, rax
    \\    mov ebp, 4
    \\.L\id\()_loop:
    \\    lea rax, [rbp+8]
    \\    cmp rax, rcx
    \\    ja .L\id\()_tail
    \\    mov rax, qword ptr [\cp+rbp]
    \\    xor rax, qword ptr [r10+rbp]
    \\    jnz .L\id\()_found
    \\    add rbp, 8
    \\    jmp .L\id\()_loop
    \\.L\id\()_found:
    \\    tzcnt rax, rax
    \\    shr eax, 3
    \\    add rbp, rax
    \\    jmp .L\id\()_done
    \\.L\id\()_tail:
    \\    cmp rbp, rcx
    \\    jae .L\id\()_done
    \\    mov al, byte ptr [\cp+rbp]
    \\    cmp al, byte ptr [r10+rbp]
    \\    jne .L\id\()_done
    \\    inc rbp
    \\    jmp .L\id\()_tail
    \\.L\id\()_done:
    \\.endm
    \\
    \\mu_deflate_core:
    \\    push rbx
    \\    push rbp
    \\    push r12
    \\    push r13
    \\    push r14
    \\    push r15
    \\    sub rsp, 24
    \\    mov qword ptr [rsp+8], rdi
    \\    mov rsi, qword ptr [rdi]
    \\    mov rax, qword ptr [rdi+8]
    \\    mov qword ptr [rsp], rax
    \\    lea rax, [rax-8]
    \\    mov qword ptr [rsp+16], rax
    \\    mov r8d, dword ptr [rdi+24]
    \\    mov r11, qword ptr [rdi+32]
    \\    mov r12, qword ptr [rdi+40]
    \\    mov r13, qword ptr [rdi+48]
    \\    mov r9, qword ptr [rdi+56]
    \\    mov rdi, qword ptr [rdi+16]
    \\    lea r14, [rip+mu_lit_tab]
    \\
    \\.p2align 5
    \\.Lloop:
    \\    cmp r9, qword ptr [rsp+16]
    \\    jae .Ldone
    \\    mov eax, dword ptr [rsi+r9]
    \\    imul ecx, eax, 0x9E3779B1
    \\    shr ecx, {[shift]d}
    \\    lea rcx, [rdi+rcx*8]
    \\    mov edx, dword ptr [rcx]
    \\    mov ebx, dword ptr [rcx+4]
    \\    mov dword ptr [rcx+4], edx
    \\    lea ebp, [r8+r9]
    \\    mov dword ptr [rcx], ebp
    \\    xor r15d, r15d
    \\    cmp edx, r8d
    \\    jb .Lcand1
    \\    sub edx, r8d
    \\    cmp eax, dword ptr [rsi+rdx]
    \\    jne .Lcand1
    \\    add rdx, rsi
    \\    lea r10, [rsi+r9]
    \\    EXTEND rdx, c0
    \\    mov r15, rbp
    \\    mov eax, dword ptr [rsi+r9]
    \\.Lcand1:
    \\    cmp ebx, r8d
    \\    jb .Lchosen
    \\    sub ebx, r8d
    \\    cmp eax, dword ptr [rsi+rbx]
    \\    jne .Lchosen
    \\    add rbx, rsi
    \\    lea r10, [rsi+r9]
    \\    EXTEND rbx, c1
    \\    cmp rbp, r15
    \\    jbe .Lchosen
    \\    mov r15, rbp
    \\    mov rdx, rbx
    \\.Lchosen:
    \\    test r15, r15
    \\    jz .Lliteral
    \\    # ---- match: r15 = length, rdx = pointer to the earlier copy ----
    \\    lea rbp, [rsi+r9]
    \\    sub rbp, rdx
    \\    dec rbp
    \\    lea rbx, [rip+mu_len_tab]
    \\    mov eax, dword ptr [rbx+r15*4]
    \\    cmp rbp, 4
    \\    jb .Lsmall
    \\    bsr rcx, rbp
    \\    lea rdx, [rcx-1]
    \\    shrx rbx, rbp, rdx
    \\    and ebx, 1
    \\    lea rcx, [rbx+rcx*2]
    \\    lea rbx, [rip+mu_dist5]
    \\    movzx ecx, byte ptr [rbx+rcx]
    \\    bzhi rbp, rbp, rdx
    \\    shl ebp, 5
    \\    or ecx, ebp
    \\    lea ebp, [rdx+5]
    \\    jmp .Lemit
    \\.Lsmall:
    \\    lea rbx, [rip+mu_dist5]
    \\    movzx ecx, byte ptr [rbx+rbp]
    \\    mov ebp, 5
    \\.Lemit:
    \\    movzx edx, ax
    \\    shr eax, 16
    \\    shlx rdx, rdx, r13
    \\    or r12, rdx
    \\    add r13, rax
    \\    shlx rcx, rcx, r13
    \\    or r12, rcx
    \\    add r13, rbp
    \\    cmp r13, 32
    \\    jb .Lnoflush
    \\    mov dword ptr [r11], r12d
    \\    add r11, 4
    \\    shr r12, 32
    \\    sub r13, 32
    \\.Lnoflush:
    \\    # ---- index the inside of the match: positions i+1 .. end-1, or only the last two when it is long ----
    \\    lea rbx, [r9+1]
    \\    lea rcx, [r9+r15]
    \\    mov rax, qword ptr [rsp+16]
    \\    cmp rcx, rax
    \\    cmova rcx, rax
    \\    cmp r15, {[cap]d}
    \\    jbe .Lins
    \\    lea rdx, [rcx-2]
    \\    cmp rdx, rbx
    \\    cmova rbx, rdx
    \\.Lins:
    \\    cmp rbx, rcx
    \\    jae .Linsdone
    \\    mov eax, dword ptr [rsi+rbx]
    \\    imul eax, eax, 0x9E3779B1
    \\    shr eax, {[shift]d}
    \\    lea rax, [rdi+rax*8]
    \\    mov edx, dword ptr [rax]
    \\    mov dword ptr [rax+4], edx
    \\    lea edx, [r8+rbx]
    \\    mov dword ptr [rax], edx
    \\    inc rbx
    \\    jmp .Lins
    \\.Linsdone:
    \\    add r9, r15
    \\    jmp .Lloop
    \\
    \\.p2align 4
    \\.Lliteral:
    \\    movzx eax, byte ptr [rsi+r9]
    \\    mov eax, dword ptr [r14+rax*4]
    \\    movzx edx, ax
    \\    shr eax, 16
    \\    shlx rdx, rdx, r13
    \\    or r12, rdx
    \\    add r13, rax
    \\    cmp r13, 32
    \\    jb .Lnolit
    \\    mov dword ptr [r11], r12d
    \\    add r11, 4
    \\    shr r12, 32
    \\    sub r13, 32
    \\.Lnolit:
    \\    inc r9
    \\    jmp .Lloop
    \\
    \\.Ldone:
    \\    mov rax, qword ptr [rsp+8]
    \\    mov qword ptr [rax+32], r11
    \\    mov qword ptr [rax+40], r12
    \\    mov qword ptr [rax+48], r13
    \\    mov qword ptr [rax+56], r9
    \\    add rsp, 24
    \\    pop r15
    \\    pop r14
    \\    pop r13
    \\    pop r12
    \\    pop rbp
    \\    pop rbx
    \\    ret
    \\.size mu_deflate_core, .-mu_deflate_core
    \\.att_syntax prefix
    \\
;
