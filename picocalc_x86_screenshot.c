// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "picocalc_x86_screenshot.h"
#include "ff.h"
#include <stdint.h>

static FIL f_screenshot;
static int32_t width, height = 0;
static FRESULT f_ok = FR_INVALID_PARAMETER;

static FRESULT screenshot_write_bmp_header() {
    if ((f_ok != FR_OK) || width <= 0 || height <= 0) {
        return f_ok;
    }
    uint32_t row_size = ((width * 3) + 3) & ~3;
    UINT bw;

    bmp_header_t header = {
        .sig = {'B', 'M'},
        .file_size = 54 + (row_size * height),
        .data_offset = 54,
        .header_size = 40,
        .width = width,
        .height = -height,
        .planes = 1,
        .bpp = 24,
        .image_size = row_size * height,
        .x_ppm = 2835,
        .y_ppm = 2835,
    };
    return f_write(&f_screenshot, &header, 54, &bw);
}

void screenshot_write_pixel(uint16_t pixel_color) {
    if ((f_ok != FR_OK)) {
        return;
    }
    UINT bw;
    uint8_t r = (uint8_t)(((pixel_color >> 11) & 0x1F) << 3);
    uint8_t g = (uint8_t)(((pixel_color >> 5) & 0x3F) << 2);
    uint8_t b = (uint8_t)((pixel_color & 0x1F) << 3);

    uint8_t bgr[3] = {b, g, r};
    f_ok = f_write(&f_screenshot, bgr, 3, &bw);
}

void screenshot_create_file(const char *path, const int32_t image_width,
                            const int32_t image_height) {
    f_ok = f_open(&f_screenshot, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (f_ok != FR_OK) {
        return;
    }
    width = image_width;
    height = image_height;
    f_ok = screenshot_write_bmp_header();
}

void screenshot_close_file() {
    if (f_ok != FR_OK) {
        return;
    }
    f_ok = f_close(&f_screenshot);
}
