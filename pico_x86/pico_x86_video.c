// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pico_x86_video.h"
#include "font4x10.h"
#include "font8x8.h"
#include "pico_x86.h"
#include "pico_x86_video.h"
#include <hardware/timer.h>
#include <stdint.h>
#include <sys/cdefs.h>

extern uint8_t *mem;
extern uint8_t io_ports[];
// extern uint16_t regs16[];
// extern uint16_t reg_ip;

static CRTC_State __scratch_y("video") crtc = {0};
static Video_Config __scratch_y("video") *video_config = nullptr;
static uint8_t *__scratch_y("video") vram = nullptr;

static uint16_t __scratch_y("video") ma_row_start = 0; // Current Memory Address (VRAM pointer)
static uint8_t __scratch_y("video") ra = 0; // Current Row Address (Scanline within character)

static uint16_t __scratch_y("video") current_crtc_scanline = 0;
static uint32_t __scratch_y("video") scanline_start_time = 0;

static uint16_t __scratch_y("video") active_display_start = 60;
static uint16_t __scratch_y("video") active_display_end = 260;

static TextGeometry __scratch_y("video") frame_text_geo = {0};

static void __time_critical_func(update_text_geometry)() {
    frame_text_geo.co80_mode = crtc.mcr_hires;
    frame_text_geo.font_height = (int)frame_text_geo.co80_mode ? 10 : 8;

    frame_text_geo.num_cols = crtc.dr_horiz_displayed;
    if (frame_text_geo.num_cols == 0) {
        frame_text_geo.num_cols = frame_text_geo.co80_mode ? 80 : 40;
    }

    frame_text_geo.font_width = (video_config && frame_text_geo.num_cols)
                                    ? (video_config->screen_width / frame_text_geo.num_cols)
                                    : (frame_text_geo.co80_mode ? 4 : 8);

    if (frame_text_geo.font_width < 1) {
        frame_text_geo.font_width = 1;
    }

    frame_text_geo.num_rows = crtc.dr_vert_displayed;
    if (frame_text_geo.num_rows == 0) {
        frame_text_geo.num_rows = 25;
    }

    frame_text_geo.max_scanline = crtc.dr_max_scan_line;
    if (frame_text_geo.co80_mode && frame_text_geo.max_scanline == 7) {
        frame_text_geo.max_scanline = 9;
    }

    if (frame_text_geo.max_scanline == 0 && frame_text_geo.num_rows == 25) {
        frame_text_geo.max_scanline = frame_text_geo.font_height - 1;
    }
}

static void __always_inline update_crtc_frame_timing() {
    if (crtc.dr_vert_total == 0) {
        return;
    }

    uint8_t char_height = frame_text_geo.max_scanline + 1;

    uint16_t vert_total_rows = crtc.dr_vert_total + 1;
    uint32_t logical_total = ((uint32_t)vert_total_rows * char_height) + crtc.dr_vert_total_adjust;
    if (logical_total < 1) {
        logical_total = 1;
    }
    if (logical_total > 0xFFFF) {
        logical_total = 0xFFFF;
    }
}

static uint8_t __always_inline cga_retrace_bits() {
    uint32_t line_pos = time_us_32() - scanline_start_time;

    uint8_t vblank = (current_crtc_scanline < active_display_start ||
                      current_crtc_scanline >= active_display_end)
                         ? 1
                         : 0;

    // sync vertical pos
    uint16_t char_height = frame_text_geo.max_scanline + 1;
    uint16_t vsync_start = active_display_start + (crtc.dr_vert_sync_pos * char_height);

    // apply interlace offset
    if (crtc.dr_interlace_mode != 0) {
        vsync_start += 1;
    }

    uint8_t vsync =
        (current_crtc_scanline >= vsync_start && current_crtc_scanline < (vsync_start + 16)) ? 1
                                                                                             : 0;

    // sync horizontal timings
    uint16_t h_total = crtc.dr_horiz_total;
    if (h_total == 0) {
        h_total = 1;
    }

    // calc hsync bounds
    uint32_t hsync_start_time = (crtc.dr_horiz_sync_pos * 64) / h_total;
    uint32_t hsync_end_time = hsync_start_time + ((crtc.dr_horiz_sync_width * 64) / h_total);

    uint8_t hblank = (line_pos >= hsync_start_time && line_pos <= hsync_end_time) ? 1 : 0;

    return (uint8_t)(0xF0 | (vsync << 3) | (vblank | hblank));
}

void __time_critical_func(video_cga_port_in)(uint32_t port) {
    if (port == 0x3DA) {
        io_ports[0x3DA] = cga_retrace_bits();
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

void __time_critical_func(video_cga_port_out)(uint32_t port) {
    if (port == 0x3D4) {
        crtc.address_register = io_ports[0x3D4];
    } else if (port == 0x3D5) {
        switch (crtc.address_register) {
        case 0x00: // Horizontal Total
            crtc.dr_horiz_total = io_ports[0x3D5];
            break;

        case 0x01: // Horizontal Displayed
            crtc.dr_horiz_displayed = io_ports[0x3D5];
            break;

        case 0x02: // Horizontal Sync Position
            crtc.dr_horiz_sync_pos = io_ports[0x3D5];
            break;

        case 0x03: // Horizontal Sync Pulse Width
            crtc.dr_horiz_sync_width = io_ports[0x3D5];
            break;

        case 0x04: // Vertical Total
            crtc.dr_vert_total = io_ports[0x3D5];
            break;

        case 0x05: // Vertical Total Adjust
            crtc.dr_vert_total_adjust = io_ports[0x3D5];
            break;

        case 0x06: // Vertical Displayed
            crtc.dr_vert_displayed = io_ports[0x3D5];
            break;

        case 0x07: // Vertical Sync Position
            crtc.dr_vert_sync_pos = io_ports[0x3D5];
            break;

        case 0x08: // Interlace Mode
            crtc.dr_interlace_mode = io_ports[0x3D5];
            break;

        case 0x09:                                          // Maximum Scan Line
            crtc.dr_max_scan_line = io_ports[0x3D5] & 0x1F; // 5-bit field on the 6845
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
            break;

        case 0x0D: // Start Address Low
            crtc.dr_start_addr_low = io_ports[0x3D5];
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
        default:
            break;
        }
    }

    // Writes to CGA Mode Control Register
    else if (port == 0x3D8) {
        uint8_t port_value = io_ports[0x3D8];

        crtc.mcr_display_reset |= ((port_value & 0x80) != 0); // bit 7 trigger display reset
        video_config->state |= ((port_value & 0x40) != 0);    // bit 6 trigger video config update
        crtc.mcr_blink_enabled = (port_value & 0x20) != 0;    // bit 5
        crtc.mcr_hires_graphics_mode = (port_value & 0x10) != 0; // 4
        crtc.mcr_video_output = (port_value & 0x8) != 0;         // 3
        crtc.mcr_color_burst_disabled = (port_value & 0x4) != 0; // 2
        crtc.mcr_graphics_mode = (port_value & 0x2) != 0;        // 1
        crtc.mcr_hires = (port_value & 0x1) != 0;                // 0

        crtc.mcr_register = port_value & ~0x80;
        // printf("BDA: 0x449 %x, 0x465 %x \n", mem[0x449], mem[0x465]);
        // printf("mcr_register status: %x enabled by software at CS:IP %04X:%04X\n",
        //     crtc.mcr_register, (CPU.CS), reg_ip);
    } else if (port == 0x3D9) {
        crtc.color_select_register = io_ports[0x3D9];
    }
}

static void __always_inline video_display_reset() {
    if (unlikely(crtc.mcr_display_reset)) {
        crtc.mcr_display_reset = false;
        // bugs, disable for now
        // video_config->display_reset_callback();
    }
}

static void __time_critical_func(render_text_scanline)(uint32_t y) {
    bool cursor_blink = (scanline_start_time % 533333) > 266666;
    bool text_blink_hide = (scanline_start_time % 1066666) >= 533333;

    uint16_t bg_color_border = textmode_palette[crtc.color_select_register & 0x0F];

    uint32_t font_height = frame_text_geo.font_height;
    uint32_t font_width = frame_text_geo.font_width;

    uint32_t source_bits = frame_text_geo.co80_mode ? 4 : 8;

    uint32_t total_rows = frame_text_geo.max_scanline + 1;
    if (total_rows < 1) {
        total_rows = 1;
    }

    uint32_t drawn_pixels = frame_text_geo.num_cols * font_width;
    uint32_t left_pad = (video_config->screen_width - drawn_pixels) / 2;
    if (left_pad < 0) {
        left_pad = 0;
    }

    for (uint32_t padding = 0; padding < left_pad; padding++) {
        video_config->display_put_pixel_callback(bg_color_border);
    }

    uint32_t out_x = 0;

    for (uint32_t x = 0; x < frame_text_geo.num_cols; x++) {
        uint32_t mem_offset = ((ma_row_start + x) & 0x1FFF) * 2;

        uint8_t character = vram[mem_offset];
        uint8_t color_attr = vram[mem_offset + 1];

        if (crtc.cursor_visible && cursor_blink &&
            (((ma_row_start + x) & 0x1FFF) == crtc.cursor_offset)) {
            bool in_cursor_range = false;
            if (crtc.dr_cursor_start <= crtc.dr_cursor_end) {
                in_cursor_range = (ra >= crtc.dr_cursor_start + (8 / source_bits) &&
                                   ra <= crtc.dr_cursor_end + (8 / source_bits));
            } else {
                in_cursor_range = ((ra >= crtc.dr_cursor_start && ra <= (total_rows - 1)) ||
                                   ra <= crtc.dr_cursor_end);
            }
            if (in_cursor_range) {
                color_attr = ((color_attr & 0x0F) << 4) | ((color_attr & 0xF0) >> 4);
            }
        }

        uint8_t fg_idx = color_attr & 0x0F;
        uint8_t bg_idx;
        bool is_blinking = false;

        if (crtc.mcr_blink_enabled) {
            bg_idx = (color_attr >> 4) & 0x07;
            is_blinking = (color_attr & 0x80) != 0;
        } else {
            bg_idx = color_attr >> 4;
        }

        bool hide_foreground = is_blinking && text_blink_hide;
        uint8_t glyph_pixels = 0;

        uint32_t scaled_row = (ra * font_height) / total_rows;
        if (scaled_row >= font_height) {
            scaled_row = font_height - 1;
        }

        if (likely(frame_text_geo.co80_mode)) {
            if (character >= 0xB0 && character <= 0xDF) {
                glyph_pixels = font4x10[(character * 10) + scaled_row];
            } else {
                if (scaled_row >= 1) {
                    glyph_pixels = font4x10[(character * 10) + (scaled_row - 1)];
                }
            }
        } else {
            glyph_pixels = font8x8[(character * 8) + scaled_row];
        }

        for (uint32_t bit = 0; bit < font_width; bit++) {
            uint32_t src_bit = (bit * source_bits) / font_width;
            if (src_bit >= source_bits) {
                src_bit = source_bits - 1;
            }
            uint8_t msb = source_bits - 1;

            uint8_t is_foreground = (glyph_pixels >> (msb - src_bit)) & 1;
            uint8_t palette_idx = (is_foreground && !hide_foreground) ? fg_idx : bg_idx;

            video_config->display_put_pixel_callback(textmode_palette[palette_idx]);
            out_x++;
        }
    }

    for (uint32_t padding = left_pad + drawn_pixels; padding < video_config->screen_width;
         padding++) {
        video_config->display_put_pixel_callback(bg_color_border);
    }
}

static void __time_critical_func(render_cga_graphics_scanline)(uint32_t y) {
    uint16_t bg_color = textmode_palette[crtc.color_select_register & 0x0F];

    uint8_t palette_num =
        crtc.mcr_color_burst_disabled ? 2 : ((crtc.color_select_register & 0x20) ? 1 : 0);
    uint8_t intensity = (crtc.color_select_register & 0x10) ? 1 : 0;
    uint8_t base_pal = (intensity * 12) + (palette_num * 4);

    uint16_t m6_fg_color = textmode_palette[crtc.color_select_register & 0x0F];
    uint16_t m6_bg_color = textmode_palette[0];

    uint8_t hd = crtc.dr_horiz_displayed;
    if (hd == 0) {
        hd = 40;
    }

    uint32_t bytes_per_row = hd * 2;
    bool needs_downscale =
        crtc.mcr_hires_graphics_mode && ((bytes_per_row * 8) > video_config->screen_width);

    uint32_t bank_offset = (ra & 1) ? 0x2000 : 0x0000;
    uint32_t base_vram_offset = ma_row_start * 2;

    uint32_t physical_pixels = 0;

    for (int32_t x_byte = 0; x_byte < bytes_per_row; x_byte++) {
        uint32_t vram_offset = base_vram_offset + bank_offset + x_byte;
        uint8_t pixel_data = vram[vram_offset & 0x3FFF];

        if (crtc.mcr_hires_graphics_mode) {
            if (needs_downscale) {
                for (int32_t bit = 7; bit >= 1; bit -= 2) {
                    uint8_t p1 = (pixel_data >> bit) & 1;
                    uint8_t p2 = (pixel_data >> (bit - 1)) & 1;
                    uint16_t color = (p1 | p2) ? m6_fg_color : m6_bg_color;

                    if (physical_pixels < video_config->screen_width) {
                        video_config->display_put_pixel_callback(color);
                        physical_pixels++;
                    }
                }
            } else {
                for (int32_t bit = 7; bit >= 0; bit--) {
                    uint8_t pixel_bit = (pixel_data >> bit) & 1;
                    uint16_t color = pixel_bit ? m6_fg_color : m6_bg_color;

                    if (physical_pixels < video_config->screen_width) {
                        video_config->display_put_pixel_callback(color);
                        physical_pixels++;
                    }
                }
            }
        } else {
            uint8_t p0_val = (pixel_data >> 6) & 0x03;
            uint8_t p1_val = (pixel_data >> 4) & 0x03;
            uint8_t p2_val = (pixel_data >> 2) & 0x03;
            uint8_t p3_val = (pixel_data >> 0) & 0x03;

            if ((physical_pixels + 4) <= video_config->screen_width) {
                video_config->display_put_pixel_callback(p0_val ? cga_palette[base_pal + p0_val]
                                                                : bg_color);
                video_config->display_put_pixel_callback(p1_val ? cga_palette[base_pal + p1_val]
                                                                : bg_color);
                video_config->display_put_pixel_callback(p2_val ? cga_palette[base_pal + p2_val]
                                                                : bg_color);
                video_config->display_put_pixel_callback(p3_val ? cga_palette[base_pal + p3_val]
                                                                : bg_color);
                physical_pixels += 4;
            }
        }
    }

    uint16_t pad_color = crtc.mcr_hires_graphics_mode ? m6_bg_color : bg_color;
    for (int32_t padding = physical_pixels; padding < video_config->screen_width; padding++) {
        video_config->display_put_pixel_callback(pad_color);
    }
}

static void __time_critical_func(pico_x86_video_cga_render_scanline)() {
    scanline_start_time = time_us_32();

    static uint32_t cached_scaled_rows = 1;
    static uint32_t cached_text_output_rows = CGA_TEXT_OUTPUT_ROWS;

    uint16_t bg_color_border = crtc.mcr_hires_graphics_mode
                                   ? textmode_palette[0]
                                   : textmode_palette[crtc.color_select_register & 0x0F];

    bool is_text_mode = !crtc.mcr_graphics_mode && !crtc.mcr_hires_graphics_mode;

    if (current_crtc_scanline == 0) {
        video_display_reset();
        update_text_geometry();
        update_crtc_frame_timing();

        int text_output_rows = CGA_TEXT_OUTPUT_ROWS;
        if (is_text_mode) {
            text_output_rows = frame_text_geo.num_rows * (frame_text_geo.max_scanline + 1);
        }
        cached_text_output_rows = text_output_rows;

        uint32_t scaled_rows = (uint32_t)text_output_rows;

        if (scaled_rows > (uint32_t)video_config->screen_height) {
            scaled_rows = video_config->screen_height;
        }

        cached_scaled_rows = scaled_rows;

        active_display_start = (video_config->screen_height - scaled_rows) / 2;
        active_display_end = active_display_start + scaled_rows;
    }

    if (current_crtc_scanline < active_display_start ||
        current_crtc_scanline >= active_display_end) {
        for (uint32_t i = 0; i < video_config->screen_width; i++) {
            video_config->display_put_pixel_callback(bg_color_border);
        }
        return;
    }

    uint32_t active_y = current_crtc_scanline - active_display_start;
    uint32_t logical_y = (active_y * cached_text_output_rows) / cached_scaled_rows;

    uint8_t cols = 0;
    uint8_t max_scanline = 0;

    if (is_text_mode) {
        cols = frame_text_geo.num_cols;
        max_scanline = frame_text_geo.max_scanline;
    } else {
        cols = crtc.dr_horiz_displayed;
        if (cols == 0) {
            cols = 40;
        }

        max_scanline = crtc.dr_max_scan_line;
        if (max_scanline == 0) {
            max_scanline = 1;
        }
    }

    uint16_t row_index = logical_y / (max_scanline + 1);
    ra = logical_y % (max_scanline + 1);

    uint16_t base_addr = (crtc.dr_start_addr_high << 8) | crtc.dr_start_addr_low;
    ma_row_start = base_addr + (row_index * cols);

    if (likely(crtc.mcr_video_output)) {
        if (likely(is_text_mode)) {
            render_text_scanline(active_y);
        } else {
            render_cga_graphics_scanline(active_y);
        }
    } else {
        // for (uint32_t i = 0; i < video_config->screen_width; i++) {
        //     video_config->display_put_pixel_callback(bg_color_border);
        // }
    }
}

void __time_critical_func(pico_x86_video_render)() {
    current_crtc_scanline = 0;
    while (1) {
        if (current_crtc_scanline == 0) {
            if (unlikely(video_config->state == VID_CFG_NEED_UPDATE)) {
                video_config->display_update_video_config_callback(video_config);
                video_config->state = VID_CFG_UPDATED;
            }
            video_config->display_begin_frame_callback();
        }
        pico_x86_video_cga_render_scanline();
        current_crtc_scanline++;
        if (current_crtc_scanline >= video_config->screen_height) {
            video_config->display_end_frame_callback();
            current_crtc_scanline = 0;
            if (unlikely(video_config->state == VID_CFG_UPDATED)) {
                video_config->display_update_video_config_callback(video_config);
                video_config->state = VID_CFG_NO_UPDATE;
            }
        }
        tight_loop_contents();
    }
}

void pico_x86_video_display_init() { vram = &mem[MAP_ADDR(CGA_VRAM_ADDR)]; }
void pico_x86_video_set_config(Video_Config *video_cfg) { video_config = video_cfg; }
