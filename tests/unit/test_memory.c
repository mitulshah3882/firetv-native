/*
 * The recompiled program's view of memory: recomp.h (as tools/n64-recomp.sh generates it; the
 * test build regenerates it from the script) plus n64rt's address-space reservation.
 *
 * The bug to keep away (decision 18): N64Recomp's header maps an address by subtracting the
 * KSEG0 base. libdragon's __rand_init reads RCP registers through KSEG1 (0xA4xxxxxx), which then
 * lands 600 MB past RDRAM: a segfault at boot. We mask to the 512 MB physical space instead, so
 * KSEG1 aliases RDRAM like on hardware, and the RCP register block (0x04000000-0x04FFFFFF) falls
 * inside n64rt's 80 MB reservation, where it reads as zeros.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "unit.h"

/* n64rt.cpp reserves this much (N64RT_ADDRESS_SPACE): RDRAM plus the RCP register block. */
#define RESERVED 0x05000000u

/* Where a recompiled 32-bit load from `addr` (a sign-extended MIPS address) lands, as an offset
 * into the host reservation. Takes the address of the access without performing it. */
#define HOST_OFFSET(addr) ((size_t)((uint8_t *)&MEM_W(0, (gpr)(int32_t)(addr)) - rdram))

TEST(mask_kseg0_kseg1_alias) {
    uint8_t *rdram = unit_rdram();
    /* Checked before any access: a wrong mapping would be a segfault, not a failed check. */
    if (!CHECK(HOST_OFFSET(0x80001230u) == 0x1230, "KSEG0 0x80001230 -> offset %zX", HOST_OFFSET(0x80001230u)) ||
        !CHECK(HOST_OFFSET(0xA0001230u) == 0x1230, "KSEG1 0xA0001230 -> offset %zX", HOST_OFFSET(0xA0001230u)) ||
        !CHECK(HOST_OFFSET(0x807FFFFCu) == 0x7FFFFC, "top of RDRAM -> offset %zX", HOST_OFFSET(0x807FFFFCu)))
        return;
    /* A store through one segment is visible through the other (and through n64rt's helpers). */
    MEM_W(0, (gpr)(int32_t)0xA0002000u) = 0x1234ABCD;
    CHECK((uint32_t)MEM_W(0, (gpr)(int32_t)0x80002000u) == 0x1234ABCDu);
    CHECK(n64_r32(rdram, 0x80002000u) == 0x1234ABCDu);
    CHECK(n64_r32(rdram, 0xA0002000u) == 0x1234ABCDu);
    CHECK(n64_r32(rdram, 0x00002000u) == 0x1234ABCDu, "physical addresses are KSEG0 to the helpers");
    /* Base + offset forms, as the generated code emits them (lw t0, -16(a0)). */
    CHECK((uint32_t)MEM_W(-16, (gpr)(int32_t)0x80002010u) == 0x1234ABCDu);
    CHECK((uint32_t)MEM_W(0x10, (gpr)(int32_t)0xA0001FF0u) == 0x1234ABCDu);
}

TEST(mask_rcp_registers_inside_reservation) {
    uint8_t *rdram = unit_rdram();
    /* RCP registers libdragon touches at boot and in its timers, DMA and VI code. */
    static const uint32_t regs[] = {
        0xA4040010u, /* SP_STATUS */  0xA410000Cu, /* DPC_STATUS */ 0xA4300004u, /* MI_VERSION */
        0xA4400010u, /* VI_V_CURRENT */ 0xA450000Cu, /* AI_STATUS */ 0xA4600010u, /* PI_STATUS */
        0xA4700010u, /* RI_REFRESH */ 0xA4800018u, /* SI_STATUS */ 0xA4FFFFFCu, /* end of the block */
    };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        const size_t off = HOST_OFFSET(regs[i]);
        if (!CHECK(off + 4 <= RESERVED, "%08X maps to host offset %zX, outside n64rt's %X-byte reservation", regs[i],
                   off, RESERVED))
            continue;
        CHECK(off == (regs[i] & 0x1FFFFFFFu), "%08X -> offset %zX", regs[i], off);
        CHECK(MEM_W(0, (gpr)(int32_t)regs[i]) == 0, "%08X reads %08X, expected the zero page", regs[i],
              (uint32_t)MEM_W(0, (gpr)(int32_t)regs[i]));
    }
}

TEST(memory_big_endian_lanes) {
    /* N64Recomp keeps each big-endian word as a native word; bytes and halves XOR the address
     * (^3, ^2) to find their lane. The program must still see big-endian order. */
    uint8_t *rdram = unit_rdram();
    const gpr a = (gpr)(int32_t)0x80003000u;
    if (!CHECK(HOST_OFFSET(0x80003000u) == 0x3000, "KSEG0 mapping")) return;
    MEM_W(0, a) = 0x11223344;
    CHECK(MEM_BU(0, a) == 0x11 && MEM_BU(1, a) == 0x22 && MEM_BU(2, a) == 0x33 && MEM_BU(3, a) == 0x44,
          "bytes %02X %02X %02X %02X", MEM_BU(0, a), MEM_BU(1, a), MEM_BU(2, a), MEM_BU(3, a));
    CHECK(MEM_HU(0, a) == 0x1122 && MEM_HU(2, a) == 0x3344, "halves %04X %04X", MEM_HU(0, a), MEM_HU(2, a));
    MEM_B(1, a) = (int8_t)0xAA;
    MEM_H(2, a) = (int16_t)0xBEEF;
    CHECK((uint32_t)MEM_W(0, a) == 0x11AABEEFu, "word %08X after byte/half stores", (uint32_t)MEM_W(0, a));
    CHECK(MEM_B(1, a) == (int8_t)0xAA, "lb sign-extends");
    /* Doublewords: the high word at the lower address. */
    SD(0x0102030405060708ull, 8, a);
    CHECK((uint32_t)MEM_W(8, a) == 0x01020304u && (uint32_t)MEM_W(12, a) == 0x05060708u);
    CHECK(LD(8, a) == 0x0102030405060708ull, "ld %016llX", (unsigned long long)LD(8, a));
    CHECK(n64_r8(rdram, 0x80003008u) == 0x01 && n64_r16(rdram, 0x8000300Eu) == 0x0708);
    n64_w16(rdram, 0xA0003008u, 0xCAFE);
    CHECK(n64_r32(rdram, 0x80003008u) == 0xCAFE0304u, "helpers write big-endian through KSEG1");
}
