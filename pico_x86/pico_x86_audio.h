// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

#define PWM_WRAP 250
#define PWM_SAMPLE_HZ 87686

void pico_x86_audio_init(uint8_t audio_pin_left, uint8_t audio_pin_right);
