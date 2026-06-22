// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "pico_x86.h"
#include "pico_x86_video.h"

extern uint8_t mem[];
extern uint8_t io_ports[];
extern uint16_t regs16[];
extern uint16_t reg_ip;

CRTC_State crtc = { };

void __time_critical_func(video_cga_port_in)(uint32_t port)
{
    if (port == 0x3DA) {
        io_ports[0x3DA] ^= 9;
    } else if (port == 0x3D5) {
        switch (crtc.address_register) {
        case 0x0E: // Cursor Location High
            io_ports[0x3D5] = crtc.dr_cursor_loc_high;
            break;
        case 0x0F: // Cursor Location Low
            io_ports[0x3D5] = crtc.dr_cursor_loc_low;
            break;
        default:
            // Unreadable registers return 0 on CGA hardware
            io_ports[0x3D5] = 0;
            break;
        }
    }
}

void __time_critical_func(video_cga_port_out)(uint32_t port)
{
    // printf("BDA: 0x449 %x, 0x465 %x \n", mem[0x449], mem[0x465]);

    if (port == 0x3D4) {
        crtc.address_register = io_ports[0x3D4];
    } else if (port == 0x3D5) {
        switch (crtc.address_register) {
        case 0x01: // Horizontal Displayed
            crtc.dr_horiz_displayed = io_ports[0x3D5];
            break;

        case 0x06: // Vertical Displayed
            crtc.dr_vert_displayed = io_ports[0x3D5];
            break;

        case 0x0A: // Cursor Start Register
            // bit 7 don't care, use it for wait cursor animation?
            // 6,5 cursor blink (00 = normal, 01 = invisible, 10 = erratic, 11 = slow)
            // Bit 6 Todo: handle erratic, and slow
            crtc.cursor_visible = !(io_ports[0x3D5] & 0x20);
            // Bits 0-4 hold the actual scanline
            crtc.dr_cursor_start = io_ports[0x3D5] & 0x1F;
            break;

        case 0x0B: // Cursor End Register
            // Bits 0-4 end scanline
            crtc.dr_cursor_end = io_ports[0x3D5] & 0x1F;
            break;

        case 0x0C: // Start Address High
            crtc.dr_start_addr_high = io_ports[0x3D5];
            // render helper
            crtc.start_address_offset = (crtc.dr_start_addr_high << 8) | crtc.dr_start_addr_low;
            break;

        case 0x0D: // Start Address Low
            crtc.dr_start_addr_low = io_ports[0x3D5];
            // render helper
            crtc.start_address_offset = (crtc.dr_start_addr_high << 8) | crtc.dr_start_addr_low;
            break;

        case 0x0E: // Cursor Location High
            crtc.dr_cursor_loc_high = io_ports[0x3D5];
            // render helper
            crtc.cursor_offset = (crtc.dr_cursor_loc_high << 8) | crtc.dr_cursor_loc_low;
            break;

        case 0x0F: // Cursor Location Low
            crtc.dr_cursor_loc_low = io_ports[0x3D5];
            // render helper
            crtc.cursor_offset = (crtc.dr_cursor_loc_high << 8) | crtc.dr_cursor_loc_low;
            break;
        }
    }

    // Writes to CGA Mode Control Register
    // 6, 7 bits unused
    else if (port == 0x3D8) {
        uint8_t port_value = io_ports[0x3D8];
        crtc.mcr_blink_enabled = (port_value & 0x20) != 0; // bit 5
        crtc.mcr_hires_graphics_mode = (port_value & 0x10) != 0; // 4
        crtc.mcr_video_output = (port_value & 0x8) != 0; // 3
        crtc.mcr_color_burst_disabled = (port_value & 0x4) != 0; // 2
        crtc.mcr_graphics_mode = (port_value & 0x2) != 0; // 1
        crtc.mcr_hires = (port_value & 0x1) != 0; // 0

        crtc.mcr_register = port_value;
        // printf("BDA: 0x449 %x, 0x465 %x \n", mem[0x449], mem[0x465]);
        // printf("mcr_register status: %x enabled by software at CS:IP %04X:%04X\n",
        //     crtc.mcr_register, regs16[REG_CS], reg_ip);
    } else if (port == 0x3D9) {
        crtc.color_select_register = io_ports[0x3D9];
    }
}
