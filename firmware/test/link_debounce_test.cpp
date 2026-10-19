// clang++ -std=c++20 -I main test/link_debounce_test.cpp -o /tmp/x && /tmp/x
#include <cassert>
#include <cstdio>

#include "link_debounce.hpp"

int main() {
    LinkDebounce d(500);
    assert(!d.update(false, 0));       // never connected
    assert(d.update(true, 100));       // up at once
    assert(d.update(false, 200));      // a short gap is not reported
    assert(d.update(false, 600));
    assert(d.update(true, 650));       // back before the limit: stays up
    assert(d.update(false, 700));      // a new gap starts from here, not from 200
    assert(d.update(false, 1100));
    assert(!d.update(false, 1200));    // gone for 500 ms: down
    assert(!d.update(false, 5000));
    assert(d.update(true, 5100));      // and up again at once
    LinkDebounce wrap(500);            // millisecond counter wrap
    assert(wrap.update(true, 0xFFFFFF00u));
    assert(wrap.update(false, 0xFFFFFF80u));
    assert(!wrap.update(false, 0x00000300u));
    std::puts("link debounce tests ok");
}
