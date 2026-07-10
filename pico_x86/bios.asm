;
; Portions of this file are derived from 8086tiny.
; Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
; Licensed under the MIT License.
; See LICENSE.txt.
;
; Modifications Copyright (c) 2026 Serg Podtynnyi
; Licensed under GPLv3 for the combined work.
;
	cpu	8086

; Here we define macros for some custom instructions that help the emulator talk with the outside
; world. They are described in detail in the hint.html file, which forms part of the emulator
; distribution.

%macro	extended_get_rtc 0
	db	0x0f, 0x01
%endmacro

%macro	extended_read_disk 0
	db	0x0f, 0x02
%endmacro

%macro	extended_write_disk 0
	db	0x0f, 0x03
%endmacro

%macro	extended_ems_call 0
	db	0x0f, 0x04
%endmacro

%macro	extended_set_rtc 0
	db	0x0f, 0x05
%endmacro

%macro	extended_set_systime 0
	db	0x0f, 0x06
%endmacro

org	100h				; BIOS loads at offset 0x0100

main:

	jmp	bios_entry

; Here go pointers to the different data tables used for instruction decoding

	dw	rm_mode12_reg1	; Table 0: R/M mode 1/2 "register 1" lookup
	dw	rm_mode012_reg2	; Table 1: R/M mode 1/2 "register 2" lookup
	dw	rm_mode12_disp	; Table 2: R/M mode 1/2 "DISP multiplier" lookup
	dw	rm_mode12_dfseg	; Table 3: R/M mode 1/2 "default segment" lookup
	dw	rm_mode0_reg1	; Table 4: R/M mode 0 "register 1" lookup
	dw	rm_mode012_reg2 ; Table 5: R/M mode 0 "register 2" lookup
	dw	rm_mode0_disp	; Table 6: R/M mode 0 "DISP multiplier" lookup
	dw	rm_mode0_dfseg	; Table 7: R/M mode 0 "default segment" lookup
	dw	xlat_ids	; Table 8: Translation of raw opcode index ("Raw ID") to function number ("Xlat'd ID")
	dw	ex_data		; Table 9: Translation of Raw ID to Extra Data
	dw	std_flags	; Table 10: How each Raw ID sets the flags (bit 1 = sets SZP, bit 2 = sets AF/OF for arithmetic, bit 3 = sets OF/CF for logic)
	dw	parity		; Table 11: Parity flag loop-up table (256 entries)
	dw	base_size	; Table 12: Translation of Raw ID to base instruction size (bytes)
	dw	i_w_adder	; Table 13: Translation of Raw ID to i_w size adder yes/no
	dw	i_mod_adder	; Table 14: Translation of Raw ID to i_mod size adder yes/no
	dw	jxx_dec_a	; Table 15: Jxx decode table A
	dw	jxx_dec_b	; Table 16: Jxx decode table B
	dw	jxx_dec_c	; Table 17: Jxx decode table C
	dw	jxx_dec_d	; Table 18: Jxx decode table D
	dw	flags_mult	; Table 19: FLAGS multipliers

; These values (BIOS ID string, BIOS date and so forth) go at the very top of memory

biosstr db	'PicoCalc x86 BIOS Revision 0.5', 0, 0
mem_top	db	0xea, 0, 0x01, 0, 0xf0, '07/10/26', 0, 0, 0xfa
biosstr2 db	'Copyright (C) 2026, Serg Podtynnyi', 0, 0

bios_entry:

	; Set up initial stack to F000:F000

	mov	sp, 0xf000
	mov	ss, sp

	push	cs
	pop	es

	push	ax

	; The emulator requires a few control registers in memory to always be zero for correct
	; instruction decoding (in particular, register look-up operations). These are the
	; emulator's zero segment (ZS) and always-zero flag (XF). Because the emulated memory
	; space is uninitialised, we need to be sure these values are zero before doing anything
	; else. The instructions we need to use to set them must not rely on look-up operations.
	; So e.g. MOV to memory is out but string operations are fine.

	cld

	xor	ax, ax
	mov	di, 24
	stosw			; Set ZS = 0
	mov	di, 49
	stosb			; Set XF = 0

	; Now we can do whatever we want! DL starts off being the boot disk.

	mov	[cs:boot_device], dl

	push	dx

	pop	ax

	; Check cold boot/warm boot. We initialise disk parameters on cold boot only

	cmp	byte [cs:boot_state], 0	; Cold boot?
	jne	boot

	mov	byte [cs:boot_state], 1	; Set flag so next boot will be warm boot

	; First, set up the disk subsystem. Only do this on the very first startup, when
	; the emulator sets up the CX/AX registers with disk information.

	; Compute the cylinder/head/sector count for the HD disk image, if present.
	; Total number of sectors is in CX:AX, or 0 if there is no HD image. First,
	; we put it in DX:CX.

	mov	dx, cx
	mov	cx, ax

	mov	[cs:hd_secs_hi], dx
	mov	[cs:hd_secs_lo], cx

	cmp	cx, 0
	je	maybe_no_hd

	mov	word [cs:num_disks], 2
	jmp	calc_hd

maybe_no_hd:

	cmp	dx, 0
	je	no_hd

	mov	word [cs:num_disks], 2
	jmp	calc_hd

no_hd:

	mov	word [cs:num_disks], 1

calc_hd:

	mov	ax, cx
	mov	word [cs:hd_max_track], 1
	mov	word [cs:hd_max_head], 1

	cmp	dx, 0		; More than 63 total sectors? If so, we have more than 1 track.
	ja	sect_overflow
	cmp	ax, 63
	ja	sect_overflow

	mov	[cs:hd_max_sector], ax
	jmp	calc_heads

sect_overflow:

	mov	cx, 63		; Calculate number of tracks
	div	cx
	mov	[cs:hd_max_track], ax
	mov	word [cs:hd_max_sector], 63

calc_heads:

	mov	dx, 0		; More than 1024 tracks? If so, we have more than 1 head.
	mov	ax, [cs:hd_max_track]
	cmp	ax, 1024
	ja	track_overflow

	jmp	calc_end

track_overflow:

	mov	cx, 1024
	div	cx
	mov	[cs:hd_max_head], ax
	mov	word [cs:hd_max_track], 1024

calc_end:

	; Convert number of tracks into maximum track (0-based) and then store in INT 41
	; HD parameter table

	mov	ax, [cs:hd_max_head]
	mov	[cs:int41_max_heads], al
	mov	ax, [cs:hd_max_track]
	mov	[cs:int41_max_cyls], ax
	mov	ax, [cs:hd_max_sector]
	mov	[cs:int41_max_sect], al

	dec	word [cs:hd_max_track]
	dec	word [cs:hd_max_head]

; Main BIOS entry point. Zero the flags, and set up registers.

boot:	mov	ax, 0
	push	ax
	popf


	push	cs
	push	cs
	pop	ds
	pop	ss
	mov	sp, 0xf000

   ; Set up the IVT. First we zero out the table

	cld

	xor	ax, ax
	mov	es, ax
	xor	di, di
	mov	cx, 512
	rep	stosw

    ; Then we load in the pointers to our interrupt handlers

	mov	di, 0
	mov	si, int_table
	mov	cx, [itbl_size]
	rep	movsb

    ; Set pointer to INT 41 table for hard disk

	mov	cx, int41
	mov	word [es:4*0x41], cx
	mov	cx, 0xf000
	mov	word [es:4*0x41 + 2], cx

    ;  Set pointer to INT 67h for EMS ---

	mov	cx, int67
	mov	word [es:4*0x67], cx
	mov	cx, 0xf000
	mov	word [es:4*0x67 + 2], cx

    ; Set up last 16 bytes of memory, including boot jump, BIOS date, machine ID byte

	mov	ax, 0xffff
	mov	es, ax
	mov	di, 0
	mov	si, mem_top
	mov	cx, 16
	rep	movsb

    ; Set up the BIOS data area

	mov	ax, BDATASEG
	mov	es, ax
	mov	di, 0
	mov	si, bios_data
	mov	cx, 0x100
	rep	movsb


   ; Reset and Intital setup needed for startup and soft reboot
   ; Set Mode 3 and needed values for CGA registers
	mov	dx, 0x3d8
	mov	al, 0x29	; CGA Mode Control
	out	dx, al
    mov	[es:0x65], al

	mov	dx, 0x3d9  ; CGA Color register
	mov	al, 0x30
	out	dx, al
    mov	[es:0x66], al

	mov	dx, 0x3d4
	mov	al, 1		; CRTC "horizontal displayed"
	out	dx, al
	mov	dx, 0x3d5
	mov	al, 80		; 80 columns
	out	dx, al

	mov	dx, 0x3d4
	mov	al, 6		; CRTC "vertical displayed"
	out	dx, al
	mov	dx, 0x3d5
	mov	al, 25		; 25 rows
	out	dx, al

	mov	dx, 0x3d4
	mov	al, 0x0C  ; start address high
	out	dx, al
	mov	dx, 0x3d5
	mov	al, 0
	out	dx, al

	mov	dx, 0x3d4
	mov	al, 0x0D ; start address low
	out	dx, al
	mov	dx, 0x3d5
	mov	al, 0


    ; Clear video memory

	mov	ax, 0xb800
	mov	es, ax
	mov	di, 0
	mov	cx, 80*25
	mov	ax, 0x0700
	rep	stosw

    ; Set up some I/O ports, between 0 and FFF. Most of them we set to 0xFF, to indicate no device present

	mov	dx, 0x61
	mov	al, 0
	out	dx, al		; Make sure the speaker is off

	mov	dx, 0x60
	out	dx, al		; No scancode

	mov	dx, 0x64
	out	dx, al		; No key waiting

	mov	dx, 0
	mov	al, 0xFF

next_out:

	inc	dx

	cmp	dx, 0x40	; We deal with the PIT channel 0 later
	je	next_out
	cmp	dx, 0x42	; We deal with the PIT channel 2 later
	je	next_out
	cmp	dx, 0x60	; Keyboard scancode
	je	next_out
	cmp	dx, 0x61	; Sound output
	je	next_out
	cmp	dx, 0x64	; Keyboard status
	je	next_out

	cmp	dx, 0x3D4	; CRTC Index Register
	je	next_out
	cmp	dx, 0x3D5	; CRTC Data Register
	je	next_out
    cmp	dx, 0x3D9	; CGA Color Select
	je	next_out
	cmp	dx, 0x3D8	; CGA Mode Control
	je	next_out

	out	dx, al

	cmp	dx, 0xFFF
	jl	next_out

	mov	al, 0

	mov	dx, 0x3DA	; CGA refresh port
	out	dx, al

	mov	dx, 0x3BC	; LPT1
	out	dx, al

	mov	dx, 0x62	; PPI - needed for memory parity checks
	out	dx, al

    ; Get initial RTC value
	push	cs
	pop	es
	mov	bx, timetable
	extended_get_rtc
	mov	ax, [es:tm_msec]
	mov	[cs:last_int8_msec], ax

    ; Print BIOS string
	mov	si, biosstr     ; Point SI to the string at the top of the file
	call	print_str
    call    print_new_line

    ; MEMORY POST TEST
	mov	si, memteststr
	call	print_str

	mov	bx, 0		; BX = Current KB being tested

	push	ds
	mov	ax, BDATASEG
	mov	ds, ax
	mov	cx, [memsize-bios_data]
	pop	ds

mem_test_loop:

	; Calculate the segment for this KB (BX * 64)
	mov	ax, bx
	push	cx
	mov	cl, 6
	shl	ax, cl
	pop	cx
	mov	es, ax
	mov	di, 0

	; SKIP the first KB (0x0000)
	cmp	bx, 0
	je	.mem_test_ok

	mov	dx, [es:di]	; Save original RAM content

	mov	word [es:di], 0xAA55
	cmp	word [es:di], 0xAA55
	jne	.mem_test_done	; If mismatch, RAM failed!

	mov	word [es:di], 0x55AA
	cmp	word [es:di], 0x55AA
	jne	.mem_test_done

	mov	[es:di], dx	; Restore original RAM

	; Update screen with counting animation
    .mem_test_ok:
	mov	ax, bx
	and	ax, 1
	jnz	.skip_print
	call print_kb_status

    .skip_print:
	inc	bx
	cmp	bx, cx
	jb	mem_test_loop

	; print the total
    .mem_test_done:
	call	print_kb_status
    call print_new_line

    ; POST success one short beep
    call post_beep


boot_disk:
    ; Read boot sector from FDD, and load it into 0:7C00

	mov	ax, 0
	mov	es, ax

	mov	ax, 0x0201
	mov	dh, 0
	mov	dl, [cs:boot_device]
	mov	cx, 1
	mov	bx, 0x7c00
	int	13h

    ; Jump to boot sector

	jmp	0:0x7c00


; ************************* INT 9h handler - keyboard (PC BIOS standard)

int9:
    push ax
    push bx
    push cx
    push ds

    mov ax, BDATASEG
    mov ds, ax           ; DS = BDA Segment

    in al, 0x60          ; Read hardware scancode from port 60h
    mov ah, al           ; Save raw scancode to AH
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

    ; 2. --- SPECIAL KEY CHECK (F-Keys, Arrows, Nav) ---
    cmp al, 0x3B
    jae .special_key

    ; 3. --- TRANSLATE SCANCODE TO ASCII ---
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
    ; Hardware exceptions for Numpad '-' and '+'
    cmp al, 0x4A
    je .numpad_minus
    cmp al, 0x4E
    je .numpad_plus

    ; All other F-keys, Arrows, and Nav keys get AL = 0x00
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

    ; 4. --- MANAGE THE BIOS CIRCULAR RING BUFFER ---
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


; ************************* INT Ah handler - timer (Hardware Paced)

inta:

	push	ax
	push	es

	; Point ES to the BIOS Data Area (Segment 0040h)
	mov	ax, BDATASEG
	mov	es, ax

	; Increment the 32-bit daily timer tick counter at 0040:006C by exactly 1
	add	word [es:0x6C], 1
	adc	word [es:0x6E], 0

	; Call the standard PC INT 8 handler (which fires INT 1C for TSRs)
	int	8

	pop	es
	pop	ax
	iret


; ************************* INT 8h handler - timer

int8:

	int	0x1c
	iret

; ************************* INT 10h handler - video services
%include "bios_video.asm"

; ************************* INT 11h - get equipment list

int11:
	mov	ax, [cs:equip]
	iret

; ************************* INT 12h - return memory size

int12:
	push	ds
	mov	ax, BDATASEG
	mov	ds, ax
	mov	ax, [ds:memsize-bios_data]
	pop	ds
	iret

; ************************* INT 13h handler - disk services

int13:
	cmp	ah, 0x00 ; Reset disk
	je	int13_reset_disk
	cmp	ah, 0x01 ; Get last status
	je	int13_last_status

	cmp	dl, 0x80 ; Hard disk being queried?
	jne	i13_diskok

	; Now, need to check an HD is installed
	cmp	word [cs:num_disks], 2
	jge	i13_diskok

	; No HD, so return an error
	mov	ah, 15 ; Report no such drive
	jmp	reach_stack_stc

  i13_diskok:

	cmp	ah, 0x02 ; Read disk
	je	int13_read_disk
	cmp	ah, 0x03 ; Write disk
	je	int13_write_disk
	cmp	ah, 0x04 ; Verify disk
	je	int13_verify
	cmp	ah, 0x05 ; Format track - does nothing here
	je	int13_format
	cmp	ah, 0x08 ; Get drive parameters (hard disk)
	je	int13_getparams
	cmp	ah, 0x0c ; Seek (hard disk)
	je	int13_seek
	cmp	ah, 0x10 ; Check if drive ready (hard disk)
	je	int13_hdready
	cmp	ah, 0x15 ; Get disk type
	je	int13_getdisktype
	cmp	ah, 0x16 ; Detect disk change
	je	int13_diskchange

	mov	ah, 1 ; Invalid function
	jmp	reach_stack_stc

	iret

  int13_reset_disk:

	jmp	reach_stack_clc

  int13_last_status:

	mov	ah, [cs:disk_laststatus]
	je	ls_no_error

	stc
	iret

    ls_no_error:

	clc
	iret

  int13_read_disk:

	push	dx

	cmp	dl, 0 ; Floppy 0
	je	i_flop_rd
	cmp	dl, 0x80 ; HD
	je	i_hd_rd

	pop	dx
	mov	ah, 1
	jmp	reach_stack_stc

    i_flop_rd:

	push	si
	push	bp

	cmp	cl, [cs:int1e_spt]
	ja	rd_error

	pop	bp
	pop	si

	mov	dl, 1		; Floppy disk file handle is stored at j[1] in emulator
	jmp	i_rd

    i_hd_rd:

	mov	dl, 0		; Hard disk file handle is stored at j[0] in emulator

    i_rd:

	push	si
	push	bp

	; Convert head/cylinder/sector number to byte offset in disk image

	call	chs_to_abs

	; Now, SI:BP contains the absolute sector offset of the block. We then multiply by 512 to get the offset into the disk image

	mov	ah, 0
	cpu	186
	shl	ax, 9
	extended_read_disk
	shr	ax, 9
	cpu	8086
	mov	ah, 0x02	; Put read code back

	cmp	al, 0
	je	rd_error

	; Read was successful. Now, check if we have read the boot sector. If so, we want to update
	; our internal table of sectors/track to match the disk format

	cmp	dx, 1		; FDD?
	jne	rd_noerror
	cmp	cx, 1		; First sector?
	jne	rd_noerror

	push	ax

	mov	al, [es:bx+24]	; Number of SPT in floppy disk BPB

	; cmp	al, 0		; If disk is unformatted, do not update the table
	; jne	rd_update_spt
	cmp	al, 9		; 9 SPT, i.e. 720K disk, so update the table
	je	rd_update_spt
	cmp	al, 18
	je	rd_update_spt	; 18 SPT, i.e. 1.44MB disk, so update the table

	pop	ax

	jmp	rd_noerror

    rd_update_spt:

	mov	[cs:int1e_spt], al
	pop	ax

    rd_noerror:

	clc
	mov	ah, 0 ; No error
	jmp	rd_finish

    rd_error:

	stc
	mov	ah, 4 ; Sector not found

    rd_finish:

	pop	bp
	pop	si
	pop	dx

	mov	[cs:disk_laststatus], ah
	jmp	reach_stack_carry

  int13_write_disk:

	push	dx

	cmp	dl, 0 ; Floppy 0
	je	i_flop_wr
	cmp	dl, 0x80 ; HD
	je	i_hd_wr

	pop	dx
	mov	ah, 1
	jmp	reach_stack_stc

    i_flop_wr:

	mov	dl, 1		; Floppy disk file handle is stored at j[1] in emulator
	jmp	i_wr

    i_hd_wr:

	mov	dl, 0		; Hard disk file handle is stored at j[0] in emulator

    i_wr:

	push	si
	push	bp
	push	cx
	push	di

	; Convert head/cylinder/sector number to byte offset in disk image

	call	chs_to_abs

	; Signal an error if we are trying to write beyond the end of the disk

	cmp	dl, 0 ; Hard disk?
	jne	wr_fine ; No - no need for disk sector valid check - NOTE: original submission was JNAE which caused write problems on floppy disk

	; First, we add the number of sectors we are trying to write from the absolute
	; sector number returned by chs_to_abs. We need to have at least this many
	; sectors on the disk, otherwise return a sector not found error.

	mov	cx, bp
	mov	di, si

	mov	ah, 0
	add	cx, ax
	adc	di, 0

	cmp	di, [cs:hd_secs_hi]
	ja	wr_error
	jb	wr_fine
	cmp	cx, [cs:hd_secs_lo]
	ja	wr_error

wr_fine:

	mov	ah, 0
	cpu	186
	shl	ax, 9
	extended_write_disk
	shr	ax, 9
	cpu	8086
	mov	ah, 0x03	; Put write code back

	cmp	al, 0
	je	wr_error

	clc
	mov	ah, 0 ; No error
	jmp	wr_finish

    wr_error:

	stc
	mov	ah, 4 ; Sector not found

    wr_finish:

	pop	di
	pop	cx
	pop	bp
	pop	si
	pop	dx

	mov	[cs:disk_laststatus], ah
	jmp	reach_stack_carry

  int13_verify:

	mov	ah, 0
	jmp	reach_stack_clc

  int13_getparams:

	cmp 	dl, 0
	je	i_gp_fl
	cmp	dl, 0x80
	je	i_gp_hd

	mov	ah, 0x01
	mov	[cs:disk_laststatus], ah
	jmp	reach_stack_stc

    i_gp_fl:

	push	cs
	pop	es
	mov	di, int1e	; ES:DI now points to floppy parameters table (INT 1E)

	mov	ax, 0
	mov	bx, 4
	mov	ch, 0x4f
	mov	cl, [cs:int1e_spt]
	mov	dx, 0x0101

	mov	byte [cs:disk_laststatus], 0
	jmp	reach_stack_clc

    i_gp_hd:

	mov	ax, 0
	mov	bx, 0
	mov	dl, 1
	mov	dh, [cs:hd_max_head]
	mov	cx, [cs:hd_max_track]
	ror	ch, 1
	ror	ch, 1
	add	ch, [cs:hd_max_sector]
	xchg	ch, cl

	mov	byte [cs:disk_laststatus], 0
	jmp	reach_stack_clc

  int13_seek:

	mov	ah, 0
	jmp	reach_stack_clc

  int13_hdready:

	cmp	byte [cs:num_disks], 2	; HD present?
	jne	int13_hdready_nohd
	cmp	dl, 0x80		; Checking first HD?
	jne	int13_hdready_nohd

	mov	ah, 0
	jmp	reach_stack_clc

    int13_hdready_nohd:

	jmp	reach_stack_stc

  int13_format:

	mov	ah, 0
	jmp	reach_stack_clc

  int13_getdisktype:

	cmp	dl, 0 ; Floppy
	je	gdt_flop
	cmp	dl, 0x80 ; HD
	je	gdt_hd

	mov	ah, 15 ; Report no such drive
	mov	[cs:disk_laststatus], ah
	jmp	reach_stack_stc

    gdt_flop:

	mov	ah, 1
	jmp	reach_stack_clc

    gdt_hd:

	mov	ah, 3
	mov	cx, [cs:hd_secs_hi]
	mov	dx, [cs:hd_secs_lo]
	jmp	reach_stack_clc

  int13_diskchange:

	mov	ah, 0 ; Disk not changed
	jmp	reach_stack_clc

; ************************* INT 14h - serial port functions

int14:
	cmp	ah, 0
	je	int14_init

	jmp	reach_stack_stc

  int14_init:

	mov	ax, 0
	jmp	reach_stack_stc

; ************************* INT 15h - get system configuration

int15:	; Here we do not support any of the functions, and just return
	; a function not supported code - like the original IBM PC/XT does.

	cmp	ah, 0xc0
	je	int15_sysconfig
	; cmp	ah, 0x41
	; je	int15_waitevent
	; cmp	ah, 0x4f
	; je	int15_intercept
	; cmp	ah, 0x88
	; je	int15_getextmem

; Otherwise, function not supported

	mov	ah, 0x86

	jmp	reach_stack_stc

 int15_sysconfig: ; Return address of system configuration table in ROM
;
	mov	bx, 0xf000
	mov	es, bx
	mov	bx, rom_config
	mov	ah, 0
;
	jmp	reach_stack_clc
;
;  int15_waitevent: ; Events not supported
;
;	mov	ah, 0x86
;
;	jmp	reach_stack_stc
;
;  int15_intercept: ; Keyboard intercept
;
;	jmp	reach_stack_stc
;
;  int15_getextmem: ; Extended memory not supported
;
;	mov	ah,0x86
;
;	jmp	reach_stack_stc

; ************************* INT 16h handler - keyboard

int16:
	cmp	ah, 0x00 ; Get keystroke (remove from buffer)
	je	kb_getkey
	cmp	ah, 0x01 ; Check for keystroke (do not remove from buffer)
	je	kb_checkkey
	cmp	ah, 0x02 ; Check shift flags
	je	kb_shiftflags
	cmp	ah, 0x12 ; Check shift flags
	je	kb_extshiftflags

	iret

  kb_getkey:

	push	es
	push	bx
	push	cx
	push	dx

	mov	bx, BDATASEG
	mov	es, bx

    kb_gkblock:

	cli

	mov	cx, [es:kbbuf_tail-bios_data]
	mov	bx, [es:kbbuf_head-bios_data]
	mov	dx, [es:bx]

	sti

	; Wait until there is a key in the buffer
	cmp	cx, bx
	je	kb_gkblock

	add	word [es:kbbuf_head-bios_data], 2
	call	kb_adjust_buf

	mov	ah, dh
	mov	al, dl

	pop	dx
	pop	cx
	pop	bx
	pop	es

	iret

  kb_checkkey:

	push	es
	push	bx
	push	cx
	push	dx

	mov	bx, BDATASEG
	mov	es, bx

	mov	cx, [es:kbbuf_tail-bios_data]
	mov	bx, [es:kbbuf_head-bios_data]
	mov	dx, [es:bx]

	sti

	; Check if there is a key in the buffer. ZF is set if there is none.
	cmp	cx, bx

	mov	ah, dh
	mov	al, dl

	pop	dx
	pop	cx
	pop	bx
	pop	es

	retf	2	; NEED TO FIX THIS!!

    kb_shiftflags:

	push	es
	push	bx

	mov	bx, BDATASEG
	mov	es, bx

	mov	al, [es:keyflags-bios_data]

	pop	bx
	pop	es

	iret

    kb_extshiftflags:

	push	es
	push	bx

	mov	bx, BDATASEG
	mov	es, bx

	mov	al, [es:keyflags-bios_data]
	mov	ah, al

	pop	bx
	pop	es

	iret

; ************************* INT 17h handler - printer

int17:
	cmp	ah, 0x01
	je	int17_initprint ; Initialise printer

	jmp	reach_stack_stc

  int17_initprint:

	mov	ah, 1 ; No printer
	jmp	reach_stack_stc

; ************************* INT 19h = reboot

int19:
	jmp	boot

; ************************* INT 1Ah - clock

int1a:
	cmp	ah, 0
	je	int1a_getsystime ; Get ticks since midnight (used for RTC time)
	cmp	ah, 1
	je	int1a_setsystime ; Set ticks since midnight (this is what DOS's TIME command actually calls)
	cmp	ah, 2
	je	int1a_gettime ; Get RTC time
	cmp	ah, 3
	je	int1a_settime ; Set RTC time (BCD - rarely called directly by DOS itself)
	cmp	ah, 4
	je	int1a_getdate ; Get RTC date
	cmp	ah, 5
	je	int1a_setdate ; Set RTC date (this is what DOS's DATE command calls)
	cmp	ah, 0x0f
	je	int1a_init    ; Initialise RTC

	iret

  int1a_getsystime:

	push	ax
	push	bx
	push	ds
	push	es

	push	cs
	push	cs
	pop	ds
	pop	es

	mov	bx, timetable

	extended_get_rtc

	mov	ax, 182  ; Clock ticks in 10 seconds
	mul	word [tm_msec]
	mov	bx, 10000
	div	bx ; AX now contains clock ticks in milliseconds counter
	mov	[tm_msec], ax

	mov	ax, 182  ; Clock ticks in 10 seconds
	mul	word [tm_sec]
	mov	bx, 10
	mov	dx, 0
	div	bx ; AX now contains clock ticks in seconds counter
	mov	[tm_sec], ax

	mov	ax, 1092 ; Clock ticks in a minute
	mul	word [tm_min] ; AX now contains clock ticks in minutes counter
	mov	[tm_min], ax

	mov	ax, 65520 ; Clock ticks in an hour
	mul	word [tm_hour] ; DX:AX now contains clock ticks in hours counter

	add	ax, [tm_msec] ; Add milliseconds in to AX
	adc	dx, 0 ; Carry into DX if necessary
	add	ax, [tm_sec] ; Add seconds in to AX
	adc	dx, 0 ; Carry into DX if necessary
	add	ax, [tm_min] ; Add minutes in to AX
	adc	dx, 0 ; Carry into DX if necessary

	push	dx
	push	ax
	pop	dx
	pop	cx

	pop	es
	pop	ds
	pop	bx
	pop	ax

	mov	al, 0
	iret

resync_tick_baseline:

	; After we jump the RTC to a new value (set time/date/systime), the
	; inta handler's msec-delta baseline (last_int8_msec) is left stale,
	; pointing at wherever the clock *used* to be. Left alone, the very
	; next inta tick computes a bogus (possibly huge, possibly negative)
	; delta from that discontinuity and mis-advances the BIOS Data Area
	; tick counter at 0040:006C that DOS's own clock is built on - which
	; is why a time you just set could appear to silently "not take" when
	; you check it again. Re-reading the RTC and resetting the baseline
	; here makes the next tick see a normal, small delta again.
	;
	; Assumes DS=ES=CS, which is true at every call site below.

	push	ax
	push	bx

	mov	bx, timetable
	extended_get_rtc
	mov	ax, [tm_msec]
	mov	[cs:last_int8_msec], ax

	pop	bx
	pop	ax
	ret

  int1a_setsystime:

	; Set ticks-since-midnight (INT 1Ah AH=01h). Input: CX:DX = tick count
	; since midnight (CX = high word, DX = low word). This is the call
	; DOS's kernel actually issues when TIME is used interactively - DOS
	; tracks time-of-day internally as a tick count, not as RTC BCD time,
	; so this (not AH=03h) is what needs to move the needle for TIME to
	; visibly take effect. We hand the raw ticks to the emulator, which
	; converts to h:m:s, merges with the existing date, and commits it.

	push	ax
	push	bx
	push	ds
	push	es

	push	cs
	push	cs
	pop	ds
	pop	es

	extended_set_systime	; Input: CX:DX (untouched above)

	call	resync_tick_baseline

	pop	es
	pop	ds
	pop	bx
	pop	ax

	jmp	reach_stack_clc

  int1a_gettime:

	; Return the system time in BCD format. DOS doesn't use this, but we need to return
	; something or the system thinks there is no RTC.

	push	ds
	push	es
	push	ax
	push	bx

	push	cs
	push	cs
	pop	ds
	pop	es

	mov	bx, timetable

	extended_get_rtc

	mov	ax, 0
	mov	cx, [tm_hour]
	call	hex_to_bcd
	mov	bh, al		; Hour in BCD is in BH

	mov	ax, 0
	mov	cx, [tm_min]
	call	hex_to_bcd
	mov	bl, al		; Minute in BCD is in BL

	mov	ax, 0
	mov	cx, [tm_sec]
	call	hex_to_bcd
	mov	dh, al		; Second in BCD is in DH

	mov	dl, 0		; Daylight saving flag = 0 always

	mov	cx, bx		; Hour:minute now in CH:CL

	pop	bx
	pop	ax
	pop	es
	pop	ds

	jmp	reach_stack_clc

  int1a_settime:

	; Set RTC time. Input: CH = hour (BCD), CL = minute (BCD),
	; DH = second (BCD), DL = daylight savings flag (ignored - the AON
	; timer we're driving doesn't have a DST concept).
	;
	; We first fetch the *current* date/time so that fields we're not
	; touching (the date) are preserved, overwrite just the time fields,
	; then hand the whole struct back to the emulator to commit.

	push	ax
	push	bx
	push	cx
	push	dx
	push	ds
	push	es

	push	cs
	push	cs
	pop	ds
	pop	es

	mov	bx, timetable
	extended_get_rtc

	mov	al, ch		; Hour, BCD
	call	bcd_to_hex
	mov	ah, 0
	mov	[tm_hour], ax
	mov	word [tm_hour+2], 0

	mov	al, cl		; Minute, BCD
	call	bcd_to_hex
	mov	ah, 0
	mov	[tm_min], ax
	mov	word [tm_min+2], 0

	mov	al, dh		; Second, BCD
	call	bcd_to_hex
	mov	ah, 0
	mov	[tm_sec], ax
	mov	word [tm_sec+2], 0

	extended_set_rtc	; AL = 0 on success, 0xFF on failure

	call	resync_tick_baseline

	cmp	al, 0

	pop	es
	pop	ds
	pop	dx
	pop	cx
	pop	bx
	pop	ax

	je	reach_stack_clc
	jmp	reach_stack_stc

  int1a_getdate:

	; Return the system date in BCD format.

	push	ds
	push	es
	push	bx
	push	ax

	push	cs
	push	cs
	pop	ds
	pop	es

	mov	bx, timetable

	extended_get_rtc

	mov	ax, 0x1900
	mov	cx, [tm_year]
	call	hex_to_bcd
	mov	cx, ax
	push	cx

	mov	ax, 1
	mov	cx, [tm_mon]
	call	hex_to_bcd
	mov	dh, al

	mov	ax, 0
	mov	cx, [tm_mday]
	call	hex_to_bcd
	mov	dl, al

	pop	cx
	pop	ax
	pop	bx
	pop	es
	pop	ds

	jmp	reach_stack_clc

  int1a_setdate:

	; Set RTC date. Input: CH = century (BCD, e.g. 0x20), CL = year within
	; century (BCD, 0-99), DH = month (BCD, 1-12), DL = day (BCD, 1-31).
	;
	; As with int1a_settime, we fetch the current date/time first so the
	; time-of-day fields are left untouched, overwrite just the date
	; fields, then commit the whole struct.

	push	ax
	push	bx
	push	cx
	push	dx
	push	ds
	push	es

	push	cs
	push	cs
	pop	ds
	pop	es

	mov	bx, timetable
	extended_get_rtc

	mov	al, cl		; Year within century, BCD
	call	bcd_to_hex
	mov	bl, al		; BL = 2-digit year (binary)

	mov	al, ch		; Century, BCD
	call	bcd_to_hex	; AL = century (binary), e.g. 20

	mov	ah, 0
	mov	ch, 100
	mul	ch		; AX = century * 100

	mov	bh, 0
	add	ax, bx		; AX = full 4-digit year

	sub	ax, 1900	; tm_year is years since 1900
	mov	[tm_year], ax
	mov	word [tm_year+2], 0

	mov	al, dh		; Month, BCD (1-12)
	call	bcd_to_hex
	dec	al		; tm_mon is 0-based (0 = January)
	mov	ah, 0
	mov	[tm_mon], ax
	mov	word [tm_mon+2], 0

	mov	al, dl		; Day of month, BCD (1-31)
	call	bcd_to_hex
	mov	ah, 0
	mov	[tm_mday], ax
	mov	word [tm_mday+2], 0

	extended_set_rtc	; AL = 0 on success, 0xFF on failure

	call	resync_tick_baseline

	cmp	al, 0

	pop	es
	pop	ds
	pop	dx
	pop	cx
	pop	bx
	pop	ax

	je	reach_stack_clc
	jmp	reach_stack_stc

  int1a_init:

	jmp	reach_stack_clc

; ************************* INT 1Ch - the other timer interrupt

int1c:

	iret

; ************************* INT 67h - EMS Manager

int67:
	extended_ems_call
	iret


; ************************* INT 1Eh - diskette parameter table

int1e:

		db 0xdf ; Step rate 2ms, head unload time 240ms
		db 0x02 ; Head load time 4 ms, non-DMA mode 0
		db 0x25 ; Byte delay until motor turned off
		db 0x02 ; 512 bytes per sector
int1e_spt	db 18	; 18 sectors per track (1.44MB)
		db 0x1B ; Gap between sectors for 3.5" floppy
		db 0xFF ; Data length (ignored)
		db 0x54 ; Gap length when formatting
		db 0xF6 ; Format filler byte
		db 0x0F ; Head settle time (1 ms)
		db 0x08 ; Motor start time in 1/8 seconds

; ************************* INT 41h - hard disk parameter table

int41:

int41_max_cyls	dw 0
int41_max_heads	db 0
		dw 0
		dw 0
		db 0
		db 11000000b
		db 0
		db 0
		db 0
		dw 0
int41_max_sect	db 0
		db 0

; ************************* ROM configuration table

rom_config	dw 16		; 16 bytes following
		db 0xfa		; Model
		db 'A'		; Submodel
		db 'C'		; BIOS revision
		db 0b00100000   ; Feature 1
		db 0b00000000   ; Feature 2
		db 0b00000000   ; Feature 3
		db 0b00000000   ; Feature 4
		db 0b00000000   ; Feature 5
		db 0, 0, 0, 0, 0, 0

; Internal state variables

num_disks	dw 0	; Number of disks present
hd_secs_hi	dw 0	; Total sectors on HD (high word)
hd_secs_lo	dw 0	; Total sectors on HD (low word)
hd_max_sector	dw 0	; Max sector number on HD
hd_max_track	dw 0	; Max track number on HD
hd_max_head	dw 0	; Max head number on HD
drive_tracks_temp dw 0
drive_sectors_temp dw 0
drive_heads_temp  dw 0
drive_num_temp    dw 0
boot_state	db 0
cga_refresh_reg	db 0

; Default interrupt handlers

int0:
int1:
int2:
int3:
int4:
int5:
int6:
int7:
intb:
intc:
intd:
inte:
intf:
int18:
int1b:
int1d:

iret

; ************ Function call library ************


post_beep:
    push ax
    push cx
    push dx

    mov al, 0xB6
    out 0x43, al

    mov ax, 0x0533          ; ~896 Hz
    out 0x42, al
    mov al, ah
    out 0x42, al

    in  al, 0x61
    push ax
    or  al, 0x03
    out 0x61, al

    mov cx, 0x8000
  .loop:
    nop
    loop .loop

    pop ax
    out 0x61, al

    pop dx
    pop cx
    pop ax
    ret

; Hex to BCD routine. Input is AX in hex (can be 0), and adds CX in hex to it, forming a BCD output in AX.

; ----------------------------------------------------
; HELPER: Prints a null-terminated string at CS:SI
; ----------------------------------------------------
print_str:
    push ax
    push bx
    push si
    cld
.loop:
    lodsb           ; Loads from [ds:si] by default
    cmp al, 0
    je .done
    mov ah, 0x0E
    mov bh, 0
    int 10h
    jmp .loop
.done:
    pop si
    pop bx
    pop ax
    ret
print_new_line:
    push ax
	mov	al, 0x0D
	mov	ah, 0x0E
	int	10h
	mov	al, 0x0A
	int	10h
    pop ax
    ret
; ----------------------------------------------------
; HELPER: Prints BX as decimal, followed by " KB OK"
; ----------------------------------------------------
print_kb_status:
	push	ax
	push	bx
	push	cx
	push	dx
	push	si

	; Reset cursor to column 0
	mov	al, 0x0D
	mov	ah, 0x0E
	int	10h

	; Re-print the prefix so it pushes the numbers to the right! ---
	mov	si, memteststr
	call	print_str

	; Calculate digits
	mov	ax, bx		; AX = number to print
	mov	cx, 0		; Digit counter
	mov	bx, 10
print_kb_div_loop:
	mov	dx, 0
	div	bx
	push	dx		; Push remainder (digit)
	inc	cx
	cmp	ax, 0
	jne	print_kb_div_loop

print_kb_digits:
	pop	ax
	add	al, '0'		; Convert integer to ASCII character
	mov	ah, 0x0E
	mov	bh, 0
	int	10h
	loop	print_kb_digits

	; Print the suffix
	mov	si, kbokstr
	call	print_str

	pop	si
	pop	dx
	pop	cx
	pop	bx
	pop	ax
	ret

hex_to_bcd:

	push	bx

	jcxz	h2bfin

  h2bloop:

	inc	ax

	; First process the low nibble of AL
	mov	bh, al
	and	bh, 0x0f
	cmp	bh, 0x0a
	jne	c1
	add	ax, 0x0006

	; Then the high nibble of AL
  c1:
	mov	bh, al
	and	bh, 0xf0
	cmp	bh, 0xa0
	jne	c2
	add	ax, 0x0060

	; Then the low nibble of AH
  c2:
	mov	bh, ah
	and	bh, 0x0f
	cmp	bh, 0x0a
	jne	c3
	add	ax, 0x0600

  c3:
	loop	h2bloop
  h2bfin:
	pop	bx
	ret

bcd_to_hex:

	; Converts a BCD byte in AL (high nibble = tens, low nibble = units, so
	; 0x00-0x99) into its binary value, returned in AL. Trashes AH.
	; Only shifts/multiplies by CL/CH are used since we're targeting 8086.

	push	bx
	push	cx

	mov	bh, al
	and	al, 0x0f	; AL = units digit

	mov	bl, al		; BL = units digit
	mov	al, bh
	mov	cl, 4
	shr	al, cl		; AL = tens digit

	mov	ah, 0
	mov	ch, 10
	mul	ch		; AX = tens digit * 10

	add	al, bl		; AL = tens*10 + units

	pop	cx
	pop	bx
	ret


; Keyboard adjust buffer head and tail. If either head or the tail are at the end of the buffer, reset them
; back to the start, since it is a circular buffer.

kb_adjust_buf:

	push	ax
	push	bx

	; Check to see if the head is at the end of the buffer (or beyond). If so, bring it back
	; to the start

	mov	ax, [es:kbbuf_end_ptr-bios_data]
	cmp	[es:kbbuf_head-bios_data], ax
	jnge	.adjust_tail

	mov	bx, [es:kbbuf_start_ptr-bios_data]
	mov	[es:kbbuf_head-bios_data], bx

.adjust_tail:

	; Check to see if the tail is at the end of the buffer (or beyond). If so, bring it back
	; to the start

	mov	ax, [es:kbbuf_end_ptr-bios_data]
	cmp	[es:kbbuf_tail-bios_data], ax
	jnge	.done

	mov	bx, [es:kbbuf_start_ptr-bios_data]
	mov	[es:kbbuf_tail-bios_data], bx

.done:

	pop	bx
	pop	ax
	ret

; Convert CHS disk position (in CH, CL and DH) to absolute sector number in BP:SI
; Floppy disks have 512 bytes per sector, 9/18 sectors per track, 2 heads. DH is head number (1 or 0), CH bits 5..0 is
; sector number, CL7..6 + CH7..0 is 10-bit cylinder/track number. Hard disks have 512 bytes per sector, but a variable
; number of tracks and heads.

chs_to_abs:

	push	ax
	push	bx
	push	cx
	push	dx

	mov	[cs:drive_num_temp], dl

	; First, we extract the track number from CH and CL.

	push	cx
	mov	bh, cl
	mov	cl, 6
	shr	bh, cl
	mov	bl, ch

	; Multiply track number (now in BX) by the number of heads

	cmp	byte [cs:drive_num_temp], 1 ; Floppy disk?

	push	dx

	mov	dx, 0
	xchg	ax, bx

	jne	chs_hd

	shl	ax, 1 ; Multiply by 2 (number of heads on FD)
	push	ax
	xor	ax, ax
	mov	al, [cs:int1e_spt]
	mov	[cs:drive_sectors_temp], ax ; Retrieve sectors per track from INT 1E table
	pop	ax

	jmp	chs_continue

chs_hd:

	mov	bp, [cs:hd_max_head]
	inc	bp
	mov	[cs:drive_heads_temp], bp

	mul	word [cs:drive_heads_temp] ; HD, so multiply by computed head count

	mov	bp, [cs:hd_max_sector] ; We previously calculated maximum HD track, so number of tracks is 1 more
	mov	[cs:drive_sectors_temp], bp

chs_continue:

	xchg	ax, bx

	pop	dx

	xchg	dh, dl
	mov	dh, 0
	add	bx, dx

	mov	ax, [cs:drive_sectors_temp]
	mul	bx

	; Now we extract the sector number (from 1 to 63) - for some reason they start from 1

	pop	cx
	mov	ch, 0
	and	cl, 0x3F
	dec	cl

	add	ax, cx
	adc	dx, 0
	mov	bp, ax
	mov	si, dx

	; Now, SI:BP contains offset into disk image file (FD or HD)

	pop	dx
	pop	cx
	pop	bx
	pop	ax
	ret


; Reaches up into the stack before the end of an interrupt handler, and sets the carry flag

reach_stack_stc:

	xchg	bp, sp
	or	word [bp+4], 1
	xchg	bp, sp
	iret

; Reaches up into the stack before the end of an interrupt handler, and clears the carry flag

reach_stack_clc:

	xchg	bp, sp
	and	word [bp+4], 0xfffe
	xchg	bp, sp
	iret

; Reaches up into the stack before the end of an interrupt handler, and returns with the current
; setting of the carry flag

reach_stack_carry:

	jc	reach_stack_stc
	jmp	reach_stack_clc


; Standard PC-compatible BIOS data area - to copy to 40:0, BDA

bios_data:

com1addr	dw	0
com2addr	dw	0
com3addr	dw	0
com4addr	dw	0
lpt1addr	dw	0
lpt2addr	dw	0
lpt3addr	dw	0
lpt4addr	dw	0
equip		dw	0b0000000000100001
		    db	0
memsize		dw	0x198 ; 40:0013
		    dw	0
keyflags	dw	0
		    db	0
kbbuf_head	dw	kbbuf-bios_data
kbbuf_tail	dw	kbbuf-bios_data
kbbuf: times 32	db	'X'
drivecal	db	0
diskmotor	db	0
motorshutoff	db	0x07
disk_laststatus	db	0
times 7		db	0
vidmode		db	0x03 ; 40:0049
vid_cols	dw	80 ; 40:004a
page_size	dw	0x1000
		    dw	0
curpos_x	db	0
curpos_y	db	0
times 7		dw	0
cur_v_end	db	7
cur_v_start	db	6
disp_page	db	0 ; 0040:0062
crtport		dw	0x3d4  ; 0040:0063 ADDR_6845
crt_mode    db  0 ; 0040:0065 CRT_MODE_SET
crt_pallete	db	0 ; 0040:0066 CRT_PALETTE
times 5		db	0
clk_dtimer	dd	0
clk_rollover	db	0
ctrl_break	db	0
soft_rst_flg	dw	0x1234
		db	0
num_hd		db	0
		db	0
		db	0
		dd	0
		dd	0
kbbuf_start_ptr	dw	0x001e
kbbuf_end_ptr	dw	0x003e
vid_rows	db	25         ; at 40:84
		db	0
		db	0
vidmode_opt	db	0 ; 0x70
		db	0 ; 0x89
		db	0 ; 0x51
		db	0 ; 0x0c
		db	0
		db	0
		db	0
		db	0
		db	0
		db	0
		db	0
		db	0
		db	0
		db	0
		db	0
kb_mode		db	0
kb_led		db	0
		db	0
		db	0
		db	0
		db	0
boot_device	db	0
crt_curpos_x	db	0
crt_curpos_y	db	0
key_now_down	db	0
next_key_fn	db	0
        	db	0
times 6 db 0    ; Formerly int7 specific variables (escape_flag, this_keystroke, etc.)
timer0_freq	dw	0xffff ; PIT channel 0 (55ms)
timer2_freq	dw	0      ; PIT channel 2
cga_vmode	db	0
vmem_offset	dw	0      ; Video RAM offset
ending:		times (0xff-($-com1addr)) db	0

; Interrupt vector table - to copy to 0:0

int_table	dw int0
          	dw 0xf000
          	dw int1
          	dw 0xf000
          	dw int2
          	dw 0xf000
          	dw int3
          	dw 0xf000
          	dw int4
          	dw 0xf000
          	dw int5
          	dw 0xf000
          	dw int6
          	dw 0xf000
          	dw int7
          	dw 0xf000
          	dw int8
          	dw 0xf000
          	dw int9
          	dw 0xf000
          	dw inta
          	dw 0xf000
          	dw intb
          	dw 0xf000
          	dw intc
          	dw 0xf000
          	dw intd
          	dw 0xf000
          	dw inte
          	dw 0xf000
          	dw intf
          	dw 0xf000
          	dw int10
          	dw 0xf000
          	dw int11
          	dw 0xf000
          	dw int12
          	dw 0xf000
          	dw int13
          	dw 0xf000
          	dw int14
          	dw 0xf000
          	dw int15
          	dw 0xf000
          	dw int16
          	dw 0xf000
          	dw int17
          	dw 0xf000
          	dw int18
          	dw 0xf000
          	dw int19
          	dw 0xf000
          	dw int1a
          	dw 0xf000
          	dw int1b
          	dw 0xf000
          	dw int1c
          	dw 0xf000
          	dw int1d
          	dw 0xf000
          	dw int1e

itbl_size	dw $-int_table


memteststr db   'Memory Test: ', 0
kbokstr    db   ' KB OK', 0


; INT 8 millisecond counter

last_int8_msec	dw	0

; Scratch variables for plot_char_gfx (INT 10h text output while in a CGA
; graphics mode - see plot_char_gfx below)
gfx_char	db	0
gfx_color	db	0
gfx_col		db	0
gfx_row		db	0
gfx_is_mode6	db	0
gfx_outb0	db	0
gfx_outb1	db	0
gfx_scanline	db	0
gfx_pixel	db	0
gfx_rep_char	db	0
gfx_rep_attr	db	0
gfx_maxcols	db	0
wc_write_attr	db	0	; 1 = AH=09h (write char+attrib), 0 = AH=0Ah (char only)

font8x8_table:
; 8x8 bitmap font, glyphs 0x00-0x7F (printable ASCII 0x20-0x7E is what
; matters here). Bit 7 of each byte = leftmost pixel of that row (MSB-first,
; matching the bit order used elsewhere in this file/picocalc_display.c for
; CGA pixel packing).
; Source data: https://github.com/dhepper/font8x8 (Daniel Hepper, Public
; Domain, based on Marcel Sondaar's public-domain IBM VGA font work), with
; each byte bit-reversed from the source's native LSB-first encoding.
; Characters 0x80-0xFF (extended/accented/box-drawing) are left blank - not
; covered by the source font's basic-Latin table.
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x01
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x02
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x03
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x04
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x05
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x06
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x07
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x08
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x09
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x0A
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x0B
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x0C
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x0D
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x0E
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x0F
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x10
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x11
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x12
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x13
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x14
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x15
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x16
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x17
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x18
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x19
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x1A
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x1B
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x1C
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x1D
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x1E
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x1F
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x20
	db	0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00	; 0x21
	db	0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x22
	db	0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00	; 0x23
	db	0x30, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x30, 0x00	; 0x24
	db	0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00	; 0x25
	db	0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00	; 0x26
	db	0x60, 0x60, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x27
	db	0x18, 0x30, 0x60, 0x60, 0x60, 0x30, 0x18, 0x00	; 0x28
	db	0x60, 0x30, 0x18, 0x18, 0x18, 0x30, 0x60, 0x00	; 0x29
	db	0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00	; 0x2A
	db	0x00, 0x30, 0x30, 0xFC, 0x30, 0x30, 0x00, 0x00	; 0x2B
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x60	; 0x2C
	db	0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x00	; 0x2D
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00	; 0x2E
	db	0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00	; 0x2F
	db	0x7C, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0x7C, 0x00	; 0x30
	db	0x30, 0x70, 0x30, 0x30, 0x30, 0x30, 0xFC, 0x00	; 0x31
	db	0x78, 0xCC, 0x0C, 0x38, 0x60, 0xCC, 0xFC, 0x00	; 0x32
	db	0x78, 0xCC, 0x0C, 0x38, 0x0C, 0xCC, 0x78, 0x00	; 0x33
	db	0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x1E, 0x00	; 0x34
	db	0xFC, 0xC0, 0xF8, 0x0C, 0x0C, 0xCC, 0x78, 0x00	; 0x35
	db	0x38, 0x60, 0xC0, 0xF8, 0xCC, 0xCC, 0x78, 0x00	; 0x36
	db	0xFC, 0xCC, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00	; 0x37
	db	0x78, 0xCC, 0xCC, 0x78, 0xCC, 0xCC, 0x78, 0x00	; 0x38
	db	0x78, 0xCC, 0xCC, 0x7C, 0x0C, 0x18, 0x70, 0x00	; 0x39
	db	0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x00	; 0x3A
	db	0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x60	; 0x3B
	db	0x18, 0x30, 0x60, 0xC0, 0x60, 0x30, 0x18, 0x00	; 0x3C
	db	0x00, 0x00, 0xFC, 0x00, 0x00, 0xFC, 0x00, 0x00	; 0x3D
	db	0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00	; 0x3E
	db	0x78, 0xCC, 0x0C, 0x18, 0x30, 0x00, 0x30, 0x00	; 0x3F
	db	0x7C, 0xC6, 0xDE, 0xDE, 0xDE, 0xC0, 0x78, 0x00	; 0x40
	db	0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00	; 0x41
	db	0xFC, 0x66, 0x66, 0x7C, 0x66, 0x66, 0xFC, 0x00	; 0x42
	db	0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00	; 0x43
	db	0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00	; 0x44
	db	0xFE, 0x62, 0x68, 0x78, 0x68, 0x62, 0xFE, 0x00	; 0x45
	db	0xFE, 0x62, 0x68, 0x78, 0x68, 0x60, 0xF0, 0x00	; 0x46
	db	0x3C, 0x66, 0xC0, 0xC0, 0xCE, 0x66, 0x3E, 0x00	; 0x47
	db	0xCC, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0xCC, 0x00	; 0x48
	db	0x78, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00	; 0x49
	db	0x1E, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78, 0x00	; 0x4A
	db	0xE6, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0xE6, 0x00	; 0x4B
	db	0xF0, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00	; 0x4C
	db	0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0x00	; 0x4D
	db	0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00	; 0x4E
	db	0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00	; 0x4F
	db	0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00	; 0x50
	db	0x78, 0xCC, 0xCC, 0xCC, 0xDC, 0x78, 0x1C, 0x00	; 0x51
	db	0xFC, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0xE6, 0x00	; 0x52
	db	0x78, 0xCC, 0xE0, 0x70, 0x1C, 0xCC, 0x78, 0x00	; 0x53
	db	0xFC, 0xB4, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00	; 0x54
	db	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFC, 0x00	; 0x55
	db	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00	; 0x56
	db	0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00	; 0x57
	db	0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x6C, 0xC6, 0x00	; 0x58
	db	0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x30, 0x78, 0x00	; 0x59
	db	0xFE, 0xC6, 0x8C, 0x18, 0x32, 0x66, 0xFE, 0x00	; 0x5A
	db	0x78, 0x60, 0x60, 0x60, 0x60, 0x60, 0x78, 0x00	; 0x5B
	db	0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00	; 0x5C
	db	0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x78, 0x00	; 0x5D
	db	0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00	; 0x5E
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF	; 0x5F
	db	0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x60
	db	0x00, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0x76, 0x00	; 0x61
	db	0xE0, 0x60, 0x60, 0x7C, 0x66, 0x66, 0xDC, 0x00	; 0x62
	db	0x00, 0x00, 0x78, 0xCC, 0xC0, 0xCC, 0x78, 0x00	; 0x63
	db	0x1C, 0x0C, 0x0C, 0x7C, 0xCC, 0xCC, 0x76, 0x00	; 0x64
	db	0x00, 0x00, 0x78, 0xCC, 0xFC, 0xC0, 0x78, 0x00	; 0x65
	db	0x38, 0x6C, 0x60, 0xF0, 0x60, 0x60, 0xF0, 0x00	; 0x66
	db	0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8	; 0x67
	db	0xE0, 0x60, 0x6C, 0x76, 0x66, 0x66, 0xE6, 0x00	; 0x68
	db	0x30, 0x00, 0x70, 0x30, 0x30, 0x30, 0x78, 0x00	; 0x69
	db	0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78	; 0x6A
	db	0xE0, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0xE6, 0x00	; 0x6B
	db	0x70, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00	; 0x6C
	db	0x00, 0x00, 0xCC, 0xFE, 0xFE, 0xD6, 0xC6, 0x00	; 0x6D
	db	0x00, 0x00, 0xF8, 0xCC, 0xCC, 0xCC, 0xCC, 0x00	; 0x6E
	db	0x00, 0x00, 0x78, 0xCC, 0xCC, 0xCC, 0x78, 0x00	; 0x6F
	db	0x00, 0x00, 0xDC, 0x66, 0x66, 0x7C, 0x60, 0xF0	; 0x70
	db	0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0x1E	; 0x71
	db	0x00, 0x00, 0xDC, 0x76, 0x66, 0x60, 0xF0, 0x00	; 0x72
	db	0x00, 0x00, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x00	; 0x73
	db	0x10, 0x30, 0x7C, 0x30, 0x30, 0x34, 0x18, 0x00	; 0x74
	db	0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0x76, 0x00	; 0x75
	db	0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00	; 0x76
	db	0x00, 0x00, 0xC6, 0xD6, 0xFE, 0xFE, 0x6C, 0x00	; 0x77
	db	0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00	; 0x78
	db	0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8	; 0x79
	db	0x00, 0x00, 0xFC, 0x98, 0x30, 0x64, 0xFC, 0x00	; 0x7A
	db	0x1C, 0x30, 0x30, 0xE0, 0x30, 0x30, 0x1C, 0x00	; 0x7B
	db	0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00	; 0x7C
	db	0xE0, 0x30, 0x30, 0x1C, 0x30, 0x30, 0xE0, 0x00	; 0x7D
	db	0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x7E
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	; 0x7F
	; 0x80-0xFF: extended chars, left blank pending a real CP437 table
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	db	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00


; Now follow the tables for instruction decode helping

; R/M mode tables

rm_mode0_reg1	db	3, 3, 5, 5, 6, 7, 12, 3
rm_mode012_reg2	db	6, 7, 6, 7, 12, 12, 12, 12
rm_mode0_disp	db	0, 0, 0, 0, 0, 0, 1, 0
rm_mode0_dfseg	db	11, 11, 10, 10, 11, 11, 11, 11

rm_mode12_reg1	db	3, 3, 5, 5, 6, 7, 5, 3
rm_mode12_disp	db	1, 1, 1, 1, 1, 1, 1, 1
rm_mode12_dfseg	db	11, 11, 10, 10, 11, 11, 10, 11

; Opcode decode tables

xlat_ids	db	9, 9, 9, 9, 7, 7, 25, 26, 9, 9, 9, 9, 7, 7, 25, 48, 9, 9, 9, 9, 7, 7, 25, 26, 9, 9, 9, 9, 7, 7, 25, 26, 9, 9, 9, 9, 7, 7, 27, 28, 9, 9, 9, 9, 7, 7, 27, 28, 9, 9, 9, 9, 7, 7, 27, 29, 9, 9, 9, 9, 7, 7, 27, 29, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 51, 54, 52, 52, 52, 52, 52, 52, 55, 55, 55, 55, 52, 52, 52, 52, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 15, 15, 24, 24, 9, 9, 9, 9, 10, 10, 10, 10, 16, 16, 16, 16, 16, 16, 16, 16, 30, 31, 32, 53, 33, 34, 35, 36, 11, 11, 11, 11, 17, 17, 18, 18, 47, 47, 17, 17, 17, 17, 18, 18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 12, 12, 19, 19, 37, 37, 20, 20, 49, 50, 19, 19, 38, 39, 40, 19, 12, 12, 12, 12, 41, 42, 43, 44, 53, 53, 53, 53, 53, 53, 53, 53, 13, 13, 13, 13, 21, 21, 22, 22, 14, 14, 14, 14, 21, 21, 22, 22, 53, 0, 23, 23, 53, 45, 6, 6, 46, 46, 46, 46, 46, 46, 5, 5
ex_data  	db	0, 0, 0, 0, 0, 0, 8, 8, 1, 1, 1, 1, 1, 1, 9, 36, 2, 2, 2, 2, 2, 2, 10, 10, 3, 3, 3, 3, 3, 3, 11, 11, 4, 4, 4, 4, 4, 4, 8, 0, 5, 5, 5, 5, 5, 5, 9, 1, 6, 6, 6, 6, 6, 6, 10, 2, 7, 7, 7, 7, 7, 7, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 21, 21, 21, 21, 21, 21, 0, 0, 0, 0, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 12, 12, 12, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 16, 22, 0, 0, 0, 0, 1, 1, 0, 255, 48, 2, 0, 0, 0, 0, 255, 255, 40, 11, 3, 3, 3, 3, 3, 3, 3, 3, 43, 43, 43, 43, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 21, 0, 0, 2, 40, 21, 21, 80, 81, 92, 93, 94, 95, 0, 0
std_flags	db	3, 3, 3, 3, 3, 3, 0, 0, 5, 5, 5, 5, 5, 5, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 5, 5, 5, 5, 5, 5, 0, 1, 3, 3, 3, 3, 3, 3, 0, 1, 5, 5, 5, 5, 5, 5, 0, 1, 3, 3, 3, 3, 3, 3, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
base_size	db	2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 0, 2, 2, 2, 2, 4, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2
i_w_adder	db	0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
i_mod_adder	db	1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1

flags_mult	db	0, 2, 4, 6, 7, 8, 9, 10, 11

jxx_dec_a	db	48, 40, 43, 40, 44, 41, 49, 49
jxx_dec_b	db	49, 49, 49, 43, 49, 49, 49, 43
jxx_dec_c	db	49, 49, 49, 49, 49, 49, 44, 44
jxx_dec_d	db	49, 49, 49, 49, 49, 49, 48, 48

parity		db	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1


BDATASEG:    equ         0040h

; This is the format of the 36-byte tm structure, returned by the emulator's RTC query call

timetable:

tm_sec		equ $
tm_min		equ $+4
tm_hour		equ $+8
tm_mday		equ $+12
tm_mon		equ $+16
tm_year		equ $+20
tm_wday		equ $+24
tm_yday		equ $+28
tm_dst		equ $+32
tm_msec		equ $+36
