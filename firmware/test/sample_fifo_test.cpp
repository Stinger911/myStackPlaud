// Host test of DynSampleFifo with a capacity that is not a power of two.
// clang++ -std=c++20 -I main -I test/stubs test/sample_fifo_test.cpp -o /tmp/x && /tmp/x
#include <cassert>
#include <cstdio>
#include <vector>

#include "sample_fifo.hpp"

int main() {
    DynSampleFifo f;
    assert(!f.alloc(0));
    assert(f.alloc(1000));
    assert(f.capacity() == 1000 && f.available() == 0);

    // fill exactly, then one more sample is refused
    std::vector<int16_t> in(1000), out(1000);
    for (int i = 0; i < 1000; ++i) in[i] = i;
    assert(f.write(in.data(), 1000) == 1000 && f.available() == 1000);
    assert(f.write(in.data(), 1) == 0);
    assert(f.read(out.data(), 1000) == 1000 && out == in && f.available() == 0);

    // wrap many times with odd sizes; the stream must come out unchanged
    int16_t next_in = 0, next_out = 0;
    uint32_t seed = 12345;
    for (int iter = 0; iter < 20000; ++iter) {
        seed = seed * 1664525u + 1013904223u;
        size_t n = (seed >> 16) % 700;
        std::vector<int16_t> a(n);
        for (auto &v : a) v = next_in++;
        size_t w = f.write(a.data(), n);
        next_in -= static_cast<int16_t>(n - w);  // the refused tail is not part of the stream
        seed = seed * 1664525u + 1013904223u;
        size_t r = (seed >> 16) % 700;
        std::vector<int16_t> b(r);
        size_t got = f.read(b.data(), r);
        for (size_t i = 0; i < got; ++i) assert(b[i] == next_out++);
        assert(f.available() <= f.capacity());
    }
    // discard
    f.release();
    assert(f.alloc(7));
    int16_t d[7] = {1, 2, 3, 4, 5, 6, 7};
    assert(f.write(d, 7) == 7);
    f.discard(3);
    int16_t o[7];
    assert(f.read(o, 7) == 4 && o[0] == 4 && o[3] == 7);
    f.discard(5);  // more than available
    assert(f.available() == 0);
    std::puts("sample_fifo_test: ok");
}
