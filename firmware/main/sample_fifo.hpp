#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_heap_caps.h"

// Single-producer / single-consumer FIFO of int16 samples (N must be a power of two).
// The producer drops what does not fit; the consumer can read or discard from the front.
template <size_t N>
class SampleFifo {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    size_t write(const int16_t *in, size_t n) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t room = N - (head - tail);
        if (n > room) n = room;
        for (size_t i = 0; i < n; ++i) buf_[(head + i) & (N - 1)] = in[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    size_t read(int16_t *out, size_t n) {
        size_t avail = available();
        if (n > avail) n = avail;
        size_t tail = tail_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(tail + i) & (N - 1)];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    void discard(size_t n) {
        size_t avail = available();
        if (n > avail) n = avail;
        tail_.store(tail_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }

    size_t available() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    std::array<int16_t, N> buf_{};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

// Same FIFO with a run-time capacity (power of two) in memory allocated once by init().
// Used for the recorder, where the capacity is a build option and the RAM may not be available.
class DynSampleFifo {
public:
    // Allocates 2^log2 samples, trying smaller sizes (down to 2^min_log2) when memory is short.
    // Returns the capacity in samples, or 0 when nothing fits.
    size_t init(unsigned log2, unsigned min_log2) {
        for (unsigned l = log2; l >= min_log2; --l) {
            buf_ = static_cast<int16_t *>(heap_caps_malloc((size_t{1} << l) * sizeof(int16_t), MALLOC_CAP_8BIT));
            if (buf_) { cap_ = size_t{1} << l; return cap_; }
        }
        return 0;
    }

    // Allocates exactly 2^log2 samples; false when the memory is not available.
    bool alloc(unsigned log2) {
        release();
        buf_ = static_cast<int16_t *>(heap_caps_malloc((size_t{1} << log2) * sizeof(int16_t), MALLOC_CAP_8BIT));
        if (!buf_) return false;
        cap_ = size_t{1} << log2;
        return true;
    }

    void release() {
        heap_caps_free(buf_);
        buf_ = nullptr;
        cap_ = 0;
        head_.store(0);
        tail_.store(0);
    }

    size_t capacity() const { return cap_; }

    size_t write(const int16_t *in, size_t n) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t room = cap_ - (head - tail);
        if (n > room) n = room;
        for (size_t i = 0; i < n; ++i) buf_[(head + i) & (cap_ - 1)] = in[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    size_t read(int16_t *out, size_t n) {
        size_t avail = available();
        if (n > avail) n = avail;
        size_t tail = tail_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(tail + i) & (cap_ - 1)];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    void discard(size_t n) {
        size_t avail = available();
        if (n > avail) n = avail;
        tail_.store(tail_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }

    size_t available() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    int16_t *buf_ = nullptr;
    size_t cap_ = 0;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};
