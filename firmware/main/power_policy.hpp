#pragma once
#include <cstdint>

// Free-space and battery policy (specs/M4-device-ux.md, S7 and S10). Pure functions and no hardware access,
// so they can be tested on the host.

// Two streams of 48 kHz, 16-bit mono: 192000 bytes per second, 11 520 000 per minute.
constexpr uint64_t kBytesPerMinute = 2ULL * 48000 * 2 * 60;

inline int32_t minutes_left(uint64_t free_bytes) { return static_cast<int32_t>(free_bytes / kBytesPerMinute); }

enum class SpaceLevel : uint8_t { Ok, Low, Stop };
constexpr int32_t kSpaceWarnMinutes = 15;  // SD < 15 MIN
constexpr int32_t kSpaceStopMinutes = 5;   // below this the recording stops cleanly

inline SpaceLevel space_level(int32_t minutes) {
    if (minutes < 0) return SpaceLevel::Ok;  // unknown: do not act on it
    if (minutes < kSpaceStopMinutes) return SpaceLevel::Stop;
    if (minutes < kSpaceWarnMinutes) return SpaceLevel::Low;
    return SpaceLevel::Ok;
}

// Battery state of charge from the cell voltage (1S Li-ion, same curve as the espp component). -1 when the
// voltage is not a battery's (nothing connected, a floating pin): the field is then hidden, not faked.
inline int battery_percent(float volts) {
    static const float kCurve[][2] = {{3.30f, 0.0f},  {3.40f, 2.0f},  {3.50f, 7.0f},  {3.60f, 17.0f}, {3.70f, 32.0f},
                                      {3.80f, 49.0f}, {3.90f, 66.0f}, {4.00f, 81.0f}, {4.10f, 93.0f}, {4.20f, 100.0f}};
    constexpr int n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (volts < 2.5f || volts > 4.6f) return -1;
    if (volts <= kCurve[0][0]) return 0;
    if (volts >= kCurve[n - 1][0]) return 100;
    for (int i = 1; i < n; ++i) {
        if (volts <= kCurve[i][0]) {
            float t = (volts - kCurve[i - 1][0]) / (kCurve[i][0] - kCurve[i - 1][0]);
            return static_cast<int>(kCurve[i - 1][1] + t * (kCurve[i][1] - kCurve[i - 1][1]) + 0.5f);
        }
    }
    return 100;
}

enum class BatteryLevel : uint8_t { Ok, Low, Stop };
constexpr int kBatteryWarnPercent = 15;  // BATT LOW
constexpr int kBatteryStopPercent = 5;   // stop cleanly
constexpr int kBatteryStopReadings = 3;  // in a row: a load spike must not end a meeting

// Feed one reading per few seconds. Unknown (-1) never warns or stops.
class BatteryMonitor {
public:
    BatteryLevel update(int percent) {
        if (percent < 0) { low_in_a_row_ = 0; return BatteryLevel::Ok; }
        low_in_a_row_ = percent <= kBatteryStopPercent ? low_in_a_row_ + 1 : 0;
        if (low_in_a_row_ >= kBatteryStopReadings) return BatteryLevel::Stop;
        if (percent <= kBatteryWarnPercent) return BatteryLevel::Low;
        return BatteryLevel::Ok;
    }

private:
    int low_in_a_row_ = 0;
};
