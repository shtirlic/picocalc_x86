; font_test.asm
; Prints all 256 characters of the IBM PC charset directly to the screen

org 100h

start:
    mov ax, 0003h
    int 10h

    mov ax, 0B800h
    mov es, ax
    xor di, di

    mov cx, 256
    mov al, 0
    mov ah, 07h

print_loop:
    stosw
    inc al
    loop print_loop

wait_for_key:
    xor ah, ah
    int 16h

exit:
    mov ax, 4C00h
    int 21h
