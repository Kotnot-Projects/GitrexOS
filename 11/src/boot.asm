BITS 16
ORG 0x7C00

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 64
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    ; VGA Mode 13h: 320x200, 256 colors, framebuffer at 0xA0000.
    mov ax, 0x0013
    int 0x10

    ; BIOS extended read: load the protected-mode kernel at physical 0x10000.
    mov si, dap
    mov word [dap + 2], KERNEL_SECTORS
    mov word [dap + 4], 0x0000
    mov word [dap + 6], 0x1000
    mov dword [dap + 8], 1
    mov dword [dap + 12], 0
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:protected_mode

disk_error:
    mov ax, 0x0003
    int 0x10
    mov si, disk_error_text
.print:
    lodsb
    test al, al
    jz $
    mov ah, 0x0E
    int 0x10
    jmp .print

BITS 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    call 0x10000
.halt:
    cli
    hlt
    jmp .halt

BITS 16
boot_drive db 0
disk_error_text db 'GITREXOS: disk read error', 0

dap:
    db 0x10, 0
    dw KERNEL_SECTORS
    dw 0
    dw 0x1000
    dq 1

gdt_start:
    dq 0
    ; flat 4 GiB code segment
    dw 0xFFFF, 0
    db 0, 0x9A, 0xCF, 0
    ; flat 4 GiB data segment
    dw 0xFFFF, 0
    db 0, 0x92, 0xCF, 0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times 510 - ($ - $$) db 0
dw 0xAA55
