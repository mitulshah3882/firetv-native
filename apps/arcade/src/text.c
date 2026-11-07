/*
 * Minimal TrueType text for SDL3 renderers (see text.h).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "text.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define ASCII_FIRST 32
#define ASCII_COUNT 95
static const int kExtras[] = {0x00A9, 0x00B7, 0x00D7, 0x2014, 0x2019, 0x2022, 0x2026, 0x25B6, 0x25C0};
#define EXTRA_COUNT ((int)(sizeof(kExtras) / sizeof(kExtras[0])))

struct Font {
    SDL_Texture *tex;
    int w, h;
    float ascent, line_height;
    stbtt_packedchar ascii[ASCII_COUNT];
    stbtt_packedchar extra[EXTRA_COUNT];
};

Font *font_load(SDL_Renderer *r, const char *path, float px) {
    size_t size = 0;
    unsigned char *ttf = (unsigned char *)SDL_LoadFile(path, &size);
    if (!ttf) {
        SDL_Log("font: cannot read %s: %s", path, SDL_GetError());
        return NULL;
    }
    Font *f = (Font *)SDL_calloc(1, sizeof(Font));
    stbtt_fontinfo info;
    stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    const float s = stbtt_ScaleForPixelHeight(&info, px);
    f->ascent = asc * s;
    f->line_height = (asc - desc + gap) * s;

    f->w = 1024;
    f->h = px > 40 ? 1024 : 512;
    unsigned char *pixels = NULL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        pixels = (unsigned char *)SDL_calloc((size_t)f->w * f->h, 1);
        stbtt_pack_context pc;
        stbtt_PackBegin(&pc, pixels, f->w, f->h, 0, 1, NULL);
        stbtt_PackSetOversampling(&pc, 2, 1);
        stbtt_pack_range ranges[2];
        SDL_zeroa(ranges);
        ranges[0].font_size = px;
        ranges[0].first_unicode_codepoint_in_range = ASCII_FIRST;
        ranges[0].num_chars = ASCII_COUNT;
        ranges[0].chardata_for_range = f->ascii;
        ranges[1].font_size = px;
        ranges[1].array_of_unicode_codepoints = (int *)kExtras;
        ranges[1].num_chars = EXTRA_COUNT;
        ranges[1].chardata_for_range = f->extra;
        const int ok = stbtt_PackFontRanges(&pc, ttf, 0, ranges, 2);
        stbtt_PackEnd(&pc);
        if (ok) break;
        SDL_free(pixels);
        pixels = NULL;
        f->h *= 2;
    }
    SDL_free(ttf);
    if (!pixels) {
        SDL_free(f);
        return NULL;
    }
    SDL_Surface *surf = SDL_CreateSurface(f->w, f->h, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < f->h; ++y) {
        Uint8 *row = (Uint8 *)surf->pixels + (size_t)y * surf->pitch;
        for (int x = 0; x < f->w; ++x) {
            row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
            row[x * 4 + 3] = pixels[(size_t)y * f->w + x];
        }
    }
    SDL_free(pixels);
    f->tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_DestroySurface(surf);
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(f->tex, SDL_SCALEMODE_LINEAR);
    return f;
}

void font_free(Font *f) {
    if (!f) return;
    SDL_DestroyTexture(f->tex);
    SDL_free(f);
}

float font_ascent(const Font *f) { return f->ascent; }
float font_line_height(const Font *f) { return f->line_height; }

static Uint32 next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    Uint32 cp;
    int n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
    else { cp = p[0] & 0x07; n = 4; }
    for (int i = 1; i < n; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *s += i; return '?'; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n;
    return cp;
}

static const stbtt_packedchar *glyph(const Font *f, Uint32 cp) {
    if (cp >= ASCII_FIRST && cp < ASCII_FIRST + ASCII_COUNT) return &f->ascii[cp - ASCII_FIRST];
    for (int i = 0; i < EXTRA_COUNT; ++i)
        if ((Uint32)kExtras[i] == cp) return &f->extra[i];
    return &f->ascii['?' - ASCII_FIRST];
}

float text_width(const Font *f, const char *s) {
    float w = 0;
    while (*s) w += glyph(f, next_cp(&s))->xadvance;
    return w;
}

float text_draw(SDL_Renderer *r, Font *f, float x, float y, const char *s, SDL_FColor c, float scale) {
    if (!f) return x;
    SDL_SetTextureColorModFloat(f->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaModFloat(f->tex, c.a);
    const float base = y + f->ascent * scale;
    while (*s) {
        const stbtt_packedchar *g = glyph(f, next_cp(&s));
        // Packed glyphs are oversampled 2x horizontally; xoff/xoff2 are already in output pixels.
        SDL_FRect src = {(float)g->x0, (float)g->y0, (float)(g->x1 - g->x0), (float)(g->y1 - g->y0)};
        SDL_FRect dst = {x + g->xoff * scale, base + g->yoff * scale, (g->xoff2 - g->xoff) * scale,
                         (g->yoff2 - g->yoff) * scale};
        if (src.w > 0 && src.h > 0) SDL_RenderTexture(r, f->tex, &src, &dst);
        x += g->xadvance * scale;
    }
    return x;
}

float text_draw_glow(SDL_Renderer *r, Font *f, float x, float y, const char *s, SDL_FColor c, SDL_FColor glow,
                     float radius, float scale) {
    if (!f) return x;
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_ADD);
    static const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
    for (int ring = 1; ring <= 2; ++ring) {
        const float rr = radius * ring * 0.5f;
        SDL_FColor g = glow;
        g.a = glow.a / (float)ring;
        for (int i = 0; i < 8; ++i) text_draw(r, f, x + dirs[i][0] * rr, y + dirs[i][1] * rr, s, g, scale);
    }
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    return text_draw(r, f, x, y, s, c, scale);
}
