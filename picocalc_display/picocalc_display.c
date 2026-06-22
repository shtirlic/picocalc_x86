/*
 * Portions of this file are derived from st7789_lcd.
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Licensed under the BSD-3-Clause.
 * See LICENSE.txt.
 *
 * Modifications Copyright (c) 2026 Serg Podtynnyi
 * Licensed under GPLv3 for the combined work.
 */

#include <stdio.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"

#include <pico/multicore.h>

#include "picocalc_display.h"
#include "font4x10.h"
#include "picocalc_display.pio.h"
#include "hardware/dma.h"

#include "pico_x86_video.h"

#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 320
#endif

#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 320
#endif

#define SERIAL_CLK_DIV 1.92f
#define MADCTL_BGR_PIXEL_ORDER (1 << 3)
#define MADCTL_ROW_COLUMN_EXCHANGE (1 << 5)
#define MADCTL_COLUMN_ADDRESS_ORDER_SWAP (1 << 6)

static uint sm_video_output = 0;
static PIO pio = pio0;

static uint8_t* vram = NULL;
static const CRTC_State* crtc = NULL;

static const uint8_t init_seq[] = {

    // Software reset
    1, 20, 0x01,

    1, 0, 0x28,

    1, 24, 0x11,

    // Positive Gamma Control
    16, 0, 0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A,
    0x0F,

    // Negative Gamma Control
    16, 0, 0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37,
    0x0F,

    // Power Control 1
    3, 0, 0xC0, 0x17, 0x15,

    // Power Control 2
    2, 0, 0xC1, 0x41,

    // VCOM Control
    4, 0, 0xC5, 0x00, 0x12, 0x80,

    // Interface Mode Control
    2, 0, 0xB0, 0x00,

    // Display Inversion ON
    1, 0, 0x21,

    // Frame Rate Control
    2, 0, 0xB1, 0xA0,

    // Display Inversion Control
    // 2, 0, 0xB4, 0x02,

    // Display Function Control
    4, 0, 0xB6, 0x02, 0x02, 0x3B,

    // Entry Mode Set
    2, 0, 0xB7, 0xC6,

    // Set Image Function
    2, 0, 0xE9, 0x00,

    // Adjust Control 3
    5, 0, 0xF7, 0xA9, 0x51, 0x2C, 0x82,

    2, 0, 0x36,
    MADCTL_COLUMN_ADDRESS_ORDER_SWAP | MADCTL_BGR_PIXEL_ORDER, // SetMADCTL

    // 2, 0, 0x36,
    // MADCTL_ROW_COLUMN_EXCHANGE | MADCTL_BGR_PIXEL_ORDER, // Set MADCTL

    // 2, 0, 0x36, MADCTL_BGR_PIXEL_ORDER,
    // 2, 0, 0x36,
    // MADCTL_COLUMN_ADDRESS_ORDER_SWAP | MADCTL_BGR_PIXEL_ORDER |
    // 	MADCTL_ROW_COLUMN_EXCHANGE, // Set MADCTL

    // Memory Access Control (MADCTL) - MX, BGR
    // 2, 0, 0x36, 0x48,

    // Pixel Interface Format
    2, 0, 0x3A, 0x55,

    // // CASET: column addresses (Added back in for window bounding)
    // 5, 0, 0x2a, 0x00, 0x00, SCREEN_WIDTH >> 8, SCREEN_WIDTH & 0xff,

    // // RASET: row addresses (Added back in for window bounding)
    // 5, 0, 0x2b, 0x00, 0x00, SCREEN_HEIGHT >> 8, SCREEN_HEIGHT & 0xff,

    // Main screen turn on (120ms delay)
    1, 0, 0x29,

    0 // Terminate list
};

static __always_inline void lcd_set_dc_cs(const bool dc, const bool cs)
{
    sleep_us(5);
    gpio_put_masked(
        (1u << TFT_DC_PIN) | (1u << TFT_CS_PIN), !!dc << TFT_DC_PIN | !!cs << TFT_CS_PIN);
    sleep_us(5);
}

static __always_inline void lcd_write_cmd(const uint8_t* cmd, size_t count)
{
    if (*cmd != 0x2c) {
        //     printf("\n LCD: 0x%02X => ", *cmd);
    }
    picocalc_display_wait_idle(pio, sm_video_output);
    lcd_set_dc_cs(0, 0);
    picocalc_display_put(pio, sm_video_output, *cmd++);
    if (count >= 2) {
        picocalc_display_wait_idle(pio, sm_video_output);
        lcd_set_dc_cs(1, 0);
        for (size_t i = 0; i < count - 1; ++i) {
            //       printf("0x%02X ", *cmd);
            picocalc_display_put(pio, sm_video_output, *cmd++);
        }
    }
    picocalc_display_wait_idle(pio, sm_video_output);
    lcd_set_dc_cs(1, 1);
}

static inline void lcd_set_window(
    const uint16_t x, const uint16_t y, const uint16_t width, const uint16_t height)
{
    uint8_t screen_width_cmd[5] = { 0x2a, 0x00, 0x00, 0x00, 0x00 };
    uint8_t screen_height_cmd[5] = { 0x2b, 0x00, 0x00, 0x00, 0x00 };

    screen_width_cmd[1] = x >> 8;
    screen_width_cmd[2] = x & 0xff;
    screen_width_cmd[3] = (x + width - 1) >> 8;
    screen_width_cmd[4] = (x + width - 1) & 0xff;

    screen_height_cmd[1] = y >> 8;
    screen_height_cmd[2] = y & 0xff;
    screen_height_cmd[3] = (y + height - 1) >> 8;
    screen_height_cmd[4] = (y + height - 1) & 0xff;

    lcd_write_cmd(screen_width_cmd, 5);
    lcd_write_cmd(screen_height_cmd, 5);
}

static inline void lcd_init(const uint8_t* init_seq)
{
    while (*init_seq) {
        lcd_write_cmd(init_seq + 2, *init_seq);
        sleep_ms(init_seq[1] * 5);
        init_seq += *init_seq + 2;
    }
}

static __always_inline void start_pixels()
{
    const uint8_t cmd = 0x2c; // RAMWR
    lcd_write_cmd(&cmd, 1);
    lcd_set_dc_cs(1, 0);
}

static void test_pattern()
{
    start_pixels();
    for (int i = 0; i < 320 * 320; i++) {
        picocalc_display_put(pio, sm_video_output, 0x00);
        picocalc_display_put(pio, sm_video_output, 0x1F);
    }
    sleep_ms(1000);
}

void picocalc_display_init()
{
    gpio_init(TFT_CS_PIN);
    gpio_init(TFT_DC_PIN);
    gpio_init(TFT_RST_PIN);
    gpio_init(TFT_LED_PIN);

    const uint offset = pio_add_program(pio, &picocalc_display_program);
    sm_video_output = pio_claim_unused_sm(pio, true);
    picocalc_display_program_init(
        pio, sm_video_output, offset, TFT_DATA_PIN, TFT_CLK_PIN, SERIAL_CLK_DIV);

    gpio_set_dir(TFT_CS_PIN, GPIO_OUT);
    gpio_set_dir(TFT_DC_PIN, GPIO_OUT);
    gpio_set_dir(TFT_RST_PIN, GPIO_OUT);
    gpio_set_dir(TFT_LED_PIN, GPIO_OUT);

    gpio_put(TFT_CS_PIN, 1);
    gpio_put(TFT_RST_PIN, 1);
    lcd_init(init_seq);

    lcd_set_window(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
}

void picocalc_display_set_vram(uint8_t* vram_address, const uint16_t width, const uint16_t height)
{
    vram = vram_address;
}

void picocalc_display_set_crtc(const CRTC_State* crtc_state) { crtc = crtc_state; }

void __time_critical_func(render_text_mode)()
{
    start_pixels();

    // Dynamically read columns from CRTC Register 0x01, fallback to MCR if zero
    int num_cols = crtc->dr_horiz_displayed;
    if (num_cols == 0) {
        num_cols = crtc->mcr_hires ? 80 : 40;
    }

    // Dynamically read rows from CRTC Register 0x06, fallback to 25 if zero
    int num_rows = crtc->dr_vert_displayed;
    if (num_rows == 0) {
        num_rows = 25;
    }

    int pixel_scale = crtc->mcr_hires ? 1 : 2;

    const int VERTICAL_MARGIN = 35;

    uint64_t cga_now = time_us_64();
    bool cursor_blink = (cga_now % 533333) > 266666;
    bool text_blink_hide = (cga_now % 1066666) >= 533333;

    uint8_t border_color_idx = crtc->color_select_register & 0x0F;
    uint16_t border_color = textmode_palette[border_color_idx];

    // Top margin
    for (int i = 0; i < (SCREEN_WIDTH * VERTICAL_MARGIN); i++) {
        picocalc_display_put(pio, sm_video_output, border_color >> 8);
        picocalc_display_put(pio, sm_video_output, border_color & 0xFF);
    }

    for (int text_row = 0; text_row < num_rows; text_row++) {
        for (int font_y = 0; font_y < 10; font_y++) {
            for (int text_col = 0; text_col < num_cols; text_col++) {

                uint16_t cell_index = text_row * num_cols + text_col;
                uint16_t abs_index = crtc->start_address_offset + cell_index;
                uint32_t mem_offset = abs_index * 2;

                uint8_t character = vram[mem_offset];
                uint8_t color_attr = vram[mem_offset + 1];

                if (crtc->cursor_visible && cursor_blink && (abs_index == crtc->cursor_offset)) {
                    bool in_cursor_range = false;
                    if (crtc->dr_cursor_start <= crtc->dr_cursor_end) {
                        in_cursor_range
                            = (font_y >= crtc->dr_cursor_start && font_y <= crtc->dr_cursor_end);
                    } else {
                        in_cursor_range = ((font_y >= crtc->dr_cursor_start && font_y <= 7)
                            || font_y <= crtc->dr_cursor_end);
                    }
                    if (in_cursor_range) {
                        color_attr = ((color_attr & 0x0F) << 4) | ((color_attr & 0xF0) >> 4);
                    }
                }

                uint8_t fg_idx = color_attr & 0x0F;
                uint8_t bg_idx;
                bool is_blinking = false;

                // blinking or color intensity
                if (crtc->mcr_blink_enabled) {
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
                        picocalc_display_put(pio, sm_video_output, color >> 8);
                        picocalc_display_put(pio, sm_video_output, color & 0xFF);
                    }
                }
            }
        }
    }

    // Bottom margin
    for (int i = 0; i < (SCREEN_WIDTH * VERTICAL_MARGIN); i++) {
        picocalc_display_put(pio, sm_video_output, border_color >> 8);
        picocalc_display_put(pio, sm_video_output, border_color & 0xFF);
    }
}

void __time_critical_func(render_cga_graphics)()
{
    start_pixels();

    bool is_1bpp_mode = crtc->mcr_hires_graphics_mode;
    const int OUTPUT_ROWS = 240;
    const int VERTICAL_MARGIN = (SCREEN_HEIGHT - OUTPUT_ROWS) / 2;

    // --- DECODE PALETTE & BACKGROUND SETTINGS ---
    // Background color (low 4 bits mapped to standard text palette)
    uint16_t bg_color = textmode_palette[crtc->color_select_register & 0x0F];

    // Palette selection
    uint8_t palette_num;

    // Check if the Color Burst is disabled (MCR Bit 2) to trigger the unofficial Mode 5 palette
    if (crtc->mcr_color_burst_disabled) {
        palette_num = 2; // Palette 2: Cyan, Red, White
    } else {
        // Standard Mode 4: Bit 5 determines Palette 0 vs Palette 1
        palette_num = (crtc->color_select_register & 0x20) ? 1 : 0;
    }

    // Intensity selection (Bit 4 determines Low vs High intensity)
    uint8_t intensity = (crtc->color_select_register & 0x10) ? 1 : 0;

    // Base palette index calculation:
    // intensity * 12 (to skip to high intensity half) + palette_num * 4
    uint8_t base_pal = (intensity * 12) + (palette_num * 4);

    // Mode 6 uses the color_select for the foreground and Black for the background
    uint16_t m6_fg_color = textmode_palette[crtc->color_select_register & 0x0F];
    uint16_t m6_bg_color = textmode_palette[0]; // Black

    // Top margin
    for (int i = 0; i < (SCREEN_WIDTH * VERTICAL_MARGIN); i++) {
        picocalc_display_put(pio, sm_video_output, bg_color >> 8);
        picocalc_display_put(pio, sm_video_output, bg_color & 0xFF);
    }

    for (int out_y = 0; out_y < OUTPUT_ROWS; out_y++) {
        int y = (out_y * 200) / OUTPUT_ROWS;
        uint32_t bank_offset = (y & 1) ? 0x2000 : 0x0000;
        uint32_t row_offset = (y >> 1) * 80;

        for (int x_byte = 0; x_byte < 80; x_byte++) {
            // Apply CRTC start address in case of hardware panning
            uint32_t vram_offset
                = (crtc->start_address_offset * 2) + bank_offset + row_offset + x_byte;
            uint8_t pixel_data = vram[vram_offset & 0x3FFF]; // Mask to 16KB window

            if (is_1bpp_mode) {
                for (int p = 0; p < 4; p++) {
                    uint8_t bit_hi = (pixel_data >> (7 - p * 2)) & 1;
                    uint8_t bit_lo = (pixel_data >> (6 - p * 2)) & 1;
                    uint16_t color = (bit_hi | bit_lo) ? m6_fg_color : m6_bg_color;

                    picocalc_display_put(pio, sm_video_output, color >> 8);
                    picocalc_display_put(pio, sm_video_output, color & 0xFF);
                }
                continue;
            }

            uint8_t p0_val = (pixel_data >> 6) & 0x03;
            uint8_t p1_val = (pixel_data >> 4) & 0x03;
            uint8_t p2_val = (pixel_data >> 2) & 0x03;
            uint8_t p3_val = (pixel_data >> 0) & 0x03;

            // Pixel value 0 always draws the background color
            uint16_t c0 = p0_val ? cga_palette[base_pal + p0_val] : bg_color;
            uint16_t c1 = p1_val ? cga_palette[base_pal + p1_val] : bg_color;
            uint16_t c2 = p2_val ? cga_palette[base_pal + p2_val] : bg_color;
            uint16_t c3 = p3_val ? cga_palette[base_pal + p3_val] : bg_color;

            picocalc_display_put(pio, sm_video_output, c0 >> 8);
            picocalc_display_put(pio, sm_video_output, c0 & 0xFF);
            picocalc_display_put(pio, sm_video_output, c1 >> 8);
            picocalc_display_put(pio, sm_video_output, c1 & 0xFF);
            picocalc_display_put(pio, sm_video_output, c2 >> 8);
            picocalc_display_put(pio, sm_video_output, c2 & 0xFF);
            picocalc_display_put(pio, sm_video_output, c3 >> 8);
            picocalc_display_put(pio, sm_video_output, c3 & 0xFF);
        }
    }

    // Bottom margin
    for (int i = 0; i < (SCREEN_WIDTH * VERTICAL_MARGIN); i++) {
        picocalc_display_put(pio, sm_video_output, bg_color >> 8);
        picocalc_display_put(pio, sm_video_output, bg_color & 0xFF);
    }
}

void __time_critical_func(picocalc_display_refresh)()
{
    if (!crtc->mcr_video_output) {
        // Maybe draw black screen?
        return;
    }

    if (crtc->mcr_graphics_mode || crtc->mcr_hires_graphics_mode) {
        render_cga_graphics();
    } else {
        render_text_mode();
    }
}

// Basic show for bitmaps
void picocalc_display_show_image(const uint8_t* image, size_t size)
{
    start_pixels();
    for (size_t i = 0; i < size; i++) {
        picocalc_display_put(pio, sm_video_output, image[i]);
    }
}
