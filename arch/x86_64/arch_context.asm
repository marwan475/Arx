bits 64
default rel

section .text

global arch_init_context
global arch_task_entry_trampoline
global arch_save_switch_and_execute_context
extern task_exit

; struct arch_task_context layout (x86_64):
; 0: rbx 8: rbp 16: r12 24: r13 32: r14 40: r15 48: rip 56: rsp
; 64: rflags 80: aligned FXSAVE image
%define CTX_RBX 0
%define CTX_RBP 8
%define CTX_R12 16
%define CTX_R13 24
%define CTX_R14 32
%define CTX_R15 40
%define CTX_RIP 48
%define CTX_RSP 56
%define CTX_RFLAGS 64
%define CTX_FX_STATE 80

; void arch_init_context(
;     struct arch_task_context* context, ; rdi
;     void* stack_top,                   ; rsi
;     arch_task_entry_t entry,           ; rdx
;     void* arg                          ; rcx
; )
arch_init_context:
    mov qword [rdi + CTX_RBX], 0
    mov qword [rdi + CTX_RBP], 0
    mov [rdi + CTX_R12], rdx
    mov [rdi + CTX_R13], rcx
    mov qword [rdi + CTX_R14], 0
    mov qword [rdi + CTX_R15], 0

    mov rax, rsi
    and rax, -16
    mov [rdi + CTX_RSP], rax

    lea rax, [rel arch_task_entry_trampoline]
    mov [rdi + CTX_RIP], rax
    mov qword [rdi + CTX_RFLAGS], 0x202
    fxsave64 [rdi + CTX_FX_STATE]

    ret

arch_task_entry_trampoline:
    mov rdi, r13
    call r12
    call task_exit
    ud2

; void arch_save_switch_and_execute_context(struct arch_task_context* out_current_context, const struct arch_task_context* new_context)
; rdi = out_current_context
; rsi = new_context
arch_save_switch_and_execute_context:
    pushfq
    pop rax
    mov [rdi + CTX_RFLAGS], rax
    cli
    fxsave64 [rdi + CTX_FX_STATE]
    mov [rdi + CTX_RBX], rbx
    mov [rdi + CTX_RBP], rbp
    mov [rdi + CTX_R12], r12
    mov [rdi + CTX_R13], r13
    mov [rdi + CTX_R14], r14
    mov [rdi + CTX_R15], r15

    mov rax, [rsp]
    mov [rdi + CTX_RIP], rax
    lea rax, [rsp + 8]
    mov [rdi + CTX_RSP], rax

    fxrstor64 [rsi + CTX_FX_STATE]

    mov rbx, [rsi + CTX_RBX]
    mov rbp, [rsi + CTX_RBP]
    mov r12, [rsi + CTX_R12]
    mov r13, [rsi + CTX_R13]
    mov r14, [rsi + CTX_R14]
    mov r15, [rsi + CTX_R15]

    mov rax, [rsi + CTX_RIP]
    mov rsp, [rsi + CTX_RSP]

    push rax
    push qword [rsi + CTX_RFLAGS]
    popfq

    ret

; SysV arguments: user RIP in rdi, user RSP in rsi, arg0 in rdx, arg1 in rcx.
; No C compiler register allocation is involved after building the return frame.
global arch_x86_64_enter_user_mode
arch_x86_64_enter_user_mode:
    cli
    push qword 0x23
    push rsi
    push qword 0x202
    push qword 0x1b
    push rdi
    mov rdi, rdx
    mov rsi, rcx
    fninit
    ldmxcsr [rel user_initial_mxcsr]
    pxor xmm0, xmm0
    pxor xmm1, xmm1
    pxor xmm2, xmm2
    pxor xmm3, xmm3
    pxor xmm4, xmm4
    pxor xmm5, xmm5
    pxor xmm6, xmm6
    pxor xmm7, xmm7
    pxor xmm8, xmm8
    pxor xmm9, xmm9
    pxor xmm10, xmm10
    pxor xmm11, xmm11
    pxor xmm12, xmm12
    pxor xmm13, xmm13
    pxor xmm14, xmm14
    pxor xmm15, xmm15
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    swapgs
    iretq

section .rodata
align 4
user_initial_mxcsr: dd 0x1f80
