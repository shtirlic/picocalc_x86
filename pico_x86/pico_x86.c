/*
 * Portions of this file are derived from 8086tiny.
 * Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
 * Licensed under the MIT License.
 * See LICENSE.txt.
 *
 * Modifications Copyright (c) 2026 Serg Podtynnyi
 * Licensed under GPLv3 for the combined work.
 */

#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/aon_timer.h"
#include "pico/util/queue.h"
#include <pico/platform/sections.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "picocalc_southbridge.h"

#include "pico_x86.h"
#include "pico_x86_floppy.h"
#include "pico_x86_keyb.h"
#include "pico_x86_pit.h"
#include "pico_x86_serial.h"
#include "pico_x86_setup.h"
#include "pico_x86_video.h"

static uint8_t __aligned(4) sram[RAM_SIZE];
uint8_t __scratch_y("cpu") *mem = nullptr;

uint8_t __aligned(4) __scratch_x("io") io_ports[IO_PORT_COUNT] = {0};

// Group 1: ModR/M Decode (Tables 0-7) - Indexed by 3-bit i_rm (0-7)
static uint8_t __scratch_y("cpu") rm_decode_table[8][8];

// Group 2: Opcode Decode (Tables 8, 9, 10, 14) - Indexed by 8-bit opcode (0-255)
static opcode_decode_t __scratch_y("cpu") op_decode_table[256];

// Group 3: Instruction Size (Tables 12, 13) - Indexed by 8-bit opcode (0-255)
static inst_size_t __scratch_y("cpu") inst_size_table[256];

// Group 4: Jump Logic (Tables 15-18) - Indexed by 3-bit condition (0-7)
static uint8_t __scratch_y("cpu") jmp_decode_table[4][8];

static opcode_decode_t __scratch_y("cpu") CPU_OPCODE = {0};

static uint8_t __scratch_y("cpu") * opcode_stream, raw_opcode_id,
    seg_override_en, i_rm, i_w, i_reg, i_mod, i_mod_size, i_d, i_reg4bit, rep_mode,
    rep_override_en, trap_flag, scratch_uchar, io_hi_lo, spkr_en, shift_count, int8_asap;

uint8_t *__scratch_y("cpu") regs8 = nullptr;
static uint8_t power_action = POWER_ACTION_REBOOT;

uint16_t *__scratch_y("cpu") regs16 = nullptr;
uint16_t __scratch_y("cpu") seg_override;

static uint32_t __scratch_y("cpu") op_source, op_dest, rm_addr, op_to_addr,
    op_from_addr, i_data0, i_data1, i_data2, scratch_uint, scratch2_uint;
int32_t __scratch_y("cpu") op_result, scratch_int;

struct timespec __scratch_y() ts;
struct tm __scratch_y() clock_tm;
static bool __scratch_y() isr_ready;

static queue_t __scratch_y() local_kbd_queue;
queue_t __scratch_y() *kbd_queue = nullptr;

// Helper macros

typedef int8_t __attribute__((aligned(1), may_alias)) unaligned_int8_t;
typedef uint8_t __attribute__((aligned(1), may_alias)) unaligned_uint8_t;
typedef int16_t __attribute__((aligned(1), may_alias)) unaligned_int16_t;
typedef uint16_t __attribute__((aligned(1), may_alias)) unaligned_uint16_t;
typedef int32_t __attribute__((aligned(1), may_alias)) unaligned_int32_t;
typedef uint32_t __attribute__((aligned(1), may_alias)) unaligned_uint32_t;

#define CAST(type) *(unaligned_##type *)&

// clang-format off

// Decode mod, r_m and reg fields in instruction
#define DECODE_RM_REG                                                                              \
    scratch2_uint = !i_mod << 2,                                                                   \
    op_to_addr = rm_addr = i_mod < 3                                                               \
    ? SEGREG(seg_override_en ? seg_override : rm_decode_table[scratch2_uint + 3][i_rm],        \
    rm_decode_table[scratch2_uint][i_rm],                                                \
    regs16[rm_decode_table[scratch2_uint + 1][i_rm]] + rm_decode_table[scratch2_uint + 2][i_rm] * i_data1 +)                          \
    : GET_REG_ADDR(i_rm),                                                                      \
    op_from_addr = GET_REG_ADDR(i_reg),                                                            \
    i_d && (scratch_uint = op_from_addr, op_from_addr = rm_addr, op_to_addr = scratch_uint)

// Return memory-mapped register location (offset into mem array) for register
// #reg_id
#define GET_REG_ADDR(reg_id)                                                                       \
    (REGS_BASE + (i_w ? ((reg_id) << 1) : (((reg_id) << 1) + ((reg_id) >> 2)) & 7))

// Returns number of top bit in operand (i.e. 8 for 8-bit operands, 16 for
// 16-bit operands)
#define TOP_BIT ((i_w + 1) << 3)

// [I]MUL/[I]DIV/DAA/DAS/ADC/SBB helpers
#define MUL_MACRO(op_data_type, out_regs) (set_opcode(0x10),    \
        (out_regs)[i_w + 1] = (op_result = CAST(op_data_type) mem[rm_addr] * (op_data_type) * (out_regs)) >> 16, \
        CPU.AX = op_result,     \
        set_OF(set_CF(op_result - (op_data_type)op_result)))
#define DIV_MACRO(out_data_type, in_data_type, out_regs)                                           \
    (scratch_int = CAST(out_data_type) mem[rm_addr])                                              \
    && !(scratch2_uint = (in_data_type)(scratch_uint = (out_regs[i_w + 1] << 16) + CPU.AX) / scratch_int,\
    scratch2_uint - (out_data_type)scratch2_uint)                                  \
    ? out_regs[i_w + 1] = scratch_uint - scratch_int * (*out_regs = scratch2_uint)     \
    : pc_interrupt(0)

#define DAA_DAS(op1,op2) \
	set_AF((((scratch_uchar = CPU.AL) & 0x0F) > 9) || CPU.AF) && (op_result = (CPU.AL op1 6), \
    set_CF(CPU.CF || (CPU.AL op2 scratch_uchar))), \
	set_CF((CPU.AL > 0x9f) || CPU.CF) && (op_result = (CPU.AL op1 0x60))

#define ADC_SBB_MACRO(a)    \
    OP(a## = CPU.CF +),                                                                            \
    set_CF((CPU.CF && (op_result == op_dest)) || (a op_result < a(int32_t) op_dest)),                \
    set_AF_OF_arith()

// Execute arithmetic/logic operations in emulator memory/registers
#define R_M_OP(dest, op, src)                                                                      \
    (i_w ? op_dest = CAST(uint16_t)(dest),                                                         \
    op_result = CAST(uint16_t) dest op(op_source = CAST(uint16_t) src)                         \
    : (op_dest = (dest), op_result = dest op(op_source = CAST(uint8_t) src)))

#define R_M_OP_64_EQUALS(dest,op,src) \
    (i_w ? op_dest = CAST(uint16_t)dest, \
    op_result = CAST(uint16_t)dest = (uint64_t)CAST(uint16_t)dest op (uint64_t)(op_source = CAST(uint16_t)src) \
    : (op_dest = dest, op_result = dest = (uint64_t)dest op (uint64_t)(op_source = CAST(uint8_t)src)))


#define MEM_OP(dest, op, src) R_M_OP(mem[dest], op, mem[src])
#define OP(op) MEM_OP(op_to_addr, op, op_from_addr)

// Increment or decrement a register #reg_id (usually SI or DI), depending on
// direction flag and operand size (given by i_w)
#define INDEX_INC(reg_id) (regs16[reg_id] -= ((CPU.DF << 1) - 1) * (i_w + 1))

// Helpers for stack operations
#define R_M_PUSH(a) (i_w = 1, R_M_OP(mem[SEGREG(REG_SS, REG_SP, --)], =, a))
#define R_M_POP(a) (i_w = 1, CPU.SP += 2, R_M_OP(a, =, mem[SEGREG(REG_SS, REG_SP, -2 +)]))

// Returns sign bit of an 8-bit or 16-bit operand
// #define SIGN_OF(a) (i_w ? ((int16_t)(a) < 0) : ((int8_t)(a) < 0))
#define SIGN_OF(a) (1 & (i_w ? CAST(int16_t)a : (a)) >> (TOP_BIT - 1))

#define SEGREG(reg_seg, reg_ofs, op)                                                               \
    MAP_ADDR((regs16[reg_seg] << 4) + (uint16_t)(op regs16[reg_ofs]))

// Jump helper for direct threading
#define NEXT_OP goto next_opcode

// clang-format on
// Helper functions

// Set carry flag
static int8_t __always_inline set_CF(int32_t new_CF) { return CPU.CF = !!new_CF; }

// Set auxiliary flag
static int8_t __always_inline set_AF(int32_t new_AF) { return CPU.AF = !!new_AF; }

// Set overflow flag
static int8_t __always_inline set_OF(int32_t new_OF) { return CPU.OF = !!new_OF; }

// Set auxiliary and overflow flag after arithmetic operations
static int8_t __always_inline set_AF_OF_arith() {
    set_AF((op_source ^= op_dest ^ op_result) & 0x10);
    if (op_result == op_dest) {
        return set_OF(0);
    }
    return set_OF(1 & (CPU.CF ^ op_source >> (TOP_BIT - 1)));
}

// Assemble and return emulated CPU FLAGS register in scratch_uint
static void __always_inline(make_flags)() {
    // 8086 has reserved and unused flags set to 1
    scratch_uint = 0xF002 | (CPU.CF << 0) | (CPU.PF << 2) | (CPU.AF << 4) | (CPU.ZF << 6) |
                   (CPU.SF << 7) | (CPU.TF << 8) | (CPU.IF << 9) | (CPU.DF << 10) | (CPU.OF << 11);
}

// Set emulated CPU FLAGS register
static void __always_inline set_flags(int32_t new_flags) {
    CPU.CF = new_flags & 1;
    CPU.PF = (new_flags >> 2) & 1;
    CPU.AF = (new_flags >> 4) & 1;
    CPU.ZF = (new_flags >> 6) & 1;
    CPU.SF = (new_flags >> 7) & 1;
    CPU.TF = (new_flags >> 8) & 1;
    CPU.IF = (new_flags >> 9) & 1;
    CPU.DF = (new_flags >> 10) & 1;
    CPU.OF = (new_flags >> 11) & 1;
}

// Convert raw opcode to translated opcode index. This condenses a large number
// of different encodings of similar instructions into a much smaller number of
// distinct functions, which we then execute
static void __always_inline set_opcode(uint8_t opcode) {
    CPU_OPCODE = op_decode_table[opcode];
    raw_opcode_id = opcode;
}
// Execute INT #interrupt_num on the emulated machine
static char __time_critical_func(pc_interrupt)(uint8_t interrupt_num) {
    set_opcode(0xCD); // Decode like INT

    make_flags();
    R_M_PUSH(scratch_uint);
    R_M_PUSH(CPU.CS);
    R_M_PUSH(CPU.IP);
    MEM_OP(REGS_BASE + (REG_CS << 1), =, (interrupt_num << 2) + 2);
    R_M_OP(CPU.IP, =, mem[interrupt_num << 2]);

    // if (interrupt_num == 0x10 && CPU.AH != 0x0E) {
    //     printf("Int: %x, AH=%x AL=%x \n", interrupt_num, CPU.AH, CPU.AL);
    // }
    return CPU.TF = CPU.IF = trap_flag = 0;
}

// AAA and AAS instructions - op is +1 for AAA, and -1 for AAS
static int32_t __always_inline AAA_AAS(int8_t w_op) {
    return (CPU.AX += 262 * w_op * set_AF(set_CF(((CPU.AL & 0x0F) > 9) || CPU.AF)), CPU.AL &= 0x0F);
}

// Read a byte from an I/O port
static uint8_t __always_inline io_port_in(uint32_t port) {
    if (port == 0x20) {
        io_ports[0x20] = 0;
    } else if (port >= 0x40 && port <= 0x42) {
        io_ports[port] = pico_x86_pit_in(port);
    } else if (port >= 0x03D0 && port <= 0x03DF) {
        video_cga_port_in(port);
    } else if (port == 0x61) { // ppi
        io_ports[0x61] = pico_x86_pit_get_port61_state(io_ports[0x61]);
    } else if (port >= 0x3F8 && port <= 0x3FF) {
        pico_x86_serial_port_in(port);
    } else if (port == 0x321) { // xt hdd
        io_ports[0x321] = 0;
    }
    return io_ports[port];
}

// Write a byte to an I/O port
static void __always_inline io_port_out(uint32_t port, uint8_t val) {
    if (port == 0x61) { // ppi
        io_hi_lo = 0;
        spkr_en |= (val & 3);
        pico_x86_pit_set_speaker_control(val);
    }

    if (port >= 0x40 && port <= 0x43) {
        pico_x86_pit_out(port, val);
    }

    if (port >= 0x03D0 && port <= 0x03DF) {
        video_cga_port_out(port);
    }

    if (port >= 0x3F8 && port <= 0x3FF) {
        pico_x86_serial_port_out(port);
    }
}

// PicoCalc specific keyboard handling
static void keyboard_process() {
    static int32_t kbd_event = 0;
    if (unlikely(queue_try_remove(kbd_queue, &kbd_event))) {
        uint8_t scancode = kbd_event & 0xFF;

        // PicoCalc: Reboot/Screenshot on short press power key
        if (unlikely(scancode == 0x91)) {
            printf("Power key pressed\n");
            if (power_action == POWER_ACTION_REBOOT) {
                watchdog_reboot(0, 0, 10);
            } else {
                BIT_SET(io_ports[0x3D8], 6);
                video_cga_port_out(0x3D8);
            }
            return;
        }

        if (KBD_GET_STATE(kbd_event) == KBD_STATE_RELEASE) {
            scancode |= 0x80;
        }
        io_ports[0x60] = scancode;
        pc_interrupt(9);
    }
}

extern FATFS fs;
extern const uint8_t binary_bios_bin_start[];
extern const uint8_t binary_bios_bin_end[];

static FIL fpd, fpfd;
static FRESULT fr;

static uint8_t floppy_present = 0;

uint8_t pico_x86_get_floppy_enabled() { return floppy_present; }
void pico_x86_set_floppy_enabled(uint8_t enabled) { floppy_present = enabled; }

uint8_t pico_x86_get_power_action() { return power_action; }
void pico_x86_set_power_action(uint8_t action) { power_action = action; }

void __always_inline pico_x86_timer_tick() {
    if (int8_asap < 0xFF) {
        int8_asap++;
    }
}

static void __always_inline __isr isr() {
    if ((int8_asap)) {
        pc_interrupt(0xA);
        int8_asap--;
        if (int8_asap == 0) {
            keyboard_process();
        }
    } else if (unlikely(pico_x86_serial_int_pending())) {
        pc_interrupt(0x0C); // Trigger IRQ 4
    }
}

void pico_x86_run() {
    printf("\n▼ Memory Size %d: bytes\n", RAM_SIZE);

    mem = sram;

    // regs16 and regs8 point to F000:0, the start of memory-mapped registers.
    regs16 = (uint16_t *)(regs8 = mem + REGS_BASE);

    // CS is initialised to F000
    CPU.CS = 0xF000;

    // Trap flag off
    CPU.TF = 0;

    // Set IP to 0100
    CPU.IP = BIOS_LOAD_OFFSET;

    // TODO: bios override if present
#ifndef BIOS_EMBED_ONLY
    FIL fpb;
    fr = f_open(&fpb, "0:/x86/bios.bin", FA_READ);

    if (fr == FR_OK && f_size(&fpb) > BIOS_MAX_SIZE) {
        printf("\n[FATAL ERROR] BIOS image is %llu bytes, exceeds the %d byte "
               "limit for the F000 ROM window!\n",
               f_size(&fpb), BIOS_MAX_SIZE);
        f_close(&fpb);
        while (1) {
        }
    }

    // Load BIOS image into F000:0100, and set IP to 0100
    UINT br;
    fr = f_read(&fpb, regs8 + BIOS_LOAD_OFFSET, BIOS_MAX_SIZE, &br);
    if (fr == FR_OK) {
        printf("\n▼ BIOS Image Size: %d bytes\n", br);
    } else {
        printf("\n[FATAL ERROR] BIOS image is missing, empty, or SD card failed! "
               "FATFS Code: %d\n",
               fr);
    }
    f_close(&fpb);
#else
    size_t bios_size = (size_t)(binary_bios_bin_end - binary_bios_bin_start);
    printf("\n▼ BIOS found in Flash at 0x%p, size: %zu bytes\n", binary_bios_bin_start, bios_size);

    if (bios_size > BIOS_MAX_SIZE) {
        printf("\n[FATAL ERROR] BIOS image is %zu bytes, exceeds the %d byte "
               "limit for the F000 ROM window!\n",
               bios_size, BIOS_MAX_SIZE);
        while (1) {
        };
    }
    memcpy(regs8 + BIOS_LOAD_OFFSET, binary_bios_bin_start, bios_size);
#endif

    fr = f_open(&fpd, "0:/x86/hd.img", FA_READ | FA_WRITE);
    if (fr != FR_OK || f_size(&fpd) == 0) {
        printf("\n[FATAL ERROR] disk image is missing, empty, or SD card failed! "
               "FATFS Code: %d\n",
               fr);
        while (1) {
        };
    } else {
        printf("▼ DISK Image Size: %llu bytes\n", fpd.obj.objsize);
    }

    fr = f_open(&fpfd, FDD_IMAGE_PATH, FA_READ | FA_WRITE);
    if (fr != FR_OK || f_size(&fpfd) == 0) {
        if (fr == FR_OK) {
            f_close(&fpfd);
        }
        printf("▼ No floppy image found, creating blank 1.44MB floppy image\n");
        create_blank_floppy_image();
        fr = f_open(&fpfd, FDD_IMAGE_PATH, FA_READ | FA_WRITE);
    }
    if (fr == FR_OK && f_size(&fpfd) > 0) {
        printf("▼ FDD Image Size: %llu bytes\n", fpfd.obj.objsize);
        floppy_present = 1;
    } else {
        printf("▼ Floppy image unavailable - drive A: not available\n");
        floppy_present = 0;
    }

    CAST(uint32_t) CPU.AX = fpd.obj.objsize >> 9;

    // Load instruction decoding helper tables
    for (uint8_t i = 0; i < 20; i++) {
        uint16_t table_addr = regs16[0x81 + i];
        for (uint16_t j = 0; j < 256; j++) {
            uint8_t val = regs8[table_addr + j];
            // ModR/M tables (0-7): only need 8 entries
            if (i < 8 && j < 8) {
                rm_decode_table[i][j] = val;
                // Opcode decode (Tables 8, 9, 10, 14)
            } else if (i == TABLE_XLAT_OPCODE) {
                op_decode_table[j].xlat_id = val;
            } else if (i == TABLE_XLAT_SUBFUNCTION) {
                op_decode_table[j].subfn = val;
            } else if (i == TABLE_STD_FLAGS) {
                op_decode_table[j].flags = val;
            } else if (i == TABLE_I_MOD_SIZE) {
                op_decode_table[j].mod_size = val;
                // Instruction size decode (Tables 12, 13)
            } else if (i == TABLE_BASE_INST_SIZE) {
                inst_size_table[j].base_size = val;
            } else if (i == TABLE_I_W_SIZE) {
                inst_size_table[j].w_size = val;
                // Jump logic tables (15-18): only need 8 entries
            } else if (i >= TABLE_COND_JUMP_DECODE_A && i <= TABLE_COND_JUMP_DECODE_D && j < 8) {
                jmp_decode_table[i - TABLE_COND_JUMP_DECODE_A][j] = val;
            }
        }
    }
    pico_x86_pit_init();
    if (queue_init(&local_kbd_queue, sizeof(int32_t), 16)) {
        kbd_queue = &local_kbd_queue;
    }
    pico_x86_cpu();
}

void pico_x86_cpu() {
    //  GOTO Dispatch Table
    static const void *__scratch_y("cpu") dispatch_table[58] = {
        &&OP_0,  &&OP_1,  &&OP_2,  &&OP_3,  &&OP_4,  &&OP_5,  &&OP_6,  &&OP_7,  &&OP_8,  &&OP_9,
        &&OP_10, &&OP_11, &&OP_12, &&OP_13, &&OP_14, &&OP_15, &&OP_16, &&OP_17, &&OP_18, &&OP_19,
        &&OP_20, &&OP_21, &&OP_22, &&OP_23, &&OP_24, &&OP_25, &&OP_26, &&OP_27, &&OP_28, &&OP_29,
        &&OP_30, &&OP_31, &&OP_32, &&OP_33, &&OP_34, &&OP_35, &&OP_36, &&OP_37, &&OP_38, &&OP_39,
        &&OP_40, &&OP_41, &&OP_42, &&OP_43, &&OP_44, &&OP_45, &&OP_46, &&OP_47, &&OP_48, &&OP_49,
        &&OP_50, &&OP_51, &&OP_52, &&OP_53, &&OP_54, &&OP_55, &&OP_56, &&OP_57};

start:
    // Instruction execution loop
    opcode_stream = mem + MAP_ADDR((CPU.CS << 4) + CPU.IP);

    // Terminates if CS:IP = 0:0
    if (unlikely(opcode_stream == mem)) {
        goto exit_emulation;
    }

    set_opcode(*opcode_stream);

    // Extract i_w and i_d fields from instruction
    i_reg4bit = opcode_stream[0] & 0x7; // Extracts bits 2, 1, and 0
    i_w = i_reg4bit & 0x1;              // Extracts bit 0
    i_d = (i_reg4bit >> 1) & 0x1;       // Extracts bit 1

    // Extract instruction data fields
    i_data0 = CAST(int16_t) opcode_stream[1];
    shift_count = i_data1 = CAST(int16_t) opcode_stream[2];
    i_data2 = CAST(int16_t) opcode_stream[3];

    // seg_override_en and rep_override_en contain number of instructions to
    // hold segment override and REP prefix respectively
    seg_override_en && seg_override_en--;
    rep_override_en && rep_override_en--;

    // CPU_OPCODE.mod_size > 0 indicates that opcode uses i_mod/i_rm/i_reg, so decode
    // them
    if (CPU_OPCODE.mod_size > 0) {
        i_mod = EXTRACT_BITS(i_data0, 7, 6);
        i_reg = EXTRACT_BITS(i_data0, 5, 3);
        i_rm = i_data0 & 0x7;

        if ((!i_mod && i_rm == 6) || (i_mod == 2)) {
            i_data2 = CAST(int16_t) opcode_stream[4];
            shift_count = opcode_stream[4];
        } else if (i_mod != 1) {
            i_data2 = i_data1;
        } else {
            i_data1 = (int8_t)i_data1;
            shift_count = opcode_stream[3];
        }

        DECODE_RM_REG;
    }
    if (unlikely(CPU_OPCODE.xlat_id >= count_of(dispatch_table))) {
        goto OP_NOP;
    }

    // Instruction execution unit
    goto *dispatch_table[CPU_OPCODE.xlat_id];

OP_0: // Conditional jump
    scratch_uchar = EXTRACT_BITS(raw_opcode_id, 3, 1);
    CPU.IP += (int8_t)i_data0 * (i_w ^ (regs8[jmp_decode_table[0][scratch_uchar]] ||
                                        regs8[jmp_decode_table[1][scratch_uchar]] ||
                                        regs8[jmp_decode_table[2][scratch_uchar]] ^
                                            regs8[jmp_decode_table[3][scratch_uchar]]));
    NEXT_OP;
OP_1: // MOV reg, imm
    i_w = !!(raw_opcode_id & 8);
    R_M_OP(mem[GET_REG_ADDR(i_reg4bit)], =, i_data0);
    NEXT_OP;
OP_3: // PUSH regs16
    R_M_PUSH(regs16[i_reg4bit]);
    NEXT_OP;
OP_4: // POP regs16
    R_M_POP(regs16[i_reg4bit]);
    NEXT_OP;
OP_2: // INC|DEC regs16
    i_w = 1;
    i_d = 0;
    i_reg = i_reg4bit;
    DECODE_RM_REG;
    i_reg = CPU_OPCODE.subfn;
    /* Fallthrough */
OP_5:                // INC|DEC|JMP|CALL|PUSH
    if (i_reg < 2) { // INC|DEC
        MEM_OP(op_from_addr, += 1 - (i_reg << 1) +, REGS_BASE + (REG_ZERO << 1)),
            op_source = 1, set_AF_OF_arith(), set_OF(op_dest + 1 - i_reg == 1 << (TOP_BIT - 1)),
            (CPU_OPCODE.xlat_id == 5) && (set_opcode(0x10), 0); // Decode like ADC
    } else if (i_reg != 6) {                                    // JMP|CALL
        i_reg - 3 || R_M_PUSH(CPU.CS),                          // CALL (far)
            i_reg & 2 && R_M_PUSH(CPU.IP + 2 + (i_mod * (i_mod != 3)) +
                                  ((!i_mod && i_rm == 6) << 1)),         // CALL (near or far)
            i_reg & 1 && (CPU.CS = CAST(int16_t) mem[op_from_addr + 2]), // JMP|CALL (far)
            R_M_OP(CPU.IP, =, mem[op_from_addr]),
            set_opcode(0x9A); // Decode like CALL
    } else {                  // PUSH
        R_M_PUSH(mem[rm_addr]);
    }
    NEXT_OP;
OP_6: // TEST r/m, imm16 / NOT|NEG|MUL|IMUL|DIV|IDIV reg
      // F6 /4 8bit     /5 Signed AX = AL * r/m8
      // F7 /4 16bit    /5 Signed double-width product
      //
    op_to_addr = op_from_addr;
    switch (i_reg) {
    case 0:
        set_opcode(0x20);
        CPU.IP += i_w + 1;
        R_M_OP(mem[op_to_addr], &, i_data2);
        break;
    case 2:
        OP(= ~);
        break;
    case 3:
        OP(= -);
        op_dest = 0;
        set_opcode(0x28);
        set_CF(op_result > op_dest);
        break;
    case 4:
        i_w ? MUL_MACRO(uint16_t, regs16) : MUL_MACRO(uint8_t, regs8);
        break;
    case 5:
        i_w ? MUL_MACRO(int16_t, regs16) : MUL_MACRO(int8_t, regs8);
        break;
    case 6:
        i_w ? DIV_MACRO(uint16_t, uint32_t, regs16) : DIV_MACRO(uint8_t, uint16_t, regs8);
        break;
    case 7:
        i_w ? DIV_MACRO(int16_t, int32_t, regs16) : DIV_MACRO(int8_t, int16_t, regs8);
        break;
    default:
        break;
    }
    NEXT_OP;
OP_7: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP AL/AX, immed
    rm_addr = REGS_BASE;
    i_data2 = i_data0;
    i_mod = 3;
    i_reg = CPU_OPCODE.subfn;
    CPU.IP--;
    /* Fallthrough */
OP_8: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP reg, immed
    op_to_addr = rm_addr;
    CPU.SCRATCH = (i_d |= !i_w) ? (int8_t)i_data2 : i_data2;
    op_from_addr = REGS_BASE + (REG_SCRATCH << 1);
    CPU.IP += !i_d + 1;
    set_opcode(0x08 * (CPU_OPCODE.subfn = i_reg));
    /* Fallthrough */
OP_9: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP|MOV reg, r/m
    switch (CPU_OPCODE.subfn) {
    case 0:
        OP(+=);
        set_CF(op_result < op_dest);
        break;
    case 1:
        OP(|=);
        break;
    case 2:
        ADC_SBB_MACRO(+);
        break;
    case 3:
        ADC_SBB_MACRO(-);
        break;
    case 4:
        OP(&=);
        break;
    case 5:
        OP(-=);
        set_CF(op_result > op_dest);
        break;
    case 6:
        OP(^=);
        break;
    case 7:
        OP(-);
        set_CF(op_result > op_dest);
        break;
    case 8:
        OP(=);
        break;
    default:
        break;
    }
    NEXT_OP;
OP_10: // MOV sreg, r/m | POP r/m | LEA reg, r/m
    if ((!i_w)) {
        i_w = 1;
        i_reg += 8;
        DECODE_RM_REG;
        if (i_d &&
            (op_to_addr == REGS_BASE + (REG_CS << 1) || op_to_addr > REGS_BASE + (REG_DS << 1))) {
            goto OP_INVALID;
        }
        if (!i_d && (op_from_addr > REGS_BASE + (REG_DS << 1))) {
            goto OP_INVALID;
        }
        OP(=);
    } else if (!i_d) { // LEA
        seg_override_en = 1, seg_override = REG_ZERO, DECODE_RM_REG,
        R_M_OP(mem[op_from_addr], =, rm_addr);
    } else { // POP
        R_M_POP(mem[rm_addr]);
    }
    NEXT_OP;
OP_11: // MOV AL/AX, [loc]
    i_mod = i_reg = 0;
    i_rm = 6;
    i_data1 = i_data0;
    DECODE_RM_REG;
    MEM_OP(op_from_addr, =, op_to_addr);
    NEXT_OP;
OP_12: // ROL|ROR|RCL|RCR|SHL|SHR|SAR reg/mem, 1/CL/imm (80186)
    scratch2_uint = SIGN_OF(mem[rm_addr]);
    scratch_uint = CPU_OPCODE.subfn ? CPU.IP++, (shift_count & 0x1F) : (i_d ? (CPU.CL & 0x1F) : 1);

    if (scratch_uint) {
        if (i_reg < 4) { // Rotate operations
            scratch_uint %= (i_reg >> 1) + TOP_BIT, R_M_OP(scratch2_uint, =, mem[rm_addr]);
        }
        if (i_reg & 1) { // Rotate/shift right operations
            R_M_OP_64_EQUALS(mem[rm_addr], >>, scratch_uint);
        } else { // Rotate/shift left operations
            R_M_OP_64_EQUALS(mem[rm_addr], <<, scratch_uint);
        }
        if (i_reg > 3) {                         // Shift operations
            CPU_OPCODE.flags = FLAGS_UPDATE_SZP; // Shift instructions affect SZP
        }
        if (i_reg > 4) { // SHR or SAR
            set_CF((uint64_t)op_dest >> (scratch_uint - 1) & 1);
        }

        switch (i_reg) {
        case 0:
            R_M_OP_64_EQUALS(mem[rm_addr], +, scratch2_uint >> (TOP_BIT - scratch_uint));
            set_OF(SIGN_OF(op_result) ^ set_CF(op_result & 1));
            break;
        case 1:
            scratch2_uint &= (1 << scratch_uint) - 1,
                R_M_OP_64_EQUALS(mem[rm_addr], +, scratch2_uint << (TOP_BIT - scratch_uint));
            set_OF(SIGN_OF(op_result * 2) ^ set_CF(SIGN_OF(op_result)));
            break;
        case 2:
            R_M_OP_64_EQUALS(mem[rm_addr], +((uint32_t)regs8[FLAG_CF] << (scratch_uint - 1)) +,
                             scratch2_uint >> (1 + TOP_BIT - scratch_uint));
            set_OF(SIGN_OF(op_result) ^
                   set_CF(scratch2_uint & (uint64_t)1 << (TOP_BIT - scratch_uint)));
            break;
        case 3:
            R_M_OP_64_EQUALS(mem[rm_addr], +((uint64_t)regs8[FLAG_CF] << (TOP_BIT - scratch_uint)) +
                             , scratch2_uint << (1 + TOP_BIT - scratch_uint));
            set_CF(scratch2_uint & (uint64_t)1 << (scratch_uint - 1));
            set_OF(SIGN_OF(op_result) ^ SIGN_OF(op_result * 2));
            break;
        case 4:
            set_OF(SIGN_OF(op_result) ^ set_CF(SIGN_OF(op_dest << (scratch_uint - 1))));
            break;
        case 5:
            set_OF(SIGN_OF(op_dest));
            break;
        case 7:
            scratch_uint < TOP_BIT || set_CF(scratch2_uint);
            set_OF(0);
            R_M_OP_64_EQUALS(mem[rm_addr], +,
                             scratch2_uint *=
                             ~((uint64_t)(((uint64_t)1 << TOP_BIT) - 1) >> scratch_uint));
            break;
        default:
            break;
        }
    }
    NEXT_OP;
OP_13: // LOOPxx|JCZX
    scratch_uint = !!--CPU.CX;
    switch (i_reg4bit) {
    case 0:
        scratch_uint &= !CPU.ZF;
        break;
    case 1:
        scratch_uint &= CPU.ZF;
        break;
    case 3:
        scratch_uint = !++CPU.CX;
        break;
    default:
        break;
    }
    CPU.IP += scratch_uint * (int8_t)i_data0;
    NEXT_OP;
OP_14: // JMP | CALL short/near
    CPU.IP += 3 - i_d;
    if (!i_w) {
        if (i_d) {
            CPU.IP = 0;
            CPU.CS = i_data2;
        } else {
            R_M_PUSH(CPU.IP);
        }
    }
    CPU.IP += i_d && i_w ? (int8_t)i_data0 : i_data0;
    NEXT_OP;
OP_15: // TEST reg, r/m
    MEM_OP(op_from_addr, &, op_to_addr);
    NEXT_OP;
OP_16: // XCHG AX, regs16
    i_w = 1;
    op_to_addr = REGS_BASE;
    op_from_addr = GET_REG_ADDR(i_reg4bit);
    /* Fallthrough */
OP_24: // NOP|XCHG reg, r/m
    if (op_to_addr != op_from_addr) {
        OP(^=);
        MEM_OP(op_from_addr, ^=, op_to_addr);
        OP(^=);
    }
    NEXT_OP;
OP_17: // MOVSx|STOSx|LODSx
    scratch2_uint = seg_override_en ? seg_override : REG_DS;
    scratch_uint = rep_override_en ? CPU.CX : 1;
    if (trap_flag && scratch_uint > 1) {
        scratch_uint = 1;
    }
    for (; scratch_uint; scratch_uint--) {
        MEM_OP(CPU_OPCODE.subfn < 2 ? SEGREG(REG_ES, REG_DI, ) : REGS_BASE, =,
               CPU_OPCODE.subfn & 1 ? REGS_BASE : SEGREG(scratch2_uint, REG_SI, ));
        CPU_OPCODE.subfn & 1 || INDEX_INC(REG_SI);
        CPU_OPCODE.subfn & 2 || INDEX_INC(REG_DI);
        rep_override_en && CPU.CX--;
    }
    NEXT_OP;
OP_18: // CMPSx|SCASx
    scratch2_uint = seg_override_en ? seg_override : REG_DS;

    scratch_uint = rep_override_en ? CPU.CX : 1;
    if (trap_flag && scratch_uint > 1) {
        scratch_uint = 1;
    }
    if (scratch_uint) {
        for (; scratch_uint; rep_override_en || scratch_uint--) {
            MEM_OP(CPU_OPCODE.subfn ? REGS_BASE : SEGREG(scratch2_uint, REG_SI, ), -,
                   SEGREG(REG_ES, REG_DI, ));
            CPU_OPCODE.subfn || INDEX_INC(REG_SI);
            INDEX_INC(REG_DI);
            if (rep_override_en) {
                if (!(--CPU.CX && ((!op_result) == rep_mode))) {
                    (scratch_uint = 0);
                } else if (trap_flag) {
                    scratch_uint = 0;
                }
            }
        }
        CPU_OPCODE.flags = FLAGS_UPDATE_SZP | FLAGS_UPDATE_AO_ARITH; // Funge to set SZP/AO flags
        set_CF(op_result > op_dest);
    }
    NEXT_OP;
OP_19: // RET|RETF|IRET
    i_d = i_w;
    R_M_POP(CPU.IP);
    if (CPU_OPCODE.subfn) {
        R_M_POP(CPU.CS);
    }
    if (CPU_OPCODE.subfn & 2) {
        set_flags(R_M_POP(scratch_uint));
    } else if (!i_d) {
        CPU.SP += i_data0;
    }
    NEXT_OP;
OP_20: // MOV r/m, immed
    R_M_OP(mem[op_from_addr], =, i_data2);
    NEXT_OP;
OP_21: // IN AL/AX, DX/imm8
    scratch_uint = CPU_OPCODE.subfn ? CPU.DX : (uint8_t)i_data0;
    if (likely(scratch_uint < IO_PORT_COUNT)) {
        io_port_in(scratch_uint);
        R_M_OP(CPU.AL, =, io_ports[scratch_uint]);
    } else {
        CPU.AL = 0xFF;
    }
    NEXT_OP;
OP_22: // OUT DX/imm8, AL/AX
    scratch_uint = CPU_OPCODE.subfn ? CPU.DX : (uint8_t)i_data0;
    if (likely(scratch_uint < IO_PORT_COUNT)) {
        R_M_OP(io_ports[scratch_uint], =, CPU.AL);
        io_port_out(scratch_uint, CPU.AL);
    }
    NEXT_OP;
OP_23: // REPxx
    rep_override_en = 2;
    rep_mode = i_w;
    seg_override_en && seg_override_en++;
    NEXT_OP;
OP_25: // PUSH reg
    R_M_PUSH(regs16[CPU_OPCODE.subfn]);
    NEXT_OP;
OP_26: // POP reg
    R_M_POP(regs16[CPU_OPCODE.subfn]);
    NEXT_OP;
OP_27: // xS: segment overrides
    seg_override_en = 2;
    seg_override = CPU_OPCODE.subfn;
    rep_override_en && rep_override_en++;
    NEXT_OP;
OP_28: // DAA/DAS
    i_w = 0;
    if (CPU_OPCODE.subfn) {
        DAA_DAS(-=, >);
    } else {
        DAA_DAS(+=, <); // extra = 0 for DAA, 1 for DAS
    }
    NEXT_OP;
OP_29: // AAA/AAS
    op_result = AAA_AAS(CPU_OPCODE.subfn - 1);
    NEXT_OP;
OP_30: // CBW
    CPU.AH = -SIGN_OF(CPU.AL);
    NEXT_OP;
OP_31: // CWD
    CPU.DX = -SIGN_OF(CPU.AX);
    NEXT_OP;
OP_32: // CALL FAR imm16:imm16
    R_M_PUSH(CPU.CS);
    R_M_PUSH(CPU.IP + 5);
    CPU.CS = i_data2;
    CPU.IP = i_data0;
    NEXT_OP;
OP_33: // PUSHF
    make_flags();
    R_M_PUSH(scratch_uint);
    NEXT_OP;
OP_34: // POPF
    set_flags(R_M_POP(scratch_uint));
    NEXT_OP;
OP_35: // SAHF
    make_flags();
    set_flags((scratch_uint & 0xFF00) + CPU.AH);
    NEXT_OP;
OP_36: // LAHF
    make_flags();
    CPU.AH = scratch_uint;
    NEXT_OP;
OP_37: // LES|LDS reg, r/m
    i_w = i_d = 1;
    DECODE_RM_REG;
    OP(=);
    MEM_OP(REGS_BASE + CPU_OPCODE.subfn, =, rm_addr + 2);
    NEXT_OP;
OP_38: // INT 3
    ++CPU.IP;
    pc_interrupt(3);
    NEXT_OP;
OP_39: // INT imm8
    CPU.IP += 2;
    pc_interrupt(i_data0);
    NEXT_OP;
OP_40: // into
    ++CPU.IP;
    CPU.OF && pc_interrupt(4);
    NEXT_OP;
OP_41: // AAM
    i_data0 &= 0xFF;
    if (i_data0) {
        CPU.AH = CPU.AL / i_data0;
        op_result = CPU.AL %= i_data0;
    } else {
        pc_interrupt(0);
    }
    NEXT_OP;
OP_42: // AAD
    i_w = 0;
    CPU.AX = op_result = 0xFF & (CPU.AL + (i_data0 * CPU.AH));
    NEXT_OP;
OP_43: // SALC
    CPU.AL = -CPU.CF;
    NEXT_OP;
OP_44: // XLAT
    CPU.AL = mem[SEGREG(seg_override_en ? seg_override : REG_DS, REG_BX, CPU.AL +)];
    NEXT_OP;
OP_45: // CMC
    CPU.CF ^= 1;
    NEXT_OP;
OP_46: // CLC|STC|CLI|STI|CLD|STD
    regs8[CPU_OPCODE.subfn >> 1] = CPU_OPCODE.subfn & 1;
    NEXT_OP;
OP_47: // TEST AL/AX, immed
    R_M_OP(CPU.AL, &, i_data0);
    NEXT_OP;
OP_48: // Emulator-specific 0F xx opcodes
    switch ((int8_t)i_data0) {
    case 0: { // INT 14h Serial Port I/O (COM1)
        pico_x86_serial_ctl();
        break;
    }
    case 1: {
        aon_timer_get_time(&ts);
        uint32_t dest = SEGREG(REG_ES, REG_BX, );

        if (likely(aon_timer_get_time_calendar(&clock_tm))) {
            CAST(uint32_t)
            mem[dest + 0] = clock_tm.tm_sec;
            CAST(uint32_t)
            mem[dest + 4] = clock_tm.tm_min;
            CAST(uint32_t)
            mem[dest + 8] = clock_tm.tm_hour;
            CAST(uint32_t)
            mem[dest + 12] = clock_tm.tm_mday;
            CAST(uint32_t)
            mem[dest + 16] = clock_tm.tm_mon;
            CAST(uint32_t)
            mem[dest + 20] = clock_tm.tm_year;
            CAST(uint32_t)
            mem[dest + 24] = clock_tm.tm_wday;
            CAST(uint32_t)
            mem[dest + 28] = clock_tm.tm_yday;
            CAST(uint32_t)
            mem[dest + 32] = clock_tm.tm_isdst;
        } else {
            memset(&mem[dest], 0, 36);
        }
        // The BIOS expects the milliseconds as a 16-bit integer at offset
        // 36
        CAST(int16_t)
        mem[dest + 36] = ts.tv_nsec / 1000000;
        break;
    }
    case 2: { // DISK READ
        UINT br = 0;
        if (unlikely(CPU.AX == 0)) {
            CPU.AX = 0;
            break;
        }
        //  disk index in DX before this call: 0 = hd, 1 = fp
        uint8_t is_floppy = CPU.DX & 1;
        if (unlikely(is_floppy && !floppy_present)) {
            CPU.AX = 0; // No media in drive A:
            break;
        }
        FIL *fp = is_floppy ? &fpfd : &fpd;
        DWORD abs_sector = ((DWORD)CPU.SI << 16) | CPU.BP;
        if (likely(f_lseek(fp, abs_sector << 9) == FR_OK)) {
            f_read(fp, mem + SEGREG(REG_ES, REG_BX, ), CPU.AX, &br);
        }

        if (unlikely(br == 0)) {
            if (is_floppy) {
                CPU.AX = 0;
                break;
            }
            printf("\n[FATAL ERROR] Disk read failed at absolute sector %lu!\n", abs_sector);
            while (1) {
            };
        }
        CPU.AX = br;
        break;
    }
    case 3: { // DISK WRITE
        UINT bw = 0;
        //  disk index in DX before this call: 0 = hd, 1 = fp
        uint8_t is_floppy = CPU.DX & 1;
        if (unlikely(is_floppy && !floppy_present)) {
            CPU.AX = 0; // No media in drive A:
            break;
        }
        FIL *fp = is_floppy ? &fpfd : &fpd;
        DWORD abs_sector = ((DWORD)CPU.SI << 16) | CPU.BP;
        if (likely(f_lseek(fp, abs_sector << 9) == FR_OK)) {
            f_write(fp, mem + SEGREG(REG_ES, REG_BX, ), CPU.AX, &bw);
        }
        if (unlikely(bw == 0)) {
            if (is_floppy) {
                CPU.AX = 0;
                break;
            }
            printf("\n[FATAL ERROR] Disk write failed at absolute sector %lu!\n", abs_sector);
            while (1) {
            };
        }
        CPU.AX = bw;
        break;
    }
    case 4: { // INT9 keyboard scancode decode
        pico_x86_keyb_process_scancode(CPU.AL);
        break;
    }
    case 5: {
        uint32_t src = SEGREG(REG_ES, REG_BX, );
        static struct tm new_tm = {0};
        new_tm.tm_sec = (int32_t)CAST(uint32_t) mem[src + 0];
        new_tm.tm_min = (int32_t)CAST(uint32_t) mem[src + 4];
        new_tm.tm_hour = (int32_t)CAST(uint32_t) mem[src + 8];
        new_tm.tm_mday = (int32_t)CAST(uint32_t) mem[src + 12];
        new_tm.tm_mon = (int32_t)CAST(uint32_t) mem[src + 16];
        new_tm.tm_year = (int32_t)CAST(uint32_t) mem[src + 20];

        bool ok = aon_timer_is_running() ? aon_timer_set_time_calendar(&new_tm)
                                         : aon_timer_start_calendar(&new_tm);
        if (likely(ok)) {
            CPU.AL = 0x00; // Success
        } else {
            CPU.AL = 0xFF; // Failure
        }
        break;
    }
    case 6: {
        uint32_t ticks = ((uint32_t)CPU.CX << 16) | CPU.DX;
        uint32_t hour = ticks / 65520;
        uint32_t rem = ticks % 65520;
        uint32_t min = rem / 1092;
        rem %= 1092;
        uint32_t sec = (rem * 10) / 182;

        if (unlikely(hour > 23))
            hour = 23;

        static struct tm new_tm = {0};
        if (likely(aon_timer_get_time_calendar(&new_tm))) {
            new_tm.tm_hour = hour;
            new_tm.tm_min = min;
            new_tm.tm_sec = sec;

            bool ok = aon_timer_is_running() ? aon_timer_set_time_calendar(&new_tm)
                                             : aon_timer_start_calendar(&new_tm);
            if (likely(ok)) {
                CPU.AL = 0x00;
            } else {
                CPU.AL = 0xFF;
            }
        } else {
            CPU.AL = 0xFF;
        }
        break;
    }

    case 7: { // BIOS setup menu (F1 during POST)
        CPU.AL = pico_x86_bios_setup_menu();
        break;
    }
    default:
        break;
    }
    NEXT_OP;

OP_49: // ENTER (0xC8)
    R_M_PUSH(CPU.BP);
    scratch_uint = CPU.SP;
    scratch_int = (uint8_t)i_data2 & 0x1F;
    if (scratch_int > 0) {
        for (; scratch_int > 1; --scratch_int) {
            CPU.BP -= 2;
            R_M_PUSH(CAST(uint16_t) mem[SEGREG(REG_SS, REG_BP, )]);
        }
        R_M_PUSH(scratch_uint);
    }
    CPU.BP = scratch_uint;
    CPU.SP -= i_data0;
    NEXT_OP;

OP_50: // LEAVE (0xC9)
    CPU.SP = CPU.BP;
    R_M_POP(CPU.BP);
    NEXT_OP;

OP_51: // PUSHA
    scratch_uint = CPU.SP;
    R_M_PUSH(CPU.AX);
    R_M_PUSH(CPU.CX);
    R_M_PUSH(CPU.DX);
    R_M_PUSH(CPU.BX);
    R_M_PUSH(scratch_uint);
    R_M_PUSH(CPU.BP);
    R_M_PUSH(CPU.SI);
    R_M_PUSH(CPU.DI);
    NEXT_OP;
OP_52: // BOUND reg16, m16&16 (0x62)
    if (unlikely(i_mod == 3)) {
        goto OP_INVALID;
    }
    i_w = 1;
    i_d = 0;
    DECODE_RM_REG;
    if ((CAST(int16_t) mem[op_from_addr] < CAST(int16_t) mem[op_to_addr]) ||
        (CAST(int16_t) mem[op_from_addr] > CAST(int16_t) mem[op_to_addr + 2]))
        pc_interrupt(5);
    NEXT_OP;
OP_53:
    switch (raw_opcode_id) {
    // case 0x9B: // WAIT
    //     break;
    // case 0xD8: // FPU ESC
    // case 0xD9:
    // case 0xDA:
    // case 0xDB:
    // case 0xDC:
    // case 0xDD:
    // case 0xDE:
    // case 0xDF:
    //     break;
    case 0xF0: // LOCK
        seg_override_en && seg_override_en++;
        rep_override_en && rep_override_en++;
        break;
    case 0xF4: // HLT
        if (CPU.IF) {
            __wfi();
        }
        break;
    default:
        break;
    }
    goto OP_NOP;
OP_54: // POPA
    R_M_POP(CPU.DI);
    R_M_POP(CPU.SI);
    R_M_POP(CPU.BP);
    R_M_POP(scratch_uint);
    R_M_POP(CPU.BX);
    R_M_POP(CPU.DX);
    R_M_POP(CPU.CX);
    R_M_POP(CPU.AX);
    NEXT_OP;
OP_55: //  IMUL reg, r/m, imm8/imm16 (0x6B/0x69) | PUSH imm8/imm16 (0x6A/0x68)
    if (i_w) {
        DECODE_RM_REG;
        if (i_d) {
            scratch_int = (int16_t)(int8_t)i_data2;
        } else {
            scratch_int = (int16_t)i_data2;
        }
        op_dest = CAST(int16_t) mem[rm_addr];
        op_result = (int32_t)(int16_t)op_dest * scratch_int;
        regs16[i_reg] = op_result;
        set_OF(set_CF(op_result - (int32_t)(int16_t)op_result));
    } else {
        if (i_d) {
            scratch_int = (int16_t)(int8_t)i_data0;
        } else {
            scratch_int = (int16_t)i_data0;
        }
        R_M_PUSH(scratch_int);
    }
    NEXT_OP;
OP_56: // INSB|INSW
    for (scratch_uint = rep_override_en ? CPU.CX : 1; scratch_uint; scratch_uint--) {
        if (likely(CPU.DX < IO_PORT_COUNT)) {
            io_port_in(CPU.DX);
            if (i_w) {
                io_port_in(CPU.DX + 1);
                uint16_t wval = io_ports[CPU.DX] | ((uint16_t)io_ports[CPU.DX + 1] << 8);
                *(uint16_t *)&mem[SEGREG(REG_ES, REG_DI, )] = wval;
            } else {
                mem[SEGREG(REG_ES, REG_DI, )] = io_ports[CPU.DX];
            }
        } else {
            if (i_w) {
                *(uint16_t *)&mem[SEGREG(REG_ES, REG_DI, )] = 0xFFFF;
            } else {
                mem[SEGREG(REG_ES, REG_DI, )] = 0xFF;
            }
        }
        INDEX_INC(REG_DI);
    }
    if (rep_override_en) {
        CPU.CX = 0;
    }
    NEXT_OP;
OP_57: // OUTSB|OUTSW
    scratch2_uint = seg_override_en ? seg_override : REG_DS;
    for (scratch_uint = rep_override_en ? CPU.CX : 1; scratch_uint; scratch_uint--) {
        if (likely(CPU.DX < IO_PORT_COUNT)) {
            R_M_OP(io_ports[CPU.DX], =, mem[SEGREG(scratch2_uint, REG_SI, )]);
            io_port_out(CPU.DX, io_ports[CPU.DX]);
            if (i_w) {
                io_port_out(CPU.DX + 1, io_ports[CPU.DX + 1]);
            }
        }
        INDEX_INC(REG_SI);
    }
    if (rep_override_en) {
        CPU.CX = 0;
    }
    NEXT_OP;
OP_NOP: // NOP instruction (0x90)
    NEXT_OP;
OP_INVALID: // Invalid opcode exception
    pc_interrupt(6);
    NEXT_OP;
next_opcode:

    // Memory guard agains probing more than RAM_SIZE
    *(uint32_t *)(mem + RAM_SIZE) = 0xFFFFFFFF;

    CPU.IP += (((i_mod * (i_mod != 3)) + ((!i_mod && i_rm == 6) << 1)) * CPU_OPCODE.mod_size) +
              inst_size_table[raw_opcode_id].base_size +
              (inst_size_table[raw_opcode_id].w_size * (i_w + 1));

    // If instruction needs to update SF, ZF and PF, set them as appropriate
    if (CPU_OPCODE.flags & FLAGS_UPDATE_SZP) {
        CPU.SF = SIGN_OF(op_result);
        CPU.ZF = !op_result;

        CPU.PF = !(hweight8(op_result) & 1);

        // If instruction is an arithmetic or logic operation, also set
        // AF/OF/CF as appropriate.
        if (CPU_OPCODE.flags & FLAGS_UPDATE_AO_ARITH) {
            set_AF_OF_arith();
        }
        if (CPU_OPCODE.flags & FLAGS_UPDATE_OC_LOGIC) {
            set_CF(0), set_OF(0);
        }
    }

    // Application has set trap flag, so fire INT 1
    if (unlikely(trap_flag != 0)) {
        pc_interrupt(1);
    }

    trap_flag = CPU.TF;

    // If a timer tick or serial is pending, interrupts are enabled, and no
    // overrides/REP are active, then process the tick and check for new
    // keystrokes in isr
    isr_ready = ((!seg_override_en && !rep_override_en && CPU.IF && !trap_flag) != 0);
    if ((isr_ready)) {
        isr();
    }

    goto start;

exit_emulation:
#ifdef DEBUG_CONSOLE
    printf("\n!!! EMULATOR EXITED MAIN LOOP !!!\n");
    printf("Final CPU State -> CS: %04X | IP: %04X\n\n", CPU.CS, CPU.IP);
#endif
}
