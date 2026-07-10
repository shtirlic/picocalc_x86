// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

void picocalc_display_init();
void picocalc_display_begin_frame();
void picocalc_display_put_color(uint16_t color);
void picocalc_display_show_image(const uint8_t* image, size_t size);
