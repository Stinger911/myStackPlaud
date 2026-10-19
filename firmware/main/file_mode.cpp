#include "file_mode.hpp"

#include <atomic>
#include <cstring>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdmmc_cmd.h"
#include "tusb.h"
#include "usb_device_uac.h"

namespace {

const char *TAG = "files";

// Same card wiring as the recorder.
constexpr spi_host_device_t kSdHost = SPI3_HOST;
constexpr gpio_num_t kSdSclk = GPIO_NUM_40;
constexpr gpio_num_t kSdMosi = GPIO_NUM_14;
constexpr gpio_num_t kSdMiso = GPIO_NUM_39;
constexpr gpio_num_t kSdCs = GPIO_NUM_12;

constexpr uint32_t kFileModeMagic = 0xF11E0001;
RTC_NOINIT_ATTR uint32_t g_rtc_file_mode;

sdmmc_card_t *s_card = nullptr;
std::atomic<bool> s_active{false};
std::atomic<bool> s_ejected{false};

bool card_open() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = kSdMosi;
    bus.miso_io_num = kSdMiso;
    bus.sclk_io_num = kSdSclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    if (spi_bus_initialize(kSdHost, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) return false;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = kSdHost;
    host.max_freq_khz = CONFIG_MYPLAUD_SD_FREQ_KHZ;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = kSdCs;
    slot.host_id = kSdHost;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) host.max_freq_khz = 20000;  // the proven clock
        sdspi_dev_handle_t handle;
        if (sdspi_host_init() != ESP_OK) break;
        if (sdspi_host_init_device(&slot, &handle) != ESP_OK) {
            sdspi_host_deinit();
            break;
        }
        host.slot = handle;
        s_card = new sdmmc_card_t();
        if (sdmmc_card_init(&host, s_card) == ESP_OK) {
            sdmmc_card_print_info(stdout, s_card);
            return true;
        }
        delete s_card;
        s_card = nullptr;
        sdspi_host_remove_device(handle);
        sdspi_host_deinit();
    }
    spi_bus_free(kSdHost);
    return false;
}

}  // namespace

void file_mode_enter() {
    g_rtc_file_mode = kFileModeMagic;
    esp_restart();
    while (true) {}
}

void file_mode_leave() {
    g_rtc_file_mode = 0;
    esp_restart();
    while (true) {}
}

bool file_mode_boot_requested() {
    bool requested = g_rtc_file_mode == kFileModeMagic;
    g_rtc_file_mode = 0;
    return requested;
}

FileModeState file_mode_start() {
    s_active = true;  // the descriptors below now describe a flash drive
    if (!card_open()) ESP_LOGW(TAG, "no card");
    uac_device_config_t cfg = {};  // no audio callbacks: only the USB stack and its task are used
    if (uac_device_init(&cfg) != ESP_OK) return FileModeState::UsbFailed;
    return s_card ? FileModeState::Ok : FileModeState::NoCard;
}

bool file_mode_ejected() { return s_ejected.load(); }

uint32_t file_mode_sectors() { return s_card ? static_cast<uint32_t>(s_card->csd.capacity) : 0; }

// ---------------------------------------------------------------- USB descriptors
// The audio descriptors come from usb_device_uac. In file mode they are replaced (the linker wraps the
// two callbacks, see main/CMakeLists.txt) by a device with one mass-storage interface.

extern "C" {

const uint8_t *__real_tud_descriptor_device_cb(void);
const uint8_t *__real_tud_descriptor_configuration_cb(uint8_t index);

const uint8_t *__wrap_tud_descriptor_device_cb(void) {
    static const tusb_desc_device_t desc = {
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = 0,  // the class is in the interface
        .bDeviceSubClass = 0,
        .bDeviceProtocol = 0,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor = CONFIG_UAC_TUSB_VID,
        .idProduct = CONFIG_UAC_TUSB_PID + 1,  // a different product: the computer does not mix it up with the headset
        .bcdDevice = 0x0100,
        .iManufacturer = 0x01,
        .iProduct = 0x02,
        .iSerialNumber = 0x03,
        .bNumConfigurations = 0x01,
    };
    if (!s_active.load()) return __real_tud_descriptor_device_cb();
    return reinterpret_cast<const uint8_t *>(&desc);
}

const uint8_t *__wrap_tud_descriptor_configuration_cb(uint8_t index) {
    static const uint8_t desc[] = {
        TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN, 0x00, 100),
        TUD_MSC_DESCRIPTOR(0, 0, 0x01, 0x81, 64),  // full speed bulk endpoints
    };
    if (!s_active.load()) return __real_tud_descriptor_configuration_cb(index);
    return desc;
}

// ---------------------------------------------------------------- USB mass storage

void tud_msc_inquiry_cb(uint8_t, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    std::memcpy(vendor_id, "myPlaud ", 8);
    std::memcpy(product_id, "SD card         ", 16);
    std::memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t) {
    if (!s_card || s_ejected.load()) {
        tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, 0x3A, 0x00);  // medium not present
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t, uint32_t *block_count, uint16_t *block_size) {
    *block_count = s_card ? static_cast<uint32_t>(s_card->csd.capacity) : 0;
    *block_size = 512;
}

bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool load_eject) {
    if (load_eject && !start) s_ejected = true;  // eject on the computer
    return true;
}

int32_t tud_msc_read10_cb(uint8_t, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    if (!s_card || offset != 0 || bufsize % 512) return -1;
    return sdmmc_read_sectors(s_card, buffer, lba, bufsize / 512) == ESP_OK ? static_cast<int32_t>(bufsize) : -1;
}

int32_t tud_msc_write10_cb(uint8_t, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    if (!s_card || offset != 0 || bufsize % 512) return -1;
    return sdmmc_write_sectors(s_card, buffer, lba, bufsize / 512) == ESP_OK ? static_cast<int32_t>(bufsize) : -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *, uint16_t) {
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);  // command not supported
    (void)scsi_cmd;
    return -1;
}

}  // extern "C"
