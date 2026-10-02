.global _start
.text
_start:
    mov $0x1337, %rax
    mov $0xA11E, %rdi
    syscall

    mov $60, %rax
    xor %rdi, %rdi
    syscall

1:
    jmp 1b
