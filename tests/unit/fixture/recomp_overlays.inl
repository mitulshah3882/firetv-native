/*
 * Stand-in for N64Recomp's generated recomp_overlays.inl: one empty text section.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "recomp.h"
#include "funcs.h"
#include "librecomp/sections.h"

static FuncEntry section_0_funcs[1] = {};
static SectionTableEntry section_table[] = {
    { .rom_addr = 0, .ram_addr = 0x80000400, .size = 0, .funcs = section_0_funcs, .num_funcs = 0, .relocs = nullptr, .num_relocs = 0, .index = 0, .got_ram_addr = std::nullopt },
};
