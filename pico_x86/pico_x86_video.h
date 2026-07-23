// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "pico/stdlib.h"

#define CGA_VRAM_ADDR 0xb8000

typedef struct __attribute__((packed, aligned(4))) {
    // Hardware Index State
    // ADDR_6845  0040:0063 0x3D4

    /**
     * @brief   Register Definitions and Access Rights
     *
     * The following definitions map the hardware registers to their corresponding
     * hex offsets. Access modifiers (Read/Write, Write Only, Read Only) are
     * documented inline for each register.
     *
     * Register Summary Table:
     * -------------------------------------------------------------------
     * Reg  | Name                          | Read/Write Access
     * -------------------------------------------------------------------
     * 00H  | Horizontal Total              | Write only
     * 01H  | Horizontal Displayed          | Write only
     * 02H  | Horizontal Sync Position      | Write only
     * 03H  | Horizontal Sync Pulse Width   | Write only
     * 04H  | Vertical Total                | Write only
     * 05H  | Vertical Total Adjust         | Write only
     * 06H  | Vertical Displayed            | Write only
     * 07H  | Vertical Sync Position        | Write only
     * 08H  | Interlace Mode                | Write only
     * 09H  | Maximum Scan Line             | Write only
     * 0AH  | Cursor Start                  | Write only
     * 0BH  | Cursor End                    | Write only
     * 0CH  | Start Address High            | Write only
     * 0DH  | Start Address Low             | Write only
     * 0EH  | Cursor Location High          | Read/Write
     * 0FH  | Cursor Location Low           | Read/Write
     * 10H  | Light Pen High                | Read only
     * 11H  | Light Pen Low                 | Read only
     * -------------------------------------------------------------------
     */

    uint8_t address_register; // Stores the last value written to 0x3D4

    // Screen Geometry (Registers 0x01 and 0x06)
    uint8_t dr_horiz_displayed; // Number of columns (characters per row)
    uint8_t dr_vert_displayed; // Number of rows (character rows per screen)

    // Cursor Shape & Visibility (Registers 0x0A and 0x0B)
    uint8_t dr_cursor_start; // Start scanline (usually 0-7 for CGA)
    uint8_t dr_cursor_end; // End scanline (usually 0-7 for CGA)

    // Start Address (Registers 0x0C and 0x0D)
    uint8_t dr_start_addr_high;
    uint8_t dr_start_addr_low;

    // Cursor Location (Registers 0x0E and 0x0F)
    uint8_t dr_cursor_loc_high; // High byte of 1D cursor position
    uint8_t dr_cursor_loc_low; // Low byte of 1D cursor position

    // Renderer Helpers
    uint16_t cursor_offset; // Combined 16-bit linear offset (High << 8 | Low)
    uint16_t start_address_offset; // Combined Start Address offset
    bool cursor_visible; // True if hardware disable bit is NOT set

    // CGA Bios Modes
    //   0 - 40x25 alpha (color burst disabled)         00101100b (2CH)
    //   1 - 40x25 alpha                                00101000b (28H)
    //   2 - 80x25 alpha (color burst disabled)         00101101b (2DH)
    //   3 - 80x25 alpha                                00101001b (29H)
    //   4 - 320x200 graphics                           00101010b (2AH)
    //   5 - 320x200 graphics (color burst disabled)    00101110b (2EH)
    //   6 - 640x200 graphics                           00011100b (1CH)
    //   7 - 80x25 alpha (MDA only)                     00101001b (29H)
    // 11H - 640x480 graphics (MCGA only)               00011000b (18H)

    // CGA Mode Control (Port 0x3D8) 40:0065
    bool mcr_display_reset; // bit 7 display reset / custom

    bool mcr_blink_enabled; // bit 5 blinking attribute
    bool mcr_hires_graphics_mode; // bit 4 true if 640-wide graphics modes/ false  all other
    bool mcr_video_output; // bit 3 true video enabled / false screen blank
    bool mcr_color_burst_disabled; // bit 2  true color burst disabled/ false enabled
    bool mcr_graphics_mode; // bit 1 true 320-graphics mode / false all other
    bool mcr_hires; // bit 0  true 80-char modes / false 40 char modes

    uint8_t mcr_register;

    // CGA Color Select Register
    /** 0x3D9
     * Write: Color Select Register
     *
     * Bit Layout:
     * | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
     * | - | - | b | i | I | R | G | B |
     *
     * Bit Descriptions:
     *
     * Bits 0-3 (I, R, G, B): Color
     *      - Text modes: IRGB of border
     *      - Graphics modes: IRGB of background
     *
     * Bit 4 (i): Intensity
     *      - Text modes: Background intensity
     *      - Mid-res graphics: Pixel intensity
     *
     * Bit 5 (b): Blue
     *      - Select grafx palette 0 or 1 (1 adds blue)
     *
     * Bits 6-7: Unused
     */
    uint8_t color_select_register;

} CRTC_State; // CGA CRTC

static const uint16_t textmode_palette[16] = {
    0x0000, // 0  - Black
    0x0015, // 1  - Blue
    0x0540, // 2  - Green
    0x0555, // 3  - Cyan
    0xA800, // 4  - Red
    0xA815, // 5  - Magenta
    0xAA00, // 6  - Brown (Hardware fix applied)
    0xD6BA, // 7  - Light Gray
    0x52AA, // 8  - Dark Gray
    0x52FF, // 9  - Light Blue
    0x57EA, // 10 - Light Green
    0x57FF, // 11 - Light Cyan
    0xFAAA, // 12 - Light Red
    0xFAFF, // 13 - Light Magenta
    0xFFEA, // 14 - Yellow
    0xF7DE // 15 - White
};

static const uint16_t cga_palette[24] = {
    // LOW INTENSITY (Bit 4 = 0)
    0x0000, 0x0540, 0xA800, 0xAA00, // Palette 0: Green, Red, Brown
    0x0000, 0x0555, 0xA815, 0xD6BA, // Palette 1: Cyan, Magenta, Light Gray
    0x0000, 0x0555, 0xA800, 0xD6BA, // Palette 2: Cyan, Red, Light Gray

    // HIGH INTENSITY (Bit 4 = 1)
    0x0000, 0x07E0, 0xF800, 0xFFE0, // Palette 0: L. Green, L. Red, Yellow
    0x0000, 0x07FF, 0xF81F, 0xFFFF, // Palette 1: L. Cyan, L. Magenta, White
    0x0000, 0x07FF, 0xF800, 0xFFFF // Palette 2: L. Cyan, L. Red, White
};

void __time_critical_func(video_cga_port_in)(uint32_t port);
void __time_critical_func(video_cga_port_out)(uint32_t port);

typedef void (*video_display_put_color_cb)(uint16_t color);
typedef void (*video_display_reset_cb)();
typedef void (*video_display_begin_frame_cb)();

typedef struct __attribute__((packed, aligned(4))) {
    uint16_t screen_width;
    uint16_t screen_height;

    video_display_put_color_cb display_put_color_callback;
    video_display_reset_cb display_reset_callback;
    video_display_begin_frame_cb display_begin_frame_callback;

} Video_Config;

void video_display_init(void);
void video_cga_render(void);
void video_set_config(Video_Config* video_cfg);
