// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#include "pico_x86_audio.h"
#include "pico_x86_pit.h"

#include <math.h>

#define SPKR_AMPLITUDE 180

static uint8_t audio_pin_l, audio_pin_r;
static uint32_t pwm_smpl_hz;

static uint16_t wave_counter;
static uint8_t speaker_high = 0;

static void picocalc_sound_init(irq_handler_t my_handler)
{
    gpio_set_function(audio_pin_l, GPIO_FUNC_PWM);
    gpio_set_function(audio_pin_r, GPIO_FUNC_PWM);

    const int slice_l = pwm_gpio_to_slice_num(audio_pin_l);
    const int slice_r = pwm_gpio_to_slice_num(audio_pin_r);

    pwm_clear_irq(slice_l);
    pwm_set_irq_enabled(slice_l, true);

    if (slice_l != slice_r) {
        pwm_clear_irq(slice_r);
        pwm_set_irq_enabled(slice_r, false);
    }

    irq_set_exclusive_handler(PWM_IRQ_WRAP, my_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    uint32_t sys_hz = clock_get_hz(clk_sys);
    float clkdiv_raw = (float)sys_hz / ((float)PWM_SAMPLE_HZ * (PWM_WRAP + 1));
    float clkdiv = roundf(clkdiv_raw * 16.0f) / 16.0f;
    pwm_smpl_hz = (uint32_t)(sys_hz / (clkdiv * (PWM_WRAP + 1)));

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, clkdiv);
    pwm_config_set_wrap(&config, PWM_WRAP);

    pwm_init(slice_l, &config, true);
    if (slice_l != slice_r) {
        pwm_init(slice_r, &config, true);
    }

    pwm_set_chan_level(slice_l, pwm_gpio_to_channel(audio_pin_l), 0);
    pwm_set_chan_level(slice_r, pwm_gpio_to_channel(audio_pin_r), 0);
}

static void __isr __time_critical_func(pico_x86_speaker_irq)()
{
    pwm_clear_irq(pwm_gpio_to_slice_num(audio_pin_l));

    uint16_t level = 0;

    if (pico_x86_pit_speaker_data_enabled()) {
        uint32_t freq = pico_x86_pit_channel2_freq_hz();

        if (freq) {
            uint32_t half_period = pwm_smpl_hz / (2 * freq);
            if (half_period == 0)
                half_period = 1;

            if (++wave_counter >= half_period) {
                wave_counter = 0;
                speaker_high ^= 1;
            }
        } else {
            wave_counter = 0;
            speaker_high = pico_x86_pit_channel2_output_bit();
        }

        level = speaker_high ? SPKR_AMPLITUDE : 0;
    } else {
        speaker_high = 0;
        wave_counter = 0;
    }

    pwm_set_chan_level(pwm_gpio_to_slice_num(audio_pin_l), pwm_gpio_to_channel(audio_pin_l), level);
    pwm_set_chan_level(pwm_gpio_to_slice_num(audio_pin_r), pwm_gpio_to_channel(audio_pin_r), level);
}

void pico_x86_audio_init(uint8_t audio_pin_left, uint8_t audio_pin_right)
{
    audio_pin_l = audio_pin_left;
    audio_pin_r = audio_pin_right;

    picocalc_sound_init(pico_x86_speaker_irq);
}
