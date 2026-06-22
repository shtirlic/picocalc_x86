;
; Portions of this file are derived from 8086tiny.
; Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
; Licensed under the MIT License.
; See LICENSE.txt.
;
; Modifications Copyright (c) 2026 Serg Podtynnyi
; Licensed under GPLv3 for the combined work.
;

; ************************* INT 10h handler - video services

int10:
	cmp	ah, 0x00 ; Set video mode
	je	int10_set_vm
	cmp	ah, 0x01 ; Set cursor shape
	je	int10_set_cshape
	cmp	ah, 0x02 ; Set cursor position
	je	int10_set_cursor
	cmp	ah, 0x03 ; Get cursur position
	je	int10_get_cursor
    cmp	ah, 0x05 ; Set active display page
	je	int10_set_page
	cmp	ah, 0x06 ; Scroll up window
	je	int10_scrollup
	cmp	ah, 0x07 ; Scroll down window
	je	int10_scrolldown
	cmp	ah, 0x08 ; Get character at cursor
	je	int10_charatcur
	cmp	ah, 0x09 ; Write char and attribute
	je	int10_write_char_attrib
	cmp	ah, 0x0a ; Write char only (attribute unchanged)
	je	int10_write_char_only
	cmp	ah, 0x0b ; Set background color or palette
	je	int10_set_bg_palette
	cmp	ah, 0x0e ; Write character at cursor position
	je	int10_write_char
	cmp	ah, 0x0f ; Get video mode
	je	int10_get_vm
	; cmp	ah, 0x10 ; Palette and Color Routing
	; je	int10_palette
	cmp	ah, 0x1a ; Feature check
	je	int10_features

	iret

  int10_set_vm:

	push	dx
	push	cx
	push	bx
	push	es

	cmp	al, 4 ; CGA mode 4
	je	int10_switch_to_cga_gfx
	cmp	al, 5
	je	int10_switch_to_cga_gfx
	cmp	al, 6
	je	int10_switch_to_cga_gfx

    push	ax

	; Switch back to CGA Text Mode
	mov	dx, BDATASEG
	mov	es, dx

	cmp	al, 0		; Mode 0: 40x25 B&W
	je	.set_40
	cmp	al, 1		; Mode 1: 40x25 Color
	je	.set_40

.set_80:
	mov	word [es:vid_cols-bios_data], 80
	mov	word [es:page_size-bios_data], 0x1000
	mov	bl, 80
	jmp	.apply_crtc

.set_40:
	mov	word [es:vid_cols-bios_data], 40
	mov	word [es:page_size-bios_data], 0x0800
	mov	bl, 40

.apply_crtc:

	mov	dx, 0x3d4
	mov	al, 1
	out	dx, al
	mov	dx, 0x3d5
	mov	al, bl		; Apply 40 or 80 columns
	out	dx, al

	mov	dx, 0x3d4
	mov	al, 6
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
	out	dx, al

	pop	ax

	mov	bx, BDATASEG
	mov	es, bx

	mov	[es:vidmode-bios_data], al

	mov	bh, 7		; Black background, white foreground
	call	clear_screen	; ANSI clear screen

	; CGA Overscan/Color Select register (0x3D9)
	mov	dx, 0x3d9
	mov	al, 0x30
	out	dx, al
	mov	[es:0x66], al

    ; CGA mode select register
    mov dx, 0x3d8
    mov al, byte [es:vidmode-bios_data]  ; AL = Mode (0, 1, 2, 3, or 7)

    ; Color Burst bit (Bit 2)
    mov ah, al
    and ah, 1
    xor ah, 1
    shl ah, 1
    shl ah, 1

    ; 80-Column bit (Bit 0)
    shr al, 1
    and al, 1

    or al, ah
    or al, 0x28

    out dx, al
    mov	[es:0x65], al
    ; pop ax

  svmn_exit:

	pop	es
	pop	bx
	pop	cx
	pop	dx
	iret

int10_switch_to_cga_gfx:
	; Switch to True CGA Graphics Mode (320x200 or 640x200)

	mov	dx, BDATASEG
	mov	es, dx

	mov	[es:vidmode-bios_data], al	      ; Save the requested video mode (4, 5, or 6) to BDA
	mov	word [es:0x63], 0x3D4 ; UPDATE: Set CRTC base port in BDA to CGA (Color)

	; Program the standard CGA CRTC (0x3D4 / 0x3D5)
	mov	dx, 0x3d4
	mov	al, 1		; R1: Horizontal Displayed
	out	dx, al
	mov	dx, 0x3d5
	mov	al, 0x28	; 40 columns (40 * 8 = 320 pixels)
	out	dx, al

	mov	dx, 0x3d4
	mov	al, 6		; R6: Vertical Displayed
	out	dx, al
	mov	dx, 0x3d5
	mov	al, 0x64	; 100 character rows (CGA hardware draws 2 scanlines per row = 200 pixels)
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
	out	dx, al


    ; CGA Mode Control Register (0x3D8)
    mov dx, 0x3D8
    mov al, byte [es:vidmode-bios_data]  ; Load mode from BDA (4, 5, or 6)

    ; 1. Calculate the +4 offset for Mode 5
    mov ah, al              ; Save a copy of the mode
    and al, 1               ; Isolate Bit 0 (Yields 1 for Mode 5, 0 for 4/6)
    shl al, 1               ; Multiply by 2 (8086 compatible)
    shl al, 1               ; Multiply by 2 again (Total x4)
    add al, 0x2A            ; Base: AL is now 0x2E (Mode 5) or 0x2A (Mode 4/6)

    ; 2. Calculate the -14 (0x0E) offset for Mode 6
    and ah, 2               ; Isolate Bit 1 (Yields 2 for Mode 6, 0 for 4/5)
    neg ah                  ; Mathematical trick: 2 becomes 0xFE (-2). 0 stays 0.
    and ah, 0x0E            ; Mask it: 0xFE & 0x0E becomes 0x0E (14). 0 stays 0.
    sub al, ah              ; Subtract: Mode 6 does 0x2A - 0x0E = 0x1C.

    out dx, al
    mov [es:0x65], al

    ; CGA Color Select Register (0x3D9)
    mov dx, 0x3D9
    mov al, byte [es:vidmode-bios_data]  ; Load mode from BDA (4, 5, or 6)

    ; 1. Calculate the offset (0x00 for modes 4/5, 0x0F for mode 6)
    shr al, 1               ; Move Mode Bit 1 down to Bit 0
    and al, 1               ; Isolate it (Mode 4/5 = 0, Mode 6 = 1)
    neg al                  ; Mathematical trick: 1 becomes 0xFF (-1). 0 stays 0.
    and al, 0x0F            ; Mask it: 0xFF & 0x0F becomes 0x0F. 0 stays 0.

    ; 2. Add the base value
    add al, 0x30            ; AL is now 0x3F (Mode 6) or 0x30 (Modes 4/5)

    ; 3. Output to port and save to BDA
    out dx, al
    mov [es:0x66], al       ; Update CGA overscan register copy in BDA

	mov	bh, 0       ; Use black background to clear screen
	call	clear_screen

	jmp	svmn_exit


int10_set_cshape:
	push	ds
	push	ax
	push	cx
	push	dx

	mov	ax, BDATASEG
	mov	ds, ax

	; save the requested start and end scanlines to the BDA
	mov	[cur_v_start-bios_data], ch
	mov	[cur_v_end-bios_data], cl

    ; write_crtc
	mov	dx, [crtport-bios_data]		; Fetch CRTC Index Port (e.g., 0x3D4)

	; Set Cursor Start Register (Index 0x0A)
	mov	al, 0x0A
	out	dx, al
	inc	dx				; Move to CRTC Data Port (e.g., 0x3D5)
	mov	al, ch
	out	dx, al

	dec	dx				; Move back to CRTC Index Port

	; Set Cursor End Register (Index 0x0B)
	mov	al, 0x0B
	out	dx, al
	inc	dx				; Move to CRTC Data Port
	mov	al, cl
	out	dx, al

	pop	dx
	pop	cx
	pop	ax
	pop	ds
	iret

int10_set_cursor:
	push	ds
	push	ax
	push	bx
	push	dx

	mov	ax, BDATASEG
	mov	ds, ax

	; 1. Save to BIOS Data Area (for legacy software reads)
	mov	[curpos_y-bios_data], dh
	mov	[crt_curpos_y-bios_data], dh
	mov	[curpos_x-bios_data], dl
	mov	[crt_curpos_x-bios_data], dl

	; 2. Calculate 1D Hardware Offset: (DH * 80) + DL
    mov	al, dh
	mov	bl, [vid_cols-bios_data] ;
	mul	bl		; AX = row * cols
    mov	dh, 0		; Clear DH so DX just contains DL (column)
	add	ax, dx		; AX = (row * cols) + col
	mov	bx, ax		; Save the final offset into BX (BH=High, BL=Low)

	; 3. Send offset to CRTC Hardware
	mov	dx, [crtport-bios_data] ; Fetch CRTC Index Port (0x3D4)

	; Set Cursor Location High Register (Index 0x0E)
	mov	al, 0x0E
	out	dx, al
	inc	dx		; Move to Data Port (0x3D5)
	mov	al, bh		; High byte of offset
	out	dx, al

	dec	dx		; Move back to Index Port (0x3D4)

	; Set Cursor Location Low Register (Index 0x0F)
	mov	al, 0x0F
	out	dx, al
	inc	dx		; Move to Data Port (0x3D5)
	mov	al, bl		; Low byte of offset
	out	dx, al

	pop	dx
	pop	bx
	pop	ax
	pop	ds
	iret


  ; TODO: A separate cursor is maintained for each of up to 8 display pages
  int10_get_cursor:

	push	es

	mov	cx, BDATASEG
	mov	es, cx

    mov ch, [es:cur_v_start-bios_data]
    mov cl, [es:cur_v_end-bios_data]
	mov	dh, [es:curpos_y-bios_data]
	mov	dl, [es:curpos_x-bios_data]

	pop	es

	iret

int10_set_page:
	push	ds
	push	ax
	push	bx
	push	cx
	push	dx

	mov	bx, BDATASEG
	mov	ds, bx

	; 1. Save the requested active page (AL) to the BDA
	mov	[disp_page-bios_data], al

	; 2. Calculate the VRAM byte offset: Offset = Page * page_size
	mov	bl, al
	mov	bh, 0			; BX = requested page (0-3)
	mov	ax, [page_size-bios_data] ; AX = size of a single page (0x1000)
	mul	bx			; AX = Byte Offset in VRAM (e.g., 0x1000, 0x2000)

	; 3. Store the calculated byte offset back into the BDA
	mov	[vmem_offset-bios_data], ax

	; 4. Calculate CRTC Word Offset
	; The hardware CRTC uses a word offset, so we divide the byte offset by 2
	shr	ax, 1
	mov	cx, ax			; CH = High Byte, CL = Low Byte

	; 5. Send the word offset to the CRTC Start Address Registers
	mov	dx, [crtport-bios_data]	; Fetch CRTC Index Port (0x3D4)

	; Set Start Address High Register (Index 0x0C)
	mov	al, 0x0C
	out	dx, al
	inc	dx			; Move to Data Port (0x3D5)
	mov	al, ch
	out	dx, al

	dec	dx			; Move back to Index Port (0x3D4)

	; Set Start Address Low Register (Index 0x0D)
	mov	al, 0x0D
	out	dx, al
	inc	dx			; Move to Data Port (0x3D5)
	mov	al, cl
	out	dx, al

	pop	dx
	pop	cx
	pop	bx
	pop	ax
	pop	ds
	iret



  int10_scrollup:
	cmp	al, 0 ; Clear window
	jne	cls_partial

	cmp	cx, 0 ; Start of screen
	jne	cls_partial

	cmp	dl, 0x4f ; Clearing columns 0-79
	jb	cls_partial

	cmp	dh, 0x18 ; Clearing rows 0-24 (or more)
	jb	cls_partial

	call	clear_screen
	iret

  cls_partial:
	mov	bl, al		; Number of rows to scroll are now in bl
	cmp	bl, 0		; Clear whole window?
	jne	int10_scroll_up_vmem_update
	mov	bl, 25		; 25 rows

int10_scroll_up_vmem_update:
	push	bx
	push	ax
	push	ds
	push	es
	push	cx
	push	dx
	push	si
	push	di

	push	bx
	mov	bx, 0xb800
	mov	es, bx
	mov	ds, bx
	pop	bx

	mov	bl, al
	cmp	bl, 0
	jne	cls_vmem_scroll_up_next_line
	mov	bl, 25      ; AL=0 means clear whole window

    cls_vmem_scroll_up_next_line:
	push	cx
	push	dx

cls_vmem_scroll_up_one:
	push	bx
	push	dx
	mov	ax, 0
	mov	al, ch		; Start row number is now in AX

	push	ds
	mov	bx, BDATASEG
	mov	ds, bx
	mov	bx, [vid_cols-bios_data]
	pop	ds

	mul	bx
	add	al, cl
	adc	ah, 0		; Character number is now in AX
	mov	bx, 2
	mul	bx		; Memory location is now in AX
	pop	dx
	pop	bx

    mov	di, ax
	mov	si, ax

    push	bx
	push	ds
	mov	bx, BDATASEG
	mov	ds, bx
	mov	bx, [vid_cols-bios_data]
	shl	bx, 1
	pop	ds

	add	si, bx
    pop	bx

	mov	ax, 0
	add	al, dl
	adc	ah, 0
	inc	ax
	sub	al, cl
	sbb	ah, 0		; AX now contains the number of characters from the row to copy

	cmp	ch, dh
	jae	cls_vmem_scroll_up_one_done

vmem_scroll_up_copy_next_row:
	push	cx
	mov	cx, ax		; CX is now the length (in words) of the row to copy
	cld
	rep	movsw		; Scroll the line up
	pop	cx

	inc	ch		; Move onto the next row
	jmp	cls_vmem_scroll_up_one

    cls_vmem_scroll_up_one_done:
	push	cx
	mov	cx, ax		; CX is now the length (in words) of the row to copy
	mov	ah, bh		; Attribute for new line
	mov	al, 0x20	; Write Space
	cld
	rep	stosw
	pop	cx

	pop	dx
	pop	cx

	dec	bl		; Scroll whole text block another line
	cmp	bl, 0
	je	cls_vmem_scroll_up_done
	jmp	cls_vmem_scroll_up_next_line

    cls_vmem_scroll_up_done:
	pop	di
	pop	si
	pop	dx
	pop	cx
	pop	es
	pop	ds
	pop	ax
	pop	bx
	iret


  int10_scrolldown:

	cmp	al, 0 ; Clear window
	jne	cls_partial_down

	cmp	cx, 0 ; Start of screen
	jne	cls_partial_down

	cmp	dl, 0x4f ; Clearing columns 0-79
	jne	cls_partial_down

	cmp	dh, 0x18 ; Clearing rows 0-24 (or more)
	jl	cls_partial_down

	call	clear_screen
	iret

  cls_partial_down:
	mov	bx, 0
	mov	bl, al		; Number of rows to scroll are now in bl

	cmp	bl, 0		; Clear whole window?
	jne	int10_scroll_down_vmem_update

	mov	bl, 25		; 25 rows

int10_scroll_down_vmem_update:
	push	ax
	push	bx
	push	ds
	push	es
	push	cx
	push	dx
	push	si
	push	di

	push	bx
	mov	bx, 0xb800
	mov	es, bx
	mov	ds, bx
	pop	bx

	mov	bl, al
	cmp	bl, 0
	jne	cls_vmem_scroll_down_next_line
	mov	bl, 25      ; AL=0 means clear whole window

    cls_vmem_scroll_down_next_line:
	push	cx	; WINDOW BOUNDS (CH, CL)
	push	dx	; WINDOW BOUNDS (DH, DL)

    cls_vmem_scroll_down_one:
	push	bx
	push	dx
	mov	ax, 0
	mov	al, dh		; End row number is now in AX

    push	bx
	push	ds
	mov	bx, BDATASEG
	mov	ds, bx
	mov	bx, [vid_cols-bios_data]
	pop	ds

	mul	bx
	add	al, cl
	adc	ah, 0		; Character number is now in AX
	mov	bx, 2
	mul	bx		; Memory location (start of final row) is now in AX
	pop	dx
	pop	bx

    mov	di, ax
	mov	si, ax

	push	ds
	mov	bx, BDATASEG
	mov	ds, bx
	mov	bx, [vid_cols-bios_data]
	shl	bx, 1
	pop	ds

	sub	si, bx
    pop	bx

	mov	ax, 0
	add	al, dl
	adc	ah, 0
	inc	ax
	sub	al, cl
	sbb	ah, 0		; AX now contains the number of characters from the row to copy

	cmp	ch, dh
	jae	cls_vmem_scroll_down_one_done

	push	cx
	mov	cx, ax		; CX is now the length (in words) of the row to copy
	cld
	rep	movsw		; Scroll the line down
	pop	cx

	dec	dh		; Move onto the next row
	jmp	cls_vmem_scroll_down_one

    cls_vmem_scroll_down_one_done:
	push	cx
	mov	cx, ax		; CX is now the length (in words) of the row to copy
	mov	ah, bh		; Attribute for new line
	mov	al, 0x20	; Write Space
	cld
	rep	stosw
	pop	cx

	pop	dx	; WINDOW BOUNDS
	pop	cx	; WINDOW BOUNDS

	dec	bl		; Scroll whole text block another line
	cmp	bl, 0
	je	cls_vmem_scroll_down_done
	jmp	cls_vmem_scroll_down_next_line

    cls_vmem_scroll_down_done:
	pop	di
	pop	si
	pop	dx
	pop	cx
	pop	es
	pop	ds
	pop	bx
	pop	ax
	iret


  int10_charatcur:

	push	ds
	push	es
	push	bx
	push	dx

	mov	bx, BDATASEG
	mov	es, bx

	mov	bx, 0xb800
	mov	ds, bx

    mov	bx, [es:vid_cols-bios_data]
	shl	bx, 1
	mov	ax, 0
	mov	al, [es:curpos_y-bios_data]
	mul	bx

	mov	bx, 0
	mov	bl, [es:curpos_x-bios_data]
	add	ax, bx
	add	ax, bx
	mov	bx, ax

	mov	ax, [bx]

	pop	dx
	pop	bx
	pop	es
	pop	ds

	iret

  i10_unsup:

	iret

; ---------------------------------------------------------------
; plot_char_gfx: plot one 8x8 font glyph into the CGA graphics
; framebuffer (modes 4/5/6). Used so INT 10h text output (AH=0x0E
; teletype, AH=0x09 write char+attrib) draws readable pixels while
; in a graphics mode
;
; In:  AL = character to plot
;      BL = foreground color (2-bit index for modes 4/5, bit 0 for
;           mode 6: 0 = off/black, nonzero = on/white)
;      CL = character-cell column (0-based)
;      DL = character-cell row (0-based, 0-24)
; Out: none. All registers preserved.
;
; ---------------------------------------------------------------
plot_char_gfx:
	push	ax
	push	bx
	push	cx
	push	dx
	push	si
	push	di
	push	es
	push	ds

	mov	[cs:gfx_char], al
	mov	[cs:gfx_color], bl
	mov	[cs:gfx_col], cl
	mov	[cs:gfx_row], dl

	; si -> glyph's 8 row-bytes (CS-relative, MSB-first bit order)
	xor	ah, ah
	mov	si, ax
	shl	si, 1
	shl	si, 1
	shl	si, 1
	add	si, font8x8_table

	; is this mode 6 (1bpp)?
	mov	ax, BDATASEG
	mov	ds, ax
	mov	al, [vidmode-bios_data]
	mov	byte [cs:gfx_is_mode6], 0
	cmp	al, 6
	jne	.not_m6
	mov	byte [cs:gfx_is_mode6], 1
.not_m6:

	mov	ax, 0xb800
	mov	es, ax

	mov	byte [cs:gfx_scanline], 0
.scanline_loop:
	; py = row*8 + scanline
	mov	al, [cs:gfx_row]
	mov	ah, 0
	mov	bx, 8
	mul	bx
	mov	bl, [cs:gfx_scanline]
	mov	bh, 0
	add	ax, bx			; ax = py (0-199)

	; bank_offset = (py & 1) ? 0x2000 : 0 ; row_offset = (py/2)*80
	mov	dx, ax
	and	dx, 1
	mov	cl, 13
	shl	dx, cl
	shr	ax, 1
	mov	bx, 80
	mul	bx
	add	ax, dx
	mov	di, ax

	mov	al, [cs:gfx_col]
	mov	ah, 0
	cmp	byte [cs:gfx_is_mode6], 1
	je	.col1
	shl	ax, 1			; modes 4/5: 2 bytes per char column
.col1:
	add	di, ax

	; fetch font byte for this scanline
	mov	bx, si
	mov	al, [cs:gfx_scanline]
	mov	ah, 0
	add	bx, ax
	mov	al, [cs:bx]		; al = glyph row byte, bit7=leftmost pixel

	cmp	byte [cs:gfx_is_mode6], 1
	jne	.mode45

	; ---- mode 6: 1bpp, direct byte-for-byte ----
	cmp	byte [cs:gfx_color], 0
	jne	.m6_fg
	xor	al, al
.m6_fg:
	mov	[es:di], al
	jmp	.scanline_done

.mode45:
	; ---- modes 4/5: expand 8x 1bpp pixels into 2x 2bpp bytes ----
	mov	bl, al			; bl = glyph byte, shifted one bit at a time
	mov	byte [cs:gfx_outb0], 0
	mov	byte [cs:gfx_outb1], 0
	mov	byte [cs:gfx_pixel], 0
.pixloop:
	shl	bl, 1			; MSB -> CF
	jnc	.px_bg
	mov	al, [cs:gfx_color]
	and	al, 3
	jmp	.px_have
.px_bg:
	xor	al, al
.px_have:
	mov	cl, [cs:gfx_pixel]
	cmp	cl, 4
	jae	.px_second
	mov	ch, 6
	sub	ch, cl
	sub	ch, cl			; ch = 6 - 2*pixel (shift amount)
	mov	cl, ch
	shl	al, cl
	or	[cs:gfx_outb0], al
	jmp	.px_done
.px_second:
	sub	cl, 4
	mov	ch, 6
	sub	ch, cl
	sub	ch, cl
	mov	cl, ch
	shl	al, cl
	or	[cs:gfx_outb1], al
.px_done:
	inc	byte [cs:gfx_pixel]
	mov	al, [cs:gfx_pixel]
	cmp	al, 8
	jb	.pixloop

	mov	al, [cs:gfx_outb0]
	mov	[es:di], al
	mov	al, [cs:gfx_outb1]
	mov	[es:di+1], al

.scanline_done:
	inc	byte [cs:gfx_scanline]
	mov	al, [cs:gfx_scanline]
	cmp	al, 8
	jb	.scanline_loop

	pop	ds
	pop	es
	pop	di
	pop	si
	pop	dx
	pop	cx
	pop	bx
	pop	ax
	ret

  int10_write_char:

    cmp al, 0x07            ; BEL (beep) instead of printing/advancing cursor
    je  int10_beep

	mov	[cs:gfx_color], bl	; capture foreground color (graphics modes
					; only) before bx gets reused as scratch below

	push	ds
	push	es
	push	cx
	push	dx
	push	ax
	push	bp
	push	bx

	push	ax

	mov	cl, al
	mov	ch, 7

	mov	bx, BDATASEG
	mov	es, bx

	cmp	byte [es:vidmode-bios_data], 4
	jb	.text_mode_write

	cmp	cl, 0x0A		; LF/CR/BS only move the cursor
	je	int10_write_char_skip_lines
	cmp	cl, 0x0D
	je	int10_write_char_skip_lines
	cmp	cl, 0x08
	je	int10_write_char_skip_lines

	mov	al, cl
	mov	bl, [cs:gfx_color]
	mov	cl, [es:curpos_x-bios_data]
	mov	dl, [es:curpos_y-bios_data]
	call	plot_char_gfx
	jmp	int10_write_char_skip_lines

.text_mode_write:
	mov	bx, 0xb800
	mov	ds, bx

    mov	bx, [es:vid_cols-bios_data]
	shl	bx, 1
	mov	ax, 0
	mov	al, [es:curpos_y-bios_data]
	mul	bx

	mov	bx, 0
	mov	bl, [es:curpos_x-bios_data]
	shl	bx, 1
	add	bx, ax

	cmp	cl, 0x0A	; Is it a Line Feed?
	je	int10_write_char_skip_lines
	cmp	cl, 0x0D	; Is it a Carriage Return?
	je	int10_write_char_skip_lines
	cmp	cl, 0x08	; Is it a Backspace?
	je	int10_write_char_skip_lines

	mov	[bx], cx	; Only write if it is a real character

	jmp	int10_write_char_skip_lines

int10_write_char_attrib:
	mov	byte [cs:wc_write_attr], 1
	jmp	int10_write_char_common

int10_write_char_only:
	mov	byte [cs:wc_write_attr], 0
	jmp	int10_write_char_common

int10_write_char_common:
	push	ds
	push	es
	push	cx
	push	dx
	push	ax
	push	bp
	push	bx
	push	di

	mov	dx, cx		; Save repetition count in DX
	mov	[cs:gfx_rep_char], al
	mov	[cs:gfx_rep_attr], bl

    mov	bx, BDATASEG
	mov	es, bx

	cmp	byte [es:vidmode-bios_data], 4
	jb	.text_mode_write

	mov	al, [es:vidmode-bios_data]
	mov	byte [cs:gfx_maxcols], 40
	cmp	al, 6
	jne	.gfx_maxcols_ok
	mov	byte [cs:gfx_maxcols], 80
.gfx_maxcols_ok:

	mov	al, [es:curpos_x-bios_data]
	mov	[cs:gfx_col], al
	mov	al, [es:curpos_y-bios_data]
	mov	[cs:gfx_row], al

.gfx_rep_loop:
	cmp	dx, 0
	je	.gfx_rep_done
	mov	al, [cs:gfx_col]
	cmp	al, [cs:gfx_maxcols]
	jae	.gfx_rep_done

	mov	al, [cs:gfx_rep_char]
	mov	bl, [cs:gfx_rep_attr]
	mov	cl, [cs:gfx_col]
	mov	dl, [cs:gfx_row]
	call	plot_char_gfx

	inc	byte [cs:gfx_col]
	dec	dx
	jmp	.gfx_rep_loop

.gfx_rep_done:
	jmp	.done

.text_mode_write:
	mov	bx, 0xb800
	mov	ds, bx
	mov	bx, [es:vid_cols-bios_data]
	shl	bx, 1
	mov	ax, 0
	mov	al, [es:curpos_y-bios_data]

	push dx		; Protect DX (repetition count)
	mul	bx		; AX = Y * (COLS * 2)
	pop	dx		; Restore DX

	mov	bx, 0
	mov	bl, [es:curpos_x-bios_data]
	shl	bx, 1
	add	bx, ax

    push	es
	push	ds
	pop	es		; ES = 0xb800
	mov	di, bx
	mov	cx, dx		; CX = repetition count
	cld

	cmp	byte [cs:wc_write_attr], 0
	je	.char_only_loop

	; AH=09h: overwrite both character and attribute bytes
	mov	al, [cs:gfx_rep_char]
	mov	ah, [cs:gfx_rep_attr]
	rep	stosw
	jmp	.text_done

.char_only_loop:
	; AH=0Ah: write character byte only, leave the attribute byte alone
	cmp	cx, 0
	je	.text_done
	mov	al, [cs:gfx_rep_char]
	mov	[es:di], al
	add	di, 2
	dec	cx
	jmp	.char_only_loop

.text_done:
	pop	es

.done:
	pop	di
	pop	bx
	pop	bp
	pop	ax
	pop	dx
	pop	cx
	pop	es
	pop	ds
	iret


    int10_write_char_skip_lines:

	pop	ax

	push	es
	pop	ds

	cmp	al, 0x08
	jne	int10_write_char_attrib_inc_x

	dec	byte [curpos_x-bios_data]
	dec	byte [crt_curpos_x-bios_data]
	cmp	byte [curpos_x-bios_data], 0
	jg	int10_write_char_attrib_done

	mov	byte [curpos_x-bios_data], 0
	mov	byte [crt_curpos_x-bios_data], 0
	jmp	int10_write_char_attrib_done

    int10_write_char_attrib_inc_x:

	cmp	al, 0x0A	; New line?
	je	int10_write_char_attrib_newline

	cmp	al, 0x0D	; Carriage return?
	jne	int10_write_char_attrib_not_cr

	mov	byte [curpos_x-bios_data], 0
	mov	byte [crt_curpos_x-bios_data], 0
	jmp	int10_write_char_attrib_done

    int10_write_char_attrib_not_cr:

	inc	byte [curpos_x-bios_data]
	inc	byte [crt_curpos_x-bios_data]

	push	bx
	mov	bl, [vid_cols-bios_data]
	cmp	byte [curpos_x-bios_data], bl
	pop	bx

	jge	int10_write_char_attrib_newline
	jmp	int10_write_char_attrib_done

int10_write_char_attrib_newline:

	mov	byte [curpos_x-bios_data], 0
	mov	byte [crt_curpos_x-bios_data], 0
	inc	byte [curpos_y-bios_data]
	inc	byte [crt_curpos_y-bios_data]

	cmp	byte [curpos_y-bios_data], 25
	jb	int10_write_char_attrib_done
	mov	byte [curpos_y-bios_data], 24
	mov	byte [crt_curpos_y-bios_data], 24

	mov	bh, 7
	mov	al, 1
	mov	cx, 0

	; Set DL to max column (vid_cols - 1), DH to 24 (0x18)
	mov	dl, [vid_cols-bios_data]
	dec	dl
	mov	dh, 0x18

	pushf
	push	cs
	call	int10_scroll_up_vmem_update

int10_write_char_attrib_done:

	mov	al, [curpos_y-bios_data]
	mov	bl, [vid_cols-bios_data]
	mul	bl			; AX = Y * COLS
	mov	dl, [curpos_x-bios_data]
	mov	dh, 0
	add	ax, dx			; AX = (Y * COLS) + X
	mov	bx, ax			; Save offset to BX (BH=High, BL=Low)

	mov	dx, [crtport-bios_data]	; Fetch CRTC index port (0x3D4)

	mov	al, 0x0E		; Register 14: Cursor Location High
	out	dx, al
	inc	dx
	mov	al, bh
	out	dx, al

	dec	dx

	mov	al, 0x0F		; Register 15: Cursor Location Low
	out	dx, al
	inc	dx
	mov	al, bl
	out	dx, al

	pop	bx
	pop	bp
	pop	ax
	pop	dx
	pop	cx
	pop	es
	pop	ds

	iret
  int10_get_vm:

	push	es

	mov	ax, BDATASEG
	mov	es, ax

    mov ah, [es:vid_cols-bios_data]
	mov	al, [es:vidmode-bios_data]
	mov	bh, [es:disp_page-bios_data]

	pop	es

	iret


int10_beep:
    push ax
    push bx
    push cx
    push dx
    push si
    push es

    sti

    ; Program PIT channel 2: mode 3 (square wave), lobyte/hibyte access
    mov al, 0xB6
    out 0x43, al

    mov ax, 0x0533          ; reload 1331 -> ~896 Hz, BIOS BEL tone
    out 0x42, al
    mov al, ah
    out 0x42, al

    ; Gate the counter into the speaker and enable the speaker driver
    in  al, 0x61
    mov bl, al               ; original port-0x61 state
    or  al, 0x03
    out 0x61, al

    ; Busy-wait ~3 ticks (~156 ms) on the BDA tick counter at 0040:006C
    mov cx, BDATASEG
    mov es, cx
    mov ax, [es:0x6C]
    mov dx, [es:0x6E]
    add ax, 3
    adc dx, 0

  int10_beep_wait:
    mov cx, [es:0x6E]        ; current high word of tick count
    mov si, [es:0x6C]        ; current low word of tick count
    cmp cx, dx
    ja  int10_beep_done
    jb  int10_beep_wait
    cmp si, ax
    jb  int10_beep_wait

  int10_beep_done:
    mov al, bl               ; restore port-0x61 state -> speaker off
    out 0x61, al

    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret

int10_set_bg_palette:
	push	ds
	push	ax
	push	bx
	push	dx

	mov	ax, BDATASEG
	mov	ds, ax

	mov	dx, 0x3D9		; CGA Color Select Register port

	cmp	bh, 0x00
	je	.set_background
	cmp	bh, 0x01
	je	.set_palette
	jmp	.done			; Ignore if BH is anything else

.set_background:
	; BH = 0: Set background/border color
	; BL = Color value (0-31)
	mov	al, [0x66]		; Retrieve current palette state from BDA
	and	al, 0xE0		; Keep the upper 3 bits (which hold palette selection)
	and	bl, 0x1F		; Mask BL to ensure it only has the 5-bit color value
	or	al, bl			; Merge the new background color
	mov	[0x66], al		; Save the updated state back to BDA
	out	dx, al			; Push to CGA hardware
	jmp	.done

.set_palette:
	; BH = 1: Set color palette
	; BL = Palette ID (0 = Green/Red/Brown, 1 = Cyan/Magenta/White)
	mov	al, [0x66]		; Retrieve current palette state from BDA
	and	al, 0xDF		; Clear bit 5 (the active palette bit)
	cmp	bl, 0
	je	.write_out
	or	al, 0x20		; If BL > 0, set bit 5 for Alternate Palette
.write_out:
	mov	[0x66], al		; Save the updated state back to BDA
	out	dx, al			; Push to CGA hardware

.done:
	pop	dx
	pop	bx
	pop	ax
	pop	ds
	iret

; int10_palette:
; 	cmp al, 0x03       ; Sub-function 03h: Toggle Blink/Intensity
; 	jne int10_palette_done

; 	push dx
; 	push ax

; 	mov dx, 0x3D8      ; CGA Mode Control Register

; 	cmp bl, 0          ; BL=0 means Blink OFF (High Intensity ON)
; 	je .blink_off

; .blink_on:
; 	mov al, 0x29       ; Bit 5 = 1
; 	out dx, al
; 	jmp .finish

; .blink_off:
; 	mov al, 0x09       ; Bit 5 = 0
; 	out dx, al

; .finish:
; 	pop ax
; 	pop dx

; int10_palette_done:
; 	iret

int10_features:

	; Signify we have an active Color Graphics Adapter (CGA)
	cmp	al, 0x00	; AL=00 is the standard "Get Display Combination Code" request
	jne	.unsupported

	mov	al, 0x1a	; AL=1A means the function is supported by this BIOS
	mov	bx, 0x0002	; BL=02 (Active Display is CGA). BH=00 (No alternate display)
	iret

.unsupported:
	iret


; Clear screen

clear_screen:
	push	ax
	push	bx
	push	cx
	push	es
	push	di

	mov	ax, BDATASEG
	mov	es, ax
	mov	byte [es:curpos_x-bios_data], 0
	mov	byte [es:crt_curpos_x-bios_data], 0
	mov	byte [es:curpos_y-bios_data], 0
	mov	byte [es:crt_curpos_y-bios_data], 0

	push	es
	cld
	mov	ax, 0xb800
	mov	es, ax
	mov	di, 0
	mov	al, 0x20
	mov	ah, bh
	mov	cx, 8192
	rep	stosw
	pop	es

	pop	di
	pop	es
	pop	cx
	pop	bx
	pop	ax
	ret
