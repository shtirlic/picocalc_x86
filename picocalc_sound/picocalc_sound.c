// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <math.h>
#include "picocalc_sound.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"

uint32_t pwm_sample_hz;

void picocalc_sound_init(irq_handler_t my_handler)
{
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

    const int slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
    const int slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);

    pwm_clear_irq(slice_l);
    pwm_clear_irq(slice_r);
    pwm_set_irq_enabled(slice_l, true);
    pwm_set_irq_enabled(slice_r, true);

    irq_set_exclusive_handler(PWM_IRQ_WRAP, my_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    uint32_t sys_hz = clock_get_hz(clk_sys);
    float clkdiv_raw = (float)sys_hz / ((float)PWM_SAMPLE_HZ * (PWM_WRAP + 1));
    float clkdiv = roundf(clkdiv_raw * 16.0f) / 16.0f;
    pwm_sample_hz = (uint32_t)(sys_hz / (clkdiv * (PWM_WRAP + 1)));

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, clkdiv);
    pwm_config_set_wrap(&config, PWM_WRAP);

    pwm_init(slice_l, &config, true);
    pwm_init(slice_r, &config, true);

    pwm_set_chan_level(slice_l, PWM_CHAN_A, 0);
    pwm_set_chan_level(slice_r, PWM_CHAN_B, 0);
}
