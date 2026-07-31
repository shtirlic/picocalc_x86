// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later
//
// the Intel 8253/8254 Programmable Interval Timer (PIT)

#include <pico/time.h>
#include <string.h>

#include "pico_x86.h"
#include "pico_x86_pit.h"

static struct repeating_timer xt_timer;

pit_channel_t pit_channels[3];

static volatile uint8_t speaker_data_enable;

static uint16_t __always_inline bcd_to_bin(uint16_t bcd)
{
    return (bcd & 0xF) + (((bcd >> 4) & 0xF) * 10) + ((bcd >> 8) & 0xF) * 100
        + ((bcd >> 12) & 0xF) * 1000;
}

static uint16_t __always_inline bin_to_bcd(uint16_t bin)
{
    return (bin % 10) | (((bin / 10) % 10) << 4) | (((bin / 100) % 10) << 8)
        | (((bin / 1000) % 10) << 12);
}

static uint32_t __always_inline effective_max(const pit_channel_t* c)
{
    uint32_t r = c->bcd ? bcd_to_bin(c->reload) : c->reload;
    if (r == 0)
        r = c->bcd ? 10000 : 65536;
    return r;
}

void pico_x86_pit_init(void)
{
    memset((void*)pit_channels, 0, sizeof(pit_channels));
    speaker_data_enable = 0;
    for (int i = 0; i < 3; i++) {
        pit_channel_t* c = &pit_channels[i];
        c->rw_mode = 3;
        c->mode = 3;
        c->reload = 0;
        c->pos = 65535;
        c->output = 1;
        c->gate = (i == 2) ? 0 : 1;
        c->armed = (i == 2) ? 0 : 1;
        c->count_loaded = (i == 2) ? 0 : 1;
    }
}

static bool __time_critical_func(xt_timer_callback)(struct repeating_timer* t)
{
    uint32_t irq0_events = pico_x86_pit_advance(PIT_TICKS_PER_INTERVAL);

    // Drop excess ticks
    if (unlikely(irq0_events > 0)) {
        pico_x86_timer_tick();
    }
    return true;
}

void pico_x86_pit_timer_init()
{
    add_repeating_timer_us(-PIT_TICK_INTERVAL_US, xt_timer_callback, NULL, &xt_timer);
}

static void __always_inline pit_load(pit_channel_t* c)
{
    uint32_t max = effective_max(c);
    c->pos = max - 1;

    if (c->mode == 1 || c->mode == 5) {
        c->armed = 0;
    } else {
        c->armed = (c->gate && c->count_loaded) ? 1 : 0;
    }
}

static void __always_inline pit_status_byte(const pit_channel_t* c) { (void)c; }

static void __time_critical_func(pit_readback)(uint8_t cmd)
{
    uint8_t latch_count = !(cmd & 0x20);
    uint8_t latch_status = !(cmd & 0x10);

    for (int ch = 0; ch < 3; ch++) {
        if (!(cmd & (2 << ch))) {
            continue;
        }

        pit_channel_t* c = &pit_channels[ch];

        if (latch_status) {
            uint8_t status = (uint8_t)((c->output << 7) | (0 << 6) | (c->rw_mode << 4)
                | (c->mode << 1) | c->bcd);
            c->latch = status;
            c->latched = 2;
        } else if (latch_count && c->latched != 1) {
            uint32_t cur = c->pos + 1;
            c->latch = c->bcd ? bin_to_bcd((uint16_t)cur) : (uint16_t)cur;
            c->latched = 1;
            c->read_toggle = 0;
        }
    }
}

static void __time_critical_func(pit_command)(uint8_t cmd)
{
    uint8_t sc = (cmd >> 6) & 3;

    if (sc == 3) {
        pit_readback(cmd);
        return;
    }

    uint8_t rw = (cmd >> 4) & 3;
    pit_channel_t* c = &pit_channels[sc];

    if (rw == 0) {
        if (c->latched != 1) {
            uint32_t cur = c->pos + 1;
            c->latch = c->bcd ? bin_to_bcd((uint16_t)cur) : (uint16_t)cur;
            c->latched = 1;
            c->read_toggle = 0;
        }
        return;
    }

    uint8_t mode = (cmd >> 1) & 7;
    if (mode > 5) {
        mode &= 3;
    }

    c->rw_mode = rw;
    c->mode = mode;
    c->bcd = cmd & 1;
    c->write_toggle = 0;
    c->armed = 0;
    c->count_loaded = 0;
    c->output = (mode == 0) ? 0 : 1;
    pit_status_byte(c);
}

void __time_critical_func(pico_x86_pit_out)(uint16_t port, uint8_t val)
{
    if (port == 0x43) {
        pit_command(val);
        return;
    }

    pit_channel_t* c = &pit_channels[port - 0x40];

    switch (c->rw_mode) {
    case 1:
        c->reload = (uint16_t)((c->reload & 0xFF00) | val);
        c->count_loaded = 1;
        pit_load(c);
        break;
    case 2:
        c->reload = (uint16_t)((c->reload & 0x00FF) | (val << 8));
        c->count_loaded = 1;
        pit_load(c);
        break;
    case 3:
    default:
        if (c->write_toggle == 0) {
            c->reload = (uint16_t)((c->reload & 0xFF00) | val);
            c->write_toggle = 1;
        } else {
            c->reload = (uint16_t)((c->reload & 0x00FF) | (val << 8));
            c->write_toggle = 0;
            c->count_loaded = 1;
            pit_load(c);
        }
        break;
    }
}

uint8_t __time_critical_func(pico_x86_pit_in)(uint16_t port)
{
    pit_channel_t* c = &pit_channels[port - 0x40];

    if (c->latched == 2) {
        c->latched = 0;
        return (uint8_t)c->latch;
    }

    uint16_t value;
    if (c->latched == 1) {
        value = c->latch;
    } else {
        uint32_t cur = c->pos + 1;
        value = c->bcd ? bin_to_bcd((uint16_t)cur) : (uint16_t)cur;
    }

    uint8_t result;
    switch (c->rw_mode) {
    case 1:
        result = value & 0xFF;
        c->latched = 0;
        break;
    case 2:
        result = (value >> 8) & 0xFF;
        c->latched = 0;
        break;
    case 3:
    default:
        if (c->read_toggle == 0) {
            result = value & 0xFF;
            c->read_toggle = 1;
        } else {
            result = (value >> 8) & 0xFF;
            c->read_toggle = 0;
            c->latched = 0;
        }
        break;
    }
    return result;
}

static void __always_inline pit_set_gate2(uint8_t level)
{
    pit_channel_t* c = &pit_channels[2];
    level = level ? 1 : 0;
    if (c->gate == level) {
        return;
    }

    c->gate = level;

    if (!level) {
        if (c->mode == 2 || c->mode == 3) {
            c->armed = 0;
            c->output = 1;
        }
        return;
    }

    if (c->mode == 1 || c->mode == 5) {
        pit_load(c);
        c->armed = 1;
        c->output = 0;
    } else if (c->mode == 2 || c->mode == 3) {
        pit_load(c);
    } else if (c->mode == 0 || c->mode == 4) {
        if (c->count_loaded)
            c->armed = 1;
    }
}

void __always_inline pico_x86_pit_set_speaker_control(uint8_t val)
{
    speaker_data_enable = (val >> 1) & 1;
    pit_set_gate2(val & 1);
}

static inline uint32_t __always_inline advance_pit_channel(pit_channel_t* c, uint32_t ticks)
{
    if (!c->armed || !c->gate) {
        return 0;
    }

    if (__builtin_expect(ticks <= c->pos, 1)) {
        c->pos -= ticks;
        return 0;
    }

    uint32_t max = effective_max(c);
    uint32_t remain = ticks - (c->pos + 1);
    uint32_t events = 1 + (remain / max);
    c->pos = max - 1 - (remain % max);

    if ((1 << c->mode) & 0x31) {
        c->output = 1;
        c->armed = 0;
        return 1;
    }

    if (events & 1) {
        c->output ^= 1;
    }

    return events;
}

uint32_t __always_inline pico_x86_pit_advance(uint32_t ticks)
{
    if (likely(ticks == 0)) {
        return 0;
    }

    uint32_t irq0_events = advance_pit_channel(&pit_channels[0], ticks);

    advance_pit_channel(&pit_channels[1], ticks);
    advance_pit_channel(&pit_channels[2], ticks);

    return irq0_events;
}

uint32_t __always_inline(pico_x86_pit_channel2_freq_hz)(void)
{
    if (!speaker_data_enable) {
        return 0;
    }

    pit_channel_t* c = &pit_channels[2];
    if (!c->armed || !c->gate) {
        return 0;
    }

    if (c->mode != 2 && c->mode != 3) {
        return 0;
    }

    uint32_t max = effective_max(c);
    if (max == 0) {
        return 0;
    }

    return (uint32_t)(PIT_BASE_HZ / max);
}

uint8_t __always_inline pico_x86_pit_channel2_output_bit() { return pit_channels[2].output; }
uint8_t __always_inline pico_x86_pit_speaker_data_enabled() { return speaker_data_enable; }

uint8_t __always_inline pico_x86_pit_get_port61_state(uint8_t current_port61_val)
{
    current_port61_val &= ~0x30;
    current_port61_val |= (pit_channels[1].output << 4);
    current_port61_val |= (pit_channels[2].output << 5);

    return current_port61_val;
}
