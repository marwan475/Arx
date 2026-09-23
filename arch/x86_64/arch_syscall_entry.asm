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

%define SYSCALL_FRAME_SYSCALL_NUMBER_OFFSET 0
%define SYSCALL_FRAME_ARG0_OFFSET 8
%define SYSCALL_FRAME_ARG1_OFFSET 16
%define SYSCALL_FRAME_ARG2_OFFSET 24
%define SYSCALL_FRAME_ARG3_OFFSET 32
%define SYSCALL_FRAME_ARG4_OFFSET 40
%define SYSCALL_FRAME_ARG5_OFFSET 48
%define SYSCALL_FRAME_USER_RIP_OFFSET 56
%define SYSCALL_FRAME_USER_CS_OFFSET 64
%define SYSCALL_FRAME_USER_RFLAGS_OFFSET 72
%define SYSCALL_FRAME_USER_RSP_OFFSET 80
%define SYSCALL_FRAME_USER_SS_OFFSET 88
%define SYSCALL_FRAME_SIZE 96

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

    ; C ABI: pass pointer to syscall frame in rdi.
    mov rdi, rsp

    call arch_syscall_dispatch

    ; Keep return value in rax. Restore volatile arg registers for a clean user return.
    mov rdi, [rsp + SYSCALL_FRAME_ARG0_OFFSET]
    mov rsi, [rsp + SYSCALL_FRAME_ARG1_OFFSET]
    mov rdx, [rsp + SYSCALL_FRAME_ARG2_OFFSET]
    mov r10, [rsp + SYSCALL_FRAME_ARG3_OFFSET]
    mov r8,  [rsp + SYSCALL_FRAME_ARG4_OFFSET]
    mov r9,  [rsp + SYSCALL_FRAME_ARG5_OFFSET]

    add rsp, 56

    swapgs
    iretq