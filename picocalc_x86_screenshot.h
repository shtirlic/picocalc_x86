// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t sig[2];
    uint32_t file_size;
    uint32_t reserved;
    uint32_t data_offset;
    uint32_t header_size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    int32_t x_ppm;
    int32_t y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_header_t;

void screenshot_create_file(const char *path, int32_t image_width, int32_t image_height);
void screenshot_close_file(void);
void screenshot_write_pixel(uint16_t pixel_color);
