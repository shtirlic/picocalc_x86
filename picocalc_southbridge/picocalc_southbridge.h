// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <pico/stdlib.h>
#include <pico/platform.h>
#include <hardware/gpio.h>
#include <hardware/i2c.h>

// SouthBridge I2C Params
#define SB_I2C_I i2c1
#define SB_I2C_SDA 6
#define SB_I2C_SCL 7
#define SB_I2C_SPEED 10000
#define SB_I2C_ADDR 0x1F
#define SB_I2C_TIMEOUT_US 10000

// SouthBridge registers
#define SB_REG_KEY (0x04) // *key status
#define SB_REG_BKL (0x05) // *backlight
#define SB_REG_RST (0x08) // *reset
#define SB_REG_FIF (0x09) // *fifo
#define SB_REG_BK2 (0x0A) // *keyboard backlight
#define SB_REG_BAT (0x0B) // *battery
#define SB_REG_OFF (0x0E) // *power off
#define SB_WRITE (0x80) // write to register

#define KBD_STATE_MASK 0x6000
#define KBD_STATE_SHIFT 13

#define KBD_STATE_PRESS (1 << KBD_STATE_SHIFT)
#define KBD_STATE_HOLD (2 << KBD_STATE_SHIFT)
#define KBD_STATE_RELEASE (3 << KBD_STATE_SHIFT)

#define KBD_MAKE_STATE(state) ((state) << KBD_STATE_SHIFT)
#define KBD_GET_STATE(event) (((event) & KBD_STATE_MASK))

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitClear(value, bit) ((value) &= ~(1 << (bit)))
#define bitSet(value, bit) ((value) |= (1 << (bit)))

void picocalc_southbridge_init();
int picocalc_southbridge_battery();
int picocalc_southbridge_backlight(uint8_t);
uint16_t picocalc_southbridge_kb_read();
