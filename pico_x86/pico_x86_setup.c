// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <pico/platform/compiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pico/aon_timer.h"
#include "pico/time.h"
#include "pico_x86.h"
#include "pico_x86_setup.h"
#include "pico_x86_video.h"
#include "picocalc_southbridge.h"

extern uint8_t *mem;
extern uint8_t io_ports[];

#define CGA_COLS 80
#define CGA_ROWS 25

#define COLOR_BLACK 0
#define COLOR_BLUE 1
#define COLOR_RED 4
#define COLOR_LGRAY 7
#define COLOR_YELLOW 14
#define COLOR_WHITE 15

// char color
#define ATTR(fg, bg) ((uint8_t)(((bg) << 4) | (fg)))

// box draw
#define CH_DTL 0xC9
#define CH_DTR 0xBB
#define CH_DBL 0xC8
#define CH_DBR 0xBC
#define CH_DH 0xCD
#define CH_DV 0xBA
#define CH_SH 0xC4
#define CH_SV 0xB3
#define CH_STL 0xDA
#define CH_STR 0xBF
#define CH_SBL 0xC0
#define CH_SBR 0xD9
#define CH_T_LEFT 0xC3
#define CH_T_RIGHT 0xB4
#define CH_T_DOWN 0xD1
#define CH_T_UP 0xC1
#define CH_T_L_DBL 0xC7
#define CH_T_R_DBL 0xB6
#define CH_CROSS 0xC5
#define CH_SEL_TRI 0x10

// keys
#define KEY_ESC 0x01
#define KEY_ENTER 0x1C
#define KEY_LEFT 0x4B
#define KEY_RIGHT 0x4D
#define KEY_UP 0x48
#define KEY_DOWN 0x50
#define KEY_PAGE_UP 0x49
#define KEY_PAGE_DOWN 0x51
#define KEY_MINUS 0x0C
#define KEY_PLUS_EQUALS 0x0D
#define KEY_KP_PLUS 0x4E
#define KEY_KP_MINUS 0x4A
#define KEY_F10 0x44

// main menu layout
#define MENU_COL_LEFT 5
#define MENU_COL_RIGHT 45
#define MENU_ROW_TOP 5
#define MENU_ROW_SAVE 11
#define MENU_ROW_STEP 2

// std (CMOS) menu layout
#define STD_LABEL_X 2
#define STD_VALUE_X 21
#define STD_ROW_DATE 4
#define STD_ROW_TIME 5
#define STD_ROW_FLOPPY 7
#define STD_ROW_POWER 8
#define STD_ROW_BOOT1 9
#define STD_ROW_BOOT2 10
#define STD_ROW_MEM_BASE 13
#define STD_ROW_MEM_EXT 14
#define STD_ROW_MEM_OTHER 15
#define STD_ROW_MEM_TOTAL 17

// memory info panel (right side box)
#define MEM_BOX_X 41
#define MEM_BOX_Y 12
#define MEM_BOX_W 38
#define MEM_BOX_H 7
#define MEM_ROW_SEP (MEM_BOX_Y + 4)
#define MEM_LABEL_X 46
#define MEM_VALUE_X 68

// BDA (BIOS Data Area) offsets, standard IBM PC layout
#define BDA_BASE_MEM_KB 0x413

typedef struct {
    struct tm time;
    int floppy_enabled;
    int fd_first;
    int power_action;
} setup_config_t;

typedef enum {
    ITEM_MONTH,
    ITEM_NUMBER_2,
    ITEM_NUMBER_02,
    ITEM_YEAR,
    ITEM_BOOLEAN,
    ITEM_BOOTDEV_1,
    ITEM_BOOTDEV_2,
    ITEM_POWER_ACTION
} item_type_t;

typedef enum { PAGE_MAIN, PAGE_CMOS, PAGE_COUNT } page_id_t;
typedef enum { ACTION_OPEN_PAGE, ACTION_SAVE_EXIT, ACTION_DISCARD_EXIT } menu_action_t;

typedef struct {
    uint8_t x, y;
    const char *label;
    const char *help;
    menu_action_t action;
    page_id_t target;
} main_menu_item_t;

typedef struct {
    uint8_t label_x, label_y;
    const char *label;
    uint8_t x, y;
    item_type_t type;
    int *var_ptr;
    int min_val;
    int max_val;
    const char *help;
} std_menu_item_t;

typedef struct {
    const char *title;
    uint8_t has_tree_divider;
    uint8_t has_change_hint;
    int item_count;
    int (*get_x)(int i);
    int (*get_y)(int i);
    void (*draw)(int cursor);
    void (*adjust)(int cursor, int delta); // NULL if page has no editable items
} page_t;

static setup_config_t g_cfg;

static const main_menu_item_t main_menu[] = {
    {MENU_COL_LEFT, MENU_ROW_TOP, "STANDARD CMOS SETUP", "Time, Date, Hard Disk Type...",
     ACTION_OPEN_PAGE, PAGE_CMOS},
    {MENU_COL_RIGHT, MENU_ROW_SAVE, "SAVE & EXIT SETUP", "Save Data to CMOS & Exit Setup",
     ACTION_SAVE_EXIT, PAGE_MAIN},
    {MENU_COL_RIGHT, MENU_ROW_SAVE + MENU_ROW_STEP, "EXIT WITHOUT SAVING",
     "Abandon all Data & Exit Setup", ACTION_DISCARD_EXIT, PAGE_MAIN},
};

#define MAIN_MENU_COUNT (count_of(main_menu))

static const std_menu_item_t std_menu[] = {
    {STD_LABEL_X, STD_ROW_DATE, "Date (mm:dd:yy)  : ", 26, STD_ROW_DATE, ITEM_MONTH,
     &g_cfg.time.tm_mon, 0, 11, "Change Month"},
    {0, 0, NULL, 30, STD_ROW_DATE, ITEM_NUMBER_2, &g_cfg.time.tm_mday, 1, 31, "Change Day"},
    {0, 0, NULL, 33, STD_ROW_DATE, ITEM_YEAR, &g_cfg.time.tm_year, 80, 137, "Change Year"},
    {STD_LABEL_X, STD_ROW_TIME, "Time (hh:mm:ss)  : ", STD_VALUE_X, STD_ROW_TIME, ITEM_NUMBER_02,
     &g_cfg.time.tm_hour, 0, 23, "Change Hour"},
    {0, 0, NULL, 26, STD_ROW_TIME, ITEM_NUMBER_02, &g_cfg.time.tm_min, 0, 59, "Change Minute"},
    {0, 0, NULL, 31, STD_ROW_TIME, ITEM_NUMBER_02, &g_cfg.time.tm_sec, 0, 59, "Change Second"},
    {STD_LABEL_X, STD_ROW_FLOPPY, "Floppy Drive A   : ", STD_VALUE_X, STD_ROW_FLOPPY, ITEM_BOOLEAN,
     &g_cfg.floppy_enabled, 0, 1, "Enable or Disable Floppy Drive A"},
    {STD_LABEL_X, STD_ROW_POWER, "Power Button     : ", STD_VALUE_X, STD_ROW_POWER,
     ITEM_POWER_ACTION, &g_cfg.power_action, 0, 1, "Set Power Key Action: Screenshot or Reboot"},
    {STD_LABEL_X, STD_ROW_BOOT1, "1st Boot Device  : ", STD_VALUE_X, STD_ROW_BOOT1, ITEM_BOOTDEV_1,
     &g_cfg.fd_first, 0, 1, "Configure Boot Device Priority"},
    {STD_LABEL_X, STD_ROW_BOOT2, "2nd Boot Device  : ", STD_VALUE_X, STD_ROW_BOOT2, ITEM_BOOTDEV_2,
     &g_cfg.fd_first, 0, 1, "Configure Boot Device Priority"}};

#define STD_MENU_COUNT (count_of(std_menu))

static void set_cursor_visible(int visible) {
    io_ports[0x3D4] = 0x0A;
    video_cga_port_out(0x3D4);
    io_ports[0x3D5] = visible ? 0x06 : 0x20;
    video_cga_port_out(0x3D5);
}

static void int10_write_char_attrib(int col, int row, char ch, uint8_t attr) {
    if (col < 0 || col >= CGA_COLS || row < 0 || row >= CGA_ROWS) {
        return;
    }
    uint32_t addr = MAP_ADDR(CGA_VRAM_ADDR + (((row * CGA_COLS) + col) * 2));
    mem[addr] = (uint8_t)ch;
    mem[addr + 1] = attr;
}

static void int10_write_string(int col, int row, const char *str, uint8_t attr) {
    for (int i = 0; str[i]; i++) {
        int10_write_char_attrib(col + i, row, str[i], attr);
    }
}

static void int10_fill_rect(int col, int row, int w, int h, char ch, uint8_t attr) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int10_write_char_attrib(col + x, row + y, ch, attr);
        }
    }
}

static void int10_draw_box(int col, int row, int w, int h, uint8_t attr) {
    int10_write_char_attrib(col, row, CH_DTL, attr);
    int10_write_char_attrib(col + w - 1, row, CH_DTR, attr);
    int10_write_char_attrib(col, row + h - 1, CH_DBL, attr);
    int10_write_char_attrib(col + w - 1, row + h - 1, CH_DBR, attr);
    for (int x = 1; x < w - 1; x++) {
        int10_write_char_attrib(col + x, row, CH_DH, attr);
        int10_write_char_attrib(col + x, row + h - 1, CH_DH, attr);
    }
    for (int y = 1; y < h - 1; y++) {
        int10_write_char_attrib(col, row + y, CH_DV, attr);
        int10_write_char_attrib(col + w - 1, row + y, CH_DV, attr);
    }
}

static void int10_draw_box_single(int col, int row, int w, int h, uint8_t attr) {
    int10_write_char_attrib(col, row, CH_STL, attr);
    int10_write_char_attrib(col + w - 1, row, CH_STR, attr);
    int10_write_char_attrib(col, row + h - 1, CH_SBL, attr);
    int10_write_char_attrib(col + w - 1, row + h - 1, CH_SBR, attr);
    for (int x = 1; x < w - 1; x++) {
        int10_write_char_attrib(col + x, row, CH_SH, attr);
        int10_write_char_attrib(col + x, row + h - 1, CH_SH, attr);
    }
    for (int y = 1; y < h - 1; y++) {
        int10_write_char_attrib(col, row + y, CH_SV, attr);
        int10_write_char_attrib(col + w - 1, row + y, CH_SV, attr);
    }
}

static void draw_hsep(int row, int gap_col, uint8_t attr) {
    for (int x = 1; x <= 78; x++) {
        if (x == gap_col) {
            continue;
        }
        int10_write_char_attrib(x, row, CH_SH, attr);
    }
    int10_write_char_attrib(0, row, CH_T_L_DBL, attr);
    int10_write_char_attrib(79, row, CH_T_R_DBL, attr);
}

static void draw_help_bar(const char *help) {
    int10_fill_rect(1, 22, 78, 2, ' ', ATTR(COLOR_WHITE, COLOR_BLUE));
    int10_write_string(40 - ((int)strlen(help) / 2), 22, help, ATTR(COLOR_YELLOW, COLOR_BLUE));
}

static int clamp_wrap(int val, int min, int max) {
    int range = max - min + 1;
    val -= min;
    val = ((val % range) + range) % range;
    return val + min;
}

static int get_wday(int d, int m, int y) {
    static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    y -= m < 3;
    return (y + (y / 4) - (y / 100) + (y / 400) + t[m - 1] + d) % 7;
}

static void draw_std_item(const std_menu_item_t *item, uint8_t label_attr, uint8_t val_attr) {
    static const char *mon_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[16] = {0};

    if (item->label) {
        int10_write_string(item->label_x, item->label_y, item->label, label_attr);
    }

    switch (item->type) {
    case ITEM_MONTH:
        snprintf(buf, sizeof(buf), "%s", mon_names[*(item->var_ptr)]);
        break;
    case ITEM_NUMBER_2:
        snprintf(buf, sizeof(buf), "%2d", *(item->var_ptr));
        break;
    case ITEM_NUMBER_02:
        snprintf(buf, sizeof(buf), "%02d", *(item->var_ptr));
        break;
    case ITEM_YEAR:
        snprintf(buf, sizeof(buf), "%4d", *(item->var_ptr) + 1900);
        break;
    case ITEM_BOOLEAN:
        snprintf(buf, sizeof(buf), "%s", *(item->var_ptr) ? "Enabled " : "Disabled");
        break;
    case ITEM_BOOTDEV_1:
        snprintf(buf, sizeof(buf), "%s", *(item->var_ptr) ? "Floppy" : "HDD   ");
        break;
    case ITEM_BOOTDEV_2:
        snprintf(buf, sizeof(buf), "%s", *(item->var_ptr) ? "HDD   " : "Floppy");
        break;
    case ITEM_POWER_ACTION:
        snprintf(buf, sizeof(buf), "%s",
                 *(item->var_ptr) == POWER_ACTION_REBOOT ? "Reboot    " : "Screenshot");
        break;
    }
    int10_write_string(item->x, item->y, buf, val_attr);
}

// per-page get_x/get_y/draw, referenced by the pages[] table below

static int main_get_x(int i) { return main_menu[i].x; }
static int main_get_y(int i) { return main_menu[i].y; }
static int cmos_get_x(int i) { return std_menu[i].x; }
static int cmos_get_y(int i) { return std_menu[i].y; }

static void draw_main_page(int cursor) {
    uint8_t bg_attr = ATTR(COLOR_WHITE, COLOR_BLUE);
    uint8_t val_attr = ATTR(COLOR_YELLOW, COLOR_BLUE);
    uint8_t sel_attr = ATTR(COLOR_WHITE, COLOR_RED);

    for (int i = 0; i < MAIN_MENU_COUNT; i++) {
        uint8_t attr = (i == cursor) ? sel_attr : val_attr;
        int width = (int)strlen(main_menu[i].label);
        int10_fill_rect(main_menu[i].x - 1, main_menu[i].y, width + 2, 1, ' ', bg_attr);
        if (i == cursor) {
            int10_fill_rect(main_menu[i].x - 1, main_menu[i].y, width + 2, 1, ' ', sel_attr);
        }
        int10_write_string(main_menu[i].x, main_menu[i].y, main_menu[i].label, attr);
    }

    draw_help_bar(main_menu[cursor].help);
}

static void draw_memory_info(void) {
    uint8_t label_attr = ATTR(COLOR_WHITE, COLOR_BLUE);
    uint8_t val_attr = ATTR(COLOR_YELLOW, COLOR_BLUE);
    char buf[8];

    int base_kb = mem[MAP_ADDR(BDA_BASE_MEM_KB)] | (mem[MAP_ADDR(BDA_BASE_MEM_KB + 1)] << 8);
    int ext_kb = 0;
    int total_kb = LOW_MEM_LIMIT / 1024;
    int other_kb = total_kb - base_kb;
    if (other_kb < 0) {
        other_kb = 0;
    }

    int10_draw_box_single(MEM_BOX_X, MEM_BOX_Y, MEM_BOX_W, MEM_BOX_H, label_attr);

    int10_write_char_attrib(MEM_BOX_X, MEM_ROW_SEP, CH_T_LEFT, label_attr);
    int10_write_char_attrib(MEM_BOX_X + MEM_BOX_W - 1, MEM_ROW_SEP, CH_T_RIGHT, label_attr);
    for (int x = 1; x < MEM_BOX_W - 1; x++) {
        int10_write_char_attrib(MEM_BOX_X + x, MEM_ROW_SEP, CH_SH, label_attr);
    }

    int10_write_string(MEM_LABEL_X, STD_ROW_MEM_BASE, "Base Memory:", label_attr);
    snprintf(buf, sizeof(buf), "%dK", base_kb);
    int10_write_string(MEM_VALUE_X, STD_ROW_MEM_BASE, buf, val_attr);

    int10_write_string(MEM_LABEL_X, STD_ROW_MEM_EXT, "Extended Memory:", label_attr);
    snprintf(buf, sizeof(buf), "%dK", ext_kb);
    int10_write_string(MEM_VALUE_X, STD_ROW_MEM_EXT, buf, val_attr);

    int10_write_string(MEM_LABEL_X, STD_ROW_MEM_OTHER, "Other Memory:", label_attr);
    snprintf(buf, sizeof(buf), "%dK", other_kb);
    int10_write_string(MEM_VALUE_X, STD_ROW_MEM_OTHER, buf, val_attr);

    int10_write_string(MEM_LABEL_X, STD_ROW_MEM_TOTAL, "Total Memory:", label_attr);
    snprintf(buf, sizeof(buf), "%dK", base_kb + ext_kb + other_kb);
    int10_write_string(MEM_VALUE_X, STD_ROW_MEM_TOTAL, buf, val_attr);
}

static void draw_cmos_page(int cursor) {
    static const char *day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    uint8_t label_attr = ATTR(COLOR_WHITE, COLOR_BLUE);
    uint8_t val_attr = ATTR(COLOR_YELLOW, COLOR_BLUE);
    uint8_t sel_attr = ATTR(COLOR_WHITE, COLOR_RED);

    char buf[32];
    int wday = get_wday(g_cfg.time.tm_mday, g_cfg.time.tm_mon + 1, g_cfg.time.tm_year + 1900);
    snprintf(buf, sizeof(buf), "%s, ", day_names[wday]);

    int10_write_string(21, STD_ROW_DATE, buf, val_attr);
    int10_write_string(29, STD_ROW_DATE, " ", val_attr);
    int10_write_string(32, STD_ROW_DATE, " ", val_attr);
    int10_write_string(23, STD_ROW_TIME, " : ", val_attr);
    int10_write_string(28, STD_ROW_TIME, " : ", val_attr);

    for (int i = 0; i < STD_MENU_COUNT; i++) {
        uint8_t attr = (i == cursor) ? sel_attr : val_attr;
        draw_std_item(&std_menu[i], label_attr, attr);
    }

    draw_memory_info();

    draw_help_bar(std_menu[cursor].help);
}

static void std_adjust_value(int cursor, int delta) {
    const std_menu_item_t *item = &std_menu[cursor];
    if (item->type == ITEM_BOOLEAN || item->type == ITEM_BOOTDEV_1 ||
        item->type == ITEM_BOOTDEV_2 || item->type == ITEM_POWER_ACTION) {
        *(item->var_ptr) = !(*(item->var_ptr));
    } else {
        *(item->var_ptr) = clamp_wrap(*(item->var_ptr) + delta, item->min_val, item->max_val);
    }
}

static const page_t pages[PAGE_COUNT] = {
    [PAGE_MAIN] = {"PicoCalc x86 BIOS Setup Utility", 1, 0, MAIN_MENU_COUNT, main_get_x, main_get_y,
                   draw_main_page, NULL},
    [PAGE_CMOS] = {"STANDARD CMOS SETUP", 0, 1, STD_MENU_COUNT, cmos_get_x, cmos_get_y,
                   draw_cmos_page, std_adjust_value},
};

static void draw_award_frame(page_id_t page) {
    uint8_t bg_attr = ATTR(COLOR_WHITE, COLOR_BLUE);
    int10_fill_rect(0, 0, CGA_COLS, CGA_ROWS, ' ', bg_attr);

    const char *bios_info = (const char *)&mem[MAP_ADDR(0xF012A)];
    const char *bios_info2 = (const char *)&mem[MAP_ADDR(0xF0145)];
    const char *bios_date = (const char *)&mem[MAP_ADDR(0xFFFF5)];

    char buf[81] = {0};
    snprintf(buf, sizeof(buf), " %s %s, %s", bios_info, bios_date, bios_info2);
    int10_write_string(4, 0, buf, bg_attr);

    const char *title = pages[page].title;
    int10_write_string((CGA_COLS - strlen(title)) / 2, 1, title, bg_attr);

    int10_draw_box(0, 2, CGA_COLS, 23, bg_attr);

    if (pages[page].has_tree_divider) {
        for (int y = 3; y <= 17; y++) {
            int10_write_char_attrib(39, y, CH_SV, bg_attr);
        }
        int10_write_char_attrib(39, 2, CH_T_DOWN, bg_attr);
        int10_write_char_attrib(39, 18, CH_T_UP, bg_attr);
    }

    draw_hsep(18, pages[page].has_tree_divider ? 39 : -1, bg_attr);

    int10_write_string(2, 19, "Esc : Quit", bg_attr);
    int10_write_string(2, 20, "F10 : Save & Exit Setup", bg_attr);
    int10_write_string(41, 19, "\x18 \x19 \x1A \x1B : Select Item", bg_attr);
    if (pages[page].has_change_hint) {
        int10_write_string(41, 20, "+ / -   : Change Value", bg_attr);
    }

    draw_hsep(21, -1, bg_attr);
}

static void draw_award_menu(page_id_t page, int cursor) { pages[page].draw(cursor); }

static uint8_t wait_key(void) {
    for (;;) {
        sleep_ms(16);
        static int32_t kbd_event = 0;
        if (queue_try_remove(kbd_queue, &kbd_event)) {
            if (KBD_GET_STATE(kbd_event) == KBD_STATE_RELEASE) {
                continue;
            }
            return (uint8_t)(kbd_event & 0xFF);
        }
    }
}

static int nav_move(page_id_t page, int cursor, int dx, int dy) {
    int count = pages[page].item_count;
    int best_dist = 99999;
    int next = cursor;
    int cx = pages[page].get_x(cursor);
    int cy = pages[page].get_y(cursor);

    for (int i = 0; i < count; i++) {
        if (i == cursor) {
            continue;
        }
        int ix = pages[page].get_x(i);
        int iy = pages[page].get_y(i);

        if (dx > 0 && (iy != cy || ix < cx)) {
            continue;
        }
        if (dx < 0 && (iy != cy || ix > cx)) {
            continue;
        }
        if (dy > 0 && iy <= cy) {
            continue;
        }
        if (dy < 0 && iy >= cy) {
            continue;
        }

        int dist = (dx != 0) ? abs(ix - cx) : ((abs(iy - cy) * 100) + abs(ix - cx));
        if (dist < best_dist) {
            best_dist = dist;
            next = i;
        }
    }

    return (next != cursor) ? next : ((cursor + (dx > 0 || dy > 0 ? 1 : count - 1)) % count);
}

static uint8_t commit_and_exit(const setup_config_t *cfg) {
    set_cursor_visible(1);
    pico_x86_set_floppy_enabled(cfg->floppy_enabled);
    pico_x86_set_power_action(cfg->power_action);

    struct tm t = cfg->time;
    if (aon_timer_is_running()) {
        aon_timer_set_time_calendar(&t);
    } else {
        aon_timer_start_calendar(&t);
    }

    while (queue_try_remove(kbd_queue, nullptr)) {
        sleep_ms(30);
    }
    return cfg->fd_first ? 0x00 : 0x80;
}

static uint8_t finish_setup(const setup_config_t *cfg) {
    int10_fill_rect(0, 0, CGA_COLS, CGA_ROWS, ' ', ATTR(COLOR_WHITE, COLOR_BLACK));
    return commit_and_exit(cfg);
}

uint8_t pico_x86_bios_setup_menu(void) {
    set_cursor_visible(0);

    g_cfg.floppy_enabled = pico_x86_get_floppy_enabled();
    g_cfg.fd_first = 0;
    g_cfg.power_action = pico_x86_get_power_action();

    if (!aon_timer_get_time_calendar(&g_cfg.time)) {
        memset(&g_cfg.time, 0, sizeof(g_cfg.time));
    }

    setup_config_t original = g_cfg;
    while (picocalc_southbridge_kb_read() != -1) {
        sleep_ms(10);
    }

    page_id_t page = PAGE_MAIN;
    int cursors[PAGE_COUNT] = {0};
    draw_award_frame(page);

    for (;;) {
        draw_award_menu(page, cursors[page]);
        uint8_t key = wait_key();

        switch (key) {
        case KEY_ESC:
            if (page != PAGE_MAIN) {
                page = PAGE_MAIN;
                draw_award_frame(page);
            } else {
                return finish_setup(&original);
            }
            break;

        case KEY_F10:
            return finish_setup(&g_cfg);

        case KEY_UP:
            cursors[page] = nav_move(page, cursors[page], 0, -1);
            break;
        case KEY_DOWN:
            cursors[page] = nav_move(page, cursors[page], 0, 1);
            break;
        case KEY_LEFT:
            cursors[page] = nav_move(page, cursors[page], -1, 0);
            break;
        case KEY_RIGHT:
            cursors[page] = nav_move(page, cursors[page], 1, 0);
            break;

        case KEY_KP_PLUS:
        case KEY_PAGE_UP:
        case KEY_PLUS_EQUALS:
            if (pages[page].adjust) {
                pages[page].adjust(cursors[page], 1);
            }
            break;

        case KEY_KP_MINUS:
        case KEY_PAGE_DOWN:
        case KEY_MINUS:
            if (pages[page].adjust) {
                pages[page].adjust(cursors[page], -1);
            }
            break;

        case KEY_ENTER:
            if (page == PAGE_MAIN) {
                const main_menu_item_t *item = &main_menu[cursors[PAGE_MAIN]];
                switch (item->action) {
                case ACTION_OPEN_PAGE:
                    page = item->target;
                    draw_award_frame(page);
                    break;
                case ACTION_SAVE_EXIT:
                    return finish_setup(&g_cfg);
                case ACTION_DISCARD_EXIT:
                    return finish_setup(&original);
                }
            } else if (pages[page].adjust) {
                pages[page].adjust(cursors[page], 1);
            }
            break;

        default:
            break;
        }
    }
}
