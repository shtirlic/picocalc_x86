// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "pico/stdlib.h"

#ifndef SERIAL_UART_ID
#define SERIAL_UART_ID uart0
#endif
#ifndef SERIAL_TX_PIN
#define SERIAL_TX_PIN PICO_DEFAULT_UART_TX_PIN
#endif
#ifndef SERIAL_RX_PIN
#define SERIAL_RX_PIN PICO_DEFAULT_UART_RX_PIN
#endif

#define DEFAULT_SERIAL_BAUDRATE 2400

void serial_hw_init(void);
void serial_port_in(uint16_t port);
void serial_port_out(uint16_t port);
void serial_ctl(void);
bool serial_int_pending(void);
