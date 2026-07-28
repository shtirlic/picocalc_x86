// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later
//
// the Intel 8253/8254 Programmable Interval Timer (PIT)

#pragma once

#include <stdint.h>

#define PIT_BASE_HZ 1193182UL

#define PIT_TICK_INTERVAL_US 1000
#define PIT_TICKS_PER_INTERVAL ((PIT_BASE_HZ * PIT_TICK_INTERVAL_US) / 1000000UL)

typedef struct {
    volatile uint16_t reload;
    volatile uint32_t pos;
    volatile uint16_t latch;
    volatile uint8_t rw_mode;
    volatile uint8_t mode;
    volatile uint8_t bcd;
    volatile uint8_t write_toggle;
    volatile uint8_t read_toggle;
    volatile uint8_t latched;
    volatile uint8_t armed;
    volatile uint8_t gate;
    volatile uint8_t output;
    volatile uint8_t count_loaded;
} pit_channel_t;

extern pit_channel_t pit_channels[3];

void pico_x86_pit_init(void);
void __time_critical_func(pico_x86_pit_out)(uint16_t port, uint8_t val);
uint8_t __time_critical_func(pico_x86_pit_in)(uint16_t port);
void pico_x86_pit_set_speaker_control(uint8_t val);
uint32_t pico_x86_pit_advance(uint32_t ticks);
uint32_t pico_x86_pit_channel2_freq_hz(void);
uint8_t pico_x86_pit_channel2_output_bit(void);
uint8_t pico_x86_pit_speaker_data_enabled(void);
uint8_t pico_x86_pit_get_port61_state(uint8_t current_port61_val);
void pico_x86_pit_timer_init(void);
