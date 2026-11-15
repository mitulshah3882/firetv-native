// A minimal persistent worker pool for per-frame parallel loops (the shape a game's update would
// use): run() splits [0, count) into `parts` contiguous ranges, the caller runs the first one and
// workers the rest, and it returns when all are done. Workers spin briefly between jobs and then
// sleep, so back-to-back dispatches inside a frame stay cheap.
//
// SPDX-FileCopyrightText: 2026 Mitul Shah
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class WorkerPool {
public:
    explicit WorkerPool(int workers) {
        for (int w = 0; w < workers; ++w) threads_.emplace_back([this, w] { loop(w + 1); });
    }
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_.store(true);
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    int workers() const { return int(threads_.size()); }

    // parts <= workers() + 1
    void run(int count, int parts, const std::function<void(int, int)>& fn) {
        if (parts <= 1 || threads_.empty()) {
            fn(0, count);
            return;
        }
        if (parts > workers() + 1) parts = workers() + 1;
        job_ = &fn;
        count_ = count;
        parts_ = parts;
        remaining_.store(int(threads_.size()), std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mu_);
            generation_.fetch_add(1, std::memory_order_release);
        }
        cv_.notify_all();
        fn(0, count / parts);
        while (remaining_.load(std::memory_order_acquire) > 0) relax();
    }

private:
    static void relax() {
#if defined(__arm__) || defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
    }
    void loop(int index) {
        int seen = 0;
        for (;;) {
            for (int spin = 0; spin < 4000 && generation_.load(std::memory_order_acquire) == seen; ++spin) relax();
            if (generation_.load(std::memory_order_acquire) == seen) {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&] { return stop_.load() || generation_.load(std::memory_order_acquire) != seen; });
            }
            if (stop_.load()) return;
            seen = generation_.load(std::memory_order_acquire);
            if (index < parts_) {
                const int b = int(int64_t(count_) * index / parts_);
                const int e = int(int64_t(count_) * (index + 1) / parts_);
                (*job_)(b, e);
            }
            remaining_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<int> generation_{0};
    std::atomic<int> remaining_{0};
    std::atomic<bool> stop_{false};
    const std::function<void(int, int)>* job_ = nullptr;
    int count_ = 0, parts_ = 0;
};
