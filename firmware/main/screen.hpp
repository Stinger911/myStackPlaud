#pragma once
#include <cstddef>
#include <cstdint>

// Draw one frame: a CONNECTED / DISCONNECTED banner, optional text lines and, when
// connected, the waveform of `samples` (mono, `count` samples, int16).
// `top_text` is drawn under the banner, `bottom_text` at the bottom edge (either may be nullptr).
// Renders into an internal RGB565 framebuffer and pushes it to the LCD.
void screen_draw(bool connected, const int16_t *samples, size_t count, const char *top_text = nullptr,
                 const char *bottom_text = nullptr);
