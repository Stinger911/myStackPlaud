#pragma once
#include <cstddef>
#include <cstdint>

// Two-stream recorder to the microSD card ("me" = microphone, "others" = what the host plays).
// Both streams are fed from the I2S tasks in 10 ms blocks on the same clock, so the two WAV files
// stay aligned to within one block. Files: /sdcard/MYPLAUD/R<nnnn>ME.WAV, R<nnnn>OT.WAV, R<nnnn>.TXT
// (8.3 names, mono, 16-bit, 48 kHz). All SD work happens in a writer task.

// Ask the writer task to start (when idle) or stop (when recording). Non-blocking.
void recorder_toggle();

// Feed one block of mono samples; ignored unless recording. Non-blocking, drops what does not fit.
void recorder_feed_me(const int16_t *samples, size_t count);
void recorder_feed_others(const int16_t *samples, size_t count);

// Short status for the screen, e.g. "REC OFF", "REC 0003 45S", "NO SD".
void recorder_status(char *out, size_t out_size);

// Structured state for the screen (see screen.hpp). `session` is the current or last session.
enum class RecorderState : uint8_t { Idle, Starting, Recording, Stopping, NoSd, WriteError };
struct RecorderInfo {
    RecorderState state;
    uint32_t session;
    uint32_t elapsed_s;  // length of the microphone stream so far
    uint32_t drops;      // lost samples in the current session
};
RecorderInfo recorder_info();

// Start the writer task. Call once.
void recorder_init();
