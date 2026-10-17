.global _start
.text
_start:
    test $15, %rsp
    jnz .Lfail
    mov %rsp, %r14
    mov $0x11223344, %rbx
    mov $0x22334455, %rbp
    mov $0x33445566, %r12
    mov $0x44556677, %r13
    mov $0x55667788, %r15
    movdqa .Lsimd_pattern(%rip), %xmm0
    movdqa %xmm0, %xmm7
    movdqa %xmm0, %xmm15
    ldmxcsr .Luser_mxcsr(%rip)
    fldcw .Luser_control(%rip)
    mov $0x1337, %rax
    mov $0xA11E, %rdi
    mov $0xBEEF, %rsi
    mov $0xCDEF, %rdx
    mov $0xDEAD, %r10
    mov $0xBABE, %r8
    mov $0xFEED, %r9
    std
    syscall
    test %rax, %rax
    jnz .Lfail
    cmp $0xA11E, %rdi
    jne .Lfail
    cmp $0xBEEF, %rsi
    jne .Lfail
    cmp $0xCDEF, %rdx
    jne .Lfail
    cmp $0xDEAD, %r10
    jne .Lfail
    cmp $0xBABE, %r8
    jne .Lfail
    cmp $0xFEED, %r9
    jne .Lfail
    cmp $0x11223344, %rbx
    jne .Lfail
    cmp $0x22334455, %rbp
    jne .Lfail
    cmp $0x33445566, %r12
    jne .Lfail
    cmp $0x44556677, %r13
    jne .Lfail
    cmp $0x55667788, %r15
    jne .Lfail
    cmp %r14, %rsp
    jne .Lfail
    pushfq
    pop %rax
    test $0x400, %rax
    jz .Lfail
    cld
    pcmpeqb .Lsimd_pattern(%rip), %xmm0
    pmovmskb %xmm0, %eax
    cmp $0xffff, %eax
    jne .Lfail
    pcmpeqb .Lsimd_pattern(%rip), %xmm7
    pmovmskb %xmm7, %eax
    cmp $0xffff, %eax
    jne .Lfail
    pcmpeqb .Lsimd_pattern(%rip), %xmm15
    pmovmskb %xmm15, %eax
    cmp $0xffff, %eax
    jne .Lfail
    sub $16, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    cmpl $0x3f80, (%rsp)
    jne .Lfail
    cmpw $0x077f, 4(%rsp)
    jne .Lfail
    add $16, %rsp

    # Real handler entry/restorer/rt_sigreturn through the syscall assembly.
    # Queue a blocked SIGUSR1, then deliver on unmask using an alternate stack.
    sub $64, %rsp
    lea .Lsignal_handler(%rip), %rax
    mov %rax, 0(%rsp)
    movq $0x0c000004, 8(%rsp) # SA_RESTORER | SA_ONSTACK | SA_SIGINFO
    lea .Lsignal_restorer(%rip), %rax
    mov %rax, 16(%rsp)
    movq $0, 24(%rsp)
    mov $13, %eax
    mov $10, %edi
    mov %rsp, %rsi
    xor %edx, %edx
    mov $8, %r10d
    syscall
    test %rax, %rax
    jnz .Lfail
    lea .Lsignal_stack(%rip), %rax
    mov %rax, 0(%rsp)
    movq $0, 8(%rsp)
    movq $8192, 16(%rsp)
    mov $131, %eax
    mov %rsp, %rdi
    xor %esi, %esi
    syscall
    test %rax, %rax
    jnz .Lfail
    movq $512, 0(%rsp)
    mov $14, %eax
    xor %edi, %edi
    mov %rsp, %rsi
    xor %edx, %edx
    mov $8, %r10d
    syscall
    test %rax, %rax
    jnz .Lfail
    mov $39, %eax
    syscall
    mov %rax, %rbx
    mov %rax, %rdi
    mov $10, %esi
    mov $62, %eax
    syscall
    test %rax, %rax
    jnz .Lfail
    cmpl $0, .Lsignal_count(%rip)
    jne .Lfail
    movdqa .Lsimd_pattern(%rip), %xmm0
    mov $0x76543210, %r13
    mov $14, %eax
    mov $1, %edi
    mov %rsp, %rsi
    xor %edx, %edx
    mov $8, %r10d
    syscall
    test %rax, %rax
    jnz .Lfail
    cmpl $1, .Lsignal_count(%rip)
    jne .Lfail
    cmp $0x76543210, %r13
    jne .Lfail
    mov $39, %eax
    syscall
    cmp %rax, %rbx
    jne .Lfail
    pcmpeqb .Lsimd_pattern(%rip), %xmm0
    pmovmskb %xmm0, %eax
    cmp $0xffff, %eax
    jne .Lfail
    movq $2, 8(%rsp)
    mov $131, %eax
    mov %rsp, %rdi
    xor %esi, %esi
    syscall
    test %rax, %rax
    jnz .Lfail
    add $64, %rsp
    xor %rdi, %rdi
    jmp .Lexit
.Lfail:
    cld
    mov $1, %rdi
.Lexit:
    mov $60, %rax
    syscall
1:
    jmp 1b

.Lsignal_handler:
    cmp $10, %edi
    jne .Lfail
    cmpl $10, (%rsi)
    jne .Lfail
    cmpq $0x76543210, 80(%rdx) # ucontext + mcontext + R13
    jne .Lfail
    lea .Lsignal_stack(%rip), %rax
    cmp %rax, %rsp
    jb .Lfail
    add $8192, %rax
    cmp %rax, %rsp
    jae .Lfail
    mov %rsp, %rax
    and $15, %eax
    cmp $8, %eax
    jne .Lfail
    incl .Lsignal_count(%rip)
    xor %ebx, %ebx
    xor %ebp, %ebp
    xor %r12d, %r12d
    xor %r13d, %r13d
    xor %r14d, %r14d
    xor %r15d, %r15d
    pxor %xmm0, %xmm0
    ret
.Lsignal_restorer:
    mov $15, %eax
    syscall
    ud2

.bss
.balign 16
.Lsignal_stack: .skip 8192
.Lsignal_count: .skip 4

.section .rodata
.balign 16
.Lsimd_pattern:
    .quad 0x1122334455667788, 0x8877665544332211
.Luser_mxcsr:
    .long 0x3f80
.Luser_control:
    .short 0x077f
.section .note.GNU-stack,"",@progbits
