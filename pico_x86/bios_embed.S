// SPDX-FileCopyrightText: Copyright (c) 2026 Serg Podtynnyi
// SPDX-License-Identifier: GPL-3.0-or-later

.section .rodata
.global _binary_bios_bin_start
.global _binary_bios_bin_end

_binary_bios_bin_start:
    .incbin "pico_x86/bios.bin"
_binary_bios_bin_end:
