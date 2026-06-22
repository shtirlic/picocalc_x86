// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include <stdio.h>

#define AUDIO_PIN_L 26
#define AUDIO_PIN_R 27

#define PWM_WRAP 250
#define PWM_SAMPLE_HZ 87686

void picocalc_sound_init(irq_handler_t);
