/*
 * Section/function table types used by N64Recomp's generated recomp_overlays.inl.
 * (Our own minimal definitions; the generated file uses C++20 designated initializers.)
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

#include "recomp.h"

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

struct FuncEntry {
    recomp_func_t* func;
    uint32_t offset;   /* from the section's start */
    uint32_t rom_size;
};

struct RelocEntry {
    uint32_t offset;
    uint32_t target_section_offset;
    uint32_t type;
    uint16_t target_section;
};

struct SectionTableEntry {
    uint32_t rom_addr;
    uint32_t ram_addr;
    uint32_t size;
    FuncEntry* funcs;
    size_t num_funcs;
    RelocEntry* relocs;
    size_t num_relocs;
    size_t index;
    std::optional<uint32_t> got_ram_addr;
};
