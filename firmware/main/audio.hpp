#pragma once
#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"

// Audio of the Cardputer ADV: ES8311 codec on the internal I2C bus, one full-duplex I2S bus.
// Own setup (the espp audio code delivered no microphone data on this board and hides errors).
//
//  - capture:  codec ADC -> mono int16 -> callback (called from the capture task)
//  - playback: audio_play() -> FIFO -> I2S TX task -> codec DAC -> headphone jack / speaker
using MicSamplesCb = void (*)(const int16_t *samples, size_t count);

bool audio_start(uint32_t sample_rate, MicSamplesCb mic_cb);

// Queue mono samples for playback (non-blocking; drops what does not fit).
void audio_play(const int16_t *samples, size_t count);

// Host controls. `volume` is 0..100 (0 = -50 dB, 100 = 0 dB, as delivered by usb_device_uac).
void audio_set_host_volume(uint32_t volume);
void audio_set_mute(bool mute);
bool audio_host_muted();

// Device headphone level 0..10 (0 = mute, 10 = the DAC cap), 3 dB per step below the host's volume.
// Only the playback path: the ME and OT recordings do not see it. Call before audio_start() to set the start level.
void audio_set_level(int level);
int audio_level();

// The internal I2C bus (ES8311 at 0x18, keyboard controller at 0x34); valid after audio_start().
i2c_master_bus_handle_t audio_i2c_bus();

// Short sounds for the device's own feedback (specs/M4-device-ux.md, Sounds). They are mixed into the
// headphone/speaker path only, after the "others" stream has been handed to the recorder, so the OT
// recording never contains them. The microphone can still hear the speaker, so the caller plays them
// while the recorder is not writing (see main.cpp). Silent at device level 0 and on host mute.
enum class Sound : uint8_t { None, Start, Stop, Warning, Error, Tick };
void audio_beep(Sound sound);

struct AudioStats {
    uint32_t read_ok;       // successful microphone reads
    uint32_t read_err;      // failed microphone reads
    int last_err;           // last esp_err_t from a microphone read
    int raw_peak;           // peak |sample| of both slots since last call (reset on read)
    uint32_t play_underrun; // times playback ran dry and had to re-buffer
    uint32_t play_trim;     // times the playback backlog was trimmed
    uint32_t play_fill;     // current playback FIFO fill, in samples
};
AudioStats audio_stats();
