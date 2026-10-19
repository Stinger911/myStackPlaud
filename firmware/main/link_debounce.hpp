#pragma once
#include <cstdint>

// Host link as shown on the screen (specs/M4-device-ux.md, S11). The USB stack reports the bus going away
// and coming back for short moments (enumeration, a host resetting the port), which must not flicker the
// header: a link comes up at once and is reported down only after it has been gone for `down_after_ms`.
class LinkDebounce {
public:
    explicit LinkDebounce(uint32_t down_after_ms = 500) : down_after_ms_(down_after_ms) {}

    bool update(bool raw_up, uint32_t now_ms) {
        if (raw_up) {
            up_ = true;
            gone_ = false;
        } else if (up_) {
            if (!gone_) {
                gone_ = true;
                gone_since_ms_ = now_ms;
            }
            if (now_ms - gone_since_ms_ >= down_after_ms_) up_ = false;
        }
        return up_;
    }

private:
    uint32_t down_after_ms_;
    bool up_ = false;
    bool gone_ = false;
    uint32_t gone_since_ms_ = 0;
};
