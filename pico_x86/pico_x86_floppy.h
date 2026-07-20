// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "pico/stdlib.h"
#include "ff.h"

// Standard 3.5" 1.44MB floppy, formatted FAT12
#define FDD_SECTOR_SIZE 512u
#define FDD_TOTAL_SECTORS 2880u
#define FDD_RESERVED_SECTORS 1u
#define FDD_NUM_FATS 2u
#define FDD_SECTORS_PER_FAT 9u
#define FDD_ROOT_ENTRIES 224u
#define FDD_ROOT_DIR_SECTORS ((FDD_ROOT_ENTRIES * 32u) / FDD_SECTOR_SIZE)
#define FDD_META_SECTORS                                                                           \
    (FDD_RESERVED_SECTORS + FDD_NUM_FATS * FDD_SECTORS_PER_FAT + FDD_ROOT_DIR_SECTORS)

#define FDD_IMAGE_PATH "0:/x86/fd.img"

// clang-format off
static void create_blank_floppy_image()
{
    static FIL f;
    if (f_open(&f, FDD_IMAGE_PATH, FA_CREATE_NEW | FA_WRITE) != FR_OK)
        return;

    UINT bw;

    static const uint8_t boot_sector[FDD_SECTOR_SIZE] = {
        [0x00] = 0xEB,
        [0x01] = 0x3C,
        [0x02] = 0x90, // jmp short + nop
        [0x03] = 'P','I','C','O','X','8','6',' ',
        [0x0B] = 0x00,
        [0x0C] = 0x02, // bytes/sector = 512
        [0x0D] = 1, // sectors/cluster
        [0x0E] = FDD_RESERVED_SECTORS,
        [0x0F] = 0, // reserved sectors
        [0x10] = FDD_NUM_FATS, // number of FATs
        [0x11] = FDD_ROOT_ENTRIES & 0xFF,
        [0x12] = FDD_ROOT_ENTRIES >> 8, // root dir entries
        [0x13] = FDD_TOTAL_SECTORS & 0xFF,
        [0x14] = FDD_TOTAL_SECTORS >> 8, // total sectors
        [0x15] = 0xF0, // media descriptor (3.5" 1.44MB)
        [0x16] = FDD_SECTORS_PER_FAT,
        [0x17] = 0, // sectors/FAT
        [0x18] = 18,
        [0x19] = 0, // sectors/track
        [0x1A] = 2,
        [0x1B] = 0, // heads
        [0x24] = 0x00, // drive number
        [0x26] = 0x29, // extended boot signature present
        [0x27] = 0x12,
        [0x28] = 0x34,
        [0x29] = 0x56,
        [0x2A] = 0x78, // volume serial number
        [0x2B] = 'N','O',' ','N','A','M','E',' ',' ',' ',' ',
        [0x36] = 'F','A','T','1','2',' ',' ',' ',
        [0x1FE] = 0x55,
        [0x1FF] = 0xAA // boot sector signature
    };
    f_write(&f, boot_sector, sizeof(boot_sector), &bw);

    static const uint8_t fat_header[FDD_SECTOR_SIZE] = { [0] = 0xF0, [1] = 0xFF, [2] = 0xFF };
    static const uint8_t zero_sector[FDD_SECTOR_SIZE] = { 0 };

    for (int fat = 0; fat < FDD_NUM_FATS; fat++) {
        f_write(&f, fat_header, sizeof(fat_header), &bw);
        for (unsigned i = 1; i < FDD_SECTORS_PER_FAT; i++)
            f_write(&f, zero_sector, sizeof(zero_sector), &bw);
    }

    // --- Root dir ---
    for (unsigned i = 0; i < FDD_ROOT_DIR_SECTORS; i++)
        f_write(&f, zero_sector, sizeof(zero_sector), &bw);

    // --- Data area: free  ---
    for (unsigned i = FDD_META_SECTORS; i < FDD_TOTAL_SECTORS; i++)
        f_write(&f, zero_sector, sizeof(zero_sector), &bw);

    f_close(&f);
}
