bits 64
default rel

section .text

global arch_x86_64_syscall_entry
extern arch_syscall_dispatch

; Must match arch_info.syscall_ctx layout in x86_64.h
%define SYSCALL_CTX_KERNEL_RSP_OFFSET 0
%define SYSCALL_CTX_USER_RSP_OFFSET 8
%define SYSCALL_CTX_ACTIVE_OFFSET 16

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
%define SYSCALL_FRAME_SIZE 176

arch_x86_64_syscall_entry:
    swapgs
    lfence
    cld

    mov qword [gs:SYSCALL_CTX_USER_RSP_OFFSET], rsp
    mov rsp, qword [gs:SYSCALL_CTX_KERNEL_RSP_OFFSET]
    and rsp, -16

    ; Extension after the existing 96-byte prefix: all other GP registers and
    ; the kernel FXSAVE address. Required for signal delivery/rt_sigreturn.
    push qword 0
    push qword 0
    push r11
    push rcx
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx

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
    ; C may use SSE/x87 even for ordinary memory operations.
    sub rsp, 512
    xor eax, eax
    mov rdi, rsp
    mov ecx, 64
    rep stosq
    fxsave64 [rsp]
    fninit
    ldmxcsr [rel syscall_kernel_mxcsr]
    lea rdi, [rsp + 512]
    mov [rdi + 160], rsp

    ; The complete frame is on the kernel stack and rsp is 16-byte aligned.
    ; Permit TLB IPIs while the C handler takes memory-management locks.
    mov qword [gs:SYSCALL_CTX_ACTIVE_OFFSET], 1
    sti
    call arch_syscall_dispatch

    ; Protect the return frame and swapgs transition until iretq restores IF.
    cli
    mov qword [gs:SYSCALL_CTX_ACTIVE_OFFSET], 0
    fxrstor64 [rsp]
    add rsp, 512
    ; Keep return value in rax. Restore volatile arg registers for a clean user return.
    mov rdi, [rsp + SYSCALL_FRAME_ARG0_OFFSET]
    mov rsi, [rsp + SYSCALL_FRAME_ARG1_OFFSET]
    mov rdx, [rsp + SYSCALL_FRAME_ARG2_OFFSET]
    mov r10, [rsp + SYSCALL_FRAME_ARG3_OFFSET]
    mov r8,  [rsp + SYSCALL_FRAME_ARG4_OFFSET]
    mov r9,  [rsp + SYSCALL_FRAME_ARG5_OFFSET]
    mov rbx, [rsp + 96]
    mov rbp, [rsp + 104]
    mov r12, [rsp + 112]
    mov r13, [rsp + 120]
    mov r14, [rsp + 128]
    mov r15, [rsp + 136]
    mov rcx, [rsp + 144]
    mov r11, [rsp + 152]

    add rsp, 56

    swapgs
    iretq

section .rodata
align 4
syscall_kernel_mxcsr: dd 0x1f80
