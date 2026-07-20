/*
 * Portions of this file are derived from 8086tiny.
 * Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
 * Licensed under the MIT License.
 * See LICENSE.txt.
 *
 * Modifications Copyright (c) 2026 Serg Podtynnyi <serg@podtynnyi.com>
 * Licensed under GPLv3 for the combined work.
 */

#pragma once

#include "hardware/hazard3/features.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define hweight8(w) __builtin_popcount((uint8_t)(w))
#define hweight16(w) __builtin_popcount((uint16_t)(w))
#define hweight32(w) __builtin_popcount((uint32_t)(w))

#define BIT(nr) (1UL << (nr))

// Extracts bits from 'high' down to 'low' (inclusive)
#ifdef __hazard3_extension_xh3bextm
// Use a single hardware instruction on the RP2350
#define EXTRACT_BITS(val, high, low) __hazard3_bextmi(((high) - (low) + 1), (val), (low))
#else
// Fallback for standard C
#define EXTRACT_BITS(val, high, low) (((val) >> (low)) & ((1UL << ((high) - (low) + 1)) - 1))
#endif

// Generates a contiguous bitmask. Example: GENMASK(7, 4) creates 0b11110000
// (0xF0)
#define GENMASK(h, l) (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (32 - 1 - (h))))

// Emulator system constants
#define IO_PORT_COUNT 0x400
#define RAM_SIZE 0x74000
#define REGS_BASE 0x70000
#define LOW_MEM_LIMIT 0x6C000 // Available memrory to DOS

// clang-format off
static uint32_t __always_inline MAP_ADDR(uint32_t A) {

    A &= 0xFFFFF;

    // Direct map
    if (likely(A < LOW_MEM_LIMIT))
        return A;

    // CGA VRAM (Strictly 16KB: 0xB8000-0xBBFFF)
    if (A >= 0xB8000 && A < 0xBC000)
        return 0x6C000 + (A - 0xB8000);

    // BIOS ROM F0000-FFFFF (Aliased into 16KB physical space)
    if (A >= 0xF0000)
        return 0x70000 + ((A - 0xF0000) & 0x3FFF);

    // all other unmapped memory
    return 0x74000 + (A & 3);
}

// 16-bit register decodes
enum reg16 {
    REG_AX = 0, // Accumulator: Primary register for arithmetic, logic, and I/O operations.
    REG_CX, // Counter: Used primarily as a loop and string operation counter.
    REG_DX, // Data: Used for I/O port addressing and large (32-bit) multiply/divide operations.
    REG_BX, // Base: Used as a base pointer for memory addressing.
    REG_SP, // Stack Pointer: Points to the top of the current hardware stack.
    REG_BP, // Base Pointer: Used to reference arguments and local variables on the stack.
    REG_SI, // Source Index: Used as the source offset for string and memory copy operations.
    REG_DI, // Destination Index: Used as the destination offset for string and memory copy
            // operations.
    REG_ES, // Extra Segment: Additional data segment, often used as the destination for string ops.
    REG_CS, // Code Segment: Points to the memory segment containing the currently executing CPU
            // instructions.
    REG_SS, // Stack Segment: Points to the memory segment containing the CPU stack.
    REG_DS, // Data Segment: The default memory segment for most data read/write operations.

    // --- Emulator-Specific Virtual Registers ---
    REG_ZERO, // Virtual Zero: An internal emulator shortcut representing a hardcoded zero value.
    REG_SCRATCH // Scratchpad: An internal temporary register used by the emulator for complex macro
                // calculations.
};

// 8-bit register decodes
enum reg8 {
    REG_AL = 0, // Accumulator Low: Lower 8 bits of AX (often used for I/O and specific math ops).
    REG_AH, // Accumulator High: Upper 8 bits of AX.
    REG_CL, // Counter Low: Lower 8 bits of CX (heavily used as the count for shift/rotate ops).
    REG_CH, // Counter High: Upper 8 bits of CX.
    REG_DL, // Data Low: Lower 8 bits of DX.
    REG_DH, // Data High: Upper 8 bits of DX.
    REG_BL, // Base Low: Lower 8 bits of BX.
    REG_BH // Base High: Upper 8 bits of BX.
};

// FLAGS register decodes (Offsets into the regs8 array)
enum cpu_flag {
    FLAG_CF = 40, // Carry Flag: Set on arithmetic carry/borrow out of the most significant bit.
    FLAG_PF, // Parity Flag: Set if the lowest byte of a result has an even number of 1s.
    FLAG_AF, // Auxiliary Flag: Set on carry/borrow between bit 3 and 4 (used for BCD math).
    FLAG_ZF, // Zero Flag: Set if the result of an arithmetic or logic operation is zero.
    FLAG_SF, // Sign Flag: Set to the most significant bit of the result (1 means negative).
    FLAG_TF, // Trap Flag: Enables single-step execution for debugging.
    FLAG_IF, // Interrupt Enable Flag: Determines if the CPU responds to hardware interrupts.
    FLAG_DF, // Direction Flag: Controls direction of string operations (0 = auto-increment, 1 =
             // auto-decrement).
    FLAG_OF // Overflow Flag: Set if signed arithmetic results in a value too large/small to fit.
};

// Lookup tables in the BIOS binary
enum bios_table {
    TABLE_XLAT_OPCODE = 8, // Opcode -> internal ID.
    TABLE_XLAT_SUBFUNCTION = 9, // Group opcode sub-ID.
    TABLE_STD_FLAGS = 10, // Modified flags bitmask.
    TABLE_PARITY_FLAG = 11, // Quick parity lookup.
    TABLE_BASE_INST_SIZE = 12, // Base instruction bytes.
    TABLE_I_W_SIZE = 13, // Bytes added by 'w' bit.
    TABLE_I_MOD_SIZE = 14, // Bytes added by ModR/M.
    TABLE_COND_JUMP_DECODE_A = 15, // Jump logic matrix A.
    TABLE_COND_JUMP_DECODE_B = 16, // Jump logic matrix B.
    TABLE_COND_JUMP_DECODE_C = 17, // Jump logic matrix C.
    TABLE_COND_JUMP_DECODE_D = 18, // Jump logic matrix D.
    TABLE_FLAGS_BITFIELDS = 19 // Map to actual 16-bit FLAGS.
};

// Bitfields for TABLE_STD_FLAGS values
enum flag_update_type {
    FLAGS_UPDATE_SZP = 1, // Update Sign, Zero, and Parity.
    FLAGS_UPDATE_AO_ARITH = 2, // Update Aux and Overflow (Math ops).
    FLAGS_UPDATE_OC_LOGIC = 4 // Clear Overflow and Carry (Logic ops).
};

extern uint16_t picocalc_southbridge_kb_read();

void pico_x86_run();
void pico_x86_cpu();
