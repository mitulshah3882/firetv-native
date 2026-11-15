// CPU tests: what a game's update loop can afford per 16.7 ms frame on the 4x Cortex-A55
// (armv7 userspace, NEON): particle integration (scalar vs NEON, 1-4 threads), circle-circle
// collisions through a spatial hash, the spring-mass "warping grid", and the cost of a thread-pool
// dispatch itself.
//
// SPDX-FileCopyrightText: 2026 Mitul Shah
// SPDX-License-Identifier: MIT
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "bench.h"
#include "pool.h"

namespace {

// Runs `step` repeatedly: a short warm-up, then three repetitions sized to ~150 ms each.
template <class F>
Stat measure_cpu(F&& step, double target_ms = 150.0) {
    double t0 = now_ms();
    int warm = 0;
    while (now_ms() - t0 < 60.0 || warm < 3) { step(); ++warm; }
    t0 = now_ms();
    for (int i = 0; i < 3; ++i) step();
    const double est = std::max(1e-3, (now_ms() - t0) / 3.0);
    const int iters = std::max(3, std::min(2000, int(target_ms / est)));
    double t[3], c[3];
    for (int r = 0; r < 3; ++r) {
        const double c0 = thread_cpu_ms(), w0 = now_ms();
        for (int i = 0; i < iters; ++i) step();
        t[r] = (now_ms() - w0) / iters;
        c[r] = (thread_cpu_ms() - c0) / iters;
    }
    return make_stat(t, c, iters);
}

// ---- collisions ---------------------------------------------------------------------------------
struct Bodies {
    std::vector<float> x, y, vx, vy, dvx, dvy;
    std::vector<int> cellOf, cellStart, order;
    float r = 0.01f;
    int n = 0, g = 1;
    void init(int count) {
        n = count;
        r = std::sqrt(0.25f / (3.14159f * n));  // bodies cover ~25% of the unit box
        g = std::max(1, int(1.0f / (2.0f * r)));
        x.resize(n); y.resize(n); vx.resize(n); vy.resize(n); dvx.resize(n); dvy.resize(n);
        cellOf.resize(n); order.resize(n); cellStart.resize(g * g + 1);
        uint32_t s = 12345;
        auto rnd = [&s] { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; };
        for (int i = 0; i < n; ++i) {
            x[i] = rnd(); y[i] = rnd(); vx[i] = (rnd() - 0.5f) * 0.2f; vy[i] = (rnd() - 0.5f) * 0.2f;
        }
    }
    void build_hash() {  // counting sort of bodies by cell
        std::fill(cellStart.begin(), cellStart.end(), 0);
        for (int i = 0; i < n; ++i) {
            const int cx = std::min(g - 1, std::max(0, int(x[i] * g)));
            const int cy = std::min(g - 1, std::max(0, int(y[i] * g)));
            cellOf[i] = cy * g + cx;
            cellStart[cellOf[i] + 1]++;
        }
        for (int c = 0; c < g * g; ++c) cellStart[c + 1] += cellStart[c];
        std::vector<int>& fill = scratch;
        fill.assign(cellStart.begin(), cellStart.end() - 1);
        for (int i = 0; i < n; ++i) order[fill[cellOf[i]]++] = i;
    }
    void collide(int b, int e) {  // each body accumulates its own push: threads never share writes
        const float d0 = 2.0f * r, d02 = d0 * d0;
        for (int i = b; i < e; ++i) {
            const int c = cellOf[i], cx = c % g, cy = c / g;
            float ax = 0, ay = 0;
            for (int oy = -1; oy <= 1; ++oy) {
                const int ny = cy + oy;
                if (ny < 0 || ny >= g) continue;
                for (int ox = -1; ox <= 1; ++ox) {
                    const int nx = cx + ox;
                    if (nx < 0 || nx >= g) continue;
                    const int cell = ny * g + nx;
                    for (int k = cellStart[cell]; k < cellStart[cell + 1]; ++k) {
                        const int j = order[k];
                        if (j == i) continue;
                        const float dx = x[i] - x[j], dy = y[i] - y[j];
                        const float dd = dx * dx + dy * dy;
                        if (dd < d02 && dd > 1e-12f) {
                            const float d = std::sqrt(dd);
                            const float push = (d0 - d) / d * 0.5f;
                            ax += dx * push;
                            ay += dy * push;
                        }
                    }
                }
            }
            dvx[i] = ax;
            dvy[i] = ay;
        }
    }
    void integrate(int b, int e) {
        const float dt = 1.0f / 60.0f;
        for (int i = b; i < e; ++i) {
            vx[i] = (vx[i] + dvx[i] * 8.0f + (0.5f - x[i]) * 0.05f * dt) * 0.999f;
            vy[i] = (vy[i] + dvy[i] * 8.0f + (0.5f - y[i]) * 0.05f * dt) * 0.999f;
            x[i] += vx[i] * dt + dvx[i] * 0.5f;
            y[i] += vy[i] * dt + dvy[i] * 0.5f;
            if (x[i] < 0) { x[i] = 0; vx[i] = -vx[i]; } else if (x[i] > 1) { x[i] = 1; vx[i] = -vx[i]; }
            if (y[i] < 0) { y[i] = 0; vy[i] = -vy[i]; } else if (y[i] > 1) { y[i] = 1; vy[i] = -vy[i]; }
        }
    }
    std::vector<int> scratch;
};

// ---- spring-mass grid (the "warping grid" of a neon arena) ------------------------------------
struct SpringGrid {
    int w = 0, h = 0;
    std::vector<float> px, py, vx, vy, nx, ny, rx, ry;
    void init(int cols, int rows) {
        w = cols; h = rows;
        const int n = w * h;
        px.resize(n); py.resize(n); vx.assign(n, 0); vy.assign(n, 0); nx.resize(n); ny.resize(n);
        rx.resize(n); ry.resize(n);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                const int k = j * w + i;
                rx[k] = px[k] = float(i) / (w - 1);
                ry[k] = py[k] = float(j) / (h - 1);
            }
    }
    // Jacobi step: forces from the four neighbours (Hooke springs with rest length), an anchor
    // spring to the rest position, an orbiting gravity well, damping. Reads p, writes n.
    void step(int b, int e, float t) {
        const float L = 1.0f / (w - 1), Ly = 1.0f / (h - 1), ks = 30.0f, ka = 2.0f, dt = 1.0f / 60.0f;
        const float wx = 0.5f + 0.3f * std::cos(t), wy = 0.5f + 0.3f * std::sin(t);
        for (int k = b; k < e; ++k) {
            const int i = k % w, j = k / w;
            float fx = (rx[k] - px[k]) * ka, fy = (ry[k] - py[k]) * ka;
            auto spring = [&](int o, float rest) {
                const float dx = px[o] - px[k], dy = py[o] - py[k];
                const float d = std::sqrt(dx * dx + dy * dy) + 1e-6f;
                const float f = ks * (d - rest) / d;
                fx += dx * f;
                fy += dy * f;
            };
            if (i > 0) spring(k - 1, L);
            if (i < w - 1) spring(k + 1, L);
            if (j > 0) spring(k - w, Ly);
            if (j < h - 1) spring(k + w, Ly);
            const float gx = wx - px[k], gy = wy - py[k];
            const float g = 0.002f / (gx * gx + gy * gy + 0.01f);
            fx += gx * g;
            fy += gy * g;
            const float nvx = (vx[k] + fx * dt) * 0.97f, nvy = (vy[k] + fy * dt) * 0.97f;
            vx[k] = nvx;
            vy[k] = nvy;
            nx[k] = px[k] + nvx * dt;
            ny[k] = py[k] + nvy * dt;
        }
    }
    void swap() { px.swap(nx); py.swap(ny); }
};

char gBuf[160];
const char* fmt(const char* f, double a, double b = 0) {
    snprintf(gBuf, sizeof gBuf, f, a, b);
    return gBuf;
}

}  // namespace

void run_cpu_bench() {
    WorkerPool pool(3);  // + the calling thread = 4 cores
    char params[96];

    // Pool dispatch cost (empty job split 4 ways).
    if (!bench_progress("cpu: pool dispatch")) return;
    {
        std::function<void(int, int)> nop = [](int, int) {};
        Stat s = measure_cpu([&] { pool.run(4, 4, nop); }, 60.0);
        bench_report("cpu", "pool_dispatch", "parts=4", s, fmt("%.1f us per dispatch", s.med * 1e3));
    }

    // Particle integration: scalar AoS, one thread.
    for (int n : {16384, 65536, 262144}) {
        snprintf(params, sizeof params, "n=%d threads=1", n);
        if (!bench_progress(params)) return;
        std::vector<float> s(4 * n);
        for (int i = 0; i < n; ++i) { s[4 * i] = (i % 997) / 498.5f - 1; s[4 * i + 1] = (i % 991) / 495.5f - 1; s[4 * i + 2] = 0.1f; s[4 * i + 3] = -0.1f; }
        SimParams sp{1.0f / 60.0f, 0.2f, -0.1f};
        Stat st = measure_cpu([&] { sim_scalar_aos(s.data(), 0, n, sp); });
        bench_report("cpu", "particles_scalar", params, st, fmt("%.1f Mparticles/s", n / st.med / 1e3));
    }
    // NEON SoA, 1..4 threads.
    for (int n : {65536, 262144}) {
        ParticlesSoA P;
        P.init(n, 777);
        for (int threads = 1; threads <= 4; ++threads) {
            snprintf(params, sizeof params, "n=%d threads=%d", n, threads);
            if (!bench_progress(params)) return;
            SimParams sp{1.0f / 60.0f, 0.2f, -0.1f};
            std::function<void(int, int)> job = [&](int b, int e) {
                sim_neon_soa(P, b & ~3, e == n ? e : (e & ~3), sp, nullptr);
            };
            Stat st = measure_cpu([&] { pool.run(n, threads, job); });
            bench_report("cpu", "particles_neon", params, st, fmt("%.1f Mparticles/s", n / st.med / 1e3));
        }
    }

    // Collisions through a uniform-grid spatial hash (hash build single-threaded).
    for (int n : {1000, 2000, 4000, 8000}) {
        Bodies B;
        B.init(n);
        for (int threads : {1, 4}) {
            snprintf(params, sizeof params, "bodies=%d threads=%d", n, threads);
            if (!bench_progress(params)) return;
            std::function<void(int, int)> col = [&](int b, int e) { B.collide(b, e); };
            std::function<void(int, int)> integ = [&](int b, int e) { B.integrate(b, e); };
            Stat st = measure_cpu([&] {
                B.build_hash();
                pool.run(n, threads, col);
                pool.run(n, threads, integ);
            });
            bench_report("cpu", "collide_hash", params, st, fmt("%.2f ms per step", st.med));
        }
    }

    // Spring grid.
    for (int cols : {96, 192}) {
        const int rows = cols * 9 / 16;
        SpringGrid G;
        G.init(cols, rows);
        for (int threads : {1, 4}) {
            snprintf(params, sizeof params, "grid=%dx%d threads=%d", cols, rows, threads);
            if (!bench_progress(params)) return;
            float t = 0;
            std::function<void(int, int)> job = [&](int b, int e) { G.step(b, e, t); };
            Stat st = measure_cpu([&] {
                pool.run(cols * rows, threads, job);
                G.swap();
                t += 1.0f / 60.0f;
            });
            bench_report("cpu", "spring_grid", params, st, fmt("%.3f ms per step", st.med));
        }
    }
}
