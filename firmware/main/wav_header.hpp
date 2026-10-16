#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Mono s16le WAV header padded to one SD sector. A "JUNK" chunk fills the space between "fmt " and
// "data", so the audio starts at byte 512. With whole-sector writes after that, FATFS can pass the
// buffer straight to the card (multi-block write) instead of merging every write into partial
// sectors. Players skip unknown chunks, so the files stay ordinary WAVs.
constexpr size_t kWavHeaderBytes = 512;

inline void wav_put_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF;
}
inline void wav_put_u16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

// Fills `h` (kWavHeaderBytes) for `data_bytes` of PCM at `rate` Hz.
inline void build_wav_header(uint8_t *h, uint32_t data_bytes, uint32_t rate) {
    constexpr uint32_t kFmtEnd = 36;                              // "RIFF"(12) + "fmt " chunk (24)
    constexpr uint32_t kDataChunk = kWavHeaderBytes - 8;          // "data" id and size sit just before the audio
    constexpr uint32_t kJunkPayload = kDataChunk - kFmtEnd - 8;   // 460 bytes
    std::memset(h, 0, kWavHeaderBytes);
    std::memcpy(h, "RIFF", 4);
    wav_put_u32(h + 4, static_cast<uint32_t>(kWavHeaderBytes - 8) + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    wav_put_u32(h + 16, 16);
    wav_put_u16(h + 20, 1);  // PCM
    wav_put_u16(h + 22, 1);  // mono
    wav_put_u32(h + 24, rate);
    wav_put_u32(h + 28, rate * 2);
    wav_put_u16(h + 32, 2);
    wav_put_u16(h + 34, 16);
    std::memcpy(h + kFmtEnd, "JUNK", 4);
    wav_put_u32(h + kFmtEnd + 4, kJunkPayload);
    std::memcpy(h + kDataChunk, "data", 4);
    wav_put_u32(h + kDataChunk + 4, data_bytes);
}
