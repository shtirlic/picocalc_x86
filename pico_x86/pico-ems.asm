; SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
; SPDX-License-Identifier: GPL-3.0-or-later

cpu 8086
org 0

    ; --- DOS Device Header (Starts at CS:0000) ---
    dw 0FFFFh, 0FFFFh   ; Link to next device
    dw 8000h            ; Attribute: Char Device
    dw strategy         ; Pointer to Strategy routine
    dw interrupt        ; Pointer to Interrupt routine
    db 'EMMXXXX0'       ; EXACTLY AT OFFSET 0x000A!

strategy:
    mov word [cs:pkt_off], bx
    mov word [cs:pkt_seg], es
    retf

interrupt:
    push ax
    push bx
    push ds
    push es

    lds bx, [cs:pkt_off]

    cmp byte [bx+2], 0  ;  INIT ?
    jne .done

    ; --- INIT ROUTINE: Hook INT 67h ---
    xor ax, ax
    mov es, ax
    cli
    mov word [es:019Ch], ems_entry
    mov word [es:019Eh], cs
    sti

    ; memory footprint
    mov word [bx+14], driver_end
    mov word [bx+16], cs

.done:
    mov word [bx+3], 0100h  ; Done

    pop es
    pop ds
    pop bx
    pop ax
    retf

pkt_off dw 0
pkt_seg dw 0

ems_entry:
    ; our int 67h
    ; emulator macro and return
    db 0x0F, 0x04
    iret

driver_end:
