// Step 1 of the myPlaud USB-headset test:
// the Cardputer ADV enumerates as a USB audio device (speaker + mic) and shows
// CONNECTED / DISCONNECTED plus the waveform of the audio the host plays to it.
//
// Debugging aids (the console is not available while the USB port is the audio device):
//  - the top HUD line shows frame / speaker-callback / mic-callback counters;
//  - a watchdog task restarts the device if the main loop stops reporting and stores the
//    stage it was stuck in; the bottom HUD line shows it after the restart.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_device_uac.h"

#include "config.hpp"
#include "debug.hpp"
#include "m5stack-cardputer.hpp"
#include "sample_ring.hpp"
#include "screen.hpp"

static const char *TAG = "myplaud";

volatile uint32_t g_dbg_stage = 0;

namespace {

SampleRing<4096> g_ring;
std::atomic<uint32_t> g_speaker_cb{0};
std::atomic<uint32_t> g_mic_cb{0};
volatile int64_t g_heartbeat_us = 0;

// Survive a software restart (not a power cycle) to report why the watchdog fired.
constexpr uint32_t kStuckMagic = 0x57C0DE01;
RTC_NOINIT_ATTR uint32_t g_rtc_magic;
RTC_NOINIT_ATTR uint32_t g_rtc_stage;
RTC_NOINIT_ATTR uint32_t g_rtc_frame;
RTC_NOINIT_ATTR uint32_t g_rtc_speaker_cb;
volatile uint32_t g_frame = 0;

// Host -> device audio ("the others"). Keep only the first channel for the waveform.
esp_err_t on_speaker_data(uint8_t *buf, size_t len, void *) {
    g_speaker_cb.fetch_add(1);
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
    g_mic_cb.fetch_add(1);
    std::memset(buf, 0, len);
    *bytes_read = len;
    return ESP_OK;
}

void on_mute(uint32_t mute, void *) { ESP_LOGI(TAG, "host mute=%u", static_cast<unsigned>(mute)); }
void on_volume(uint32_t volume, void *) { ESP_LOGI(TAG, "host volume=%u", static_cast<unsigned>(volume)); }

void watchdog_task(void *) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int64_t last = g_heartbeat_us;
        if (last != 0 && esp_timer_get_time() - last > 2'000'000) {
            g_rtc_stage = g_dbg_stage;
            g_rtc_frame = g_frame;
            g_rtc_speaker_cb = g_speaker_cb.load();
            g_rtc_magic = kStuckMagic;
            esp_restart();
        }
    }
}

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(TAG, "app_main: start");
#if CONFIG_MYPLAUD_BOOT_ONLY
    while (true) {
        ESP_LOGI(TAG, "boot-only build alive");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    // Report a stall detected before the last restart.
    static char bottom[48] = "BOOT OK";
    if (g_rtc_magic == kStuckMagic) {
        std::snprintf(bottom, sizeof(bottom), "STUCK S%u F%u A%u", static_cast<unsigned>(g_rtc_stage),
                      static_cast<unsigned>(g_rtc_frame), static_cast<unsigned>(g_rtc_speaker_cb));
        g_rtc_magic = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));  // let a host open the console before anything risky runs
    ESP_LOGI(TAG, "app_main: getting Cardputer object");
    auto &cp = espp::M5StackCardputer::get();
    ESP_LOGI(TAG, "app_main: initialize_lcd");
    if (!cp.initialize_lcd()) {
        ESP_LOGE(TAG, "LCD init failed");
        return;
    }
    ESP_LOGI(TAG, "app_main: LCD ready, backlight on");
    // The LEDC (PWM) backlight from espp stays dark on the ADV; driving GPIO38 directly works.
    gpio_reset_pin(GPIO_NUM_38);
    gpio_set_direction(GPIO_NUM_38, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_38, 1);
    // A synchronous clear first: it drains the transaction left over from panel init.
    cp.display_driver()->clear(0, 0, kLcdWidth, kLcdHeight, 0xFFFF);  // inverted black
    ESP_LOGI(TAG, "Cardputer variant detected, LCD ready");

#if CONFIG_MYPLAUD_SCREEN_TEST
    ESP_LOGI(TAG, "SCREEN TEST build: USB audio disabled");
#else
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
#endif

    g_heartbeat_us = esp_timer_get_time();
    xTaskCreate(watchdog_task, "myplaud_wd", 3072, nullptr, 6, nullptr);

    static std::array<int16_t, kLcdWidth * kSamplesPerColumn> window;  // too big for the main task stack
    char top[48];
    while (true) {
        g_dbg_stage = 1;
#if CONFIG_MYPLAUD_SCREEN_TEST
        // Fake state and a moving sine so every part of the screen code is exercised.
        bool connected = (g_frame / 60) % 2 == 0;
        for (size_t i = 0; i < window.size(); ++i) {
            window[i] = static_cast<int16_t>(20000.0f * sinf((i + g_frame * 16) * 0.02f));
        }
        if (g_frame % 30 == 0) ESP_LOGI(TAG, "frame %u connected=%d", static_cast<unsigned>(g_frame), connected);
#else
        bool connected = tud_mounted();
        g_dbg_stage = 2;
        g_ring.snapshot(window.data(), window.size());
#endif
        g_dbg_stage = 3;
        std::snprintf(top, sizeof(top), "F%u A%u M%u", static_cast<unsigned>(g_frame),
                      static_cast<unsigned>(g_speaker_cb.load()), static_cast<unsigned>(g_mic_cb.load()));
        screen_draw(connected, window.data(), window.size(), top, bottom);
        g_frame = g_frame + 1;
        g_heartbeat_us = esp_timer_get_time();
        g_dbg_stage = 4;
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
