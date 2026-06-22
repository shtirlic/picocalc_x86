// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pico_x86_video.h"

#ifndef TFT_RST_PIN
#define TFT_RST_PIN 15
#endif

#ifndef TFT_CS_PIN
#define TFT_CS_PIN 13
#endif

#ifndef TFT_LED_PIN
#define TFT_LED_PIN 9
#endif

#ifndef TFT_CLK_PIN
#define TFT_CLK_PIN 10
#endif

#ifndef TFT_DATA_PIN
#define TFT_DATA_PIN 11
#endif

#ifndef TFT_DC_PIN
#define TFT_DC_PIN 14
#endif

// static const uint16_t textmode_palette[16] = {
//     0x0000, // 0: Black         (Standard)
//     0x1108, // 1: Blue          (Deep Navy - prevents background glare)
//     0x2404, // 2: Green         (Slightly darker)
//     0x2410, // 3: Cyan          (Desaturated, readable on navy)
//     0x8104, // 4: Red           (Deeper red)
//     0x8110, // 5: Magenta       (Softer magenta)
//     0x8304, // 6: Brown         (True brown, not muddy red)
//     0xA514, // 7: Light Gray    (Dimmed to reduce halo effect)
//     0x5290, // 8: Dark Gray     (Slightly lifted for contrast)
//     0x4318, // 9: Bright Blue   (Muted for readability)
//     0x4688, // 10: Bright Green (Less toxic)
//     0x469A, // 11: Bright Cyan  (Softened significantly for 4x10 text)
//     0xD208, // 12: Bright Red   (Slightly desaturated)
//     0xD21A, // 13: Bright Magenta (Slightly desaturated)
//     0xD688, // 14: Yellow       (Warm yellow, less acidic)
//     0xE71C // 15: White        (Off-white/Soft gray to eliminate blooming)
// };

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

void picocalc_display_init();
void picocalc_display_refresh();
void picocalc_display_set_crtc(const CRTC_State* crtc_state);
void picocalc_display_set_vram(uint8_t* buffer, const uint16_t width, const uint16_t height);
void picocalc_display_show_image(const uint8_t* image, size_t size);
