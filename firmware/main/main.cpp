// Step 1 of the myPlaud USB-headset test:
// the Cardputer ADV enumerates as a USB audio device (speaker + mic) and shows
// CONNECTED / DISCONNECTED plus the waveform of the audio the host plays to it.

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_device_uac.h"

#include "config.hpp"
#include "m5stack-cardputer.hpp"
#include "sample_ring.hpp"
#include "screen.hpp"

static const char *TAG = "myplaud";

namespace {

SampleRing<4096> g_ring;

// Host -> device audio ("the others"). Keep only the first channel for the waveform.
esp_err_t on_speaker_data(uint8_t *buf, size_t len, void *) {
    auto *s = reinterpret_cast<const int16_t *>(buf);
    size_t frames = len / (sizeof(int16_t) * kUacChannels);
    std::array<int16_t, 512> mono;
    size_t i = 0;
    while (i < frames) {
        size_t n = std::min(frames - i, mono.size());
        for (size_t k = 0; k < n; ++k) mono[k] = s[(i + k) * kUacChannels];
        g_ring.push(mono.data(), n);
        i += n;
    }
    return ESP_OK;
}

// Device -> host audio ("me"). Step 1: silence. Real mic comes in step 2.
esp_err_t on_mic_data(uint8_t *buf, size_t len, size_t *bytes_read, void *) {
    std::memset(buf, 0, len);
    *bytes_read = len;
    return ESP_OK;
}

void on_mute(uint32_t mute, void *) { ESP_LOGI(TAG, "host mute=%u", static_cast<unsigned>(mute)); }
void on_volume(uint32_t volume, void *) { ESP_LOGI(TAG, "host volume=%u", static_cast<unsigned>(volume)); }

}  // namespace

extern "C" void app_main() {
    auto &cp = espp::M5StackCardputer::get();
    if (!cp.initialize_lcd()) {
        ESP_LOGE(TAG, "LCD init failed");
        return;
    }
    cp.brightness(60.0f);
    ESP_LOGI(TAG, "Cardputer variant detected, LCD ready");

    uac_device_config_t config = {};
    config.output_cb = on_speaker_data;
    config.input_cb = on_mic_data;
    config.set_mute_cb = on_mute;
    config.set_volume_cb = on_volume;
    config.cb_ctx = nullptr;
    if (uac_device_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "uac_device_init failed");
        return;
    }

    std::array<int16_t, kLcdWidth * kSamplesPerColumn> window;
    while (true) {
        bool connected = tud_mounted();
        g_ring.snapshot(window.data(), window.size());
        screen_draw(connected, window.data(), window.size());
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
