// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include "pico/stdlib.h"

#include "pico_x86.h"
#include "pico_x86_video.h"
#include "font4x10.h"

extern uint8_t mem[];
extern uint8_t io_ports[];
// extern uint16_t regs16[];
// extern uint16_t reg_ip;

CRTC_State __scratch_y("video") crtc = { };
static uint16_t __scratch_y("video") screen_width;
static uint16_t __scratch_y("video") screen_height;
static uint8_t* __scratch_y("video") vram = NULL;

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

        crtc.mcr_display_reset |= ((port_value & 0x80) != 0); // bit 7 custom reset
        crtc.mcr_blink_enabled = (port_value & 0x20) != 0; // bit 5
        crtc.mcr_hires_graphics_mode = (port_value & 0x10) != 0; // 4
        crtc.mcr_video_output = (port_value & 0x8) != 0; // 3
        crtc.mcr_color_burst_disabled = (port_value & 0x4) != 0; // 2
        crtc.mcr_graphics_mode = (port_value & 0x2) != 0; // 1
        crtc.mcr_hires = (port_value & 0x1) != 0; // 0

        crtc.mcr_register = port_value & ~0x80;
        // printf("BDA: 0x449 %x, 0x465 %x \n", mem[0x449], mem[0x465]);
        // printf("mcr_register status: %x enabled by software at CS:IP %04X:%04X\n",
        //     crtc.mcr_register, regs16[REG_CS], reg_ip);
    } else if (port == 0x3D9) {
        crtc.color_select_register = io_ports[0x3D9];
    }
}

static void __time_critical_func(render_text_mode)(video_put_color_cb put_color)
{
    int num_cols = crtc.dr_horiz_displayed;
    if (num_cols == 0) {
        num_cols = crtc.mcr_hires ? 80 : 40;
    }

    int num_rows = crtc.dr_vert_displayed;
    if (num_rows == 0) {
        num_rows = 25;
    }

    int pixel_scale = crtc.mcr_hires ? 1 : 2;

    const int VERTICAL_MARGIN = 35;

    uint64_t cga_now = time_us_64();
    bool cursor_blink = (cga_now % 533333) > 266666;
    bool text_blink_hide = (cga_now % 1066666) >= 533333;

    uint16_t bg_color = textmode_palette[crtc.color_select_register & 0x0F];

    // Top margin
    for (int i = 0; i < (screen_width * VERTICAL_MARGIN); i++) {
        put_color(bg_color);
    }

    for (int text_row = 0; text_row < num_rows; text_row++) {
        for (int font_y = 0; font_y < 10; font_y++) {
            for (int text_col = 0; text_col < num_cols; text_col++) {

                uint16_t cell_index = text_row * num_cols + text_col;
                uint16_t abs_index = crtc.start_address_offset + cell_index;
                uint32_t mem_offset = abs_index * 2;

                uint8_t character = vram[mem_offset];
                uint8_t color_attr = vram[mem_offset + 1];

                if (crtc.cursor_visible && cursor_blink && (abs_index == crtc.cursor_offset)) {
                    bool in_cursor_range = false;
                    if (crtc.dr_cursor_start <= crtc.dr_cursor_end) {
                        in_cursor_range
                            = (font_y >= crtc.dr_cursor_start && font_y <= crtc.dr_cursor_end);
                    } else {
                        in_cursor_range = ((font_y >= crtc.dr_cursor_start && font_y <= 7)
                            || font_y <= crtc.dr_cursor_end);
                    }
                    if (in_cursor_range) {
                        color_attr = ((color_attr & 0x0F) << 4) | ((color_attr & 0xF0) >> 4);
                    }
                }

                uint8_t fg_idx = color_attr & 0x0F;
                uint8_t bg_idx;
                bool is_blinking = false;

                // blinking or color intensity
                if (crtc.mcr_blink_enabled) {
                    bg_idx = (color_attr >> 4) & 0x07;
                    is_blinking = (color_attr & 0x80) != 0;
                } else {
                    bg_idx = color_attr >> 4;
                    is_blinking = false;
                }

                bool hide_foreground = is_blinking && text_blink_hide;

                // 1-pixel shift for text, ignore box-drawing
                uint8_t glyph_pixels = 0;
                if (character >= 0xB0 && character <= 0xDF) {
                    glyph_pixels = font_4x10[(character * 10) + font_y];
                } else {
                    if (font_y >= 1) {
                        glyph_pixels = font_4x10[(character * 10) + (font_y - 1)];
                    }
                }

                for (int bit = 0; bit < 4; bit++) {
                    uint8_t is_foreground = (glyph_pixels >> (3 - bit)) & 1;
                    uint8_t palette_idx = (is_foreground && !hide_foreground) ? fg_idx : bg_idx;
                    uint16_t color = textmode_palette[palette_idx];

                    for (int s = 0; s < pixel_scale; s++) {
                        put_color(color);
                    }
                }
            }
        }
    }

    // Bottom margin
    for (int i = 0; i < (screen_width * VERTICAL_MARGIN); i++) {
        put_color(bg_color);
    }
}

static void __time_critical_func(render_cga_graphics)(video_put_color_cb put_color)
{

    bool is_1bpp_mode = crtc.mcr_hires_graphics_mode;
    const int OUTPUT_ROWS = 200;
    const int VERTICAL_MARGIN = (screen_height - OUTPUT_ROWS) / 2;

    // --- DECODE PALETTE & BACKGROUND SETTINGS ---
    // Background color (low 4 bits mapped to standard text palette)
    uint16_t bg_color = textmode_palette[crtc.color_select_register & 0x0F];

    uint8_t palette_num;

    // Check if the Color Burst is disabled (MCR Bit 2) to trigger the unofficial Mode 5 palette
    if (crtc.mcr_color_burst_disabled) {
        palette_num = 2; // Palette 2: Cyan, Red, White
    } else {
        // Standard Mode 4: Bit 5 determines Palette 0 vs Palette 1
        palette_num = (crtc.color_select_register & 0x20) ? 1 : 0;
    }

    // Intensity selection (Bit 4 determines Low vs High intensity)
    uint8_t intensity = (crtc.color_select_register & 0x10) ? 1 : 0;

    // Base palette index calculation:
    // intensity * 12 (to skip to high intensity half) + palette_num * 4
    uint8_t base_pal = (intensity * 12) + (palette_num * 4);

    // Mode 6 uses the color_select for the foreground and Black for the background
    uint16_t m6_fg_color = textmode_palette[crtc.color_select_register & 0x0F];
    uint16_t m6_bg_color = textmode_palette[0]; // Black

    // Top margin
    for (int i = 0; i < (screen_width * VERTICAL_MARGIN); i++) {
        put_color(bg_color);
    }

    for (int out_y = 0; out_y < OUTPUT_ROWS; out_y++) {
        int y = out_y;
        uint32_t bank_offset = (y & 1) ? 0x2000 : 0x0000;
        uint32_t row_offset = (y >> 1) * 80;

        for (int x_byte = 0; x_byte < 80; x_byte++) {
            // CRTC start address for hardware panning
            uint32_t vram_offset
                = (crtc.start_address_offset * 2) + bank_offset + row_offset + x_byte;
            uint8_t pixel_data = vram[vram_offset & 0x3FFF]; // Mask to 16KB

            if (is_1bpp_mode) {
                for (int p = 0; p < 4; p++) {
                    uint8_t hi = (pixel_data >> (7 - p * 2)) & 1;
                    uint8_t lo = (pixel_data >> (6 - p * 2)) & 1;
                    uint8_t bit = hi | lo;
                    uint16_t color = bit ? m6_fg_color : m6_bg_color;
                    put_color(color);
                }
                continue;
            }

            uint8_t p0_val = (pixel_data >> 6) & 0x03;
            uint8_t p1_val = (pixel_data >> 4) & 0x03;
            uint8_t p2_val = (pixel_data >> 2) & 0x03;
            uint8_t p3_val = (pixel_data >> 0) & 0x03;

            // Pixel 0 draws the background color
            uint16_t c0 = p0_val ? cga_palette[base_pal + p0_val] : bg_color;
            uint16_t c1 = p1_val ? cga_palette[base_pal + p1_val] : bg_color;
            uint16_t c2 = p2_val ? cga_palette[base_pal + p2_val] : bg_color;
            uint16_t c3 = p3_val ? cga_palette[base_pal + p3_val] : bg_color;

            put_color(c0);
            put_color(c1);
            put_color(c2);
            put_color(c3);
        }
    }

    // Bottom margin
    for (int i = 0; i < (screen_width * VERTICAL_MARGIN); i++) {
        put_color(bg_color);
    }
}

void __always_inline video_display_reset(video_display_reset_cb reset_cb)
{
    if (crtc.mcr_display_reset) {
        reset_cb();
        crtc.mcr_display_reset = false;
    }
}

void __always_inline video_cga_render(video_put_color_cb put_color)
{
    if (!crtc.mcr_video_output || crtc.mcr_display_reset) {
        // maybe draw black screen?
        return;
    }
    if (crtc.mcr_graphics_mode || crtc.mcr_hires_graphics_mode) {
        render_cga_graphics(put_color);
    } else {
        render_text_mode(put_color);
    }
}

void video_cga_set_resolution(uint16_t width, uint16_t height)
{
    screen_width = width;
    screen_height = height;
    vram = &mem[MAP_ADDR(CGA_VRAM_ADDR)];
}
