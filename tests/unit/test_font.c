/*
 * libdragon's monochrome fonts, drawn by the HLE (hle_rdpq.c decodes atlas layers,
 * hle_libdragon.c draws paragraphs). Two bugs to keep away:
 *   - 86854d6: a MONO atlas packs four 1bpp layers into each CI4 texel, layer j in bit (3 - j).
 *     We read bit j, so the debug font's glyphs came out as other glyphs' pixels.
 *   - c95499d: a MONO_OUTLINE atlas packs two 2-bit layers, (outline, fill). We drew only the
 *     outline bit, so helloworld's text was hollow.
 * The atlases here are packed the way libdragon's mkfont packs them (tools/mkfont/mkfont_out.cpp,
 * "Merge the four images as four bitplanes"), and the font, glyph, style and paragraph structs
 * follow libdragon's rdpq_font_internal.h and rdpq_paragraph.h (32-bit big-endian MIPS layout).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "hle_internal.h"
#include "test.h"
#include "unit.h"

void n64_rdpq_font_render_paragraph(uint8_t *rdram, recomp_context *ctx);

enum { FONT_TYPE_MONO = 1, FONT_TYPE_MONO_OUTLINE = 2, FMT_CI4 = 8 };
enum { GW = 8, GH = 4 };  /* glyph = whole atlas, 8x4 texels */

static const uint32_t FILL = 0xFF8020FFu, OUTLINE = 0x2040C0FFu;  /* RGBA32, as color_t */

/* A fixed texel pattern in which every layer differs from every other one. */
static uint8_t texel_mono(int x, int y) { return (uint8_t)((x * 7 + y * 11 + x * y * 3 + 5) & 15); }
/* Per MONO_OUTLINE layer: 0 = empty, 1 = fill, 2 = outline (mkfont's px values). */
static int texel_outline_px(int x, int y, int layer) { return (x + 2 * y + 2 * layer) % 3; }

/* sprite_t { u16 width, height; u8 bitdepth, format, hslices, vslices; pixels } */
static void write_ci4_sprite(uint8_t *rdram, uint32_t sprite, uint8_t (*texel)(int, int)) {
    n64_w16(rdram, sprite + 0, GW);
    n64_w16(rdram, sprite + 2, GH);
    n64_w8(rdram, sprite + 4, 0);
    n64_w8(rdram, sprite + 5, FMT_CI4);
    n64_w8(rdram, sprite + 6, 1);
    n64_w8(rdram, sprite + 7, 1);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x += 2)  /* CI4: the even texel in the high nibble */
            n64_w8(rdram, sprite + 8 + (uint32_t)(y * GW / 2 + x / 2), (uint8_t)(texel(x, y) << 4 | texel(x + 1, y)));
}

static uint8_t outline_texel(int x, int y) {
    return (uint8_t)(texel_outline_px(x, y, 0) << 2 | texel_outline_px(x, y, 1));  /* layer j at bits (1-j)*2 */
}

TEST(font_mono_layer_bit_order) {
    uint8_t *rdram = unit_rdram();
    recomp_context ctx = {0};
    const uint32_t sprite = 0x80100000u;
    write_ci4_sprite(rdram, sprite, texel_mono);
    for (int layer = 0; layer < 4; layer++) {
        HleTex *t = hle_sprite_texture(rdram, &ctx, sprite, layer, true);
        if (!CHECK(t && t->px && t->w == GW && t->h == GH, "layer %d: no texture", layer)) continue;
        int bad = 0;
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++) {
                const bool on = (texel_mono(x, y) >> (3 - layer)) & 1;
                const uint8_t a = t->px[(y * GW + x) * 4 + 3];
                if (a != (on ? 255 : 0) && bad++ == 0)
                    CHECK(a == (on ? 255 : 0), "layer %d texel (%d,%d) = %X: alpha %d, expected %d", layer, x, y,
                          texel_mono(x, y), a, on ? 255 : 0);
            }
        CHECK(bad == 0, "layer %d: %d of %d texels wrong", layer, bad, GW * GH);
    }
}

/* A one-atlas font in RDRAM with one glyph per layer (glyph j = tile j), and a paragraph that
 * draws glyph j at x = 4 + 12 * j, y = 6. Returns the number of glyphs drawn. */
static int render_font(uint8_t *rdram, int type, uint32_t base, uint8_t (*texel)(int, int), int nglyphs) {
    const uint32_t fnt = base, glyphs = base + 0x100, atlases = base + 0x200, styles = base + 0x300;
    const uint32_t sprite = base + 0x400, chars = base + 0x500;
    for (uint32_t i = 0; i < 0x600; i += 4) n64_w32(rdram, base + i, 0);
    n64_w32(rdram, fnt + 4, (uint32_t)type);   /* flags: the font type in the low bits */
    n64_w32(rdram, fnt + 80, glyphs);           /* rdpq_font_t::glyphs */
    n64_w32(rdram, fnt + 88, atlases);          /* rdpq_font_t::atlases */
    n64_w32(rdram, fnt + 96, styles);           /* rdpq_font_t::styles */
    n64_w32(rdram, atlases, sprite);            /* atlas_t { sprite_t *sprite; u32 size; void *up; } */
    n64_w32(rdram, styles + 0, FILL);           /* style_t { color, outline_color, ... } */
    n64_w32(rdram, styles + 4, OUTLINE);
    write_ci4_sprite(rdram, sprite, texel);
    for (int j = 0; j < nglyphs; j++) {
        /* glyph_t { xadvance, xoff, yoff, xoff2, yoff2, s, t, natlas:6 | ntile:2 } */
        const uint32_t g = glyphs + (uint32_t)j * 8;
        const uint8_t bytes[8] = {GW, 0, 0, GW, GH, 0, 0, (uint8_t)(0 << 2 | j)};
        for (int b = 0; b < 8; b++) n64_w8(rdram, g + (uint32_t)b, bytes[b]);
        /* rdpq_paragraph_char_t { font_id, atlas_id, style_id, x:12, y:12, int16 glyph } */
        const uint32_t c = chars + (uint32_t)j * 8, xy = (uint32_t)(12 * j) << 12 | 2;
        n64_w8(rdram, c + 0, 1);
        n64_w8(rdram, c + 2, 0);
        n64_w8(rdram, c + 3, (uint8_t)(xy >> 16));
        n64_w8(rdram, c + 4, (uint8_t)(xy >> 8));
        n64_w8(rdram, c + 5, (uint8_t)xy);
        n64_w16(rdram, c + 6, (uint16_t)j);
    }
    n64_w8(rdram, chars + (uint32_t)nglyphs * 8, 0);  /* another font id ends the paragraph */
    recomp_context ctx = {0};
    const float x0 = 4.0f, y0 = 4.0f;
    uint32_t xb, yb;
    SDL_memcpy(&xb, &x0, 4);
    SDL_memcpy(&yb, &y0, 4);
    ctx.r4 = N64_KSEG0(fnt);
    ctx.r5 = N64_KSEG0(chars);
    ctx.r6 = xb;
    ctx.r7 = yb;
    n64_rdpq_font_render_paragraph(rdram, &ctx);
    SDL_FlushRenderer(hle_video.r);
    return (int)(int32_t)ctx.r2;
}

static uint32_t pixel(SDL_Surface *s, int x, int y) {
    const uint8_t *p = (const uint8_t *)s->pixels + y * s->pitch + x * 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool near(uint32_t a, uint32_t b) {
    for (int sh = 8; sh < 32; sh += 8) {
        const int d = (int)((a >> sh) & 255) - (int)((b >> sh) & 255);
        if (d < -2 || d > 2) return false;
    }
    return true;
}

/* Compares glyph j's 8x4 cell on the canvas with expect(x, y, j) (an RGBA32 colour, black = none). */
static int check_glyphs(SDL_Surface *s, int nglyphs, uint32_t (*expect)(int, int, int), const char *what) {
    int bad = 0;
    for (int j = 0; j < nglyphs; j++)
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++) {
                const uint32_t want = expect(x, y, j), got = pixel(s, 4 + 12 * j + x, 6 + y);
                if (!near(got, want) && bad++ < 3)
                    CHECK(near(got, want), "%s glyph %d texel (%d,%d): %08X, expected %08X", what, j, x, y, got, want);
            }
    return bad;
}

static uint32_t expect_mono(int x, int y, int layer) {
    return (texel_mono(x, y) >> (3 - layer)) & 1 ? FILL : 0x000000FFu;
}
static uint32_t expect_outline(int x, int y, int layer) {
    const int px = texel_outline_px(x, y, layer);
    return px == 1 ? FILL : px == 2 ? OUTLINE : 0x000000FFu;
}

TEST(font_mono_paragraph) {
    uint8_t *rdram = unit_rdram();
    SDL_Surface *s = unit_canvas();
    const int n = render_font(rdram, FONT_TYPE_MONO, 0x80110000u, texel_mono, 4);
    CHECK(n == 4, "rdpq_font_render_paragraph returned %d, expected 4", n);
    const int bad = check_glyphs(s, 4, expect_mono, "MONO");
    CHECK(bad == 0, "%d of %d MONO texels wrong", bad, 4 * GW * GH);
}

TEST(font_mono_outline_paragraph) {
    uint8_t *rdram = unit_rdram();
    SDL_Surface *s = unit_canvas();
    const int n = render_font(rdram, FONT_TYPE_MONO_OUTLINE, 0x80120000u, outline_texel, 2);
    CHECK(n == 2, "rdpq_font_render_paragraph returned %d, expected 2", n);
    const int bad = check_glyphs(s, 2, expect_outline, "MONO_OUTLINE");
    CHECK(bad == 0, "%d of %d MONO_OUTLINE texels wrong (fill drawn in the fill colour, outline in the outline colour)",
          bad, 2 * GW * GH);
}
