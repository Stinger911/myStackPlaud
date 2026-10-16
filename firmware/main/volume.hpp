#pragma once
#include <algorithm>

// Headphone volume (specs/M4-device-ux.md, Headphone volume): 11 levels, 0 = mute, 10 = the current DAC cap.
// The ES8311 DAC register counts 0.5 dB per step, so 3 dB per level is 6 register steps below the host's setting.
constexpr int kVolumeMax = 10;
constexpr int kVolumeStepRegs = 6;

inline int volume_clamp(int level) { return std::clamp(level, 0, kVolumeMax); }

// DAC register for the host-set register value and the device level. Level 0 is muted by the caller as well
// (the register alone only reaches -95.5 dB).
inline int volume_dac_reg(int host_reg, int level) {
    level = volume_clamp(level);
    return std::max(0, host_reg - kVolumeStepRegs * (kVolumeMax - level));
}
