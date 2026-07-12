; ************************* INT 9h handler - keyboard (PC BIOS standard)

int9:
    push ax
    push bx
    push cx
    push ds

    ; --- READ HARDWARE & INT 15h KEYBOARD INTERCEPT ---
    in al, 0x60          ; Read hardware scancode from port 60h
    mov bl, al           ; Save raw scancode to BL just in case

    mov ah, 0x4F         ; Keyboard Intercept function
    stc                  ; Standard convention: set carry flag before calling
    int 0x15             ; Call INT 15h to let TSRs intercept/modify the key

    jc .intercept_done   ; If CF=CY (Carry Set), TSR modified AL. Keep AL.
    mov al, bl           ; If CF=NC (Carry Clear), no modification. Restore original.
.intercept_done:

    mov bx, BDATASEG
    mov ds, bx

    mov ah, al           ; Save raw scancode (original or intercepted) to AH
    and al, 0x7F         ; Strip the break bit to get the base code

    cmp al, 0x2A         ; L-Shift
    je .mod_lshift
    cmp al, 0x36         ; R-Shift
    je .mod_rshift
    cmp al, 0x1D         ; Ctrl
    je .mod_ctrl
    cmp al, 0x38         ; Alt
    je .mod_alt
    cmp al, 0x3A         ; Caps Lock
    je .mod_capslock
    jmp .decode_normal

.mod_lshift:
    mov bl, 0x02         ; L-Shift flag (bit 1)
    jmp .update_mod
.mod_rshift:
    mov bl, 0x01         ; R-Shift flag (bit 0)
    jmp .update_mod
.mod_ctrl:
    mov bl, 0x04         ; Ctrl flag (bit 2)
    jmp .update_mod
.mod_alt:
    mov bl, 0x08         ; Alt flag (bit 3)

.update_mod:
    test ah, 0x80        ; Is it a Key Release (Break)?
    jnz .mod_released
    or [0x17], bl        ; Key Pressed: Set the flag in BDA
    jmp .int9_exit
.mod_released:
    not bl
    and [0x17], bl       ; Key Released: Clear the flag in BDA
    jmp .int9_exit

.mod_capslock:
    test ah, 0x80        ; Is it a Key Release?
    jnz .int9_exit       ; Ignore releases entirely!
    xor byte [0x17], 0x40 ; Toggle the Caps Lock state (bit 6) in the BDA
    jmp .int9_exit

.decode_normal:
    test ah, 0x80        ; Ignore normal key releases
    jnz .int9_exit

    ; --- SPECIAL KEY CHECK (F-Keys, Arrows, Nav) ---
    cmp al, 0x3B
    jae .special_key

    ; --- TRANSLATE SCANCODE TO ASCII ---
    mov bx, 0
    mov bl, al           ; BX = Scancode index

    ; Check Alt first
    test byte [0x17], 0x08
    jnz .use_alt

    ; Check Ctrl next
    test byte [0x17], 0x04
    jnz .use_ctrl

    ; Check Shift (Either L-Shift or R-Shift)
    test byte [0x17], 0x03
    jnz .use_shifted

    ; Unshifted Lookup
    mov al, [cs:scan_to_ascii_unshifted + bx]
    jmp .apply_caps

.use_shifted:
    mov al, [cs:scan_to_ascii_shifted + bx]

.apply_caps:
    test byte [0x17], 0x40   ; Is Caps Lock ON?
    jz .insert_buffer        ; If not, skip to buffer insertion

    ; Caps Lock is ON. Check if the character is a letter.
    cmp al, 'A'
    jb .insert_buffer        ; Below 'A'? Not a letter.
    cmp al, 'z'
    ja .insert_buffer        ; Above 'z'? Not a letter.
    cmp al, 'Z'
    jbe .flip_case           ; A-Z? Flip it!
    cmp al, 'a'
    jae .flip_case           ; a-z? Flip it!
    jmp .insert_buffer       ; Anything in between (like [, \, ]) skip it.

.flip_case:
    xor al, 0x20             ; ASCII Magic: Flips uppercase to lowercase and vice versa!
    jmp .insert_buffer

.use_alt:
    mov al, 0x00         ; Alt combos produce AL=0x00, AH=Scancode
    jmp .insert_buffer

.use_ctrl:
    mov al, [cs:scan_to_ascii_unshifted + bx]
    cmp al, 'a'
    jb .insert_buffer
    cmp al, 'z'
    ja .insert_buffer
    and al, 0x1F         ; Converts 'a' -> 0x01 (Ctrl+A), 'o' -> 0x0F (Ctrl+O)
    jmp .insert_buffer

.special_key:
    ; Check if the scancode is in the F1-F10 range (0x3B to 0x44)
    cmp al, 0x3B
    jb .not_f_key
    cmp al, 0x44
    ja .not_f_key

    ; --- WE HAVE AN F-KEY (F1-F10) ---
    test byte [0x17], 0x08        ; Alt first (IBM order)
    jz .check_ctrl_fkey
    add ah, 0x2D                  ; Alt+F1..F10
    jmp .finish_fkey

.check_ctrl_fkey:
    test byte [0x17], 0x04        ; Ctrl next
    jz .check_shift_fkey
    add ah, 0x23                  ; Ctrl+F1..F10
    jmp .finish_fkey

.check_shift_fkey:
    test byte [0x17], 0x02        ; Left-Shift is the only Shift
    jz .finish_fkey               ; Not held -> plain F-key, AH unchanged
    add ah, 0x19                  ; Shift+F1..F10

.finish_fkey:
    mov al, 0x00             ; Set AL to 0x00 to signify an extended keycode
    jmp .insert_buffer

.not_f_key:
    ; Handle Hardware exceptions for Numpad '-' and '+'
    cmp al, 0x4A
    je .numpad_minus
    cmp al, 0x4E
    je .numpad_plus

    ; Are we holding the Ctrl key? (Bit 2 = 0x04)
    test byte [0x17], 0x04
    jz .standard_nav_key     ; If not, skip to standard navigation keys

    ; --- CTRL + NAVIGATION KEYS ---
    cmp al, 0x4B             ; Left Arrow
    je .ctrl_left
    cmp al, 0x4D             ; Right Arrow
    je .ctrl_right
    cmp al, 0x47             ; Home
    je .ctrl_home
    cmp al, 0x4F             ; End
    je .ctrl_end
    cmp al, 0x49             ; PgUp
    je .ctrl_pgup
    cmp al, 0x51             ; PgDn
    je .ctrl_pgdn
    jmp .standard_nav_key    ; If it's a different key, ignore Ctrl

.ctrl_left:
    mov ah, 0x73             ; Extended code for Ctrl + Left
    jmp .standard_nav_key
.ctrl_right:
    mov ah, 0x74             ; Extended code for Ctrl + Right
    jmp .standard_nav_key
.ctrl_home:
    mov ah, 0x77             ; Extended code for Ctrl + Home
    jmp .standard_nav_key
.ctrl_end:
    mov ah, 0x75             ; Extended code for Ctrl + End
    jmp .standard_nav_key
.ctrl_pgup:
    mov ah, 0x84             ; Extended code for Ctrl + PgUp
    jmp .standard_nav_key
.ctrl_pgdn:
    mov ah, 0x76             ; Extended code for Ctrl + PgDn

.standard_nav_key:
    ; All F-keys, Arrows, and Nav keys get AL = 0x00
    ; AH will contain either the raw scancode, or our overridden Ctrl-code
    mov al, 0x00
    jmp .insert_buffer

.numpad_minus:
    mov al, '-'
    jmp .insert_buffer

.numpad_plus:
    mov al, '+'

.insert_buffer:
    ; AL now holds ASCII (or 0x00/Control code), AH holds original Scancode
    mov cx, ax

    mov bx, [0x1C]       ; Get Tail pointer (kbbuf_tail)
    mov ax, bx
    add ax, 2            ; Advance Tail by 2 bytes
    cmp ax, 0x003E       ; Have we reached the end of the buffer?
    jne .no_wrap
    mov ax, 0x001E       ; Wrap around to start
.no_wrap:
    cmp ax, [0x1A]       ; Compare new Tail with Head (kbbuf_head)
    je .int9_exit        ; Buffer Full! Drop the keystroke

    mov [bx], cx         ; Store [ASCII | Scancode] into memory
    mov [0x1C], ax       ; Save the new Tail pointer

.int9_exit:
    ; Hardware Acknowledge (Send EOI to PIC)
    mov al, 0x20
    out 0x20, al

    pop ds
    pop cx
    pop bx
    pop ax
    iret

; --- XT Scancode to ASCII Translation Tables ---
scan_to_ascii_unshifted:
    db 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9
    db 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's'
    db 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', 39, '`', 0, 92, 'z', 'x', 'c', 'v'
    db 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0
    db 0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1'
    db '2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    times 32 db 0

scan_to_ascii_shifted:
    db 0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9
    db 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S'
    db 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', 34, '~', 0, '|', 'Z', 'X', 'C', 'V'
    db 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0
    db 0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1'
    db '2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    times 32 db 0
