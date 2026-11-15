/*
 * Loading splashes for Vega. vegaplat draws assets/image/splash.astc (if the package has one) as
 * the app's first frame, so a game that loads for a while shows its art and "LOADING" instead of
 * a black screen. This renders the 1920x1080 PNG; tools/make-splashes.sh compresses it to ASTC,
 * which the Mali GPU decodes itself (no image decoder on the device).
 *
 *   make-splash out.png art.png "Title" font.ttf [r g b]   (accent colour, 0-255)
 *
 * Built and run by tools/make-splashes.sh against the host SDL3 build.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define W 1920
#define H 1080

static void fill_from_art(SDL_Renderer *r, SDL_Texture *art) {
    /* A blurred, darkened wash of the art: shrink to a few pixels, scale back up, twice. */
    SDL_Texture *tiny = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 16, 9);
    SDL_Texture *mid = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 64, 36);
    SDL_SetTextureScaleMode(art, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(tiny, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(mid, SDL_SCALEMODE_LINEAR);
    SDL_SetRenderTarget(r, tiny);
    SDL_RenderTexture(r, art, NULL, NULL);
    SDL_SetRenderTarget(r, mid);
    SDL_RenderTexture(r, tiny, NULL, NULL);
    SDL_SetRenderTarget(r, NULL);
    SDL_RenderTexture(r, mid, NULL, NULL);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
    SDL_RenderFillRect(r, NULL);
    for (int y = 0; y < H; y++) {  /* vignette toward the top and bottom */
        const float t = fabsf(y - H / 2.f) / (H / 2.f);
        SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(150 * t * t));
        SDL_RenderLine(r, 0, (float)y, W, (float)y);
    }
    SDL_DestroyTexture(tiny);
    SDL_DestroyTexture(mid);
}

/* Text into the surface with alpha blending, centred on cx; y is the baseline. */
static void draw_text(SDL_Surface *s, const stbtt_fontinfo *f, float px, float cx, float y, const char *text,
                      Uint8 cr, Uint8 cg, Uint8 cb, float alpha, float spacing) {
    const float scale = stbtt_ScaleForPixelHeight(f, px);
    float width = 0;
    for (const char *c = text; *c; c++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(f, *c, &adv, &lsb);
        width += adv * scale + spacing;
    }
    float x = cx - (width - spacing) / 2;
    Uint32 *pix = (Uint32 *)s->pixels;
    const SDL_PixelFormatDetails *fmt = SDL_GetPixelFormatDetails(s->format);
    for (const char *c = text; *c; c++) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(f, *c, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(f, *c, scale, scale, &x0, &y0, &x1, &y1);
        const int bw = x1 - x0, bh = y1 - y0;
        if (bw > 0 && bh > 0) {
            unsigned char *bmp = malloc((size_t)(bw * bh));
            stbtt_MakeCodepointBitmap(f, bmp, bw, bh, bw, scale, scale, *c);
            for (int j = 0; j < bh; j++)
                for (int i = 0; i < bw; i++) {
                    const int px_ = (int)(x + x0 + i), py_ = (int)(y + y0 + j);
                    if (px_ < 0 || py_ < 0 || px_ >= W || py_ >= H) continue;
                    const float a = bmp[j * bw + i] / 255.f * alpha;
                    if (a <= 0) continue;
                    Uint32 *d = &pix[py_ * (s->pitch / 4) + px_];
                    Uint8 r, g, b, da;
                    SDL_GetRGBA(*d, fmt, NULL, &r, &g, &b, &da);
                    *d = SDL_MapRGBA(fmt, NULL, (Uint8)(r + (cr - r) * a), (Uint8)(g + (cg - g) * a),
                                     (Uint8)(b + (cb - b) * a), 255);
                }
            free(bmp);
        }
        x += adv * scale + spacing;
    }
}

int main(int argc, char **argv) {
    if (argc < 5) { SDL_Log("usage: make-splash out.png art.png \"Title\" font.ttf [r g b]"); return 2; }
    Uint8 ar = 120, ag = 200, ab = 255;
    if (argc >= 8) { ar = (Uint8)atoi(argv[5]); ag = (Uint8)atoi(argv[6]); ab = (Uint8)atoi(argv[7]); }

    SDL_Surface *out = SDL_CreateSurface(W, H, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(out);
    SDL_Surface *img = SDL_LoadPNG(argv[2]);
    if (!out || !r || !img) { SDL_Log("make-splash: %s", SDL_GetError()); return 1; }
    SDL_Texture *art = SDL_CreateTextureFromSurface(r, img);
    fill_from_art(r, art);

    /* The art, framed, with a soft shadow and a thin accent outline. */
    float aw, ah;
    SDL_GetTextureSize(art, &aw, &ah);
    const float dh = 620, dw = dh * aw / ah;
    const SDL_FRect dst = {(W - dw) / 2, 150, dw, dh};
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = 1; i <= 14; i++) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 22);
        SDL_RenderFillRect(r, &(SDL_FRect){dst.x - i, dst.y - i + 8, dst.w + 2 * i, dst.h + 2 * i});
    }
    for (int i = 1; i <= 3; i++) {
        SDL_SetRenderDrawColor(r, ar, ag, ab, (Uint8)(200 / i));
        SDL_RenderRect(r, &(SDL_FRect){dst.x - i, dst.y - i, dst.w + 2 * i, dst.h + 2 * i});
    }
    SDL_RenderTexture(r, art, NULL, &dst);
    SDL_RenderPresent(r);

    /* Title and "LOADING" under it. */
    FILE *ff = fopen(argv[4], "rb");
    if (!ff) { SDL_Log("make-splash: can't open %s", argv[4]); return 1; }
    fseek(ff, 0, SEEK_END);
    const long n = ftell(ff);
    fseek(ff, 0, SEEK_SET);
    unsigned char *ttf = malloc((size_t)n);
    if (fread(ttf, 1, (size_t)n, ff) != (size_t)n) return 1;
    fclose(ff);
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf, 0)) { SDL_Log("make-splash: bad font"); return 1; }
    draw_text(out, &font, 76, W / 2.f, 880, argv[3], 245, 248, 255, 1.0f, 2);
    draw_text(out, &font, 30, W / 2.f, 950, "LOADING", ar, ag, ab, 0.9f, 10);

    if (!SDL_SavePNG(out, argv[1])) { SDL_Log("make-splash: %s", SDL_GetError()); return 1; }
    return 0;
}
