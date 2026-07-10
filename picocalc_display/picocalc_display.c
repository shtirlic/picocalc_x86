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

#include "picocalc_display.h"
#include "picocalc_display.pio.h"

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

static void __always_inline lcd_set_dc_cs(const bool dc, const bool cs)
{
    sleep_us(5);
    gpio_put_masked(
        (1u << TFT_DC_PIN) | (1u << TFT_CS_PIN), !!dc << TFT_DC_PIN | !!cs << TFT_CS_PIN);
    sleep_us(5);
}

static void __always_inline lcd_write_cmd(const uint8_t* cmd, size_t count)
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

static void __always_inline lcd_set_window(
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

static void lcd_init(const uint8_t* init_seq)
{
    while (*init_seq) {
        lcd_write_cmd(init_seq + 2, *init_seq);
        sleep_ms(init_seq[1] * 5);
        init_seq += *init_seq + 2;
    }
}

static void __always_inline start_pixels()
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

// Basic show for bitmaps
void picocalc_display_show_image(const uint8_t* image, size_t size)
{
    start_pixels();
    for (size_t i = 0; i < size; i++) {
        picocalc_display_put(pio, sm_video_output, image[i]);
    }
}

void __time_critical_func(picocalc_display_begin_frame)() { start_pixels(); }

void __time_critical_func(picocalc_display_put_color)(uint16_t color)
{
    picocalc_display_put(pio, sm_video_output, color >> 8);
    picocalc_display_put(pio, sm_video_output, color & 0xFF);
}
