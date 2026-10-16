#include "settings.hpp"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
const char *TAG = "settings";
constexpr const char *kNs = "myplaud";
constexpr const char *kVolumeKey = "volume";
bool s_ready = false;
}  // namespace

void settings_init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    s_ready = (err == ESP_OK);
    if (!s_ready) ESP_LOGW(TAG, "NVS unavailable: %s", esp_err_to_name(err));
}

int settings_volume(int fallback) {
    if (!s_ready) return fallback;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return fallback;
    int8_t v = static_cast<int8_t>(fallback);
    if (nvs_get_i8(h, kVolumeKey, &v) != ESP_OK) v = static_cast<int8_t>(fallback);
    nvs_close(h);
    return v;
}

void settings_set_volume(int level) {
    if (!s_ready) return;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, kVolumeKey, static_cast<int8_t>(level));
    nvs_commit(h);
    nvs_close(h);
}
