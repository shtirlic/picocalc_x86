// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hardware/structs/bus_ctrl.h"
#include "pico/aon_timer.h"
#include "pico/rand.h"
#include "pico/util/queue.h"
#include <hardware/clocks.h>
#include <pico/multicore.h>
#include <pico/platform/common.h>
#include <pico/time.h>
#include <stdint.h>
#include <stdio.h>

#include "ff.h"
#include "picocalc_display.h"
#include "picocalc_southbridge.h"
#include "picocalc_x86_screenshot.h"
#include "psram_spi.h"
#include "tf_card.h"

#include "pico_x86.h"
#include "pico_x86_audio.h"
#include "pico_x86_pit.h"
#include "pico_x86_video.h"

#include "splash.h"

psram_spi_inst_t psram_spi;
static FATFS fs;

static void init_sound() {
    printf("\n▼ Sound Init...");
    pico_x86_audio_init(AUDIO_PIN_L, AUDIO_PIN_R);
    printf("done\n");
}

static void init_display() {
    printf("\n▼ Display Init...");
    picocalc_display_init();
    picocalc_display_show_image(image_data_splash, sizeof(image_data_splash));
    sleep_ms(500);
    printf("done\n");
}

static void video_screnshot_begin() {
    char screenshot_file_path[40];
    uint32_t random_id = get_rand_32();
    snprintf(screenshot_file_path, sizeof(screenshot_file_path),
             "0:/x86/screenshots/screen_%08X.bmp", random_id);
    screenshot_create_file(screenshot_file_path, SCREEN_WIDTH, SCREEN_HEIGHT);
}

void video_config_update(void *video_cfg) {
    Video_Config *cfg = (Video_Config *)video_cfg;
    if (cfg->state == VID_CFG_NEED_UPDATE) {
        cfg->display_begin_frame_callback = video_screnshot_begin;
        cfg->display_end_frame_callback = screenshot_close_file;
        cfg->display_put_pixel_callback = screenshot_write_pixel;
    } else {
        cfg->display_begin_frame_callback = picocalc_display_begin_frame;
        cfg->display_end_frame_callback = picocalc_display_end_frame;
        cfg->display_put_pixel_callback = picocalc_display_put_color;
    }
}

static Video_Config video_config = {
    .display_update_video_config_callback = video_config_update,
    .display_begin_frame_callback = picocalc_display_begin_frame,
    .display_end_frame_callback = picocalc_display_end_frame,
    .display_put_pixel_callback = picocalc_display_put_color,
    .screen_height = SCREEN_HEIGHT,
    .screen_width = SCREEN_WIDTH,
    .state = VID_CFG_NO_UPDATE,
};

static void __time_critical_func(display_render)() {
    pico_x86_video_set_config(&video_config);
    pico_x86_video_display_init();
    pico_x86_video_render(); // rendering loop
    __unreachable();
}

static bool __time_critical_func(sb_timer_callback)(struct repeating_timer *t) {
    int32_t e = picocalc_southbridge_kb_read();
    if (e != -1 && kbd_queue) {
        queue_try_add(kbd_queue, &e);
    }
    return true;
}

static struct repeating_timer sb_timer;
static void init_sothbridge() {
    printf("\n▼ Southbridge Init...");
    picocalc_southbridge_init();

    alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(1);
    alarm_pool_add_repeating_timer_ms(pool, 16, sb_timer_callback, NULL, &sb_timer);

    printf("done\n");
}

static void __time_critical_func(second_core)() {
    init_sothbridge();
    init_display();
    display_render();
    __unreachable();
}

static void loop() {
    while (1) {
        pico_x86_run();
        tight_loop_contents();
    }
    __unreachable();
}

static void init_peripherals() {
    printf("\n▼ Peripherals Init...");
    pico_x86_pit_timer_init();
    init_sound();

    multicore_reset_core1();
    multicore_launch_core1(second_core);
    printf("done\n");
}

static void init_ram() {
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

static void init_fs() {
    printf("\n▼ SD Card Init...");

    FATFS *lfs;

    // FIL fil;
    // DIR dp;
    pico_fatfs_spi_config_t config = {
        spi0, // if unmatched SPI pin assignments with spi0/spi1 or explicitly
              // designated as NULL, SPI PIO will be configured
        CLK_SLOW_DEFAULT, CLK_FAST_DEFAULT,
        16, // SPIx_RX
        17,
        18,  // SPIx_SCK
        19,  // SPIx_TX
        true // use internal pullup
    };
    DWORD fre_clust;
    bool spi_configured = pico_fatfs_set_config(&config);

    FRESULT fr = f_mount(&fs, "0:", 1);
    fr = f_getfree("0:", &fre_clust, &lfs);
    const DWORD tot_sect = (lfs->n_fatent - 2) * lfs->csize;
    const DWORD fre_sect = fre_clust * lfs->csize;
    printf("▼ %10d KiB total drive space.\n▼ %10d KiB available.\n", (tot_sect / 2),
           (fre_sect / 2));
    // f_unmount("");                 /* Unmount the default drive */
    fr = f_mkdir("0:/x86/screenshots");
    if (fr == FR_OK || fr == FR_EXIST) {
        printf("screenshots dir created.");
    } else {
        printf("Could not create screenshots dir.");
        while (1) {
            tight_loop_contents();
        };
    }
    printf("done\n");
}

static void init_system() {
    set_sys_clock_hz(PICO_SYS_CLOCK_MHZ * MHZ, true);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

#ifdef DEBUG_CONSOLE
    stdio_init_all();
    // setvbuf(stdout, NULL, _IONBF, 0);
#endif
}

static void init() {
    init_system();
    printf("\n\n▼ PicoCalc x86 Version: %s , Build: %s \n", APP_VERSION, __DATE__);
    printf("\n▼ PicoCalc Init... \n");
    init_ram();
    init_fs();
    init_peripherals();
    printf("\n▼ PicoCalc Ready... \n");
}

int main() {
    init();
    loop();
    __unreachable();
}
