// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <string.h>
#include "hardware/gpio.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"
#include <pico/multicore.h>
#include "pico/aon_timer.h"
#include "pico/util/datetime.h"

#include "psram_spi.h"
#include "tf_card.h"
#include "ff.h"
#include "picocalc_display.h"
#include "picocalc_southbridge.h"
#include "picocalc_sound.h"

#include "pico_x86.h"
#include "pico_x86_video.h"

#include "splash.h"

psram_spi_inst_t psram_spi;

extern uint8_t mem[];
extern uint8_t io_ports[];

static uint32_t video_frames;
static float target_fps = 60.0f;

static FATFS fs;

#define PIT_BASE_HZ 1193182UL
#define SPKR_AMPLITUDE 180 // headroom under wrap=250;

static uint16_t wave_counter;
static uint8_t speaker_high = 0; // current output state
static struct repeating_timer xt_timer;

// ISR every ~54.9ms (18.2Hz)
static bool __time_critical_func(xt_timer_callback)(struct repeating_timer* t)
{
    pico_x86_timer_tick();
    return true;
}

static void __isr(pico_x86_speaker_irq)()
{
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN_L));
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN_R));

    uint16_t level = 0;

    if ((io_ports[0x61] & 0x03) == 0x03) {
        uint16_t reload = mem[0x4AA] | (mem[0x4AB] << 8);
        if (reload == 0)
            reload = 0xFFFF;

        uint32_t half_period = (pwm_sample_hz * (uint32_t)reload) / (2 * PIT_BASE_HZ);
        if (half_period == 0)
            half_period = 1;

        if (++wave_counter >= half_period) {
            wave_counter = 0;
            speaker_high ^= 1;
        }
        level = speaker_high ? SPKR_AMPLITUDE : 0;
    } else {
        speaker_high = 0;
    }

    pwm_set_chan_level(pwm_gpio_to_slice_num(AUDIO_PIN_L), PWM_CHAN_A, level);
    pwm_set_chan_level(pwm_gpio_to_slice_num(AUDIO_PIN_R), PWM_CHAN_B, level);
}

static void(second_core)()
{
    const uint64_t frame_duration_us = 1000000ULL / target_fps;

    add_repeating_timer_us(-54925, xt_timer_callback, NULL, &xt_timer);

    uint64_t last_frame_tick = time_us_64();

    video_cga_set_resolution(SCREEN_WIDTH, SCREEN_HEIGHT);

    picocalc_display_show_image(image_data_splash, sizeof(image_data_splash));
    sleep_ms(500);

    while (1) {
        const uint64_t current_tick = time_us_64();
        if (current_tick - last_frame_tick >= frame_duration_us) {
            //       printf("Refresh LCD\n");
            //       mem[0x44E] = 0;
            // uint16_t crtc_offset = mem[MAP_ADDR(0x4AC)] | (mem[MAP_ADDR(0x4AD)] << 8);
            // uint16_t page_offset = mem[MAP_ADDR(0x44E)] | (mem[MAP_ADDR(0x44F)] << 8);
            // printf("Page Offset: %d CRTC Offset: %d\n", page_offset, crtc_offset);

            // uint8_t video_mode = mem[MAP_ADDR(0x449)];
            // printf("Video mode: %d \n", video_mode);

            video_display_reset(picocalc_display_reset);
            picocalc_display_begin_frame();
            video_cga_render(picocalc_display_put_color);
            video_frames++;
            last_frame_tick += frame_duration_us;
        }
        //     busy_wait_ms(1000);
        tight_loop_contents();
    }
    __unreachable();
}

static void loop()
{
    while (1) {
        pico_x86_run();
        tight_loop_contents();
    }
    __unreachable();
}

static void init_sound()
{
    printf("%s", "\n▼ Sound Init...");
    picocalc_sound_init(pico_x86_speaker_irq);
    printf("%s", "done\n");
}

static void init_sothbridge()
{
    printf("%s", "\n▼ Southbridge Init...");
    picocalc_southbridge_init();
    printf("%s", "done\n");
}

static void init_display()
{
    printf("%s", "\n▼ Display Init...");

    picocalc_display_init();
    multicore_reset_core1();
    multicore_launch_core1(second_core);
    printf("%s", "done\n");
}

static void init_ram()
{
    printf("%s", "▼ PSRAM Init...");
    // psram_spi = psram_qpi_init(pio1, -1);
    // psram_spi_uninit(psram_spi);
    psram_spi = psram_spi_init(pio1, -1);
    // psram_spi_uninit(psram_spi);
    // psram_spi = psram_qpi_init(pio1, -1);

    // if (init_psram()) {
    printf("%s", "done\n");
    // } else {
    // printf("%s", "failed\n");
    // }
}

static void init_fs()
{
    printf("%s", "▼ SD Card Init...");

    FATFS* lfs;

    FIL fil;
    DIR dp;
    pico_fatfs_spi_config_t config = {
        spi0, // if unmatched SPI pin assignments with spi0/spi1 or explicitly
              // designated as NULL, SPI PIO will be configured
        CLK_SLOW_DEFAULT, CLK_FAST_DEFAULT,
        16, // SPIx_RX
        17,
        18, // SPIx_SCK
        19, // SPIx_TX
        true // use internal pullup
    };
    DWORD fre_clust;
    bool spi_configured = pico_fatfs_set_config(&config);

    FRESULT fr = f_mount(&fs, "0:", 1);
    fr = f_getfree("0:", &fre_clust, &lfs);
    const DWORD tot_sect = (lfs->n_fatent - 2) * lfs->csize;
    const DWORD fre_sect = fre_clust * lfs->csize;
    printf("%s", "done\n");
    printf("▼ %10lu KiB total drive space.\n▼ %10lu KiB available.\n", tot_sect / 2, fre_sect / 2);
    // f_unmount("");                 /* Unmount the default drive */
}

static void init()
{
    set_sys_clock_khz(230000, true);
    // set_sys_clock_khz(150000, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    aon_timer_start_with_timeofday();

    stdio_init_all();
    // setvbuf(stdout, NULL, _IONBF, 0);

    printf("%s", "\n\n▼ PicoCalc x86 Version 0.1\n");

    printf("%s", "\n▼ PicoCalc Init... \n");
    init_ram();
    init_fs();
    init_sothbridge();
    init_display();
    init_sound();
    printf("%s", "\n▼ PicoCalc Ready... \n");
}

int main()
{
    init();
    loop();
    __unreachable();
}
