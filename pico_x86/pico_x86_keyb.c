// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * INT 9h keyboard decode
 */

#include "pico_x86_keyb.h"
#include <pico/platform/sections.h>
#include <stdint.h>
#include <sys/cdefs.h>

extern uint8_t *mem;

#define BDA_SEG_BASE 0x400
#define BDA(ofs) (BDA_SEG_BASE + (ofs))

#define BDA_KB_FLAGS_OFS 0x17
#define BDA_KB_HEAD_OFS 0x1A
#define BDA_KB_TAIL_OFS 0x1C
#define BDA_KB_BUF_START_OFS 0x1E
#define BDA_KB_BUF_END_OFS 0x3E

#define MOD_RSHIFT 0x01
#define MOD_LSHIFT 0x02
#define MOD_CTRL 0x04
#define MOD_ALT 0x08
#define MOD_CAPSLOCK 0x40

// XT scancode -> ASCII, indexed by scancode with the break bit stripped
static const uint8_t scan_to_ascii_unshifted[128] = {
    0,   27,  '1',  '2', '3', '4',  '5', '6', '7', '8', '9', '0', '-', '=', 8,   9,   'q', 'w', 'e',
    'r', 't', 'y',  'u', 'i', 'o',  'p', '[', ']', 13,  0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k',
    'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,
    ' ', 0,   0,    0,   0,   0,    0,   0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4',
    '5', '6', '+',  '1', '2', '3',  '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,    0,   0,   0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,    0,   0,   0,    0,   0,   0,   0,   0,   0,   0,   0};

static const uint8_t scan_to_ascii_shifted[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8,   9,   'Q', 'W', 'E',
    'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13,  0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K',
    'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,
    ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4',
    '5', '6', '+', '1', '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0};

static uint16_t __always_inline bda_read16(uint32_t addr) {
    return mem[addr] | ((uint16_t)mem[addr + 1] << 8);
}

static void __always_inline bda_write16(uint32_t addr, uint16_t value) {
    mem[addr] = (uint8_t)value;
    mem[addr + 1] = (uint8_t)(value >> 8);
}

// Insert into the BIOS keyboard ring buffer
static void __always_inline kb_buffer_insert(uint8_t al, uint8_t ah) {
    uint16_t tail = bda_read16(BDA(BDA_KB_TAIL_OFS));
    uint16_t head = bda_read16(BDA(BDA_KB_HEAD_OFS));

    uint16_t new_tail = tail + 2;
    if (new_tail == BDA_KB_BUF_END_OFS) {
        new_tail = BDA_KB_BUF_START_OFS;
    }

    if (new_tail == head) {
        return; // buffer full, drop
    }

    bda_write16(BDA(tail), (uint16_t)al | ((uint16_t)ah << 8));
    bda_write16(BDA(BDA_KB_TAIL_OFS), new_tail);
}

// Shift/Ctrl/Alt press+release and Caps Lock toggle
static int __always_inline kb_update_modifiers(uint8_t code, uint8_t released) {
    uint8_t bit;

    switch (code) {
    case 0x2A:
        bit = MOD_LSHIFT;
        break;
    case 0x36:
        bit = MOD_RSHIFT;
        break;
    case 0x1D:
        bit = MOD_CTRL;
        break;
    case 0x38:
        bit = MOD_ALT;
        break;
    case 0x3A:
        if (!released) {
            mem[BDA(BDA_KB_FLAGS_OFS)] ^= MOD_CAPSLOCK;
        }
        return 1;
    default:
        return 0;
    }

    if (released) {
        mem[BDA(BDA_KB_FLAGS_OFS)] &= (uint8_t)~bit;
    } else {
        mem[BDA(BDA_KB_FLAGS_OFS)] |= bit;
    }

    return 1;
}

// F-keys, arrows, nav keys, and numpad -/+ : scancodes 0x3B and above
static void kb_decode_special(uint8_t code, uint8_t flags) {
    if (code >= 0x3B && code <= 0x44) {
        uint8_t ah = code;
        uint8_t both_shifts = (flags & (MOD_LSHIFT | MOD_RSHIFT)) == (MOD_LSHIFT | MOD_RSHIFT);

        if (flags & MOD_ALT) {
            ah += 0x2D;
        } else if (flags & MOD_CTRL) {
            ah += 0x23;
        } else if (both_shifts) {
            ah += 0x19; // Both physical shifts held -> Shift+Fn; a single shift just reaches the
        } // F-row

        kb_buffer_insert(0x00, ah);
        return;
    }

    if (code == 0x4A) {
        kb_buffer_insert('-', code);
        return;
    }
    if (code == 0x4E) {
        kb_buffer_insert('+', code);
        return;
    }

    uint8_t ah = code;
    if (flags & MOD_CTRL) {
        switch (code) {
        case 0x4B:
            ah = 0x73; // Ctrl+Left
            break;
        case 0x4D:
            ah = 0x74; // Ctrl+Right
            break;
        case 0x47:
            ah = 0x77; // Ctrl+Home
            break;
        case 0x4F:
            ah = 0x75; // Ctrl+End
            break;
        case 0x49:
            ah = 0x84; // Ctrl+PgUp
            break;
        case 0x51:
            ah = 0x76; // Ctrl+PgDn
            break;
        default:
            break;
        }
    }
    kb_buffer_insert(0x00, ah);
}

// Scancodes below 0x3B: translate to ASCII using the current modifier state
static void kb_decode_normal(uint8_t code, uint8_t flags) {
    if (flags & MOD_ALT) {
        kb_buffer_insert(0x00, code);
        return;
    }

    if (flags & MOD_CTRL) {
        uint8_t al = scan_to_ascii_unshifted[code];
        if (al >= 'a' && al <= 'z') {
            al &= 0x1F;
        }
        kb_buffer_insert(al, code);
        return;
    }

    uint8_t al = (flags & (MOD_LSHIFT | MOD_RSHIFT)) ? scan_to_ascii_shifted[code]
                                                     : scan_to_ascii_unshifted[code];

    if ((flags & MOD_CAPSLOCK) && al >= 'A' && al <= 'z' && (al <= 'Z' || al >= 'a')) {
        al ^= 0x20;
    }

    kb_buffer_insert(al, code);
}

void __time_critical_func(pico_x86_keyb_process_scancode)(uint8_t raw_code) {
    uint8_t released = raw_code & 0x80;
    uint8_t code = raw_code & 0x7F;

    if (kb_update_modifiers(code, released)) {
        return;
    }

    if (released) {
        return;
    }

    if (code >= 0x3B) {
        kb_decode_special(code, mem[BDA(BDA_KB_FLAGS_OFS)]);
    } else {
        kb_decode_normal(code, mem[BDA(BDA_KB_FLAGS_OFS)]);
    }
}
