#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Single-producer / single-consumer ring of the most recent mono samples.
// Producer: USB audio callback. Consumer: UI task (tearing is acceptable for display).
template <size_t N>
class SampleRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    void push(const int16_t *samples, size_t n) {
        size_t w = head_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            buf_[(w + i) & (N - 1)] = samples[i];
        }
        head_.store(w + n, std::memory_order_release);
    }

    // Copy the latest n samples (n <= N) in chronological order.
    void snapshot(int16_t *out, size_t n) const {
        size_t w = head_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            out[i] = buf_[(w + N - n + i) & (N - 1)];
        }
    }

private:
    std::array<int16_t, N> buf_{};
    std::atomic<size_t> head_{0};
};
