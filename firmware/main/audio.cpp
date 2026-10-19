#include "audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iterator>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "recorder.hpp"
#include "sample_fifo.hpp"
#include "volume.hpp"

namespace {

const char *TAG = "audio";

// Cardputer ADV: ES8311 codec on the internal I2C bus and a single full-duplex I2S bus.
constexpr gpio_num_t kI2sBclk = GPIO_NUM_41;
constexpr gpio_num_t kI2sWs = GPIO_NUM_43;
constexpr gpio_num_t kI2sDout = GPIO_NUM_42;  // to the codec DAC
constexpr gpio_num_t kI2sDin = GPIO_NUM_46;   // from the codec ADC
constexpr gpio_num_t kI2cSda = GPIO_NUM_8;
constexpr gpio_num_t kI2cScl = GPIO_NUM_9;
constexpr uint8_t kEs8311Addr = 0x18;

// ES8311 DAC volume register 0x32: 0xBF = 0 dB, 0.5 dB per step, 0x00 = -95.5 dB.
// Headphone output of this codec is loud, so the level is capped well below full scale.
constexpr uint8_t kDacRegStart = 0x83;  // -30 dB until the host sets a volume
constexpr uint8_t kDacRegMax = 0x97;    // -20 dB: the loudest the host slider can make it
constexpr uint8_t kDacRegAtHost0 = 0x5B;  // host volume 0 (-50 dB)

constexpr size_t kBlock = 480;           // 10 ms at 48 kHz
constexpr size_t kPrebuffer = 4 * kBlock;  // 40 ms before playback starts or resumes
constexpr size_t kTrimAbove = 10 * kBlock; // 100 ms backlog: drop the oldest down to kPrebuffer

i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;
i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_codec = nullptr;
int s_host_reg = kDacRegStart;            // DAC register the host volume asks for (kDacRegStart until it sets one)
std::atomic<int> s_level{kVolumeMax};  // device headphone level 0..10
MicSamplesCb s_mic_cb = nullptr;

SampleFifo<8192> s_play_fifo;
std::atomic<bool> s_mute{false};

std::atomic<uint32_t> s_read_ok{0};
std::atomic<uint32_t> s_read_err{0};
std::atomic<int> s_last_err{0};
std::atomic<int> s_raw_peak{0};
std::atomic<uint32_t> s_play_underrun{0};
std::atomic<uint32_t> s_play_trim{0};

// ---------------------------------------------------------------- beeps

struct Tone {
    uint16_t hz;  // 0 = pause
    uint16_t ms;
};
constexpr Tone kStart[] = {{880, 80}};
constexpr Tone kStop[] = {{880, 80}, {0, 30}, {440, 80}};
constexpr Tone kWarning[] = {{660, 60}, {0, 40}, {660, 60}};
constexpr Tone kError[] = {{330, 1000}};
constexpr Tone kTick[] = {{1000, 15}};

struct Pattern {
    const Tone *tones;
    size_t count;
};
constexpr Pattern pattern_of(Sound s) {
    switch (s) {
        case Sound::Start: return {kStart, std::size(kStart)};
        case Sound::Stop: return {kStop, std::size(kStop)};
        case Sound::Warning: return {kWarning, std::size(kWarning)};
        case Sound::Error: return {kError, std::size(kError)};
        case Sound::Tick: return {kTick, std::size(kTick)};
        default: return {nullptr, 0};
    }
}

constexpr float kBeepAmplitude = 8000.0f;  // about -12 dBFS before the DAC register
constexpr size_t kBeepRamp = 96;           // 2 ms fade in and out against clicks

std::atomic<uint8_t> s_sound_request{0};

// Adds the running beep to `mono` (called from the tx task only).
class BeepPlayer {
public:
    void mix(int16_t *mono, size_t n) {
        uint8_t req = s_sound_request.exchange(0);
        if (req) {
            pattern_ = pattern_of(static_cast<Sound>(req));
            index_ = 0;
            left_ = 0;
            active_ = pattern_.count > 0;
            next_segment();
        }
        for (size_t i = 0; i < n && active_; ++i) {
            if (left_ == 0 && !next_segment()) break;
            if (tone_.hz) {
                size_t pos = total_ - left_;
                float env = std::min({1.0f, static_cast<float>(pos) / kBeepRamp, static_cast<float>(left_) / kBeepRamp});
                float v = kBeepAmplitude * env * std::sin(phase_);
                phase_ += 6.2831853f * tone_.hz / kSampleRate;
                if (phase_ > 6.2831853f) phase_ -= 6.2831853f;
                mono[i] = static_cast<int16_t>(std::clamp(static_cast<int>(mono[i]) + static_cast<int>(v), -32768, 32767));
            }
            --left_;
        }
    }

private:
    static constexpr float kSampleRate = 48000.0f;
    bool next_segment() {
        if (index_ >= pattern_.count) {
            active_ = false;
            return false;
        }
        tone_ = pattern_.tones[index_++];
        total_ = left_ = static_cast<size_t>(tone_.ms) * 48;
        phase_ = 0;
        return true;
    }
    Pattern pattern_{};
    Tone tone_{};
    size_t index_ = 0, left_ = 0, total_ = 0;
    float phase_ = 0;
    bool active_ = false;
};

bool codec_write(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    esp_err_t err = i2c_master_transmit(s_codec, buf, 2, 100);
    if (err != ESP_OK) ESP_LOGE(TAG, "ES8311 reg 0x%02x write: %s", reg, esp_err_to_name(err));
    return err == ESP_OK;
}

void apply_dac_volume() {
    if (!s_codec) return;
    codec_write(0x32, static_cast<uint8_t>(volume_dac_reg(s_host_reg, s_level.load())));
}

bool codec_init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = kI2cSda;
    bus_cfg.scl_io_num = kI2cScl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return false;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kEs8311Addr;
    dev_cfg.scl_speed_hz = 400000;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_codec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c add device: %s", esp_err_to_name(err));
        return false;
    }
    // Register sequences taken from the espp m5stack-cardputer component (derived from M5Unified).
    const uint8_t seq[][2] = {
        {0x00, 0x80},  // RESET: power on, CSM enabled
        {0x01, 0xBF},  // clock manager: MCLK from BCLK, DAC + ADC clocks on
        {0x02, 0x18},  // MULT_PRE = 3
        {0x0D, 0x01},  // power up analog circuits
        // microphone (ADC) path
        {0x0E, 0x02},  // enable analog PGA / ADC modulator
        {0x14, 0x1A},  // select Mic1p-Mic1n, raised PGA gain
        {0x15, 0x40},  // ADC soft ramp / ALC
        {0x16, 0x24},  // mic digital gain scale
        {0x17, 0xD7},  // ADC volume +12 dB (0xBF = 0 dB, 0.5 dB/step); +18 dB peaked at -1.5 dBFS
        {0x1C, 0x6A},  // ADC equalizer bypass
        // playback (DAC) path
        {0x12, 0x00},  // power up DAC
        {0x13, 0x10},  // enable output to the headphone drive
        {0x32, kDacRegStart},
        {0x37, 0x08},  // bypass DAC equalizer
    };
    for (const auto &e : seq) {
        if (!codec_write(e[0], e[1])) return false;
    }
    apply_dac_volume();
    ESP_LOGI(TAG, "ES8311 configured");
    return true;
}

void rx_task(void *) {
    std::array<int16_t, 2 * kBlock> raw;
    std::array<int16_t, kBlock> mono;
    uint32_t log_countdown = 0;
    while (true) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, raw.data(), raw.size() * sizeof(int16_t), &got, 200);
        if (err != ESP_OK || got == 0) {
            s_read_err.fetch_add(1);
            s_last_err.store(err);
            if (log_countdown++ % 25 == 0) ESP_LOGW(TAG, "i2s read: %s got=%u", esp_err_to_name(err), (unsigned)got);
            continue;
        }
        s_read_ok.fetch_add(1);
        size_t frames = got / (2 * sizeof(int16_t));
        int peak = 0;
        for (size_t i = 0; i < frames; ++i) {
            peak = std::max(peak, std::max(std::abs((int)raw[2 * i]), std::abs((int)raw[2 * i + 1])));
            mono[i] = raw[2 * i];  // left slot carries the ADC data
        }
        if (peak > s_raw_peak.load()) s_raw_peak.store(peak);
        recorder_feed_me(mono.data(), frames);
        if (s_mic_cb) s_mic_cb(mono.data(), frames);
    }
}

// Pulls from the playback FIFO at the pace of the I2S clock. Silence while buffering.
void tx_task(void *) {
    std::array<int16_t, kBlock> mono;
    std::array<int16_t, 2 * kBlock> stereo;
    bool playing = false;
    BeepPlayer beep;
    while (true) {
        size_t avail = s_play_fifo.available();
        if (avail > kTrimAbove) {  // host runs faster than this clock: drop the oldest
            s_play_fifo.discard(avail - kPrebuffer);
            s_play_trim.fetch_add(1);
            avail = s_play_fifo.available();
        }
        if (!playing && avail >= kPrebuffer) playing = true;
        if (playing && avail < kBlock) {  // ran dry: re-buffer
            playing = false;
            s_play_underrun.fetch_add(1);
        }
        if (playing) {
            s_play_fifo.read(mono.data(), kBlock);
        } else {
            std::fill(mono.begin(), mono.end(), 0);
        }
        recorder_feed_others(mono.data(), kBlock);  // what the host sent, even when muted here
        beep.mix(mono.data(), kBlock);              // after the recorder: the OT file never has the beeps
        if (s_mute.load() || s_level.load() == 0) std::fill(mono.begin(), mono.end(), 0);
        for (size_t i = 0; i < kBlock; ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
        size_t written = 0;
        i2s_channel_write(s_tx, stereo.data(), stereo.size() * sizeof(int16_t), &written, portMAX_DELAY);
    }
}

}  // namespace

bool audio_start(uint32_t sample_rate, MicSamplesCb mic_cb) {
    s_mic_cb = mic_cb;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return false;
    }
    i2s_std_config_t std = {};
    std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std.gpio_cfg.mclk = GPIO_NUM_NC;
    std.gpio_cfg.bclk = kI2sBclk;
    std.gpio_cfg.ws = kI2sWs;
    std.gpio_cfg.dout = kI2sDout;
    std.gpio_cfg.din = kI2sDin;
    if ((err = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK ||
        (err = i2s_channel_init_std_mode(s_rx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init std: %s", esp_err_to_name(err));
        return false;
    }
    // Both channels share BCLK/WS; TX runs from the start so the clocks certainly run.
    if ((err = i2s_channel_enable(s_tx)) != ESP_OK || (err = i2s_channel_enable(s_rx)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s enable: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "I2S running at %u Hz", (unsigned)sample_rate);
    if (!codec_init()) return false;
    xTaskCreate(rx_task, "audio_rx", 4096, nullptr, 6, nullptr);
    xTaskCreate(tx_task, "audio_tx", 4096, nullptr, 6, nullptr);
    return true;
}

void audio_beep(Sound sound) { s_sound_request.store(static_cast<uint8_t>(sound)); }

void audio_play(const int16_t *samples, size_t count) { s_play_fifo.write(samples, count); }

void audio_set_host_volume(uint32_t volume) {
    s_host_reg = static_cast<int>(std::min<uint32_t>(kDacRegAtHost0 + std::min<uint32_t>(volume, 100), kDacRegMax));
    apply_dac_volume();
}

void audio_set_level(int level) {
    s_level.store(volume_clamp(level));
    apply_dac_volume();
}

int audio_level() { return s_level.load(); }

i2c_master_bus_handle_t audio_i2c_bus() { return s_bus; }

void audio_set_mute(bool mute) { s_mute.store(mute); }

AudioStats audio_stats() {
    return {s_read_ok.load(),      s_read_err.load(),       s_last_err.load(),
            s_raw_peak.exchange(0), s_play_underrun.load(), s_play_trim.load(),
            static_cast<uint32_t>(s_play_fifo.available())};
}
