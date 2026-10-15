#include "screen.hpp"

#include <array>
#include <cstring>

#include "config.hpp"
#include "debug.hpp"
#include "m5stack-cardputer.hpp"

namespace {

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kCyan = 0x07FF;
constexpr uint16_t kGrey = 0x4208;

// Native (little-endian) RGB565: the display driver byte-swaps the buffer in place before
// sending, so we must not swap ourselves. Two buffers alternate because the SPI transfer
// is asynchronous and the driver modifies the buffer it was given.
constexpr size_t kFrameBytes = kLcdWidth * kLcdHeight * 2;
// One DMA transfer on the ESP32-S3 is limited to 32 KiB, so the frame goes out in strips.
constexpr size_t kStripRows = 27;
static_assert(kLcdHeight % kStripRows == 0, "strips must tile the frame");
static_assert(kStripRows * kLcdWidth * 2 <= 32768, "strip exceeds the SPI DMA limit");

alignas(16) std::array<uint8_t, kFrameBytes> g_frames[2];
uint8_t *g_frame = g_frames[0].data();

void put(size_t x, size_t y, uint16_t c) {
    if (x >= kLcdWidth || y >= kLcdHeight) return;
    size_t i = (y * kLcdWidth + x) * 2;
    c = ~c;  // the driver leaves the panel colour-inverted; compensate here
    g_frame[i] = c & 0xFF;
    g_frame[i + 1] = c >> 8;
}

void fill_rect(size_t x, size_t y, size_t w, size_t h, uint16_t c) {
    for (size_t j = 0; j < h; ++j)
        for (size_t i = 0; i < w; ++i) put(x + i, y + j, c);
}

// 5x7 glyphs for the letters of "CONNECTED" / "DISCONNECTED".
struct Glyph {
    char ch;
    uint8_t rows[7];  // 5 bits per row, MSB = left
};
constexpr Glyph kGlyphs[] = {
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {':', {0b00000, 0b00100, 0b00000, 0b00000, 0b00000, 0b00100, 0b00000}},
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
};

void draw_char(char ch, size_t x, size_t y, size_t scale, uint16_t c) {
    for (const auto &g : kGlyphs) {
        if (g.ch != ch) continue;
        for (size_t r = 0; r < 7; ++r)
            for (size_t col = 0; col < 5; ++col)
                if (g.rows[r] & (1 << (4 - col))) fill_rect(x + col * scale, y + r * scale, scale, scale, c);
        return;
    }
}

void draw_text_at(const char *s, size_t x, size_t y, size_t scale, uint16_t c) {
    for (size_t i = 0; s[i]; ++i) draw_char(s[i], x + i * 6 * scale, y, scale, c);
}

void draw_text(const char *s, size_t y, size_t scale, uint16_t c) {
    size_t n = std::strlen(s);
    size_t cell = 6 * scale;  // 5 px glyph + 1 px gap
    size_t x = (kLcdWidth - (n * cell - scale)) / 2;
    for (size_t i = 0; i < n; ++i) draw_char(s[i], x + i * cell, y, scale, c);
}

}  // namespace

void screen_draw(bool connected, const int16_t *samples, size_t count, const char *top_text,
                 const char *bottom_text) {
    static size_t current = 0;
    current ^= 1;
    g_frame = g_frames[current].data();
    std::memset(g_frame, 0xFF, kFrameBytes);  // inverted black

    fill_rect(0, 0, kLcdWidth, kBannerHeight, connected ? kGreen : kRed);
    draw_text(connected ? "CONNECTED" : "DISCONNECTED", (kBannerHeight - 14) / 2, 2, kWhite);

    const size_t top = kBannerHeight;
    const size_t area_h = kLcdHeight - kBannerHeight;
    const size_t mid = top + area_h / 2;
    for (size_t x = 0; x < kLcdWidth; ++x) put(x, mid, kGrey);

    if (connected && samples && count >= kLcdWidth * kSamplesPerColumn) {
        const int half = static_cast<int>(area_h / 2) - 1;
        for (size_t x = 0; x < kLcdWidth; ++x) {
            int16_t lo = 32767, hi = -32768;
            for (size_t k = 0; k < kSamplesPerColumn; ++k) {
                int16_t s = samples[x * kSamplesPerColumn + k];
                if (s < lo) lo = s;
                if (s > hi) hi = s;
            }
            int y_hi = static_cast<int>(mid) - (hi * half) / 32768;
            int y_lo = static_cast<int>(mid) - (lo * half) / 32768;
            for (int y = y_hi; y <= y_lo; ++y) put(x, static_cast<size_t>(y), kCyan);
        }
    }

    if (top_text) draw_text_at(top_text, 2, kBannerHeight + 2, 1, kWhite);
    if (bottom_text) draw_text_at(bottom_text, 2, kLcdHeight - 9, 1, kWhite);

    auto &cp = espp::M5StackCardputer::get();
    for (size_t y = 0, strip = 0; y < kLcdHeight; y += kStripRows, ++strip) {
        g_dbg_stage = 10 + strip;
        cp.write_lcd_frame(0, y, kLcdWidth, kStripRows, g_frame + y * kLcdWidth * 2);
    }
    g_dbg_stage = 20;
}
