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

    ; The stub has pushed interrupt number/error code. CS is at rsp+24.
    ; SYSCALL and kernel code already use kernel GS; user interrupts do not.
    test byte [rsp + 24], 3
    jz .kernel_gs_ready
    swapgs
.kernel_gs_ready:
    lfence
    cld

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
    sub rsp, 512
    fxsave64 [rsp]
    fninit
    ldmxcsr [rel interrupt_kernel_mxcsr]

    call ISRHANDLER

    fxrstor64 [rsp]

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

    ; CS is now at rsp+8 in the hardware return frame.
    test byte [rsp + 8], 3
    jz .return_with_kernel_gs
    swapgs
.return_with_kernel_gs:

    iretq

section .rodata
align 4
interrupt_kernel_mxcsr: dd 0x1f80
