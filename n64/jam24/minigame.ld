/*
 * Used with `ld -r` when one minigame's objects are merged into a single relocatable object (see
 * Makefile). Gathers the minigame's initialised data and its bss into one section each, bracketed
 * by symbols, so jam_boot.c can give every round a fresh copy of the minigame's globals, as the
 * jam's dlopen() of a freshly loaded DSO would. Everything else is left as it is.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
SECTIONS
{
    .data.jam_minigame : {
        __jam_data_start = .;
        *(.data .data.* .sdata .sdata.*)
        __jam_data_end = .;
    }
    .bss.jam_minigame (NOLOAD) : {
        __jam_bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        __jam_bss_end = .;
    }
}
