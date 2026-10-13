; ============================================================
; HarisOS : boot straight into 32-bit Protected Mode
; Flow: BIOS -> 16-bit boot sector -> A20 -> GDT -> CR0.PE
;       -> far jump -> 32-bit code prints directly to VGA memory
; No keypress is needed to get here. After the banner you get a
; "haris>" prompt: the keyboard works by polling the keyboard
; controller (port 0x60), because BIOS int 0x16 does not exist
; in protected mode.
; ============================================================

[org 0x7C00]                ; BIOS loads the boot sector at 0x7C00
[bits 16]                   ; CPU starts in 16-bit real mode

CODE_SEG equ gdt_code - gdt_start       ; selector 0x08
DATA_SEG equ gdt_data - gdt_start       ; selector 0x10
VGA      equ 0xB8000                    ; VGA text memory

start:
    ; --- Set up segments and stack ---
    cli                     ; Disable interrupts while changing segments
    xor ax, ax
    mov ds, ax              ; DS = 0
    mov es, ax              ; ES = 0
    mov ss, ax              ; SS = 0
    mov sp, 0x7C00          ; Stack grows downward below our code
    mov ax, 0x0003              ; BIOS: 80x25 text mode (also clears the screen)
    int 0x10

    in al, 0x92                 ; Enable A20 line (fast A20 gate)
    or al, 2
    out 0x92, al

    lgdt [gdt_descriptor]       ; Load the GDT

    mov eax, cr0                ; Set the PE bit
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode ; Far jump: loads CS, flushes the pipeline

; ------------------------------------------------------------
; GDT: flat model, code and data both cover 0 .. 4 GB
; ------------------------------------------------------------
gdt_start:
    dq 0                        ; Null descriptor (required)

gdt_code:
    dw 0xFFFF                   ; Limit 0-15
    dw 0x0000                   ; Base 0-15
    db 0x00                     ; Base 16-23
    db 10011010b                ; Present, ring 0, code, executable, readable
    db 11001111b                ; 4K granularity, 32-bit, limit 16-19 = 0xF
    db 0x00                     ; Base 24-31

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                ; Present, ring 0, data, writable
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; Size - 1
    dd gdt_start                ; Address

; ------------------------------------------------------------
; 32-bit protected mode
; Registers: EDI = VGA write pointer, AH = color attribute
;            EBX = start of the current screen row
;            EBP = where the typed text of this line begins
; ------------------------------------------------------------
[bits 32]
protected_mode:
    mov ax, DATA_SEG            ; Reload all data segments
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    call clear_screen32

    mov edi, VGA                ; Row 0: main message, white on blue
    mov esi, msg_prot
    mov ah, 0x1F
    call print_string32

    mov edi, VGA + 160 * 2      ; Row 2: color demo (EDI keeps advancing)
    mov esi, c_green
    mov ah, 0x0A
    call print_string32
    mov esi, c_red
    mov ah, 0x0C
    call print_string32
    mov esi, c_yellow
    mov ah, 0x0E
    call print_string32

    mov edi, VGA + 160 * 4      ; Row 4: first prompt

; ------------------------------------------------------------
; Keyboard input loop (32-bit, no BIOS)
; ------------------------------------------------------------
.prompt:
    mov ebx, edi                ; Remember the start of this row
    mov esi, msg_prompt
    mov ah, 0x0E                ; Yellow prompt
    call print_string32
    mov ebp, edi                ; Typing starts here (backspace stops here)

.key:
    call read_key32             ; AL = ASCII character
    cmp al, 13                  ; Enter?
    je .enter
    cmp al, 8                   ; Backspace?
    je .backspace
    mov ah, 0x0F                ; Bright white typed text
    call print_char32
    jmp .key

.backspace:
    cmp edi, ebp                ; Don't erase the prompt
    jbe .key
    sub edi, 2
    mov word [edi], 0x0720      ; Space, light gray on black
    jmp .key

.enter:
    add ebx, 160                ; Next row (80 columns * 2 bytes)
    mov edi, ebx
    cmp edi, VGA + 4000         ; Past the last row?
    jb .prompt
    call clear_screen32         ; Screen full: clear it, restart at the top
    jmp .prompt

; ------------------------------------------------------------
; clear_screen32: fill VGA memory with spaces, leave EDI = VGA
; ------------------------------------------------------------
clear_screen32:
    mov edi, VGA
    mov ecx, 80 * 25
    mov ax, 0x0720              ; Space, light gray on black
    rep stosw
    mov edi, VGA
    ret

; ------------------------------------------------------------
; print_char32: AL = character, AH = attribute, writes at EDI
; ------------------------------------------------------------
print_char32:
    mov [edi], ax               ; Low byte = character, high byte = color
    add edi, 2
    ret

; ------------------------------------------------------------
; print_string32: ESI = null-terminated string, AH = attribute
; ------------------------------------------------------------
print_string32:
    lodsb
    test al, al
    jz .done
    call print_char32
    jmp print_string32
.done:
    ret

; ------------------------------------------------------------
; read_key32: wait for a key press by polling the keyboard
; controller. Returns ASCII in AL. Keys without a character
; (Shift, Ctrl, arrows, ...) are ignored.
; ------------------------------------------------------------
read_key32:
    call update_cursor
.wait:
    in al, 0x64                 ; Status port: bit 0 = a byte is waiting
    test al, 1
    jz .wait
    in al, 0x60                 ; Read the scancode
    test al, 0x80               ; Bit 7 set = key RELEASE, ignore
    jnz .wait
    cmp al, 0x3A                ; Beyond our table? ignore
    jae .wait
    movzx eax, al
    mov al, [scancode_table + eax]
    test al, al                 ; 0 = no character for this key
    jz .wait
    ret

; ------------------------------------------------------------
; update_cursor: move the blinking hardware cursor to EDI
; ------------------------------------------------------------
update_cursor:
    mov eax, edi
    sub eax, VGA
    shr eax, 1                  ; Byte offset -> character position
    mov ecx, eax
    mov dx, 0x3D4               ; VGA index port
    mov al, 0x0F                ; Cursor position, low byte
    out dx, al
    inc dx                      ; 0x3D5 = data port
    mov al, cl
    out dx, al
    dec dx
    mov al, 0x0E                ; Cursor position, high byte
    out dx, al
    inc dx
    mov al, ch
    out dx, al
    ret

; ------------------------------------------------------------
; Data
; ------------------------------------------------------------
msg_prot   db " Now in 32-bit PROTECTED MODE! Welcome to HarisOS. ", 0
msg_prompt db "haris> ", 0
c_green    db "GREEN ", 0
c_red      db "RED ", 0
c_yellow   db "YELLOW", 0

; Scancode set 1, US layout, no Shift support. Index = scancode.
scancode_table:
    db 0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 0       ; 0x00-0x0F
    db 'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0            ; 0x10-0x1D
    db 'a','s','d','f','g','h','j','k','l',';',"'",'`', 0, '\'           ; 0x1E-0x2B
    db 'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' '           ; 0x2C-0x39

times 510-($-$$) db 0       ; Pad to 510 bytes
dw 0xAA55                   ; Boot signature
