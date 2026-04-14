bits 64
default rel

section .text

extern ISRHANDLER

; interrupts that push error code automatically
%define HAS_ERRCODE(num) ( \
 num = 8  || num = 10 || num = 11 || num = 12 || \
 num = 13 || num = 14 || num = 17 || num = 21 || \
 num = 29 || num = 30 )

%macro ISR_STUB 1
global ISR%1
ISR%1:
%if HAS_ERRCODE(%1)
    push qword %1        
%else
    push qword 0         
    push qword %1        
%endif
    jmp ISRCOMMON
%endmacro

%assign i 0
%rep 256
ISR_STUB i
%assign i i+1
%endrep

ISRCOMMON:

    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax

    mov rdi, rsp          

    mov rbx, rsp
    and rsp, -16

    call ISRHANDLER

    mov rsp, rbx          

    pop rax
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16           

    iretq