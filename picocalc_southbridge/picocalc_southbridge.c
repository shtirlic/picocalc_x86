// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <pico/stdio.h>
#include "picocalc_southbridge.h"
#include <pico/time.h>

static uint8_t sb_initialized = 0;

// clang-format off
// Lookup table to convert standard ASCII (0x20 to 0x7E) to XT Scancodes
static const uint8_t ascii_to_xt[95] = {
    0x39, // Space
    0x02, 0x28, 0x04, 0x05, 0x06, 0x08, 0x28, // ! " # $ % & '
    0x0A, 0x0B, 0x09, 0x0D, 0x33, 0x0C, 0x34, 0x35, // ( ) * + , - . /
    0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, // 0-9
    0x27, 0x27, 0x33, 0x0D, 0x34, 0x35, 0x03, // : ; < = > ? @
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, // A-M
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, // N-Z
    0x1A, 0x2B, 0x1B, 0x07, 0x0C, 0x29, // [ \ ] ^ _ `
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, // a-m
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, // n-z
    0x1A, 0x2B, 0x1B, 0x29 // { | } ~
};

static __always_inline uint8_t translate_kbd_code(uint8_t raw_code) {
    switch (raw_code) {

        // Special scancodes
        case 0xD0: return 0x46; // PAUSE/BRK (Mapped to XT Scroll Lock)
        case 0xD1: return 0x52; // INSERT
        case 0xD4: return 0x53; // DEL
        case 0xD2: return 0x47; // HOME
        case 0xD5: return 0x4F; // END
        case 0xD6: return 0x49; // PAGE_UP
        case 0xD7: return 0x51; // PAGE_DOWN

        case 0xC1: return 0x3A; // CAPS LOCK

        case 0xA1: return 0x38; // ALT
        case 0xA2: return 0x2A; // L-SHIFT
        case 0xA5: return 0x1D; // CTRL
        case 0x0A: return 0x1C; // ENTER
        case 0xB1: return 0x01; // ESC
        case 0x08: return 0x0E; // BACKSPACE
        case 0x09: return 0x0F; // TAB
        case 0xB5: return 0x48; // UP
        case 0xB6: return 0x50; // DOWN
        case 0xB4: return 0x4B; // LEFT
        case 0xB7: return 0x4D; // RIGHT
        case 0x81: return 0x3B; // F1
        case 0x82: return 0x3C; // F2
        case 0x83: return 0x3D; // F3
        case 0x84: return 0x3E; // F4
        case 0x85: return 0x3F; // F5
        case 0x86: return 0x40; // F6
        case 0x87: return 0x41; // F7
        case 0x88: return 0x42; // F8
        case 0x89: return 0x43; // F9
        case 0x90: return 0x44; // F10
    }

    // Normal scancodes
    if (raw_code >= 0x20 && raw_code <= 0x7E) {
        return ascii_to_xt[raw_code - 0x20];
    }

    return raw_code; // Fallback
}

// clang-format on
// static uint64_t wait_start = 0;

uint16_t __time_critical_func(picocalc_southbridge_kb_read)()
{
    if (!sb_initialized)
        return -1;

    // if ((time_us_64() - wait_start) < 10000)
    // return -1;

    unsigned char msg[1] = { SB_REG_FIF };
    int retval = i2c_write_timeout_us(SB_I2C_I, SB_I2C_ADDR, msg, 1, false, SB_I2C_TIMEOUT_US);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("picocalc_southbridge_kb_read i2c write error\n");
        return -1;
    }
    // wait_start = time_us_64();

    uint8_t buff[2] = { 0 };
    retval
        = i2c_read_timeout_us(SB_I2C_I, SB_I2C_ADDR, (uint8_t*)&buff, 2, false, SB_I2C_TIMEOUT_US);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("picocalc_southbridge_kb_read i2c write error\n");
        return -1;
    }

    if (retval == 2 && ((buff[0] << 8) | buff[1]) != 0) {
        uint8_t key_state = buff[0];
        uint8_t key_code = buff[1];

        // The FN button (raw 0xA3) (right shift) not a tracked modifier
        if (key_code == 0xA3)
            return -1;

#ifdef DEBUG_KBD
        printf("Raw Key: %x  State: %x, Code: %x \n", (key_code), key_state,
            translate_kbd_code(key_code) | KBD_MAKE_STATE(key_state));
#endif
        return translate_kbd_code(key_code) | KBD_MAKE_STATE(key_state);
    }

    return -1;
}

void picocalc_southbridge_init()
{
    if (sb_initialized) {
        return;
    }

    gpio_set_function(SB_I2C_SCL, GPIO_FUNC_I2C);
    gpio_set_function(SB_I2C_SDA, GPIO_FUNC_I2C);
    i2c_init(SB_I2C_I, SB_I2C_SPEED);
    gpio_pull_up(SB_I2C_SCL);
    gpio_pull_up(SB_I2C_SDA);

    sb_initialized = 1;
}

int picocalc_southbridge_battery()
{
    uint16_t buff = 0;
    unsigned char msg[2];
    msg[0] = SB_REG_BAT;

    if (sb_initialized == 0)
        return -1;

    int retval = i2c_write_timeout_us(SB_I2C_I, SB_I2C_ADDR, msg, 1, false, SB_I2C_TIMEOUT_US);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("read_battery i2c write error\n");
        return -1;
    }
    retval = i2c_read_timeout_us(
        SB_I2C_I, SB_I2C_ADDR, (unsigned char*)&buff, 2, false, SB_I2C_TIMEOUT_US);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("read_battery i2c read error read\n");
        return -1;
    }

    if (buff != 0) {
        return buff;
    }
    return -1;
}

int picocalc_southbridge_backlight(uint8_t val)
{
    uint16_t buff = 0;
    unsigned char msg[2];
    msg[0] = SB_REG_BK2;
    msg[1] = val;
    bitSet(msg[0], 7);

    if (sb_initialized == 0)
        return -1;

    int retval = i2c_write_timeout_us(SB_I2C_I, SB_I2C_ADDR, msg, 2, false, SB_I2C_TIMEOUT_US);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("read_battery i2c write error\n");
        return -1;
    }
    retval = i2c_read_timeout_us(
        SB_I2C_I, SB_I2C_ADDR, (unsigned char*)&buff, 2, false, SB_I2C_TIMEOUT_US);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("read_battery i2c read error read\n");
        return -1;
    }

    if (buff != 0) {
        return buff;
    }
    return -1;
}
