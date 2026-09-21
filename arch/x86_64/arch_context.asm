bits 64
default rel

section .text

global arch_init_context
global arch_task_entry_trampoline
global arch_save_switch_and_execute_context
extern task_exit

; struct arch_task_context layout (x86_64):
; 0: rbx 8: rbp 16: r12 24: r13 32: r14 40: r15 48: rip 56: rsp
%define CTX_RBX 0
%define CTX_RBP 8
%define CTX_R12 16
%define CTX_R13 24
%define CTX_R14 32
%define CTX_R15 40
%define CTX_RIP 48
%define CTX_RSP 56

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

    mov rbx, [rsi + CTX_RBX]
    mov rbp, [rsi + CTX_RBP]
    mov r12, [rsi + CTX_R12]
    mov r13, [rsi + CTX_R13]
    mov r14, [rsi + CTX_R14]
    mov r15, [rsi + CTX_R15]

    mov rax, [rsi + CTX_RIP]
    mov rsp, [rsi + CTX_RSP]

    push rax

    ret