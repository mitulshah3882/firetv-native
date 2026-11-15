// Shared pieces of gpu-bench: timing, result reporting and the particle simulation that the CPU
// tests and the CPU-updated particle path both run.
//
// SPDX-FileCopyrightText: 2026 Mitul Shah
// SPDX-License-Identifier: MIT
#pragma once

#include <time.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define GPUBENCH_NEON 1
#endif

static inline double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static inline double thread_cpu_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// One measurement: per-iteration milliseconds over three repetitions (median, min, max), the
// calling thread's CPU time per iteration (median) and the iterations per repetition.
struct Stat {
    double med = 0, mn = 0, mx = 0, cpu = 0;
    int iters = 0;
    bool ok = false;
};

static inline Stat make_stat(double t[3], double c[3], int iters) {
    Stat s;
    double a[3] = {t[0], t[1], t[2]}, b[3] = {c[0], c[1], c[2]};
    std::sort(a, a + 3);
    std::sort(b, b + 3);
    s.med = a[1];
    s.mn = a[0];
    s.mx = a[2];
    s.cpu = b[1];
    s.iters = iters;
    s.ok = true;
    return s;
}

// Reports a finished measurement: a "BENCH|..." line in the device log, a CSV row, and a line on
// the TV. `derived` is a short human summary (e.g. "1.9 Gpix/s").
void bench_report(const char* group, const char* name, const char* params, const Stat& s, const char* derived);
// Shows the progress page with `what` as the current step and polls input. Returns false when
// the app should stop (Back pressed or the system asked us to quit).
bool bench_progress(const char* what);
// CPU tests (cpu_bench.cpp).
void run_cpu_bench();

// ---- particle simulation (same math as the GPU compute / transform-feedback paths) -----------
struct SimParams {
    float dt, ax, ay;  // time step, attractor position (NDC)
};

// Array-of-structs state (x, y, vx, vy) per particle, plain scalar C++.
static inline void sim_scalar_aos(float* s, int b, int e, const SimParams& p) {
    for (int i = b; i < e; ++i) {
        float* q = s + 4 * i;
        const float dx = p.ax - q[0], dy = p.ay - q[1];
        const float r2 = dx * dx + dy * dy + 0.01f;
        const float k = 0.05f / r2;
        const float accx = dx * k - dy * 0.3f, accy = dy * k + dx * 0.3f;
        float vx = std::min(1.5f, std::max(-1.5f, q[2] * 0.998f + accx * p.dt));
        float vy = std::min(1.5f, std::max(-1.5f, q[3] * 0.998f + accy * p.dt));
        float x = q[0] + vx * p.dt, y = q[1] + vy * p.dt;
        if (x > 1.f || x < -1.f) { x = x > 0 ? 1.f : -1.f; vx = -vx; }
        if (y > 1.f || y < -1.f) { y = y > 0 ? 1.f : -1.f; vy = -vy; }
        q[0] = x; q[1] = y; q[2] = vx; q[3] = vy;
    }
}

struct ParticlesSoA {
    std::vector<float> x, y, vx, vy;
    int n = 0;
    void init(int count, uint32_t seed) {
        n = count;
        x.resize(n); y.resize(n); vx.resize(n); vy.resize(n);
        for (int i = 0; i < n; ++i) {
            seed = seed * 1664525u + 1013904223u; x[i] = (seed >> 8) / 8388608.0f - 1.0f;
            seed = seed * 1664525u + 1013904223u; y[i] = (seed >> 8) / 8388608.0f - 1.0f;
            seed = seed * 1664525u + 1013904223u; vx[i] = ((seed >> 8) / 8388608.0f - 1.0f) * 0.2f;
            seed = seed * 1664525u + 1013904223u; vy[i] = ((seed >> 8) / 8388608.0f - 1.0f) * 0.2f;
        }
    }
};

// Structure-of-arrays state, NEON 4-wide; optionally streams (x, y, vx, vy) interleaved to `out`
// (a mapped GL buffer) with vst4. [b, e) must be multiples of 4 except at the end of the array.
static inline void sim_neon_soa(ParticlesSoA& P, int b, int e, const SimParams& p, float* out) {
    int i = b;
#if GPUBENCH_NEON
    const float32x4_t ax = vdupq_n_f32(p.ax), ay = vdupq_n_f32(p.ay), eps = vdupq_n_f32(0.01f);
    const float32x4_t one = vdupq_n_f32(1.f), negone = vdupq_n_f32(-1.f), zero = vdupq_n_f32(0.f);
    const float32x4_t vmax = vdupq_n_f32(1.5f), vmin = vdupq_n_f32(-1.5f);
    for (; i + 4 <= e; i += 4) {
        float32x4_t px = vld1q_f32(&P.x[i]), py = vld1q_f32(&P.y[i]);
        float32x4_t vx = vld1q_f32(&P.vx[i]), vy = vld1q_f32(&P.vy[i]);
        const float32x4_t dx = vsubq_f32(ax, px), dy = vsubq_f32(ay, py);
        const float32x4_t r2 = vmlaq_f32(vmlaq_f32(eps, dx, dx), dy, dy);
        float32x4_t inv = vrecpeq_f32(r2);
        inv = vmulq_f32(vrecpsq_f32(r2, inv), inv);
        const float32x4_t k = vmulq_n_f32(inv, 0.05f);
        const float32x4_t accx = vmlsq_n_f32(vmulq_f32(dx, k), dy, 0.3f);
        const float32x4_t accy = vmlaq_n_f32(vmulq_f32(dy, k), dx, 0.3f);
        vx = vminq_f32(vmaxq_f32(vmlaq_n_f32(vmulq_n_f32(vx, 0.998f), accx, p.dt), vmin), vmax);
        vy = vminq_f32(vmaxq_f32(vmlaq_n_f32(vmulq_n_f32(vy, 0.998f), accy, p.dt), vmin), vmax);
        px = vmlaq_n_f32(px, vx, p.dt);
        py = vmlaq_n_f32(py, vy, p.dt);
        const uint32x4_t mx = vcagtq_f32(px, one), my = vcagtq_f32(py, one);
        px = vbslq_f32(mx, vbslq_f32(vcltq_f32(px, zero), negone, one), px);
        py = vbslq_f32(my, vbslq_f32(vcltq_f32(py, zero), negone, one), py);
        vx = vbslq_f32(mx, vnegq_f32(vx), vx);
        vy = vbslq_f32(my, vnegq_f32(vy), vy);
        vst1q_f32(&P.x[i], px); vst1q_f32(&P.y[i], py);
        vst1q_f32(&P.vx[i], vx); vst1q_f32(&P.vy[i], vy);
        if (out) {
            float32x4x4_t q = {{px, py, vx, vy}};
            vst4q_f32(out + 4 * i, q);
        }
    }
#endif
    for (; i < e; ++i) {
        float s[4] = {P.x[i], P.y[i], P.vx[i], P.vy[i]};
        sim_scalar_aos(s, 0, 1, p);
        P.x[i] = s[0]; P.y[i] = s[1]; P.vx[i] = s[2]; P.vy[i] = s[3];
        if (out) { out[4 * i] = s[0]; out[4 * i + 1] = s[1]; out[4 * i + 2] = s[2]; out[4 * i + 3] = s[3]; }
    }
}
