.section .text
.code32
.global _start
.extern kernel_main

_start:
    cli
    mov $0x90000, %esp
    call kernel_main

.hang:
    hlt
    jmp .hang

.section .note.GNU-stack,"",@progbits