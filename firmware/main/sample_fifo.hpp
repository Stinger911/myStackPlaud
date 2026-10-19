#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

// Same FIFO with a run-time capacity in memory allocated once by alloc(). The capacity is any number
// of samples (not only a power of two), so the recorder can take whatever RAM is free. The positions
// run over [0, 2 * capacity), which tells a full FIFO from an empty one.
class DynSampleFifo {
public:
    // Allocates exactly `samples` samples; false when the memory is not available.
    bool alloc(size_t samples) {
        release();
        if (samples == 0) return false;
        buf_ = static_cast<int16_t *>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_8BIT));
        if (!buf_) return false;
        cap_ = samples;
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
        size_t room = cap_ - used(head, tail);
        if (n > room) n = room;
        copy_in(head, in, n);
        head_.store(advance(head, n), std::memory_order_release);
        return n;
    }

    size_t read(int16_t *out, size_t n) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t avail = used(head_.load(std::memory_order_acquire), tail);
        if (n > avail) n = avail;
        size_t i = tail % cap_;
        size_t first = std::min(n, cap_ - i);
        std::memcpy(out, buf_ + i, first * sizeof(int16_t));
        std::memcpy(out + first, buf_, (n - first) * sizeof(int16_t));
        tail_.store(advance(tail, n), std::memory_order_release);
        return n;
    }

    void discard(size_t n) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t avail = used(head_.load(std::memory_order_acquire), tail);
        if (n > avail) n = avail;
        tail_.store(advance(tail, n), std::memory_order_release);
    }

    size_t available() const {
        return used(head_.load(std::memory_order_acquire), tail_.load(std::memory_order_acquire));
    }

private:
    size_t used(size_t head, size_t tail) const { return head >= tail ? head - tail : head + 2 * cap_ - tail; }
    size_t advance(size_t pos, size_t n) const {
        pos += n;
        return pos >= 2 * cap_ ? pos - 2 * cap_ : pos;
    }
    void copy_in(size_t head, const int16_t *in, size_t n) {
        size_t i = head % cap_;
        size_t first = std::min(n, cap_ - i);
        std::memcpy(buf_ + i, in, first * sizeof(int16_t));
        std::memcpy(buf_, in + first, (n - first) * sizeof(int16_t));
    }

    int16_t *buf_ = nullptr;
    size_t cap_ = 0;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};
