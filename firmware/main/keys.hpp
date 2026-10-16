#pragma once
#include <cstdint>

// Keyboard logic of the Cardputer ADV (specs/M4-device-ux.md, Controls). Pure functions and no hardware
// access, so they can be tested on the host: TCA8418 event decoding, the key map, the action table and the
// auto-repeat of the volume keys.

enum class KeyAction : uint8_t { None, ToggleDebug, VolumeUp, VolumeDown, ToggleFiles };

// The TCA8418 numbers keys k = 10 * Tr + Tc + 1 on its own 7x8 matrix; the Cardputer lays them out as a
// 4x14 grid (same as the original Cardputer): col = Tr * 2 + (Tc > 3), row = Tc % 4.
struct KeyEvent {
    uint8_t row = 0;
    uint8_t col = 0;
    bool pressed = false;
};

// Returns false for 0 (FIFO empty) and for numbers outside the 7x8 matrix.
inline bool tca8418_decode(uint8_t event, KeyEvent &out) {
    uint8_t number = event & 0x7F;
    if (number == 0) return false;
    uint8_t n = number - 1;
    uint8_t tr = n / 10, tc = n % 10;
    uint8_t col = tr * 2 + (tc > 3 ? 1 : 0);
    uint8_t row = tc % 4;
    if (tc >= 8 || col >= 14) return false;  // the controller scans 7 rows x 8 columns
    out = {row, col, (event & 0x80) != 0};
    return true;
}

constexpr uint8_t kShiftRow = 2, kShiftCol = 1;

// Characters of the 4x14 grid, unshifted and shifted. 0 = no character (modifiers).
inline char key_char(uint8_t row, uint8_t col, bool shift) {
    static const char kPlain[4][15] = {
        "`1234567890-=\b",
        "\tqwertyuiop[]\\",
        "\0\0asdfghjkl;'\n",
        "\0\0\0\0\0\0\0\0\0,./ ",
    };
    static const char kShift[4][15] = {
        "~!@#$%^&*()_+\b",
        "\tQWERTYUIOP{}|",
        "\0\0ASDFGHJKL:\"\n",
        "\0\0\0\0\0\0\0\0\0<>? ",
    };
    if (row >= 4 || col >= 14) return 0;
    return shift ? kShift[row][col] : kPlain[row][col];
}

// Which keys do what. '+' is shift and '=', '_' is shift and '-': both count, so no shift is needed.
inline KeyAction key_action(char c) {
    switch (c) {
        case '\t': return KeyAction::ToggleDebug;
        case '+': case '=': return KeyAction::VolumeUp;
        case '-': case '_': return KeyAction::VolumeDown;
        case 'f': case 'F': return KeyAction::ToggleFiles;
        default: return KeyAction::None;
    }
}

// Volume keys repeat while held (R4.9). press() returns the action to run now, tick() the repeats.
class KeyRepeater {
public:
    static constexpr uint32_t kDelayMs = 400;
    static constexpr uint32_t kPeriodMs = 120;

    KeyAction press(KeyAction a, uint32_t now_ms) {
        if (a != KeyAction::VolumeUp && a != KeyAction::VolumeDown) return a;
        held_ = a;
        next_ms_ = now_ms + kDelayMs;
        return a;
    }
    void release(KeyAction a) {
        if (a == held_) held_ = KeyAction::None;
    }
    KeyAction tick(uint32_t now_ms) {
        if (held_ == KeyAction::None || static_cast<int32_t>(now_ms - next_ms_) < 0) return KeyAction::None;
        next_ms_ = now_ms + kPeriodMs;
        return held_;
    }

private:
    KeyAction held_ = KeyAction::None;
    uint32_t next_ms_ = 0;
};
