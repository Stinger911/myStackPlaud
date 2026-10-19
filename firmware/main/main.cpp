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
#include <cstdlib>
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

#include "audio.hpp"
#include "config.hpp"
#include "recorder.hpp"
#include "debug.hpp"
#include "file_mode.hpp"
#include "g0_button.hpp"
#include "keyboard.hpp"
#include "link_debounce.hpp"
#include "power_policy.hpp"
#include "settings.hpp"
#include "volume.hpp"
#include "m5stack-cardputer.hpp"
#include "sample_fifo.hpp"
#include "sample_ring.hpp"
#include "screen.hpp"

static const char *TAG = "myplaud";

// Host stream level below which it counts as silence, and how long it must stay quiet to show SILENCE.
constexpr int kSoundThreshold = 100;  // about -50 dBFS
constexpr int64_t kSilenceAfterUs = 3'000'000;

volatile uint32_t g_dbg_stage = 0;

namespace {

SampleRing<4096> g_ring;
std::atomic<uint32_t> g_speaker_cb{0};
std::atomic<uint32_t> g_mic_cb{0};
std::atomic<uint32_t> g_mic_underrun{0};
std::atomic<int> g_mic_peak{0};
std::atomic<uint32_t> g_mic_bytes{0};
// Device microphone -> USB microphone. About 170 ms at 48 kHz.
SampleFifo<8192> g_mic_fifo;
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
        audio_play(mono.data(), n);
        i += n;
    }
    return ESP_OK;
}

// Samples from the ES8311 microphone (audio_in rx task) go into the FIFO.
void on_mic_samples(const int16_t *s, size_t n) {
    g_mic_bytes.fetch_add(n * sizeof(int16_t));
    int peak = 0;
    for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(static_cast<int>(s[i])));
    if (peak > g_mic_peak.load()) g_mic_peak.store(peak);
    g_mic_fifo.write(s, n);
}

// Device -> host audio ("me"): hand the host what the microphone captured.
esp_err_t on_mic_data(uint8_t *buf, size_t len, size_t *bytes_read, void *) {
    g_mic_cb.fetch_add(1);
    auto *out = reinterpret_cast<int16_t *>(buf);
    size_t want = len / sizeof(int16_t);
    // Keep latency low: if the host was not reading for a while, drop the stale backlog.
    if (g_mic_fifo.available() > 4 * want) g_mic_fifo.discard(g_mic_fifo.available() - 2 * want);
    size_t got = g_mic_fifo.read(out, want);
    if (got < want) {
        std::memset(out + got, 0, (want - got) * sizeof(int16_t));
        g_mic_underrun.fetch_add(1);
    }
    *bytes_read = len;
    return ESP_OK;
}

void on_mute(uint32_t mute, void *) { audio_set_mute(mute != 0); }
void on_volume(uint32_t volume, void *) { audio_set_host_volume(volume); }

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

// File mode (USB flash drive): the recorder and the headset are not running, the card belongs to the
// computer. Leaves (by restarting) on `F`, G0 or an eject on the computer.
[[noreturn]] void run_file_mode(FileModeState state, const char *notice) {
    auto &cp = espp::M5StackCardputer::get();
    int battery_pct = -1;
    int64_t last_battery_us = -10'000'000;
    int64_t eject_seen_us = 0;
    while (true) {
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_battery_us > 5'000'000) {
            last_battery_us = now_us;
            float sum = 0;
            for (int i = 0; i < 8; ++i) sum += cp.battery_voltage();
            battery_pct = battery_percent(sum / 8);
        }
        for (KeyAction a = keyboard_next(); a != KeyAction::None; a = keyboard_next())
            if (a == KeyAction::ToggleFiles) file_mode_leave();
        if (gpio_get_level(GPIO_NUM_0) == 0) file_mode_leave();
        if (file_mode_ejected()) {  // let the computer finish its last commands first
            if (eject_seen_us == 0) eject_seen_us = now_us;
            if (now_us - eject_seen_us > 1'000'000) file_mode_leave();
        }
        ScreenModel model;
        model.link = Link::UsbFiles;
        model.mode = state == FileModeState::Ok ? Mode::Files : Mode::NoSd;
        model.battery_pct = static_cast<int8_t>(battery_pct);
        if (state == FileModeState::UsbFailed) model.notice = "USB FAILED";
        (void)notice;
        screen_draw(model, static_cast<uint32_t>(now_us / 1000));
        vTaskDelay(pdMS_TO_TICKS(33));
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

    settings_init();
    const bool file_mode = file_mode_boot_requested();  // `F` restarted the device into file mode
    FileModeState file_state = FileModeState::Ok;
    audio_set_level(settings_volume(kVolumeMax));  // applied to the codec once audio_start() has set it up

    auto init_mic = [&]() {
        ESP_LOGI(TAG, "init_mic: start");
        if (!audio_start(kUacSampleRate, on_mic_samples)) {
            ESP_LOGE(TAG, "microphone init failed");
            std::snprintf(bottom, sizeof(bottom), "MIC INIT FAIL");
        }
    };

#if CONFIG_MYPLAUD_SCREEN_TEST
    ESP_LOGI(TAG, "SCREEN TEST build: USB audio disabled");
    init_mic();
#else
    // Microphone first: the UAC mic task starts pulling from the FIFO as soon as the host opens it.
    init_mic();
    if (file_mode) {
        file_state = file_mode_start();
    } else {
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
    }
#endif

    if (!keyboard_start(audio_i2c_bus())) {
        ESP_LOGE(TAG, "keyboard init failed");
        std::snprintf(bottom, sizeof(bottom), "KEYBOARD FAIL");
    }
    gpio_config_t btn = {};
    btn.pin_bit_mask = 1ULL << GPIO_NUM_0;  // G0 button: short press starts, 1 s hold stops (g0_button.hpp)
    btn.mode = GPIO_MODE_INPUT;
    btn.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&btn);
    if (file_mode) run_file_mode(file_state, bottom);
    recorder_init();
    G0Button g0;

    g_heartbeat_us = esp_timer_get_time();
    xTaskCreate(watchdog_task, "myplaud_wd", 3072, nullptr, 6, nullptr);

    static std::array<int16_t, kLcdWidth * kSamplesPerColumn> window;  // too big for the main task stack
    char dbg_counters[48];
    char dbg_recorder[48];
    char dbg_boot[96];
    int64_t last_sound_us = 0;  // last time the host stream was louder than the silence threshold
    // Tab toggles the debug view; the screentest build starts in it.
#if CONFIG_MYPLAUD_SCREEN_TEST
    bool debug_view = true;
#else
    bool debug_view = false;
#endif
    constexpr int64_t kVolumeOverlayUs = 1'500'000;
    int64_t volume_overlay_until_us = 0;  // the volume bar is shown until then
    bool host_muted = false;  // the overlay is shown when the host changes it
    BatteryMonitor battery_monitor;
    BatteryLevel battery_level = BatteryLevel::Ok;
    int battery_pct = -1;
    int64_t last_battery_us = -10'000'000;  // read at once
    bool volume_unsaved = false;          // NVS is written once the overlay has gone, not on every step
    // Feedback (R4.3). The start beep plays first and the recording begins after it, and the stop beep
    // plays once the files are closed: the microphone never hears either of them.
    constexpr int64_t kStartDelayUs = 120'000;  // the 80 ms beep plus a margin
    constexpr int64_t kFlashUs = 150'000;
    int64_t start_at_us = 0;  // a start is waiting for its beep to end
    int64_t flash_until_us = 0;
    Flash flash_colour = Flash::None;
    RecorderState prev_state = RecorderState::Idle;
    Warning prev_warning = Warning::None;
#ifdef CONFIG_MYPLAUD_BEEPS_WHILE_RECORDING
    constexpr bool kBeepsWhileRecording = true;
#else
    constexpr bool kBeepsWhileRecording = false;
#endif
    while (true) {
        g_dbg_stage = 1;
#if CONFIG_MYPLAUD_SCREEN_TEST
        // Fake state and a moving sine so every part of the screen code is exercised.
        bool connected = (g_frame / 60) % 2 == 0;
        for (size_t i = 0; i < window.size(); ++i) {
            window[i] = static_cast<int16_t>(20000.0f * sinf((i + g_frame * 16) * 0.02f));
        }
        if (g_frame % 30 == 0)
        {
            AudioStats st = audio_stats();
            ESP_LOGI(TAG, "frame %u mic_bytes=%u peak=%d | i2s ok=%u err=%u last_err=%d raw_peak=%d",
                     static_cast<unsigned>(g_frame), static_cast<unsigned>(g_mic_bytes.load()), g_mic_peak.load(),
                     static_cast<unsigned>(st.read_ok), static_cast<unsigned>(st.read_err), st.last_err, st.raw_peak);
        }
#else
        static LinkDebounce link;  // up at once, down after 0.5 s; a suspended bus (host asleep) counts as down
        bool connected = link.update(tud_mounted() && !tud_suspended(), static_cast<uint32_t>(esp_timer_get_time() / 1000));
        g_dbg_stage = 2;
        g_ring.snapshot(window.data(), window.size());
#endif
        g_dbg_stage = 3;
        int me_peak = g_mic_peak.exchange(0);
        int ot_peak = 0;
        for (int16_t s : window) ot_peak = std::max(ot_peak, std::abs(static_cast<int>(s)));
        int64_t now_us = esp_timer_get_time();
        if (ot_peak > kSoundThreshold) last_sound_us = now_us;

        RecorderInfo rec = recorder_info();
        G0Context g0_ctx = G0Context::Idle;
        if (rec.state == RecorderState::Recording) g0_ctx = G0Context::Recording;
        else if (rec.state == RecorderState::Starting || rec.state == RecorderState::Stopping || start_at_us != 0)
            g0_ctx = G0Context::Busy;
        G0Output g0_out = g0.update(gpio_get_level(GPIO_NUM_0) == 0, g0_ctx, static_cast<uint32_t>(now_us / 1000));
        if (g0_out.event == G0Event::Start && rec.state == RecorderState::Idle) {
            audio_beep(Sound::Start);
            flash_colour = Flash::Green;
            flash_until_us = now_us + kFlashUs;
            start_at_us = now_us + kStartDelayUs;
        } else if (g0_out.event != G0Event::None) {
            recorder_toggle();  // Stop (after the hold) or acknowledge an error screen
        }
        if (start_at_us != 0 && now_us >= start_at_us) {
            start_at_us = 0;
            recorder_toggle();
        }
        // The files are closed (Stopping -> Idle): the stop beep. A failed stop is the error tone instead.
        if (prev_state == RecorderState::Stopping && rec.state == RecorderState::Idle) {
            audio_beep(Sound::Stop);
            flash_colour = Flash::White;
            flash_until_us = now_us + kFlashUs;
        } else if (rec.state != prev_state && (rec.state == RecorderState::WriteError || rec.state == RecorderState::CardLost ||
                                               rec.state == RecorderState::SdFull || rec.state == RecorderState::BatteryStop)) {
            audio_beep(Sound::Error);
        }
        prev_state = rec.state;
        const bool quiet_card = rec.state != RecorderState::Recording && rec.state != RecorderState::Starting &&
                                rec.state != RecorderState::Stopping;  // nothing is being recorded
        if (now_us - last_battery_us > 5'000'000) {  // average a burst of ADC reads, then judge it
            last_battery_us = now_us;
            float sum = 0;
            for (int i = 0; i < 8; ++i) sum += cp.battery_voltage();
            battery_pct = battery_percent(sum / 8);
            battery_level = battery_monitor.update(battery_pct);
            if (battery_level == BatteryLevel::Stop) recorder_stop_for_battery();
        }
        for (KeyAction a = keyboard_next(); a != KeyAction::None; a = keyboard_next()) {
            switch (a) {
                case KeyAction::ToggleDebug: debug_view = !debug_view; break;
                case KeyAction::VolumeUp:
                case KeyAction::VolumeDown: {
                    int level = volume_clamp(audio_level() + (a == KeyAction::VolumeUp ? 1 : -1));
                    if (level != audio_level()) {
                        audio_set_level(level);
                        volume_unsaved = true;
                        if (quiet_card || kBeepsWhileRecording) audio_beep(Sound::Tick);
                    }
                    volume_overlay_until_us = now_us + kVolumeOverlayUs;
                    break;
                }
                case KeyAction::ToggleFiles:
#if !CONFIG_MYPLAUD_SCREEN_TEST
                    // Only from READY with a card: nothing is open on the card, so the restart loses nothing.
                    if (rec.state == RecorderState::Idle) file_mode_enter();
#endif
                    break;
                default: break;
            }
        }
        if (audio_host_muted() != host_muted) {
            host_muted = audio_host_muted();
            volume_overlay_until_us = now_us + kVolumeOverlayUs;
        }
        if (volume_unsaved && now_us > volume_overlay_until_us) {
            settings_set_volume(audio_level());
            volume_unsaved = false;
        }
        ScreenModel model;
        model.flash = now_us < flash_until_us ? flash_colour : Flash::None;
        model.host_mute = host_muted;
        model.volume = now_us < volume_overlay_until_us ? static_cast<int8_t>(audio_level()) : -1;
        model.link = connected ? Link::UsbAudio : Link::Off;
        model.host_sound = connected && (now_us - last_sound_us) < kSilenceAfterUs;
        model.level_me = level_from_peak(me_peak);
        model.level_ot = level_from_peak(ot_peak);
        model.session = rec.session;
        model.elapsed_s = rec.elapsed_s;
        model.drops = rec.drops;
        switch (rec.state) {
            case RecorderState::Recording:
                model.mode = g0_out.holding ? Mode::Stopping : Mode::Recording;
                model.hold_progress = g0_out.hold_progress;
                model.stop_hint = g0_out.hint;
                if (rec.drops) model.warning = Warning::Drop;
                else if (rec.low_space) model.warning = Warning::LowSpace;
                else if (battery_level != BatteryLevel::Ok) model.warning = Warning::LowBattery;
                break;
            case RecorderState::Stopping: model.mode = Mode::Saving; break;
            case RecorderState::NoSd: model.mode = Mode::NoSd; break;
            case RecorderState::WriteError: model.mode = Mode::SdError; break;
            case RecorderState::CardLost: model.mode = Mode::CardLost; break;
            case RecorderState::SdFull: model.mode = Mode::SdFull; break;
            case RecorderState::BatteryStop: model.mode = Mode::BatteryStop; break;
            default: model.mode = Mode::Ready; break;
        }
        if (kBeepsWhileRecording && model.warning != Warning::None && model.warning != prev_warning)
            audio_beep(Sound::Warning);
        prev_warning = model.warning;
        model.sd_minutes = rec.sd_minutes;
        model.battery_pct = static_cast<int8_t>(battery_pct);  // -1 hides the field
        if (std::strcmp(bottom, "BOOT OK") != 0) model.notice = bottom;  // watchdog report, init failures

        // An error always brings the status view back (the debug view cannot show it).
        if (model.mode != Mode::Ready && model.mode != Mode::Recording && model.mode != Mode::Stopping) debug_view = false;
        model.debug_view = debug_view;
        std::snprintf(dbg_counters, sizeof(dbg_counters), "F%u A%u M%u U%u P%u", static_cast<unsigned>(g_frame),
                      static_cast<unsigned>(g_speaker_cb.load()), static_cast<unsigned>(g_mic_cb.load()),
                      static_cast<unsigned>(g_mic_underrun.load()), static_cast<unsigned>(audio_stats().play_underrun));
        recorder_status(dbg_recorder, sizeof(dbg_recorder));
        std::snprintf(dbg_boot, sizeof(dbg_boot), "%s  ME %d OT %d", bottom, me_peak, ot_peak);
        model.debug_lines[0] = dbg_counters;
        model.debug_lines[1] = dbg_recorder;
        model.debug_lines[2] = dbg_boot;
        model.samples = window.data();
        model.sample_count = window.size();

#if CONFIG_MYPLAUD_SCREEN_TEST
        // Unattended SD test: record from 5 s to 15 s after start.
        if (g_frame == 150 || g_frame == 450) recorder_toggle();
#endif
        screen_draw(model, static_cast<uint32_t>(now_us / 1000));
        g_frame = g_frame + 1;
        g_heartbeat_us = esp_timer_get_time();
        g_dbg_stage = 4;
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
