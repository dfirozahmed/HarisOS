[org 0x7C00]            ; BIOS loads the boot sector at address 0x7C00
[bits 16]               ; CPU starts in 16-bit real mode

start:
    mov si, msg         ; SI = address of the message string

print:
    lodsb               ; Load byte at [SI] into AL, then SI++
    or al, al           ; Test AL (sets Zero Flag if AL == 0)
    jz hang             ; If null terminator reached, stop printing
    mov ah, 0x0E        ; BIOS teletype output function
    int 0x10            ; Call BIOS video service to print AL
    jmp print           ; Repeat for the next character

hang:
    jmp $               ; Infinite loop (jump to itself)

msg db "Welcome to HarisOS!", 0    ; Null-terminated message string

times 510-($-$$) db 0   ; Pad the rest of the sector with zeros up to 510 bytes
dw 0xAA55               ; Boot signature (required for BIOS to treat it as bootable)