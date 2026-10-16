.global _start
.text
_start:
    mov $59, %eax
    lea path(%rip), %rdi
    lea args(%rip), %rsi
    lea env(%rip), %rdx
    syscall
    mov $111, %edi
    mov $60, %eax
    syscall
    ud2
.section .rodata
path: .asciz "/dynamic_fixture.elf"
.balign 8
args: .quad path, 0
env: .quad 0
.section .note.GNU-stack,"",@progbits
