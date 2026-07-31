// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>
// Decodes and buffers the scancode currently in AL.
// Called from OP_48 (0x0f,0x04)
void pico_x86_keyb_process_scancode(uint8_t raw_code);
