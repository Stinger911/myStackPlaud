#include "keyboard.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

const char *TAG = "keyboard";

constexpr uint8_t kTcaAddr = 0x34;
constexpr uint8_t kRegCfg = 0x01;
constexpr uint8_t kRegIntStat = 0x02;
constexpr uint8_t kRegKeyEventA = 0x04;
constexpr uint8_t kRegKpGpio1 = 0x1D;
constexpr uint8_t kRegKpGpio2 = 0x1E;
constexpr uint8_t kRegKpGpio3 = 0x1F;
constexpr int kPollMs = 15;

i2c_master_dev_handle_t s_dev = nullptr;
QueueHandle_t s_queue = nullptr;

bool write_reg(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, 2, 50) == ESP_OK;
}

bool read_reg(uint8_t reg, uint8_t &value) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, &value, 1, 50) == ESP_OK;
}

void post(KeyAction a) {
    if (a != KeyAction::None) xQueueSend(s_queue, &a, 0);  // a full queue drops the key: nobody is reading
}

void keyboard_task(void *) {
    bool shift = false;
    KeyRepeater repeater;
    uint8_t held[4][14] = {};  // what each pressed key meant when it went down, so its release is matched
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        bool any = false;
        for (int n = 0; n < 10; ++n) {  // the controller's FIFO holds at most 10 events
            uint8_t raw = 0;
            if (!read_reg(kRegKeyEventA, raw) || raw == 0) break;
            any = true;
            KeyEvent k;
            if (!tca8418_decode(raw, k)) continue;
            if (k.row == kShiftRow && k.col == kShiftCol) shift = k.pressed;
            if (k.pressed) {
                KeyAction a = key_action(key_char(k.row, k.col, shift));
                held[k.row][k.col] = static_cast<uint8_t>(a);
                post(repeater.press(a, now_ms));
            } else {
                repeater.release(static_cast<KeyAction>(held[k.row][k.col]));
                held[k.row][k.col] = 0;
            }
        }
        if (any) write_reg(kRegIntStat, 0x01);
        post(repeater.tick(now_ms));
    }
}

}  // namespace

bool keyboard_start(i2c_master_bus_handle_t bus) {
    if (!bus) return false;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = kTcaAddr;
    cfg.scl_speed_hz = 400000;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(err));
        return false;
    }
    // Keypad matrix of 7 rows x 8 columns (56 keys), as the espp driver sets it up.
    if (!write_reg(kRegKpGpio1, 0x7F) || !write_reg(kRegKpGpio2, 0xFF) || !write_reg(kRegKpGpio3, 0x00)) {
        ESP_LOGE(TAG, "TCA8418 does not answer");
        return false;
    }
    uint8_t raw = 0;
    for (int i = 0; i < 16 && read_reg(kRegKeyEventA, raw) && raw != 0; ++i) {}  // drop stale events
    if (!write_reg(kRegIntStat, 0xFF) || !write_reg(kRegCfg, 0x01)) {
        ESP_LOGE(TAG, "TCA8418 config failed");
        return false;
    }
    s_queue = xQueueCreate(8, sizeof(KeyAction));
    if (!s_queue) return false;
    xTaskCreate(keyboard_task, "keyboard", 3072, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "keyboard ready");
    return true;
}

KeyAction keyboard_next() {
    KeyAction a = KeyAction::None;
    if (s_queue) xQueueReceive(s_queue, &a, 0);
    return a;
}
