/*
 * Portions of this file are derived from 8086tiny.
 * Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
 * Licensed under the MIT License.
 * See LICENSE.txt.
 *
 * Modifications Copyright (c) 2026 Serg Podtynnyi
 * Licensed under GPLv3 for the combined work.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/aon_timer.h"
#include "pico/time.h"

#include "ff.h"
#include "picocalc_southbridge.h"

#include "pico_x86.h"
#include "pico_x86_video.h"
#include "pico_x86_serial.h"
#include "pico_x86_floppy.h"

uint8_t __aligned(4) mem[RAM_SIZE + 16];

uint8_t __aligned(4) __scratch_y("io") io_ports[IO_PORT_COUNT];

typedef struct __attribute__((packed, aligned(4))) {
    uint8_t xlat_id;
    uint8_t subfunction;
    uint8_t flags;
    uint8_t mod_size;
} opcode_decode_t;

typedef struct __attribute__((packed, aligned(2))) {
    uint8_t base_size;
    uint8_t w_size;
} inst_size_t;

// Group 1: ModR/M Decode (Tables 0-7) - Indexed by 3-bit i_rm (0-7)
uint8_t __scratch_y("cpu") rm_decode_table[8][8];

// Group 2: Opcode Decode (Tables 8, 9, 10, 14) - Indexed by 8-bit opcode (0-255)
opcode_decode_t __scratch_y("cpu") op_decode_table[256];

// Group 3: Instruction Size (Tables 12, 13) - Indexed by 8-bit opcode (0-255)
inst_size_t __scratch_y("cpu") inst_size_table[256];

// Group 4: Jump Logic (Tables 15-18) - Indexed by 3-bit condition (0-7)
uint8_t __scratch_y("cpu") jmp_decode_table[4][8];

static uint8_t* opcode_stream asm("s5");
static uint32_t raw_opcode_id asm("s6");
static uint32_t seg_override_en asm("s7");

uint8_t* __scratch_y("cpu") regs8;
uint16_t* __scratch_y("cpu") regs16;

uint32_t __scratch_y("cpu") i_rm, i_w, i_reg, i_mod, i_mod_size, i_d, i_reg4bit,
    xlat_opcode_id, extra, rep_mode, rep_override_en, trap_flag, scratch_uchar, io_hi_lo, spkr_en;

uint32_t __scratch_y("cpu") int8_asap = 0;

uint16_t __scratch_y("cpu") reg_ip, seg_override;

uint32_t __scratch_y("cpu") op_source, op_dest, rm_addr, op_to_addr,
    op_from_addr, i_data0, i_data1, i_data2, scratch_uint, scratch2_uint, set_flags_type;
int32_t __scratch_y("cpu") op_result, disk[3], scratch_int;

struct timespec __scratch_y() ts;
struct tm __scratch_y() clock_tm;

// Helper macros

typedef int8_t __attribute__((aligned(1), may_alias)) unaligned_int8_t;
typedef uint8_t __attribute__((aligned(1), may_alias)) unaligned_uint8_t;
typedef int16_t __attribute__((aligned(1), may_alias)) unaligned_int16_t;
typedef uint16_t __attribute__((aligned(1), may_alias)) unaligned_uint16_t;
typedef int32_t __attribute__((aligned(1), may_alias)) unaligned_int32_t;
typedef uint32_t __attribute__((aligned(1), may_alias)) unaligned_uint32_t;

// #define CAST(type) *(type __attribute__((aligned(1))) *)&

#define CAST(type) *(unaligned_##type*)&

// Return memory-mapped register location (offset into mem array) for register
// #reg_id
#define GET_REG_ADDR(reg_id) (REGS_BASE + (i_w ? 2 * reg_id : 2 * reg_id + reg_id / 4 & 7))

// Decode mod, r_m and reg fields in instruction
#define DECODE_RM_REG                                                                              \
    scratch2_uint = 4 * !i_mod,                                                                    \
    op_to_addr = rm_addr = i_mod < 3                                                               \
        ? SEGREG(seg_override_en ? seg_override : rm_decode_table[scratch2_uint + 3][i_rm],        \
              rm_decode_table[scratch2_uint][i_rm],                                                \
              regs16[rm_decode_table[scratch2_uint + 1][i_rm]]                                     \
                  + rm_decode_table[scratch2_uint + 2][i_rm] * i_data1 +)                          \
        : GET_REG_ADDR(i_rm),                                                                      \
    op_from_addr = GET_REG_ADDR(i_reg),                                                            \
    i_d && (scratch_uint = op_from_addr, op_from_addr = rm_addr, op_to_addr = scratch_uint)

// Returns number of top bit in operand (i.e. 8 for 8-bit operands, 16 for
// 16-bit operands)
#define TOP_BIT (8 * (i_w + 1))

// Jump helper for direct threading
#define NEXT_OP goto next_opcode

// [I]MUL/[I]DIV/DAA/DAS/ADC/SBB helpers
#define MUL_MACRO(op_data_type, out_regs)                                                          \
    (set_opcode(0x10),                                                                             \
        out_regs[i_w + 1]                                                                          \
        = (op_result = CAST(op_data_type) mem[rm_addr] * (op_data_type) * out_regs) >> 16,         \
        regs16[REG_AX] = op_result, set_OF(set_CF(op_result - (op_data_type)op_result)))
#define DIV_MACRO(out_data_type, in_data_type, out_regs)                                           \
    ((scratch_int = CAST(out_data_type) mem[rm_addr])                                              \
                && !(scratch2_uint                                                                 \
                    = (in_data_type)(scratch_uint = (out_regs[i_w + 1] << 16) + regs16[REG_AX])    \
                        / scratch_int,                                                             \
                    scratch2_uint - (out_data_type)scratch2_uint)                                  \
            ? out_regs[i_w + 1] = scratch_uint - scratch_int * (*out_regs = scratch2_uint)         \
            : pc_interrupt(0))
#define DAA_DAS(op1, op2, mask, min)                                                               \
    set_AF((((scratch2_uint = regs8[REG_AL]) & 0x0F) > 9) || regs8[FLAG_AF])                       \
        && (op_result = regs8[REG_AL] op1 6,                                                       \
            set_CF(regs8[FLAG_CF] || (regs8[REG_AL] op2 scratch2_uint))),                          \
        set_CF((((mask & 1 ? scratch2_uint : regs8[REG_AL]) & mask) > min) || regs8[FLAG_CF])      \
        && (op_result = regs8[REG_AL] op1 0x60)
#define ADC_SBB_MACRO(a)                                                                           \
    OP(a## = regs8[FLAG_CF] +),                                                                    \
        set_CF(regs8[FLAG_CF] && (op_result == op_dest) || (a op_result < a(int) op_dest)),        \
        set_AF_OF_arith()

// Execute arithmetic/logic operations in emulator memory/registers
#define R_M_OP(dest, op, src)                                                                      \
    (i_w ? op_dest = CAST(uint16_t) dest,                                                          \
        op_result = CAST(uint16_t) dest op(op_source = CAST(uint16_t) src)                         \
         : (op_dest = dest, op_result = dest op(op_source = CAST(uint8_t) src)))
#define MEM_OP(dest, op, src) R_M_OP(mem[dest], op, mem[src])
#define OP(op) MEM_OP(op_to_addr, op, op_from_addr)

// Increment or decrement a register #reg_id (usually SI or DI), depending on
// direction flag and operand size (given by i_w)
#define INDEX_INC(reg_id) (regs16[reg_id] -= (2 * regs8[FLAG_DF] - 1) * (i_w + 1))

// Helpers for stack operations
#define R_M_PUSH(a) (i_w = 1, R_M_OP(mem[SEGREG(REG_SS, REG_SP, --)], =, a))
#define R_M_POP(a) (i_w = 1, regs16[REG_SP] += 2, R_M_OP(a, =, mem[SEGREG(REG_SS, REG_SP, -2 +)]))

// Returns sign bit of an 8-bit or 16-bit operand
#define SIGN_OF(a) (i_w ? ((int16_t)(a) < 0) : ((int8_t)(a) < 0))

#define SEGREG(reg_seg, reg_ofs, op) MAP_ADDR(16 * regs16[reg_seg] + (uint16_t)(op regs16[reg_ofs]))

// Helper functions

// Set carry flag
static char __always_inline set_CF(int new_CF) { return regs8[FLAG_CF] = !!new_CF; }

// Set auxiliary flag
static char __always_inline set_AF(int new_AF) { return regs8[FLAG_AF] = !!new_AF; }

// Set overflow flag
static char __always_inline set_OF(int new_OF) { return regs8[FLAG_OF] = !!new_OF; }

// Set auxiliary and overflow flag after arithmetic operations
static char __always_inline set_AF_OF_arith()
{
    set_AF((op_source ^= op_dest ^ op_result) & 0x10);
    if (op_result == op_dest)
        return set_OF(0);
    else
        return set_OF(1 & (regs8[FLAG_CF] ^ op_source >> (TOP_BIT - 1)));
}

// Assemble and return emulated CPU FLAGS register in scratch_uint
static void __always_inline(make_flags)()
{
    // 8086 has reserved and unused flags set to 1
    scratch_uint = 0xF002 | (regs8[FLAG_CF] << 0) | (regs8[FLAG_PF] << 2) | (regs8[FLAG_AF] << 4)
        | (regs8[FLAG_ZF] << 6) | (regs8[FLAG_SF] << 7) | (regs8[FLAG_TF] << 8)
        | (regs8[FLAG_IF] << 9) | (regs8[FLAG_DF] << 10) | (regs8[FLAG_OF] << 11);
}

// Set emulated CPU FLAGS register from regs8[FLAG_xx] values
static void __always_inline set_flags(int new_flags)
{
    regs8[FLAG_CF] = !!(new_flags & (1 << 0));
    regs8[FLAG_PF] = !!(new_flags & (1 << 2));
    regs8[FLAG_AF] = !!(new_flags & (1 << 4));
    regs8[FLAG_ZF] = !!(new_flags & (1 << 6));
    regs8[FLAG_SF] = !!(new_flags & (1 << 7));
    regs8[FLAG_TF] = !!(new_flags & (1 << 8));
    regs8[FLAG_IF] = !!(new_flags & (1 << 9));
    regs8[FLAG_DF] = !!(new_flags & (1 << 10));
    regs8[FLAG_OF] = !!(new_flags & (1 << 11));
}

// Convert raw opcode to translated opcode index. This condenses a large number
// of different encodings of similar instructions into a much smaller number of
// distinct functions, which we then execute
static void __always_inline set_opcode(uint8_t opcode)
{
    raw_opcode_id = opcode;
    opcode_decode_t dec = op_decode_table[opcode];
    xlat_opcode_id = dec.xlat_id;
    extra = dec.subfunction;
    set_flags_type = dec.flags;
    i_mod_size = dec.mod_size;
}
// Execute INT #interrupt_num on the emulated machine
static char __time_critical_func(pc_interrupt)(uint8_t interrupt_num)
{
    set_opcode(0xCD); // Decode like INT

    make_flags();
    R_M_PUSH(scratch_uint);
    R_M_PUSH(regs16[REG_CS]);
    R_M_PUSH(reg_ip);
    MEM_OP(REGS_BASE + 2 * REG_CS, =, 4 * interrupt_num + 2);
    R_M_OP(reg_ip, =, mem[4 * interrupt_num]);

    // if (interrupt_num == 0x10 && regs8[REG_AH] != 0x0E) {
    //     printf("Int: %x, AH=%x AL=%x \n", interrupt_num, regs8[REG_AH], regs8[REG_AL]);
    // }
    return regs8[FLAG_TF] = regs8[FLAG_IF] = 0;
}

// AAA and AAS instructions - which_operation is +1 for AAA, and -1 for AAS
static int __always_inline AAA_AAS(char which_operation)
{
    return (regs16[REG_AX]
        += 262 * which_operation * set_AF(set_CF(((regs8[REG_AL] & 0x0F) > 9) || regs8[FLAG_AF])),
        regs8[REG_AL] &= 0x0F);
}

// PicoCalc specific keyboard handling
static void __always_inline keyboard_process()
{
    int kbd_event = picocalc_southbridge_kb_read();
    if (unlikely(kbd_event != -1)) {
        uint8_t scancode = kbd_event & 0xFF;
        uint32_t state = KBD_GET_STATE(kbd_event);

        if (unlikely(scancode == 0x91)) {
            printf("Reboot key pressed\n");
            pc_interrupt(0x19);
            return;
        }

        if (state == KBD_STATE_RELEASE) {
            scancode |= 0x80;
        }
        // PicoCalc: Reboot on short press power key
        io_ports[0x60] = scancode;
        pc_interrupt(9);
    }
#ifdef DEBUG_CONSOLE
    if (unlikely(uart_is_readable(uart_default))) { }
#endif
}

extern FATFS fs;
extern const uint8_t _binary_bios_bin_start[];
extern const uint8_t _binary_bios_bin_end[];

#ifdef DEBUG_PERF
static uint64_t start_time = 0;
static uint32_t sample_instructions = 0;
#endif

static FIL fpd, fpfd;
static FRESULT fr;

static uint8_t floppy_present = 0;

void pico_x86_run()
{
    printf("\n▼ Memory Size %d: bytes\n", RAM_SIZE);

    // regs16 and regs8 point to F000:0, the start of memory-mapped registers.
    // CS is initialised to F000
    regs16 = (uint16_t*)(regs8 = mem + REGS_BASE);
    regs16[REG_CS] = 0xF000;

    // Trap flag off
    regs8[FLAG_TF] = 0;

    // Set DL equal to the boot device: 0 for the FD, or 0x80 for the HD.
    regs8[REG_DL] = 0x80;

// TODO: bios override if present
#ifndef BIOS_EMBED
    FIL fpb;
    fr = f_open(&fpb, "0:/x86/bios.bin", FA_READ);

    // Load BIOS image into F000:0100, and set IP to 0100
    UINT br;
    fr = f_read(&fpb, regs8 + (reg_ip = 0x100), 0x3E00, &br);
    if (fr == FR_OK) {
        printf("\n▼ BIOS Image Size: %d bytes\n", br);
    } else {
        printf("\n[FATAL ERROR] BIOS image is missing, empty, or SD card failed! "
               "FATFS Code: %d\n",
            fr);
    }
    f_close(&fpb);
#else
    size_t bios_size = (size_t)(_binary_bios_bin_end - _binary_bios_bin_start);
    printf("\n▼ BIOS found in Flash at 0x%p, size: %zu bytes\n", _binary_bios_bin_start, bios_size);
    memcpy(regs8 + (reg_ip = 0x100), _binary_bios_bin_start, bios_size);
#endif

    fr = f_open(&fpd, "0:/x86/hd.img", FA_READ | FA_WRITE);
    if (fr != FR_OK || f_size(&fpd) == 0) {
        printf("\n[FATAL ERROR] disk image is missing, empty, or SD card failed! "
               "FATFS Code: %d\n",
            fr);
        while (1)
            ;
    } else {
        printf("▼ DISK Image Size: %llu bytes\n", fpd.obj.objsize);
    }

    fr = f_open(&fpfd, FDD_IMAGE_PATH, FA_READ | FA_WRITE);
    if (fr != FR_OK || f_size(&fpfd) == 0) {
        if (fr == FR_OK)
            f_close(&fpfd);
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

    printf("\n▼ Starting BIOS...\n\n");

    // Set CX:AX equal to the hard disk image size, if present
    // CAST(uint32_t) regs16[REG_AX] = *disk ? lseek(*disk, 0, 2) >> 9 : 0;
    CAST(uint32_t)
    regs16[REG_AX] = fpd.obj.objsize >> 9;

    // Load instruction decoding helper tables
    for (int i = 0; i < 20; i++) {
        uint16_t table_addr = regs16[0x81 + i];
        for (int j = 0; j < 256; j++) {
            uint8_t val = regs8[table_addr + j];

            // ModR/M tables (0-7): only need 8 entries
            if (i < 8 && j < 8)
                rm_decode_table[i][j] = val;

            // Opcode decode (Tables 8, 9, 10, 14)
            else if (i == TABLE_XLAT_OPCODE)
                op_decode_table[j].xlat_id = val;
            else if (i == TABLE_XLAT_SUBFUNCTION)
                op_decode_table[j].subfunction = val;
            else if (i == TABLE_STD_FLAGS)
                op_decode_table[j].flags = val;
            else if (i == TABLE_I_MOD_SIZE)
                op_decode_table[j].mod_size = val;

            // Instruction size decode (Tables 12, 13)
            else if (i == TABLE_BASE_INST_SIZE)
                inst_size_table[j].base_size = val;
            else if (i == TABLE_I_W_SIZE)
                inst_size_table[j].w_size = val;

            // Jump logic tables (15-18): only need 8 entries
            else if (i >= TABLE_COND_JUMP_DECODE_A && i <= TABLE_COND_JUMP_DECODE_D && j < 8) {
                jmp_decode_table[i - TABLE_COND_JUMP_DECODE_A][j] = val;
            }
        }
    }

    pico_x86_cpu();
}

void pico_x86_cpu()
{
    //  GOTO Dispatch Table
    static const void* __scratch_y("cpu") dispatch_table[49] = { &&OP_0, &&OP_1, &&OP_2, &&OP_3,
        &&OP_4, &&OP_5, &&OP_6, &&OP_7, &&OP_8, &&OP_9, &&OP_10, &&OP_11, &&OP_12, &&OP_13, &&OP_14,
        &&OP_15, &&OP_16, &&OP_17, &&OP_18, &&OP_19, &&OP_20, &&OP_21, &&OP_22, &&OP_23, &&OP_24,
        &&OP_25, &&OP_26, &&OP_27, &&OP_28, &&OP_29, &&OP_30, &&OP_31, &&OP_32, &&OP_33, &&OP_34,
        &&OP_35, &&OP_36, &&OP_37, &&OP_38, &&OP_39, &&OP_40, &&OP_41, &&OP_42, &&OP_43, &&OP_44,
        &&OP_45, &&OP_46, &&OP_47, &&OP_48 };

    // Instruction execution loop. Terminates if CS:IP = 0:0
    while (opcode_stream = mem + MAP_ADDR(16 * regs16[REG_CS] + reg_ip), opcode_stream != mem) {

        set_opcode(*opcode_stream);

        // printf("CPU: %04X:%04X | Op: 0x%02X | DX: 0x%04X\n", regs16[REG_CS], reg_ip,
        // raw_opcode_id,
        //     regs16[REG_DX]);

        // Extract i_w and i_d fields from instruction
        i_reg4bit = EXTRACT_BITS(raw_opcode_id, 2, 0);
        i_w = EXTRACT_BITS(raw_opcode_id, 0, 0);
        i_d = EXTRACT_BITS(raw_opcode_id, 1, 1);

        // Extract instruction data fields
        i_data0 = CAST(int16_t) opcode_stream[1];
        i_data1 = CAST(int16_t) opcode_stream[2];
        i_data2 = CAST(int16_t) opcode_stream[3];

        // seg_override_en and rep_override_en contain number of instructions to
        // hold segment override and REP prefix respectively
        if (seg_override_en)
            seg_override_en--;
        if (rep_override_en)
            rep_override_en--;

        // i_mod_size > 0 indicates that opcode uses i_mod/i_rm/i_reg, so decode
        // them
        if (i_mod_size) {
            i_mod = EXTRACT_BITS(i_data0, 7, 6);
            i_reg = EXTRACT_BITS(i_data0, 5, 3);
            i_rm = EXTRACT_BITS(i_data0, 2, 0);

            if ((!i_mod && i_rm == 6) || (i_mod == 2))
                i_data2 = CAST(int16_t) opcode_stream[4];
            else if (i_mod != 1)
                i_data2 = i_data1;
            else // If i_mod is 1, operand is (usually) 8 bits rather than 16
                 // bits
                i_data1 = (int8_t)i_data1;

            DECODE_RM_REG;
        }

        if (unlikely(xlat_opcode_id >= 49)) {
            goto OP_NOP;
        }

        // Instruction execution unit
        goto* dispatch_table[xlat_opcode_id];

    OP_0: // Conditional jump
        scratch_uchar = EXTRACT_BITS(raw_opcode_id, 3, 1);
        reg_ip += (int8_t)i_data0
            * (i_w
                ^ (regs8[jmp_decode_table[0][scratch_uchar]]
                    || regs8[jmp_decode_table[1][scratch_uchar]]
                    || regs8[jmp_decode_table[2][scratch_uchar]]
                        ^ regs8[jmp_decode_table[3][scratch_uchar]]));
        NEXT_OP;
    OP_1: // MOV reg, imm
        i_w = EXTRACT_BITS(raw_opcode_id, 3, 3);
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
        i_reg = extra;
        /* Fallthrough */
    OP_5: // INC|DEC|JMP|CALL|PUSH
        if (unlikely(i_reg < 2)) {
            MEM_OP(op_from_addr, += 1 - 2 * i_reg +, REGS_BASE + 2 * REG_ZERO),
                op_source = 1, set_AF_OF_arith(), set_OF(op_dest + 1 - i_reg == BIT(TOP_BIT - 1)),
                (xlat_opcode_id == 5) && (set_opcode(0x10), 0);
        } else if (likely(i_reg != 6)) {
            i_reg - 3 || R_M_PUSH(regs16[REG_CS]),
                i_reg & 2
                && R_M_PUSH(reg_ip + 2 + i_mod * (i_mod != 3) + 2 * (!i_mod && i_rm == 6)),
                i_reg & 1 && ((regs16[REG_CS] = CAST(int16_t) mem[op_from_addr + 2])),
                R_M_OP(reg_ip, =, mem[op_from_addr]), set_opcode(0x9A);
        } else {
            R_M_PUSH(mem[rm_addr]);
        }
        NEXT_OP;
    OP_6: // TEST r/m, imm16 / NOT|NEG|MUL|IMUL|DIV|IDIV reg
        op_to_addr = op_from_addr;
        switch (i_reg) {
        case 0:
            set_opcode(0x20);
            reg_ip += i_w + 1;
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
        }
        NEXT_OP;
    OP_7: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP AL/AX, immed
        rm_addr = REGS_BASE;
        i_data2 = i_data0;
        i_mod = 3;
        i_reg = extra;
        reg_ip--;
        /* Fallthrough */
    OP_8: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP reg, immed
        op_to_addr = rm_addr;
        regs16[REG_SCRATCH] = (i_d |= !i_w) ? (int8_t)i_data2 : i_data2;
        op_from_addr = REGS_BASE + 2 * REG_SCRATCH;
        reg_ip += !i_d + 1;
        set_opcode(0x08 * (extra = i_reg));
        /* Fallthrough */
    OP_9: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP|MOV reg, r/m
        switch (extra) {
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
            //       if (!i_d && rm_addr >= 0x54000 && rm_addr < 0x55000) {
            //         uint8_t val = mem[rm_addr];
            //         printf("TEXT WRITE -> Addr: 0x%05X | Hex: %02X | Char:
            //         %c\n", rm_addr,
            //                val, (val >= 32 && val < 127) ? val : '.');
            //       }
            break;
        }
        NEXT_OP;
    OP_10: // MOV sreg, r/m | POP r/m | LEA reg, r/m
        if ((!i_w)) {
            i_w = 1;
            i_reg += 8;
            DECODE_RM_REG;
            OP(=);
        } else if (!i_d) {
            seg_override_en = 1;
            seg_override = REG_ZERO;
            DECODE_RM_REG;
            R_M_OP(mem[op_from_addr], =, rm_addr);
        } else {
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
    OP_12: // ROL|ROR|RCL|RCR|SHL|SHR|???|SAR reg/mem, 1/CL/imm
        scratch2_uint = SIGN_OF(mem[rm_addr]);
        scratch_uint = extra ? ++reg_ip, (int8_t)i_data1 : i_d ? 31 & regs8[REG_CL] : 1;
        if (scratch_uint) {
            if (i_reg < 4) {
                scratch_uint %= i_reg / 2 + TOP_BIT;
                R_M_OP(scratch2_uint, =, mem[rm_addr]);
            }
            if (i_reg & 1) {
                R_M_OP(mem[rm_addr], >>=, scratch_uint);
            } else {
                R_M_OP(mem[rm_addr], <<=, scratch_uint);
            }
            if (i_reg > 3) {
                set_opcode(0x10);
            }
            if (i_reg > 4) {
                set_CF(op_dest >> (scratch_uint - 1) & 1);
            }
        }
        switch (i_reg) {
        case 0:
            R_M_OP(mem[rm_addr], +=, scratch2_uint >> (TOP_BIT - scratch_uint));
            set_OF(SIGN_OF(op_result) ^ set_CF(op_result & 1));
            break;
        case 1:
            scratch2_uint &= BIT(scratch_uint) - 1;
            R_M_OP(mem[rm_addr], +=, scratch2_uint << (TOP_BIT - scratch_uint));
            set_OF(SIGN_OF(op_result * 2) ^ set_CF(SIGN_OF(op_result)));
            break;
        case 2:
            R_M_OP(mem[rm_addr], += (regs8[FLAG_CF] << (scratch_uint - 1)) +,
                scratch2_uint >> (1 + TOP_BIT - scratch_uint));
            set_OF(SIGN_OF(op_result) ^ set_CF(scratch2_uint & BIT(TOP_BIT - scratch_uint)));
            break;
        case 3:
            R_M_OP(mem[rm_addr], += (regs8[FLAG_CF] << (TOP_BIT - scratch_uint)) +,
                scratch2_uint << (1 + TOP_BIT - scratch_uint));
            set_CF(scratch2_uint & BIT(scratch_uint - 1));
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
            R_M_OP(mem[rm_addr], +=, scratch2_uint *= ~((BIT(TOP_BIT) - 1) >> scratch_uint));
            break;
        }
        NEXT_OP;
    OP_13: // LOOPxx|JCZX
        scratch_uint = !!--regs16[REG_CX];
        switch (i_reg4bit) {
        case 0:
            scratch_uint &= !regs8[FLAG_ZF];
            break;
        case 1:
            scratch_uint &= regs8[FLAG_ZF];
            break;
        case 3:
            scratch_uint = !++regs16[REG_CX];
            break;
        }
        reg_ip += scratch_uint * (int8_t)i_data0;
        NEXT_OP;
    OP_14: // JMP | CALL short/near
        reg_ip += 3 - i_d;
        if (!i_w) {
            if (i_d) {
                reg_ip = 0;
                regs16[REG_CS] = i_data2;
            } else {
                R_M_PUSH(reg_ip);
            }
        }
        reg_ip += i_d && i_w ? (int8_t)i_data0 : i_data0;
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
        for (scratch_uint = rep_override_en ? regs16[REG_CX] : 1; scratch_uint; scratch_uint--) {

            //       uint32_t pico_mem_idx = 0;
            // if (extra < 2) {
            //     uint32_t absolute_dest_addr = (((uint32_t)regs16[REG_ES]) << 4) +
            //     regs16[REG_DI]; (void)absolute_dest_addr;
            //     // pico_mem_idx = MAP_ADDR(absolute_dest_addr);
            // }

            MEM_OP(extra < 2 ? SEGREG(REG_ES, REG_DI, ) : REGS_BASE, =,
                extra & 1 ? REGS_BASE : SEGREG(scratch2_uint, REG_SI, ));

            //       --- if (extra < 2 && pico_mem_idx >= 0x50000 &&
            //       pico_mem_idx < 0x70000) {
            //         // Now mem[pico_mem_idx] holds the actual byte that was
            //         written printf("VRAM write dst: %x sram: %x, value:
            //         %02X\n", pico_mem_idx,
            //                &mem[pico_mem_idx], mem[pico_mem_idx]);
            //       }

            extra & 1 || INDEX_INC(REG_SI);
            extra & 2 || INDEX_INC(REG_DI);
        }
        if (rep_override_en)
            regs16[REG_CX] = 0;
        NEXT_OP;
    OP_18: // CMPSx|SCASx
        scratch2_uint = seg_override_en ? seg_override : REG_DS;
        if (likely((scratch_uint = rep_override_en ? regs16[REG_CX] : 1))) {
            for (; scratch_uint; rep_override_en || scratch_uint--) {
                MEM_OP(extra ? REGS_BASE : SEGREG(scratch2_uint, REG_SI, ), -,
                    SEGREG(REG_ES, REG_DI, ));
                extra || INDEX_INC(REG_SI);
                INDEX_INC(REG_DI);
                rep_override_en && !(--regs16[REG_CX] && (!op_result == rep_mode))
                    && (scratch_uint = 0);
            }
            set_flags_type = FLAGS_UPDATE_SZP | FLAGS_UPDATE_AO_ARITH;
            set_CF(op_result > op_dest);
        }
        NEXT_OP;
    OP_19: // RET|RETF|IRET
        i_d = i_w;
        R_M_POP(reg_ip);
        if (extra)
            R_M_POP(regs16[REG_CS]);
        if (extra & 2)
            set_flags(R_M_POP(scratch_uint));
        else if (!i_d)
            regs16[REG_SP] += i_data0;
        NEXT_OP;
    OP_20: // MOV r/m, immed
        R_M_OP(mem[op_from_addr], =, i_data2);
        NEXT_OP;
    OP_21: // IN AL/AX, DX/imm8
        scratch_uint = extra ? regs16[REG_DX] : (uint8_t)i_data0;
        if (likely(scratch_uint < IO_PORT_COUNT)) {
            if (scratch_uint == 0x20) {
                io_ports[0x20] = 0;
            } else if (scratch_uint == 0x40 || scratch_uint == 0x42) {
                io_ports[0x42] = --io_ports[0x40];
            } else if ((scratch_uint >= 0x03D0 && scratch_uint <= 0x03DF)) {
                video_cga_port_in(scratch_uint);
            } else if (scratch_uint == 0x60) {
                R_M_OP(regs8[REG_AL], =, io_ports[0x60]);
                io_ports[0x64] = 0;
            } else if (scratch_uint >= 0x3F8 && scratch_uint <= 0x3FF) {
                serial_port_in(scratch_uint);
            }
            R_M_OP(regs8[REG_AL], =, io_ports[scratch_uint]);
        } else {
            regs8[REG_AL] = 0xFF;
        }
        NEXT_OP;
    OP_22: // OUT DX/imm8, AL/AX
        scratch_uint = extra ? regs16[REG_DX] : (uint8_t)i_data0;
        if (likely(scratch_uint < IO_PORT_COUNT)) {
            R_M_OP(io_ports[scratch_uint], =, regs8[REG_AL]);

            if ((scratch_uint == 0x61)) {
                io_hi_lo = 0;
                spkr_en |= (regs8[REG_AL] & 3);
            }

            if ((scratch_uint == 0x40 || scratch_uint == 0x42) && (io_ports[0x43] & 6)) {
                io_hi_lo ^= 1;
                mem[0x469 + scratch_uint - io_hi_lo] = regs8[REG_AL];
            }

            if ((scratch_uint >= 0x03D0 && scratch_uint <= 0x03DF)) {
                video_cga_port_out(scratch_uint);
            }

            if (scratch_uint >= 0x3F8 && scratch_uint <= 0x3FF) {
                serial_port_out(scratch_uint);
            }
        }
        // scratch_uint == 0x3B5 && io_ports[0x3B4] == 1;
        // scratch_uint == 0x3B5 && io_ports[0x3B4] == 6;
        NEXT_OP;
    OP_23: // REPxx
        rep_override_en = 2;
        rep_mode = i_w;
        seg_override_en&& seg_override_en++;
        NEXT_OP;
    OP_25: // PUSH reg
        R_M_PUSH(regs16[extra]);
        NEXT_OP;
    OP_26: // POP reg
        R_M_POP(regs16[extra]);
        NEXT_OP;
    OP_27: // xS: segment overrides
        seg_override_en = 2;
        seg_override = extra;
        rep_override_en&& rep_override_en++;
        NEXT_OP;
    OP_28: // DAA/DAS
        i_w = 0;
        extra ? DAA_DAS(-=, >=, 0xFF, 0x99) : DAA_DAS(+=, <, 0xF0, 0x90);
        NEXT_OP;
    OP_29: // AAA/AAS
        op_result = AAA_AAS(extra - 1);
        NEXT_OP;
    OP_30: // CBW
        regs8[REG_AH] = -SIGN_OF(regs8[REG_AL]);
        NEXT_OP;
    OP_31: // CWD
        regs16[REG_DX] = -SIGN_OF(regs16[REG_AX]);
        NEXT_OP;
    OP_32: // CALL FAR imm16:imm16
        R_M_PUSH(regs16[REG_CS]);
        R_M_PUSH(reg_ip + 5);
        regs16[REG_CS] = i_data2;
        reg_ip = i_data0;
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
        set_flags((scratch_uint & 0xFF00) + regs8[REG_AH]);
        NEXT_OP;
    OP_36: // LAHF
        make_flags();
        regs8[REG_AH] = scratch_uint;
        NEXT_OP;
    OP_37: // LES|LDS reg, r/m
        i_w = i_d = 1;
        DECODE_RM_REG;
        OP(=);
        MEM_OP(REGS_BASE + extra, =, rm_addr + 2);
        NEXT_OP;
    OP_38: // INT 3
        ++reg_ip;
        pc_interrupt(3);
        NEXT_OP;
    OP_39: // INT imm8
        reg_ip += 2;
        pc_interrupt(i_data0);
        NEXT_OP;
    OP_40: // INTO
        ++reg_ip;
        regs8[FLAG_OF] && pc_interrupt(4);
        NEXT_OP;
    OP_41: // AAM
        if (i_data0 &= 0xFF) {
            regs8[REG_AH] = regs8[REG_AL] / i_data0;
            op_result = regs8[REG_AL] %= i_data0;
        } else {
            pc_interrupt(0);
        }
        NEXT_OP;
    OP_42: // AAD
        i_w = 0;
        regs16[REG_AX] = op_result = 0xFF & regs8[REG_AL] + i_data0 * regs8[REG_AH];
        NEXT_OP;
    OP_43: // SALC
        regs8[REG_AL] = -regs8[FLAG_CF];
        NEXT_OP;
    OP_44: // XLAT
        regs8[REG_AL]
            = mem[SEGREG(seg_override_en ? seg_override : REG_DS, REG_BX, regs8[REG_AL] +)];
        NEXT_OP;
    OP_45: // CMC
        regs8[FLAG_CF] ^= 1;
        NEXT_OP;
    OP_46: // CLC|STC|CLI|STI|CLD|STD
        regs8[extra / 2] = extra & 1;
        NEXT_OP;
    OP_47: // TEST AL/AX, immed
        R_M_OP(regs8[REG_AL], &, i_data0);
        NEXT_OP;
    OP_48: // Emulator-specific 0F xx opcodes
        switch ((int8_t)i_data0) {
        case 0: { // INT 14h Serial Port I/O (COM1)
            serial_ctl();
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
            if (unlikely(regs16[REG_AX] == 0)) {
                regs16[REG_AX] = 0;
                break;
            }
            //  disk index in DX before this call: 0 = hd, 1 = fp
            uint8_t is_floppy = regs16[REG_DX] & 1;
            if (unlikely(is_floppy && !floppy_present)) {
                regs16[REG_AX] = 0; // No media in drive A:
                break;
            }
            FIL* fp = is_floppy ? &fpfd : &fpd;
            DWORD abs_sector = ((DWORD)regs16[REG_SI] << 16) | regs16[REG_BP];
            if (likely(f_lseek(fp, abs_sector << 9) == FR_OK)) {
                f_read(fp, mem + SEGREG(REG_ES, REG_BX, ), regs16[REG_AX], &br);
            }

            if (unlikely(br == 0)) {
                if (is_floppy) {
                    regs16[REG_AX] = 0;
                    break;
                }
#ifdef DEBUG_CONSOLE
                printf("\n[FATAL ERROR] Disk read failed at absolute sector %lu!\n", abs_sector);
#endif
                while (1)
                    ;
            }
            regs16[REG_AX] = br;
            break;
        }
        case 3: { // DISK WRITE
            UINT bw = 0;
            //  disk index in DX before this call: 0 = hd, 1 = fp
            uint8_t is_floppy = regs16[REG_DX] & 1;
            if (unlikely(is_floppy && !floppy_present)) {
                regs16[REG_AX] = 0; // No media in drive A:
                break;
            }
            FIL* fp = is_floppy ? &fpfd : &fpd;
            DWORD abs_sector = ((DWORD)regs16[REG_SI] << 16) | regs16[REG_BP];
            if (likely(f_lseek(fp, abs_sector << 9) == FR_OK)) {
                f_write(fp, mem + SEGREG(REG_ES, REG_BX, ), regs16[REG_AX], &bw);
            }
            if (unlikely(bw == 0)) {
                if (is_floppy) {
                    regs16[REG_AX] = 0;
                    break;
                }
#ifdef DEBUG_CONSOLE
                printf("\n[FATAL ERROR] Disk write failed at absolute sector %lu!\n", abs_sector);
#endif
                while (1)
                    ;
            }
            regs16[REG_AX] = bw;
            break;
        }
        case 5: {
            uint32_t src = SEGREG(REG_ES, REG_BX, );
            struct tm new_tm = { 0 };
            new_tm.tm_sec = (int32_t)CAST(uint32_t) mem[src + 0];
            new_tm.tm_min = (int32_t)CAST(uint32_t) mem[src + 4];
            new_tm.tm_hour = (int32_t)CAST(uint32_t) mem[src + 8];
            new_tm.tm_mday = (int32_t)CAST(uint32_t) mem[src + 12];
            new_tm.tm_mon = (int32_t)CAST(uint32_t) mem[src + 16];
            new_tm.tm_year = (int32_t)CAST(uint32_t) mem[src + 20];

            int ok = aon_timer_is_running() ? aon_timer_set_time_calendar(&new_tm)
                                            : aon_timer_start_calendar(&new_tm);
            if (likely(ok)) {
                // save_rtc_to_disk(&new_tm);
                regs8[REG_AL] = 0x00; // Success
            } else {
                regs8[REG_AL] = 0xFF; // Failure
            }
            break;
        }
        case 6: {
            uint32_t ticks = ((uint32_t)regs16[REG_CX] << 16) | regs16[REG_DX];
            uint32_t hour = ticks / 65520;
            uint32_t rem = ticks % 65520;
            uint32_t min = rem / 1092;
            rem %= 1092;
            uint32_t sec = (rem * 10) / 182;

            if (unlikely(hour > 23))
                hour = 23;

            struct tm new_tm = { 0 };
            if (likely(aon_timer_get_time_calendar(&new_tm))) {
                new_tm.tm_hour = hour;
                new_tm.tm_min = min;
                new_tm.tm_sec = sec;

                int ok = aon_timer_is_running() ? aon_timer_set_time_calendar(&new_tm)
                                                : aon_timer_start_calendar(&new_tm);
                if (likely(ok)) {
                    regs8[REG_AL] = 0x00;
                } else {
                    regs8[REG_AL] = 0xFF;
                }
            } else {
                regs8[REG_AL] = 0xFF;
            }
            break;
        }
        }
        NEXT_OP;
    OP_NOP: // Catch for unimplemented opcodes
        NEXT_OP;

    next_opcode:

        inst_size_t size = inst_size_table[raw_opcode_id];

        reg_ip += (i_mod * (i_mod != 3) + 2 * (!i_mod && i_rm == 6)) * i_mod_size + size.base_size
            + size.w_size * (i_w + 1);

        // If instruction needs to update SF, ZF and PF, set them as appropriate
        if (set_flags_type & FLAGS_UPDATE_SZP) {
            regs8[FLAG_SF] = SIGN_OF(op_result);
            regs8[FLAG_ZF] = !op_result;

            regs8[FLAG_PF] = !(hweight8(op_result) & 1);

            // If instruction is an arithmetic or logic operation, also set
            // AF/OF/CF as appropriate.
            if (set_flags_type & FLAGS_UPDATE_AO_ARITH)
                set_AF_OF_arith();
            if (set_flags_type & FLAGS_UPDATE_OC_LOGIC)
                set_CF(0), set_OF(0);
        }

        // Application has set trap flag, so fire INT 1
        if (unlikely(trap_flag != 0)) {
            pc_interrupt(1);
        }

        trap_flag = regs8[FLAG_TF];

        // If a timer tick or serial is pending, interrupts are enabled, and no
        // overrides/REP are active, then process the tick and check for new
        // keystrokes
        // At the end of the loop:
        if ((!seg_override_en && !rep_override_en && regs8[FLAG_IF] && !regs8[FLAG_TF])) {
            if (unlikely(int8_asap)) {
                pc_interrupt(0xA);
                int8_asap = 0;
                keyboard_process();
            } else if (unlikely(serial_int_pending())) {
                pc_interrupt(0x0C); // Trigger IRQ 4
            }
        }

#ifdef DEBUG_PERF
        if (unlikely(sample_instructions++ >= 1000000)) {
            if (start_time != 0) {
                float mips = 1000000.0f / (time_us_64() - start_time);
                float equivalent_mhz = mips * 14.45f;
                printf("Perf: %.2f MIPS | Hardware: %.2f MHz\n ", mips, equivalent_mhz);
            }
            start_time = time_us_64();
            sample_instructions = 0;
        }
#endif
    }

    printf("\n!!! EMULATOR EXITED MAIN LOOP !!!\n");
    printf("Final CPU State -> CS: %04X | IP: %04X\n\n", regs16[REG_CS], reg_ip);
}
