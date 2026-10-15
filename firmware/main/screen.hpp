#pragma once
#include <cstddef>
#include <cstdint>

// Draw one frame: a CONNECTED / DISCONNECTED banner and, when connected,
// the waveform of `samples` (mono, `count` samples, int16).
// Renders into an internal RGB565 framebuffer and pushes it to the LCD.
void screen_draw(bool connected, const int16_t *samples, size_t count);
