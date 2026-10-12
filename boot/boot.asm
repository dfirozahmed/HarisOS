; ============================================================
; HarisOS - Stage 2 (single boot sector, no second loader)
; Features:
;   1. Print functions  (print_char, print_string, print_color)
;   2. Clear screen     (BIOS video mode set)
;   3. Colors           (text attributes)
;   4. Keyboard input   (BIOS int 0x16, echo + backspace + enter)
; Must fit in 512 bytes. NASM will error if it grows too big.
; ============================================================

[org 0x7C00]                ; BIOS loads the boot sector at 0x7C00
[bits 16]                   ; CPU starts in 16-bit real mode

start:
    ; --- Set up segments and stack ---
    cli                     ; Disable interrupts while changing segments
    xor ax, ax
    mov ds, ax              ; DS = 0
    mov es, ax              ; ES = 0
    mov ss, ax              ; SS = 0
    mov sp, 0x7C00          ; Stack grows downward below our code
    sti                     ; Re-enable interrupts (BIOS keyboard needs them)
    cld                     ; Make lodsb count upward

    ; --- 2. Clear screen: set 80x25 text mode ---
    mov ax, 0x0003          ; AH=0x00 set video mode, AL=0x03 80x25 text
    int 0x10

    ; --- 3. Colors: banner (white on blue) ---
    mov si, title
    mov bl, 0x1F            ; Attribute: high nibble = background, low = text
    call print_color
    mov si, newline
    call print_string

    ; --- 3. Colors: color demo line ---
    mov si, c_green
    mov bl, 0x0A            ; Light green on black
    call print_color
    mov si, c_red
    mov bl, 0x0C            ; Light red on black
    call print_color
    mov si, c_yellow
    mov bl, 0x0E            ; Yellow on black
    call print_color
    mov si, newline
    call print_string

    ; --- 1. Normal (plain) print ---
    mov si, info
    call print_string

; ------------------------------------------------------------
; 4. Keyboard input loop
; ------------------------------------------------------------
input_loop:
    mov si, prompt
    call print_string
    xor di, di              ; DI = number of characters typed on this line

.read:
    call read_key           ; AL = ASCII code of pressed key
    cmp al, 0x0D            ; Enter?
    je .enter
    cmp al, 0x08            ; Backspace?
    je .backspace
    cmp al, 0x20            ; Ignore control characters
    jb .read

    call print_char         ; Echo the typed character
    inc di
    jmp .read

.backspace:
    test di, di             ; Nothing typed? Don't erase the prompt
    jz .read
    dec di
    mov al, 0x08            ; Cursor back
    call print_char
    mov al, ' '             ; Erase the character
    call print_char
    mov al, 0x08            ; Cursor back again
    call print_char
    jmp .read

.enter:
    mov si, newline
    call print_string
    jmp input_loop

; ------------------------------------------------------------
; print_char: print the character in AL (teletype output)
; ------------------------------------------------------------
print_char:
    mov ah, 0x0E
    mov bh, 0x00
    int 0x10
    ret

; ------------------------------------------------------------
; print_string: print null-terminated string at SI
; ------------------------------------------------------------
print_string:
    lodsb
    or al, al
    jz .done
    call print_char
    jmp print_string
.done:
    ret

; ------------------------------------------------------------
; print_color: print null-terminated string at SI using the
; color attribute in BL (no newlines inside the string)
; ------------------------------------------------------------
print_color:
    lodsb
    or al, al
    jz .done
    mov ah, 0x09            ; Write character + attribute at cursor
    mov bh, 0x00
    mov cx, 1               ; Write it once
    int 0x10
    mov ah, 0x03            ; Read cursor position (DH=row, DL=col)
    mov bh, 0x00
    int 0x10
    inc dl                  ; Move one column right
    mov ah, 0x02            ; Set cursor position
    int 0x10
    jmp print_color
.done:
    ret

; ------------------------------------------------------------
; read_key: wait for a key press, return ASCII in AL
; ------------------------------------------------------------
read_key:
    xor ah, ah              ; AH=0x00: wait for key
    int 0x16
    ret

; ------------------------------------------------------------
; Data
; ------------------------------------------------------------
title    db " Welcome to HarisOS ", 0
c_green  db "GREEN ", 0
c_red    db "RED ", 0
c_yellow db "YELLOW", 0
info     db 13, 10, "Type something. Enter = new line, Backspace = erase.", 13, 10, 13, 10, 0
prompt   db "haris> ", 0
newline  db 13, 10, 0

times 510-($-$$) db 0       ; Pad to 510 bytes
dw 0xAA55                   ; Boot signature
