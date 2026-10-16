// Host test: writes a WAV with the 512-byte header and one second of tone, for ffprobe/ffmpeg to check.
// Run: clang++ -std=c++20 -I main test/wav_header_test.cpp -o /tmp/wht && /tmp/wht /tmp/hdr_test.wav && ffprobe /tmp/hdr_test.wav
#include <cmath>
#include <cstdio>
#include <vector>
#include "wav_header.hpp"
int main(int argc, char **argv) {
    const uint32_t rate = 48000, n = rate;  // one second
    std::vector<int16_t> pcm(n);
    for (uint32_t i = 0; i < n; ++i) pcm[i] = static_cast<int16_t>(8000 * std::sin(i * 2 * 3.14159265 * 440 / rate));
    uint8_t h[kWavHeaderBytes];
    build_wav_header(h, n * 2, rate);
    FILE *f = std::fopen(argc > 1 ? argv[1] : "hdr_test.wav", "wb");
    std::fwrite(h, 1, sizeof(h), f);
    std::fwrite(pcm.data(), 2, n, f);
    std::fclose(f);
    return 0;
}
