// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/regs/uart.h"
#include "hardware/address_mapped.h"
#include "hardware/gpio.h"

#include "pico_x86.h"
#include "pico_x86_serial.h"

extern uint8_t io_ports[];
extern uint8_t* regs8;
extern uint16_t* regs16;

static bool serial_hw_ready = false;

static uint8_t com1_lcr = 0x03; // 8-N-1 by default
static uint8_t com1_ier = 0x00;
static uint8_t com1_mcr = 0x00;
static uint8_t com1_fcr = 0x00;
static uint8_t com1_dll = 115200UL / DEFAULT_SERIAL_BAUDRATE;
static uint8_t com1_dlm = 0x00;

static bool com1_rx_irq_pending = false;
static uint8_t com1_lsr_err_latch = 0; // Latched OE/PE/FE/BI bits, LSR bit positions

static void com1_set_mcr(uint8_t val)
{
    com1_mcr = val;
    if (val & 0x10)
        hw_set_bits(&uart_get_hw(SERIAL_UART_ID)->cr, UART_UARTCR_LBE_BITS);
    else
        hw_clear_bits(&uart_get_hw(SERIAL_UART_ID)->cr, UART_UARTCR_LBE_BITS);
    if (!(val & 0x08))
        com1_rx_irq_pending = false; // OUT2 cleared: force re-arm on next enable
}

static void com1_apply_config(void)
{
    uint16_t divisor = ((uint16_t)com1_dlm << 8) | com1_dll;
    uint32_t baud = 2400;
    if (likely(divisor != 0)) {
        baud = 115200UL / divisor;
        if (unlikely(baud == 0))
            baud = 110;
    }
    uint32_t data_bits = 5 + (com1_lcr & 3);
    uint32_t stop_bits = (com1_lcr & 4) ? 2 : 1;
    uart_parity_t parity;
    if (!(com1_lcr & 0x08))
        parity = UART_PARITY_NONE; // Parity disabled
    else
        parity = (com1_lcr & 0x10) ? UART_PARITY_EVEN : UART_PARITY_ODD;

    bool brk = (com1_lcr & 0x40) != 0;

    hw_clear_bits(&uart_get_hw(SERIAL_UART_ID)->cr, UART_UARTCR_UARTEN_BITS);
    uart_set_baudrate(SERIAL_UART_ID, baud);
    uart_set_format(SERIAL_UART_ID, data_bits, stop_bits, parity);
    uart_set_break(SERIAL_UART_ID, brk);
    hw_set_bits(&uart_get_hw(SERIAL_UART_ID)->cr, UART_UARTCR_UARTEN_BITS);
    printf("[SERIAL DEBUG] serial UART: %d config change: %d %d,%d,%d break=%d\n", SERIAL_UART_ID,
        baud, data_bits, parity, stop_bits, brk);
}

void serial_hw_init(void)
{
    if (likely(serial_hw_ready))
        return;

    uart_init(SERIAL_UART_ID, DEFAULT_SERIAL_BAUDRATE);
    gpio_set_function(SERIAL_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(SERIAL_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(SERIAL_UART_ID, false, false);
    uart_set_fifo_enabled(SERIAL_UART_ID, true);
    serial_hw_ready = true;
    com1_apply_config();
    printf("[SERIAL DEBUG] UART up: TX=%d RX=%d\n", SERIAL_TX_PIN, SERIAL_RX_PIN);
}

void __time_critical_func(serial_port_in)(uint16_t port)
{
    if (unlikely(!serial_hw_ready))
        serial_hw_init();

    switch (port) {
    case 0x3F8: // RBR (data) or DLL, depending on DLAB
        if (com1_lcr & 0x80) {
            io_ports[port] = com1_dll;
        } else if (uart_is_readable(SERIAL_UART_ID)) {
            uint32_t raw = uart_get_hw(SERIAL_UART_ID)->dr;
            io_ports[port] = raw & 0xFF;

            // PL011 DR error bits -> 16550 LSR bit positions (OE=1, PE=2, FE=3, BI=4).
            // Latched here and held until LSR is read, matching real 8250/16550 behavior.
            com1_lsr_err_latch |= (uint8_t)((((raw >> 11) & 1) << 1) // OE
                | (((raw >> 9) & 1) << 2) // PE
                | (((raw >> 8) & 1) << 3) // FE
                | (((raw >> 10) & 1) << 4)); // BE -> BI

            com1_rx_irq_pending = false;
            //     printf("[SERIAL DEBUG] port IN 0x3F8: raw DR=%08lx (OE=%d BE=%d PE=%d FE=%d "
            //            "data=%02x)\n",
            //         (unsigned long)raw, (raw >> 11) & 1, (raw >> 10) & 1, (raw >> 9) & 1,
            //         (raw >> 8) & 1, raw & 0xFF);
        }
        break;
    case 0x3F9: // IER or DLM
        io_ports[port] = (com1_lcr & 0x80) ? com1_dlm : com1_ier;
        break;
    case 0x3FA: // IIR -- Dynamic interrupt reporting
        if ((com1_ier & 0x01) && uart_is_readable(SERIAL_UART_ID)) {
            io_ports[port] = 0xC4; // FIFO enabled (C0) + Received Data Available (04)
        } else {
            io_ports[port] = 0xC1; // FIFO enabled (C0) + No interrupt pending (01)
        }
        break;
    case 0x3FB: // LCR
        io_ports[port] = com1_lcr;
        break;
    case 0x3FC: // MCR
        io_ports[port] = com1_mcr;
        break;
    case 0x3FD: // LSR
        io_ports[port] = (uart_is_readable(SERIAL_UART_ID) ? 0x01 : 0x00)
            | (uart_is_writable(SERIAL_UART_ID) ? 0x60 : 0x00) | com1_lsr_err_latch;
        com1_lsr_err_latch = 0; // Reading LSR clears OE/PE/FE/BI, per 16550 spec
        break;
    case 0x3FE: // MSR -- no real modem lines; report CTS/DSR/DCD asserted
        io_ports[port] = 0xB0;
        break;
    case 0x3FF:
    default:
        break;
    }
}

void __time_critical_func(serial_port_out)(uint16_t port)
{
    if (unlikely(!serial_hw_ready))
        serial_hw_init();

    uint8_t val = io_ports[port];

    switch (port) {
    case 0x3F8: // THR (data) or DLL, depending on DLAB
        if (com1_lcr & 0x80) {
            com1_dll = val;
            com1_apply_config();
        } else if (likely(uart_is_writable(SERIAL_UART_ID))) {
            uart_putc_raw(SERIAL_UART_ID, val);
        }
        // If the emulated FIFO isn't ready, the byte is dropped
        break;
    case 0x3F9: // IER or DLM
        if (com1_lcr & 0x80) {
            com1_dlm = val;
            com1_apply_config();
        } else {
            uint8_t old_ier = com1_ier;
            com1_ier = val;
            if (!(old_ier & 0x01) && (val & 0x01))
                com1_rx_irq_pending = false;
        }
        break;
    case 0x3FA: // FCR
        if ((val & 0x01) != (com1_fcr & 0x01))
            uart_set_fifo_enabled(SERIAL_UART_ID, val & 0x01);
        if (val & 0x02) { // Clear RX FIFO
            while (uart_is_readable(SERIAL_UART_ID))
                (void)uart_get_hw(SERIAL_UART_ID)->dr;
            com1_lsr_err_latch = 0;
            com1_rx_irq_pending = false;
        }
        // Bit 2: Clear TX FIFO
        if (val & 0x04) {
            uart_get_hw(SERIAL_UART_ID)->cr &= ~UART_UARTCR_UARTEN_BITS;
            uart_get_hw(SERIAL_UART_ID)->cr |= UART_UARTCR_UARTEN_BITS;
        }
        com1_fcr = val;
        break;
    case 0x3FB: // LCR
        com1_lcr = val;
        com1_apply_config();
        break;
    case 0x3FC: // MCR -- RTS/DTR/OUT1/OUT2/loopback bits
        com1_set_mcr(val);
        break;
    case 0x3FD: // LSR
    case 0x3FE: // MSR
        break; // Read-only
    case 0x3FF: // Scratch register
    default:
        break;
    }
}

void serial_ctl(void)
{
    uint8_t ah = regs8[REG_AH];
    uint16_t port = regs16[REG_DX];

    // printf("[SERIAL DEBUG] entry: AH=%02x AL=%02x DX=%04x\n", ah, regs8[REG_AL], port);

    // We support only COM1
    if (unlikely(port != 0)) {
        printf("[SERIAL DEBUG] port %04x != 0, reporting not-present\n", port);
        regs8[REG_AH] = 0x80; // Timeout / port not present
        return;
    }

    serial_hw_init();

    switch (ah) {
    case 0x00: { // Initialize port: AL = line control byte
        static const uint32_t baud_table[8] = { 110, 150, 300, 600, 1200, 2400, 4800, 9600 };
        static const uint16_t divisor_table[8] = { 1047, 768, 384, 192, 96, 48, 24, 12 };
        uint8_t al = regs8[REG_AL];
        uint8_t baud_sel = (al >> 5) & 7;
        uint32_t data_bits = 5 + (al & 3);
        uint32_t stop_bits = (al & 4) ? 2 : 1;
        uart_parity_t parity;

        // INT 14h's AL only spends 2 bits (4:3) on parity, unlike
        // the real LCR's 3 bits (5:3) -- translate explicitly
        // rather than copying AL's bits directly into com1_lcr.
        uint8_t lcr_parity_bits;
        switch ((al >> 3) & 3) {
        case 1:
            parity = UART_PARITY_ODD;
            lcr_parity_bits = 0x08;
            break;
        case 3:
            parity = UART_PARITY_EVEN;
            lcr_parity_bits = 0x18;
            break;
        default:
            parity = UART_PARITY_NONE;
            lcr_parity_bits = 0x00;
            break;
        }

        com1_dll = divisor_table[baud_sel] & 0xFF;
        com1_dlm = (divisor_table[baud_sel] >> 8) & 0xFF;
        com1_lcr = (al & 0x07) | lcr_parity_bits; // DLAB stays 0
        com1_apply_config();

        regs8[REG_AH] = (uart_is_writable(SERIAL_UART_ID) ? 0x60 : 0x00)
            | (uart_is_readable(SERIAL_UART_ID) ? 0x01 : 0x00);
        regs8[REG_AL] = 0xB0; // Fake MSR: CTS/DSR/DCD asserted
        printf("[SERIAL DEBUG] init: baud=%lu data_bits=%lu stop_bits=%lu parity=%d "
               "-> AH=%02x\n",
            (unsigned long)baud_table[baud_sel], (unsigned long)data_bits, (unsigned long)stop_bits,
            parity, regs8[REG_AH]);
        break;
    }
    case 0x01: { // Send character in AL
        uint8_t ch = regs8[REG_AL];
        absolute_time_t deadline = make_timeout_time_ms(100);
        while (!uart_is_writable(SERIAL_UART_ID) && !time_reached(deadline))
            tight_loop_contents();

        bool writable = uart_is_writable(SERIAL_UART_ID);
        if (likely(writable)) {
            uart_putc_raw(SERIAL_UART_ID, ch);
            regs8[REG_AH] = 0x60; // THRE + TSRE, no errors
        } else {
            regs8[REG_AH] = 0x80; // Timeout
        }
        // printf("[SERIAL DEBUG] send: ch=%02x writable=%d -> AH=%02x\n", ch, writable,
        // regs8[REG_AH]);
        break;
    }
    case 0x02: { // Receive character into AL
        absolute_time_t deadline = make_timeout_time_ms(1000);
        while (!uart_is_readable(SERIAL_UART_ID) && !time_reached(deadline))
            tight_loop_contents();

        bool readable = uart_is_readable(SERIAL_UART_ID);
        if (likely(readable)) {
            uint32_t raw = uart_get_hw(SERIAL_UART_ID)->dr;
            regs8[REG_AL] = raw & 0xFF;
            regs8[REG_AH] = 0x00;
            // printf("[SERIAL DEBUG] recv raw DR=%08lx (OE=%d BE=%d PE=%d FE=%d
            // data=%02x)\n", (unsigned long)raw, (raw >> 11) & 1, (raw >> 10) & 1, (raw >>
            // 9) & 1, (raw >> 8) & 1, raw & 0xFF);
            com1_rx_irq_pending = false;
        } else {
            regs8[REG_AH] = 0x80; // Timeout, no data received
        }
        // printf("[SERIAL DEBUG] recv: readable=%d -> AH=%02x AL=%02x\n", readable,
        // regs8[REG_AH], regs8[REG_AL]);
        break;
    }
    case 0x03: { // Query port status
        regs8[REG_AH] = (uart_is_readable(SERIAL_UART_ID) ? 0x01 : 0x00)
            | (uart_is_writable(SERIAL_UART_ID) ? 0x60 : 0x00);
        regs8[REG_AL] = 0xB0; // Fake MSR: CTS/DSR/DCD asserted
#ifdef DEBUG_CONSOLE
        printf("[SERIAL DEBUG] status -> AH=%02x\n", regs8[REG_AH]);
#endif
        break;
    }
    case 0x04: { // Extended Initialize (PS/2-style)
        // BH = parity (0=none,1=odd,2=even,3=mark,4=space)
        // BL = stop bits (0=1, 1=2)
        // CH = data bits (5-8)
        // CL = baud rate code (0=110,1=150,2=300,3=600,4=1200,
        //      5=2400,6=4800,7=9600,8=19200)
        static const uint16_t ext_divisor_table[9] = { 1047, 768, 384, 192, 96, 48, 24, 12, 6 };
        static const uint8_t ext_parity_bits[5] = { 0x00, 0x08, 0x18, 0x28, 0x38 };

        uint8_t bh = regs8[REG_BH];
        uint8_t bl = regs8[REG_BL];
        uint8_t ch = regs8[REG_CH];
        uint8_t cl = regs8[REG_CL];

        if (cl > 8)
            cl = 8;
        if (ch < 5)
            ch = 5;
        if (ch > 8)
            ch = 8;
        if (bh > 4)
            bh = 0;

        uint16_t divisor = ext_divisor_table[cl];
        com1_dll = divisor & 0xFF;
        com1_dlm = (divisor >> 8) & 0xFF;
        com1_lcr = (uint8_t)((ch - 5) | ((bl & 1) << 2) | ext_parity_bits[bh]);
        com1_apply_config();

        regs8[REG_AH] = (uart_is_writable(SERIAL_UART_ID) ? 0x60 : 0x00)
            | (uart_is_readable(SERIAL_UART_ID) ? 0x01 : 0x00);
        regs8[REG_AL] = 0xB0; // Fake MSR: CTS/DSR/DCD asserted
#ifdef DEBUG_CONSOLE
        printf("[SERIAL DEBUG] ext init: divisor=%u lcr=%02x -> AH=%02x\n", divisor, com1_lcr,
            regs8[REG_AH]);
#endif
        break;
    }
    case 0x05: { // Modem Control Register access (PS/2-style)
        // AL = 00h read MCR (-> BL = current value)
        //    = 01h write MCR (BL = new value)
        if (regs8[REG_AL] == 0x01) {
            com1_set_mcr(regs8[REG_BL]);
        } else {
            regs8[REG_BL] = com1_mcr;
            regs8[REG_BH] = 0;
        }
        regs8[REG_AH] = 0;
        break;
    }
    default:
#ifdef DEBUG_CONSOLE
        printf("[SERIAL DEBUG] unknown AH=%02x -> reporting 0x80\n", ah);
#endif
        regs8[REG_AH] = 0x80;
        break;
    }
}

bool __time_critical_func(serial_int_pending)(void)
{
    bool out2_set = (com1_mcr & 0x08) != 0; // OUT2 gates the IRQ line on real 16550s

    if (out2_set && (com1_ier & 0x01) && uart_is_readable(SERIAL_UART_ID) && !com1_rx_irq_pending) {
        com1_rx_irq_pending = true; // Prevent re-triggering until read
        return true;
    } else if (!uart_is_readable(SERIAL_UART_ID)) {
        com1_rx_irq_pending = false; // Reset if the buffer empty
        return false;
    }
    return false;
}
