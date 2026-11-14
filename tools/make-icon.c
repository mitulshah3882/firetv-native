/*
 * Launcher icons for Vega. The home screen's "Your Apps & Channels" row only lists a sideloaded
 * app whose manifest icon is exactly 512x512 (PNG, < 1 MB); anything else installs fine but never
 * shows up there. Our art is 16:9 or 4:3, so the icon puts it in the centre 512x288 band (16:9,
 * so a wide tile crop shows the whole picture) over a blurred, darkened fill made from the same art.
 *
 *   make-icon out.png art.png          icon from a picture
 *   make-icon out.png --starfield      procedural starfield (the SDL3 demo has no artwork)
 *
 * Built and run by tools/make-icons.sh against the host SDL3 build.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <math.h>

#define S 512
#define BAND_H 288

static void fill_from_art(SDL_Renderer *r, SDL_Texture *art) {
    /* Blur by shrinking to a few pixels and scaling back up with linear filtering, twice. */
    SDL_Texture *tiny = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 12, 12);
    SDL_Texture *mid = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 48, 48);
    SDL_SetTextureScaleMode(art, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(tiny, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(mid, SDL_SCALEMODE_LINEAR);
    float w, h;
    SDL_GetTextureSize(art, &w, &h);
    const float side = w < h ? w : h;  /* centre square of the art */
    SDL_SetRenderTarget(r, tiny);
    SDL_RenderTexture(r, art, &(SDL_FRect){ (w - side) / 2, (h - side) / 2, side, side }, NULL);
    SDL_SetRenderTarget(r, mid);
    SDL_RenderTexture(r, tiny, NULL, NULL);
    SDL_SetRenderTarget(r, NULL);
    SDL_RenderTexture(r, mid, NULL, NULL);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 150);
    SDL_RenderFillRect(r, NULL);
    SDL_DestroyTexture(tiny);
    SDL_DestroyTexture(mid);
}

static void draw_art_band(SDL_Renderer *r, SDL_Texture *art) {
    float w, h;
    SDL_GetTextureSize(art, &w, &h);
    float dw = S, dh = S * h / w;            /* fit inside 512 x 288 */
    if (dh > BAND_H) { dh = BAND_H; dw = BAND_H * w / h; }
    const SDL_FRect dst = { (S - dw) / 2, (S - dh) / 2, dw, dh };
    SDL_SetRenderDrawColor(r, 0, 0, 0, 120);  /* soft drop shadow */
    for (int i = 1; i <= 6; i++)
        SDL_RenderFillRect(r, &(SDL_FRect){ dst.x - i, dst.y - i + 4, dst.w + 2 * i, dst.h + 2 * i });
    SDL_RenderTexture(r, art, NULL, &dst);
}

static void draw_starfield(SDL_Renderer *r) {
    for (int y = 0; y < S; y++) {             /* deep blue to black */
        const float t = fabsf(y - S / 2.f) / (S / 2.f);
        SDL_SetRenderDrawColor(r, (Uint8)(4 + 6 * (1 - t)), (Uint8)(8 + 22 * (1 - t)), (Uint8)(20 + 40 * (1 - t)), 255);
        SDL_RenderLine(r, 0, (float)y, S, (float)y);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    Uint64 seed = 0x5EED5EEDull;
    for (int i = 0; i < 900; i++) {           /* stars streaking away from the centre */
        const float a = SDL_randf_r(&seed) * 6.2831853f;
        const float d = powf(SDL_randf_r(&seed), 0.7f) * S * 0.75f;
        const float len = 2 + d * d / (S * 2.2f);
        const float cx = S / 2.f + cosf(a) * d, cy = S / 2.f + sinf(a) * d;
        const float b = 0.25f + 0.75f * SDL_randf_r(&seed);
        const bool cyan = SDL_randf_r(&seed) < 0.35f;
        for (int k = 0; k < (int)len; k++) {
            const float f = 1 - (float)k / len;
            SDL_SetRenderDrawColor(r, (Uint8)((cyan ? 70 : 230) * b * f), (Uint8)(230 * b * f), (Uint8)(255 * b * f), 255);
            SDL_RenderPoint(r, cx - cosf(a) * k, cy - sinf(a) * k);
        }
    }
    for (int y = -110; y <= 110; y++)         /* soft glow at the vanishing point */
        for (int x = -110; x <= 110; x++) {
            const float d = sqrtf((float)(x * x + y * y)) / 110.f;
            if (d >= 1) continue;
            const float g = (1 - d) * (1 - d) * (1 - d);
            SDL_SetRenderDrawColor(r, (Uint8)(120 * g), (Uint8)(220 * g), (Uint8)(255 * g), 255);
            SDL_RenderPoint(r, S / 2.f + x, S / 2.f + y);
        }
}

int main(int argc, char **argv) {
    if (argc < 3) { SDL_Log("usage: make-icon out.png (art.png | --starfield)"); return 2; }
    SDL_Surface *out = SDL_CreateSurface(S, S, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(out);
    if (!out || !r) { SDL_Log("make-icon: %s", SDL_GetError()); return 1; }
    if (!SDL_strcmp(argv[2], "--starfield")) {
        draw_starfield(r);
    } else {
        SDL_Surface *img = SDL_LoadPNG(argv[2]);
        if (!img) { SDL_Log("make-icon: %s: %s", argv[2], SDL_GetError()); return 1; }
        SDL_Texture *art = SDL_CreateTextureFromSurface(r, img);
        fill_from_art(r, art);
        draw_art_band(r, art);
    }
    SDL_RenderPresent(r);
    if (!SDL_SavePNG(out, argv[1])) { SDL_Log("make-icon: %s", SDL_GetError()); return 1; }
    return 0;
}
