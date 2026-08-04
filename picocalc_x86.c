// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hardware/structs/bus_ctrl.h"
#include "pico/aon_timer.h"
#include <hardware/clocks.h>
#include <pico/multicore.h>
#include <stdint.h>
#include <stdio.h>

#include "ff.h"
#include "picocalc_display.h"
#include "picocalc_southbridge.h"
#include "psram_spi.h"
#include "tf_card.h"

#include "pico_x86.h"
#include "pico_x86_audio.h"
#include "pico_x86_pit.h"
#include "pico_x86_video.h"

#include "splash.h"

psram_spi_inst_t psram_spi;

static FATFS fs;

static void init_sound()
{
    printf("\n▼ Sound Init...");
    pico_x86_audio_init(AUDIO_PIN_L, AUDIO_PIN_R);
    printf("done\n");
}

static void init_sothbridge()
{
    printf("\n▼ Southbridge Init...");
    picocalc_southbridge_init();
    printf("done\n");
}

static void init_display()
{
    printf("\n▼ Display Init...");
    picocalc_display_init();
    picocalc_display_show_image(image_data_splash, sizeof(image_data_splash));
    sleep_ms(500);
    printf("done\n");
}

// static uint16_t buffer[320];
// static void put_color_in_buffer(uint16_t color) { buffer[current_scanline] = color; }

static void __time_critical_func(display_render)()
{
    Video_Config video_config = {
        .display_reset_callback = picocalc_display_reset,
        .display_begin_frame_callback = picocalc_display_begin_frame,
        .display_end_frame_callback = picocalc_display_end_frame,
        .display_put_color_callback = picocalc_display_put_color,
        // .display_put_color_callback = put_color_in_buffer,
        .screen_height = SCREEN_HEIGHT,
        .screen_width = SCREEN_WIDTH,
    };
    pico_x86_video_set_config(&video_config);
    pico_x86_video_display_init();
    pico_x86_video_render(); // rendering loop
    __unreachable();
}

static void __time_critical_func(second_core)()
{
    init_sothbridge();
    init_sound();
    pico_x86_pit_timer_init();
    init_display();
    display_render();
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

static void init_peripherals()
{
    printf("\n▼ Peripherals Init...");
    multicore_reset_core1();
    multicore_launch_core1(second_core);
    printf("done\n");
}

static void init_ram()
{
    printf("\n▼ PSRAM Init...");
    // psram_spi = psram_qpi_init(pio1, -1);
    // psram_spi_uninit(psram_spi);
    psram_spi = psram_spi_init(pio1, -1);
    // psram_spi_uninit(psram_spi);
    // psram_spi = psram_qpi_init(pio1, -1);

    // if (init_psram()) {
    printf("done\n");
    // } else {
    // printf("%s", "failed\n");
    // }
}

static void init_fs()
{
    printf("\n▼ SD Card Init...");

    FATFS* lfs;

    // FIL fil;
    // DIR dp;
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
    printf("done\n");
    printf(
        "▼ %10d KiB total drive space.\n▼ %10d KiB available.\n", (tot_sect / 2), (fre_sect / 2));
    // f_unmount("");                 /* Unmount the default drive */
}

static void init_system()
{
    set_sys_clock_hz(PICO_SYS_CLOCK_MHZ * MHZ, true);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    aon_timer_start_with_timeofday();

#ifdef DEBUG_CONSOLE
    stdio_init_all();
    // setvbuf(stdout, NULL, _IONBF, 0);
#endif
}

static void init()
{
    init_system();
    printf("\n\n▼ PicoCalc x86 Version: %s , Build: %s \n", APP_VERSION, __DATE__);
    printf("\n▼ PicoCalc Init... \n");
    init_ram();
    init_fs();
    init_peripherals();
    printf("\n▼ PicoCalc Ready... \n");
}

int main()
{
    init();
    loop();
    __unreachable();
}
