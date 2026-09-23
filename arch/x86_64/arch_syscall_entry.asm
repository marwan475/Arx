bits 64
default rel

section .text

global arch_x86_64_syscall_entry
extern arch_syscall_dispatch

; Must match arch_info.syscall_ctx layout in x86_64.h
%define SYSCALL_CTX_KERNEL_RSP_OFFSET 0
%define SYSCALL_CTX_USER_RSP_OFFSET 8

; Must match USER_CS/USER_SS selectors in x86_64.h
%define USER_CS_SELECTOR 0x1b
%define USER_SS_SELECTOR 0x23

arch_x86_64_syscall_entry:
    swapgs
    cld

    mov qword [gs:SYSCALL_CTX_USER_RSP_OFFSET], rsp
    mov rsp, qword [gs:SYSCALL_CTX_KERNEL_RSP_OFFSET]
    and rsp, -16

    ; Build an iret frame for return to user mode.
    push qword USER_SS_SELECTOR
    push qword [gs:SYSCALL_CTX_USER_RSP_OFFSET]
    push r11
    push qword USER_CS_SELECTOR
    push rcx

    ; Save syscall ABI args and number.
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax

    ; C ABI: (sysno, arg0, arg1, arg2, arg3, arg4, arg5)
    mov rdi, [rsp + 0]
    mov rsi, [rsp + 8]
    mov rdx, [rsp + 16]
    mov rcx, [rsp + 24]
    mov r8,  [rsp + 32]
    mov r9,  [rsp + 40]

    ; SysV: 7th integer argument goes on stack at [rsp] before call,
    ; and stack must be 16-byte aligned at call site.
    sub rsp, 16
    mov rax, [rsp + 64]
    mov [rsp + 0], rax

    call arch_syscall_dispatch

    add rsp, 16

    ; Keep return value in rax. Restore volatile arg registers for a clean user return.
    mov rdi, [rsp + 8]
    mov rsi, [rsp + 16]
    mov rdx, [rsp + 24]
    mov r10, [rsp + 32]
    mov r8,  [rsp + 40]
    mov r9,  [rsp + 48]

    add rsp, 56

    swapgs
    iretq