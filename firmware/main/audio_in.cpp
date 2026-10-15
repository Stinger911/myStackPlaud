#include "audio_in.hpp"

#include <array>
#include <atomic>
#include <cstdlib>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

const char *TAG = "audio_in";

// Cardputer ADV: ES8311 codec on the internal I2C bus and a single full-duplex I2S bus.
constexpr gpio_num_t kI2sBclk = GPIO_NUM_41;
constexpr gpio_num_t kI2sWs = GPIO_NUM_43;
constexpr gpio_num_t kI2sDout = GPIO_NUM_42;  // to the codec DAC (unused for now)
constexpr gpio_num_t kI2sDin = GPIO_NUM_46;   // from the codec ADC
constexpr gpio_num_t kI2cSda = GPIO_NUM_8;
constexpr gpio_num_t kI2cScl = GPIO_NUM_9;
constexpr uint8_t kEs8311Addr = 0x18;

i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;
MicSamplesCb s_cb = nullptr;
std::atomic<uint32_t> s_read_ok{0};
std::atomic<uint32_t> s_read_err{0};
std::atomic<int> s_last_err{0};
std::atomic<int> s_raw_peak{0};

bool es8311_init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = kI2cSda;
    bus_cfg.scl_io_num = kI2cScl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return false;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kEs8311Addr;
    dev_cfg.scl_speed_hz = 400000;
    i2c_master_dev_handle_t dev = nullptr;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c add device: %s", esp_err_to_name(err));
        return false;
    }
    // Register sequence taken from the espp m5stack-cardputer component (derived from M5Unified).
    const uint8_t seq[][2] = {
        {0x00, 0x80},  // RESET: power on, CSM enabled
        {0x01, 0xBF},  // clock manager: MCLK from BCLK, DAC + ADC clocks on
        {0x02, 0x18},  // MULT_PRE = 3
        {0x0D, 0x01},  // power up analog circuits
        {0x0E, 0x02},  // enable analog PGA / ADC modulator
        {0x14, 0x1A},  // select Mic1p-Mic1n, raised PGA gain
        {0x15, 0x40},  // ADC soft ramp / ALC
        {0x16, 0x24},  // mic digital gain scale
        {0x17, 0xD7},  // ADC volume +12 dB (0xBF = 0 dB, 0.5 dB/step, 0xFF = +32 dB); 0 dB left speech at about -35 dBFS, +18 dB peaked at -1.5 dBFS
        {0x1C, 0x6A},  // ADC equalizer bypass
    };
    for (const auto &e : seq) {
        err = i2c_master_transmit(dev, e, 2, 100);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ES8311 reg 0x%02x write: %s", e[0], esp_err_to_name(err));
            return false;
        }
    }
    ESP_LOGI(TAG, "ES8311 configured");
    return true;
}

void rx_task(void *) {
    std::array<int16_t, 2 * 480> raw;   // 10 ms of stereo frames at 48 kHz
    std::array<int16_t, 480> mono;
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
        if (s_cb) s_cb(mono.data(), frames);
    }
}

}  // namespace

bool audio_in_start(uint32_t sample_rate, MicSamplesCb cb) {
    s_cb = cb;
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
    // Both channels share BCLK/WS; enable TX as well so the clocks certainly run (it sends zeros).
    if ((err = i2s_channel_enable(s_tx)) != ESP_OK || (err = i2s_channel_enable(s_rx)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s enable: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "I2S running at %u Hz", (unsigned)sample_rate);
    if (!es8311_init()) return false;
    xTaskCreate(rx_task, "audio_in_rx", 4096, nullptr, 6, nullptr);
    return true;
}

AudioInStats audio_in_stats() {
    return {s_read_ok.load(), s_read_err.load(), s_last_err.load(), s_raw_peak.exchange(0)};
}
