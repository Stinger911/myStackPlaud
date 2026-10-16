#include "screen.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.hpp"
#include "debug.hpp"
#include "m5stack-cardputer.hpp"

// Layout (240x135), see specs/M4-device-ux.md:
//   border 2 px | header y 2..27 | rule y 28 | centre y 29..108 | rule y 109 | footer y 110..133
// Text is a built-in 5x7 bitmap font drawn at integer scales: 1 (7 px, debug), 2 (14 px, header,
// footer, labels), 3 (21 px), 4 (28 px, READY) and 5 (35 px, elapsed time).

namespace {

// RGB565 values as seen on the panel (the frame is inverted before sending, see put()).
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kCyan = 0x07FF;
constexpr uint16_t kGrey = 0x8410;
constexpr uint16_t kDim = 0x2104;      // unlit bar segments, rules
constexpr uint16_t kDarkRed = 0x4000;  // blink "off" phase
constexpr uint16_t kOrange = 0xFD20;
constexpr uint16_t kBlue = 0x4D1F;
constexpr uint16_t kViolet = 0xB47F;

constexpr int kBorder = 2;
constexpr int kRuleTop = 28;
constexpr int kRuleBottom = 109;
constexpr int kHeaderTextY = 8;
constexpr int kFooterTextY = 115;
constexpr int kLevelsY = 91;

// Native (little-endian) RGB565: the display driver byte-swaps the buffer in place before
// sending, so we must not swap ourselves. A single buffer is enough: a frame leaves in about
// 13 ms and the next one is built 33 ms later.
constexpr size_t kFrameBytes = kLcdWidth * kLcdHeight * 2;
// One DMA transfer on the ESP32-S3 is limited to 32 KiB, so the frame goes out in strips.
constexpr size_t kStripRows = 27;
static_assert(kLcdHeight % kStripRows == 0, "strips must tile the frame");
static_assert(kStripRows * kLcdWidth * 2 <= 32768, "strip exceeds the SPI DMA limit");

alignas(16) std::array<uint8_t, kFrameBytes> g_frame_storage;
uint8_t *const g_frame = g_frame_storage.data();

// ---------------------------------------------------------------- primitives

void put(int x, int y, uint16_t c) {
    if (x < 0 || y < 0 || x >= static_cast<int>(kLcdWidth) || y >= static_cast<int>(kLcdHeight)) return;
    size_t i = (static_cast<size_t>(y) * kLcdWidth + static_cast<size_t>(x)) * 2;
    c = ~c;  // the driver leaves the panel colour-inverted; compensate here
    g_frame[i] = c & 0xFF;
    g_frame[i + 1] = c >> 8;
}

void fill_rect(int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) put(x + i, y + j, c);
}

void fill_circle(int cx, int cy, int r, uint16_t c) {
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i)
            if (i * i + j * j <= r * r) put(cx + i, cy + j, c);
}

// Progress ring: grey track, `progress` (0..1) of it drawn in `c`, starting at 12 o'clock.
void draw_ring(int cx, int cy, int r, float progress, uint16_t c) {
    const float kTwoPi = 6.2831853f;
    for (int j = -r - 1; j <= r + 1; ++j) {
        for (int i = -r - 1; i <= r + 1; ++i) {
            float d = std::sqrt(static_cast<float>(i * i + j * j));
            if (d < r - 1.5f || d > r + 1.5f) continue;
            float a = std::atan2(static_cast<float>(i), static_cast<float>(-j));  // 0 at the top, clockwise
            if (a < 0) a += kTwoPi;
            put(cx + i, cy + j, a <= progress * kTwoPi ? c : kDim);
        }
    }
}

// ---------------------------------------------------------------- font

struct Glyph {
    char ch;
    uint8_t rows[7];  // 5 bits per row, MSB = left
};
constexpr Glyph kGlyphs[] = {
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {':', {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000}},
    {'%', {0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'+', {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}},
    {'=', {0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000}},
    {'/', {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000}},
    {'<', {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010}},
    {'>', {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {'#', {0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}},
    {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
};

const Glyph *find_glyph(char ch) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));  // the font is upper case only
    for (const auto &g : kGlyphs)
        if (g.ch == ch) return &g;
    return nullptr;  // space and unknown characters
}

int glyph_gap(int scale) { return std::max(1, scale / 2); }
int glyph_advance(int scale) { return 5 * scale + glyph_gap(scale); }

int text_width(const char *s, int scale) {
    int n = static_cast<int>(std::strlen(s));
    return n ? n * glyph_advance(scale) - glyph_gap(scale) : 0;
}

void draw_glyph(char ch, int x, int y, int scale, uint16_t c) {
    const Glyph *g = find_glyph(ch);
    if (!g) return;
    for (int r = 0; r < 7; ++r)
        for (int col = 0; col < 5; ++col)
            if (g->rows[r] & (1 << (4 - col))) fill_rect(x + col * scale, y + r * scale, scale, scale, c);
}

enum class Align { Left, Center, Right };

// `x` is the left edge, the centre or the right edge depending on `align`; `y` is the top.
void draw_text(const char *s, int x, int y, int scale, uint16_t c, Align align = Align::Left) {
    int w = text_width(s, scale);
    if (align == Align::Center) x -= w / 2;
    if (align == Align::Right) x -= w;
    for (int i = 0; s[i]; ++i) draw_glyph(s[i], x + i * glyph_advance(scale), y, scale, c);
}

// ---------------------------------------------------------------- formatting

void format_elapsed(uint32_t s, char *out, size_t n) {
    uint32_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h) std::snprintf(out, n, "%u:%02u:%02u", static_cast<unsigned>(h), static_cast<unsigned>(m), static_cast<unsigned>(sec));
    else std::snprintf(out, n, "%u:%02u", static_cast<unsigned>(m), static_cast<unsigned>(sec));
}

// Lost audio as time: the recorder counts samples, which do not fit on the footer (551360 = 11 s).
void format_drops(uint32_t samples, char *out, size_t n) {
    uint32_t s = samples / kUacSampleRate;
    if (samples == 0) std::snprintf(out, n, "DROP 0");
    else if (s == 0) std::snprintf(out, n, "DROP <1S");
    else if (s < 100) std::snprintf(out, n, "DROP %uS", static_cast<unsigned>(s));
    else std::snprintf(out, n, "DROP %uM", static_cast<unsigned>(std::min<uint32_t>(s / 60, 99)));
}

// `prefix` adds "SD "; the footer drops it when a loss counter needs the room.
void format_sd(int32_t minutes, char *out, size_t n, bool prefix = true) {
    const char *p = prefix ? "SD " : "";
    if (minutes < 0) std::snprintf(out, n, "%s--", p);
    else if (minutes < 60) std::snprintf(out, n, "%s%dM", p, static_cast<int>(minutes));
    else std::snprintf(out, n, "%s%dH%02dM", p, static_cast<int>(std::min<int32_t>(minutes / 60, 99)), static_cast<int>(minutes % 60));
}

// ---------------------------------------------------------------- state table

bool is_recording(Mode m) { return m == Mode::Recording || m == Mode::Stopping; }

struct ModeStyle {
    Mode mode;
    uint16_t border;
    const char *header;  // text on the red band for failure modes, nullptr otherwise
};
constexpr ModeStyle kStyles[] = {
    {Mode::Ready, kGreen, nullptr},
    {Mode::Files, kViolet, nullptr},
    {Mode::Recording, kRed, nullptr},
    {Mode::Stopping, kRed, nullptr},
    {Mode::Saving, kGrey, nullptr},
    {Mode::NoSd, kRed, "NO SD CARD"},
    {Mode::SdError, kRed, "SD WRITE ERROR"},
    {Mode::CardLost, kRed, "CARD REMOVED"},
};

const ModeStyle &style_of(Mode m) {
    for (const auto &s : kStyles)
        if (s.mode == m) return s;
    return kStyles[0];
}

uint16_t border_colour(const ScreenModel &m, uint32_t now_ms) {
    if (is_recording(m.mode)) {
        if (m.warning == Warning::Drop) return (now_ms / 250) % 2 ? kRed : kDarkRed;
        if (m.warning == Warning::LowSpace || m.warning == Warning::LowBattery) return kOrange;
    }
    return style_of(m.mode).border;
}

bool host_present(Link l) { return l == Link::UsbAudio || l == Link::BtAudio; }

// ---------------------------------------------------------------- zones

struct LinkStyle {
    const char *text;
    uint16_t colour;
};
LinkStyle link_style(Link l) {
    switch (l) {
        case Link::UsbAudio: return {"USB AUDIO", kGreen};
        case Link::BtAudio: return {"BT AUDIO", kBlue};
        case Link::UsbFiles: return {"USB FILES", kViolet};
        default: return {"LINK OFF", kGrey};
    }
}

void draw_battery(int pct) {
    uint16_t c = pct < 15 ? kOrange : kWhite;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    draw_text(buf, 232, kHeaderTextY, 2, c, Align::Right);
    fill_rect(160, 10, 22, 11, c);  // outline
    fill_rect(162, 12, 18, 7, kBlack);
    fill_rect(162, 12, 18 * std::clamp(pct, 0, 100) / 100, 7, c);
    fill_rect(182, 13, 2, 5, c);  // contact
}

void draw_header(const ScreenModel &m) {
    const ModeStyle &st = style_of(m.mode);
    if (st.header) {
        fill_rect(kBorder, kBorder, 236, kRuleTop - kBorder, kRed);
        draw_text(st.header, 120, kHeaderTextY, 2, kWhite, Align::Center);
    } else {
        LinkStyle ls = link_style(m.link);
        fill_circle(14, 15, 6, ls.colour);
        if (m.link == Link::Off) fill_circle(14, 15, 3, kBlack);  // ring = disconnected
        draw_text(ls.text, 26, kHeaderTextY, 2, m.link == Link::Off ? kGrey : kWhite);
        if (m.battery_pct >= 0) draw_battery(m.battery_pct);
    }
    fill_rect(kBorder, kRuleTop, 236, 1, kDim);
}

void draw_bar(int x, int y, float level, uint16_t colour) {
    int lit = static_cast<int>(std::lround(std::clamp(level, 0.0f, 1.0f) * 10.0f));
    for (int i = 0; i < 10; ++i) fill_rect(x + i * 8, y, 6, 8, i < lit ? (i == 9 ? kRed : colour) : kDim);
}

void draw_levels(const ScreenModel &m) {
    draw_text("ME", 8, kLevelsY, 2, kGrey);
    draw_bar(34, kLevelsY + 3, m.level_me, kGreen);
    draw_text("OT", 124, kLevelsY, 2, kGrey);
    draw_bar(150, kLevelsY + 3, m.level_ot, kCyan);
}

struct Slot {
    const char *text;
    uint16_t colour;
};
// Right slot of the status row: a warning if there is one, otherwise the host audio state.
Slot status_slot(const ScreenModel &m) {
    switch (m.warning) {
        case Warning::LowSpace: return {"SD < 15 MIN", kOrange};
        case Warning::LowBattery: return {"BATT LOW", kOrange};
        case Warning::Drop: return {"DROPPED", kRed};
        default: break;
    }
    if (!host_present(m.link)) return {"NO HOST", kGrey};
    return m.host_sound ? Slot{"SOUND", kCyan} : Slot{"SILENCE", kGrey};
}

void draw_ready(const ScreenModel &m) {
    draw_text("READY", 120, 36, 4, kGreen, Align::Center);
    if (m.notice) {
        draw_text(m.notice, 8, 70, 2, kGrey);
    } else {
        draw_text("G0 = REC", 8, 70, 2, kGrey);
        Slot s = status_slot(m);
        draw_text(s.text, 232, 70, 2, s.colour, Align::Right);
    }
    draw_levels(m);
}

void draw_recording(const ScreenModel &m, uint32_t now_ms) {
    if (m.mode == Mode::Stopping) {
        draw_ring(16, 41, 8, m.hold_progress, kWhite);
        draw_text("HOLD TO STOP", 32, 34, 2, kWhite);
    } else {
        fill_circle(16, 41, 6, (now_ms / 500) % 2 == 0 ? kRed : kDarkRed);
        draw_text("REC", 30, 34, 2, kRed);
        Slot s = status_slot(m);
        draw_text(s.text, 232, 34, 2, s.colour, Align::Right);
    }
    char t[16];
    format_elapsed(m.elapsed_s, t, sizeof(t));
    draw_text(t, 120, 52, 5, kWhite, Align::Center);
    draw_levels(m);
}

void draw_message(const char *line1, int scale1, uint16_t c1, const char *line2, const char *line3) {
    draw_text(line1, 120, 44, scale1, c1, Align::Center);
    if (line2) draw_text(line2, 120, 76, 2, kGrey, Align::Center);
    if (line3) draw_text(line3, 120, 92, 2, kWhite, Align::Center);
}

void draw_debug(const ScreenModel &m) {
    int y = 33;
    for (const char *line : m.debug_lines) {
        if (!line) continue;
        draw_text(line, 8, y, 1, kWhite);
        y += 11;
    }
    if (m.samples && m.sample_count >= kLcdWidth * kSamplesPerColumn) {
        const int mid = 90, half = 16;
        for (int x = kBorder; x < static_cast<int>(kLcdWidth) - kBorder; ++x) {
            int16_t lo = 32767, hi = -32768;
            for (size_t k = 0; k < kSamplesPerColumn; ++k) {
                int16_t s = m.samples[static_cast<size_t>(x) * kSamplesPerColumn + k];
                lo = std::min(lo, s);
                hi = std::max(hi, s);
            }
            for (int yy = mid - (hi * half) / 32768; yy <= mid - (lo * half) / 32768; ++yy) put(x, yy, kCyan);
        }
    }
}

void draw_centre(const ScreenModel &m, uint32_t now_ms) {
    if (m.debug_view && (is_recording(m.mode) || m.mode == Mode::Ready)) {
        draw_debug(m);
        return;
    }
    switch (m.mode) {
        case Mode::Ready: draw_ready(m); break;
        case Mode::Recording:
        case Mode::Stopping: draw_recording(m, now_ms); break;
        case Mode::Saving: draw_message("SAVING...", 3, kWhite, "CLOSING FILES", nullptr); break;
        case Mode::Files:
            draw_text("FILES", 120, 36, 4, kViolet, Align::Center);
            draw_text("SD IS A DISK ON PC", 120, 72, 2, kWhite, Align::Center);
            draw_text("EJECT THERE, THEN F", 120, 90, 2, kGrey, Align::Center);
            break;
        case Mode::NoSd: draw_message("INSERT CARD", 3, kWhite, "RECORDING OFF", nullptr); break;
        case Mode::SdError:
        case Mode::CardLost: {
            char elapsed[16], saved[40];
            format_elapsed(m.elapsed_s, elapsed, sizeof(elapsed));
            std::snprintf(saved, sizeof(saved), "SAVED UP TO %s", elapsed);
            draw_text("RECORDING STOPPED", 120, 40, 2, kWhite, Align::Center);
            draw_text(saved, 120, 64, 2, kGrey, Align::Center);
            draw_text("G0 = OK", 120, 88, 2, kWhite, Align::Center);
            break;
        }
    }
}

void draw_volume_overlay(const ScreenModel &m) {
    if (m.volume < 0 || m.mode == Mode::Files) return;
    fill_rect(kBorder, 90, 236, 18, kBlack);
    draw_text("VOL", 8, kLevelsY, 2, m.volume ? kWhite : kGrey);
    if (m.volume == 0) {
        draw_text("MUTE", 120, kLevelsY, 2, kOrange, Align::Center);
        return;
    }
    for (int i = 0; i < 10; ++i) fill_rect(48 + i * 16, 94, 13, 10, i < m.volume ? kWhite : kDim);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", m.volume);
    draw_text(buf, 232, kLevelsY, 2, kWhite, Align::Right);
}

void draw_footer(const ScreenModel &m) {
    fill_rect(kBorder, kRuleBottom, 236, 1, kDim);
    char buf[32];
    if (m.mode == Mode::Files) {
        format_sd(m.sd_minutes, buf, sizeof(buf));
        draw_text(buf, 8, kFooterTextY, 2, kWhite);
        draw_text("MOUNTED", 232, kFooterTextY, 2, kViolet, Align::Right);
        return;
    }
    std::snprintf(buf, sizeof(buf), "R%04u", static_cast<unsigned>(m.session));
    draw_text(buf, 8, kFooterTextY, 2, kWhite);
    if (m.mode == Mode::NoSd || m.mode == Mode::SdError) {
        draw_text(m.mode == Mode::NoSd ? "SD --" : "SD ERR", 232, kFooterTextY, 2, kRed, Align::Right);
        return;
    }
    format_sd(m.sd_minutes, buf, sizeof(buf), m.drops == 0);
    draw_text(buf, 112, kFooterTextY, 2, m.warning == Warning::LowSpace ? kOrange : kWhite, Align::Center);
    format_drops(m.drops, buf, sizeof(buf));
    draw_text(buf, 232, kFooterTextY, 2, m.drops ? kRed : kGrey, Align::Right);
}

void push_frame() {
    auto &cp = espp::M5StackCardputer::get();
    for (size_t y = 0, strip = 0; y < kLcdHeight; y += kStripRows, ++strip) {
        g_dbg_stage = 10 + strip;
        cp.write_lcd_frame(0, y, kLcdWidth, kStripRows, g_frame + y * kLcdWidth * 2);
    }
    g_dbg_stage = 20;
}

}  // namespace

float level_from_peak(int peak) {
    if (peak <= 0) return 0.0f;
    float db = 20.0f * std::log10(static_cast<float>(std::min(peak, 32768)) / 32768.0f);
    return std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f);
}

void screen_draw(const ScreenModel &m, uint32_t now_ms) {
    std::memset(g_frame, 0xFF, kFrameBytes);  // inverted black
    uint16_t border = border_colour(m, now_ms);
    fill_rect(0, 0, kLcdWidth, kLcdHeight, border);
    fill_rect(kBorder, kBorder, kLcdWidth - 2 * kBorder, kLcdHeight - 2 * kBorder, kBlack);

    draw_header(m);
    draw_centre(m, now_ms);
    draw_volume_overlay(m);
    draw_footer(m);
    push_frame();
}
