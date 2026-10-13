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

.section .rodata
.balign 16
.Lsimd_pattern:
    .quad 0x1122334455667788, 0x8877665544332211
.Luser_mxcsr:
    .long 0x3f80
.Luser_control:
    .short 0x077f
.section .note.GNU-stack,"",@progbits
