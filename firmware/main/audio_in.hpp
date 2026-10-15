#pragma once
#include <cstddef>
#include <cstdint>

// Microphone capture from the ES8311 codec of the Cardputer ADV (own I2S/I2C setup, bypassing the
// espp audio code so errors are visible). Samples are mono int16 at `sample_rate`.
using MicSamplesCb = void (*)(const int16_t *samples, size_t count);

bool audio_in_start(uint32_t sample_rate, MicSamplesCb cb);

struct AudioInStats {
    uint32_t read_ok;       // successful i2s_channel_read calls
    uint32_t read_err;      // failed reads (timeouts etc.)
    int last_err;           // last esp_err_t from i2s_channel_read
    int raw_peak;           // peak |sample| of both slots since last call (reset on read)
};
AudioInStats audio_in_stats();
