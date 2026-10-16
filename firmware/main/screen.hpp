#pragma once
#include <cstddef>
#include <cstdint>

// Screen model and renderer for the 240x135 LCD (design: specs/M4-device-ux.md).
// The caller fills a ScreenModel from the live state; screen_draw() derives the look from it
// (border colour, header, status row, elapsed time, levels, footer) and pushes one frame.

// Host link shown in the header. The modes are mutually exclusive.
enum class Link : uint8_t {
    Off,       // LINK OFF: no host
    UsbAudio,  // USB AUDIO: USB headset (UAC)
    BtAudio,   // BT AUDIO: reserved, not available on the ESP32-S3
    UsbFiles,  // USB FILES: SD card exposed to the computer (mass storage)
};

enum class Mode : uint8_t {
    Ready,      // S1
    Files,      // S0
    Recording,  // S2
    Stopping,   // S3: G0 held, the ring shows hold_progress
    Saving,     // S4
    NoSd,       // S5
    SdError,    // S6
    CardLost,   // S9
};

// Warnings shown in the status row while recording (S7, S8, S10).
enum class Warning : uint8_t { None, LowSpace, LowBattery, Drop };

struct ScreenModel {
    Link link = Link::Off;
    Mode mode = Mode::Ready;
    Warning warning = Warning::None;

    bool host_sound = false;     // host audio present (SOUND) or not (SILENCE)
    float hold_progress = 0.0f;  // 0..1 while mode == Stopping
    bool stop_hint = false;      // show "HOLD TO STOP" (short press while recording)

    uint32_t session = 0;     // R<nnnn>
    uint32_t elapsed_s = 0;   // recording time
    int32_t sd_minutes = -1;  // remaining recording time on the card, -1 = unknown
    uint32_t drops = 0;       // lost samples reported by the recorder (shown as seconds)
    int8_t battery_pct = -1;  // -1 = unknown: the battery field is hidden

    float level_me = 0.0f;  // 0..1, see level_from_peak()
    float level_ot = 0.0f;
    int8_t volume = -1;            // 0..10 shows the volume overlay, -1 hides it
    const char *notice = nullptr;  // optional boot message shown in READY (e.g. watchdog report)

    // Secondary debug view: replaces the centre area when debug_view is true.
    bool debug_view = false;
    const char *debug_lines[3] = {nullptr, nullptr, nullptr};
    const int16_t *samples = nullptr;  // mono waveform of the host stream
    size_t sample_count = 0;
};

// Map a 16-bit peak value to the 0..1 level used by the bars (-48 dBFS .. 0 dBFS).
float level_from_peak(int peak);

// Render one frame for time `now_ms` (drives blinking) and push it to the LCD.
void screen_draw(const ScreenModel &m, uint32_t now_ms);
