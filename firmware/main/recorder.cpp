#include "recorder.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "config.hpp"
#include "sample_fifo.hpp"

namespace {

const char *TAG = "recorder";

// Cardputer ADV: microSD on its own SPI bus.
constexpr spi_host_device_t kSdHost = SPI3_HOST;
constexpr gpio_num_t kSdSclk = GPIO_NUM_40;
constexpr gpio_num_t kSdMosi = GPIO_NUM_14;
constexpr gpio_num_t kSdMiso = GPIO_NUM_39;
constexpr gpio_num_t kSdCs = GPIO_NUM_12;
constexpr char kMount[] = "/sdcard";
constexpr char kDir[] = "/sdcard/MYPLAUD";

constexpr size_t kChunk = 2048;  // samples per SD write (4 KiB)
constexpr int64_t kHeaderUpdateUs = CONFIG_MYPLAUD_HEADER_SYNC_SECONDS * 1'000'000LL;
constexpr size_t kMaxEvents = 32;
constexpr int64_t kSlowUs = 200'000;  // an SD operation slower than this counts as a stall

enum State { kIdle, kStarting, kRecording, kStopping, kErrNoSd, kErrWrite };

// A span of samples that did not fit in the FIFO; it is replaced by zeros to keep the timeline.
struct DropEvent {
    uint32_t start;   // position in the stream, in samples
    uint32_t length;  // samples
};

// One recorded stream. The I2S task is the only producer; the writer task is the only consumer.
struct Stream {
    DynSampleFifo fifo;
    std::atomic<int64_t> t0{0};
    uint32_t timeline = 0;          // samples accepted into the FIFO (zeros included)
    uint32_t gap = 0;               // samples lost and not yet replaced by zeros
    uint32_t events = 0;            // number of drop events (may exceed kMaxEvents)
    DropEvent log[kMaxEvents] = {};
    std::atomic<uint32_t> high_water{0};  // most samples ever waiting in the FIFO
};

Stream s_me;
Stream s_ot;
std::atomic<uint32_t> s_write_max_ms{0};  // slowest fwrite
std::atomic<uint32_t> s_sync_max_ms{0};   // slowest header rewrite + fsync
std::atomic<uint32_t> s_stalls{0};        // SD operations slower than kSlowUs
int64_t s_start_us = 0;
int64_t s_stop_us = 0;
std::atomic<bool> s_feed{false};
std::atomic<int> s_state{kIdle};
std::atomic<bool> s_toggle{false};
std::atomic<uint32_t> s_drops{0};
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_me_samples{0};

bool s_mounted = false;
FILE *s_f_me = nullptr;
FILE *s_f_ot = nullptr;
uint32_t s_bytes_me = 0;
uint32_t s_bytes_ot = 0;
int16_t s_chunk[kChunk];

bool mount_sd() {
    if (s_mounted) return true;
    spi_bus_config_t bus = {};
    bus.mosi_io_num = kSdMosi;
    bus.miso_io_num = kSdMiso;
    bus.sclk_io_num = kSdSclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t err = spi_bus_initialize(kSdHost, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return false;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = kSdHost;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = kSdCs;
    slot.host_id = kSdHost;
    esp_vfs_fat_mount_config_t mnt = {};
    mnt.format_if_mount_failed = false;
    mnt.max_files = 4;
    mnt.allocation_unit_size = 16 * 1024;
    sdmmc_card_t *card = nullptr;
    err = esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &mnt, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount: %s", esp_err_to_name(err));
        spi_bus_free(kSdHost);
        return false;
    }
    sdmmc_card_print_info(stdout, card);
    s_mounted = true;
    return true;
}

void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }
void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }

// Rewrite the 44-byte header for `data_bytes` of PCM, then return to the end of the file.
bool write_wav_header(FILE *f, uint32_t data_bytes) {
    uint8_t h[44];
    std::memcpy(h, "RIFF", 4);  put_u32(h + 4, 36 + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);  put_u32(h + 16, 16);
    put_u16(h + 20, 1);  put_u16(h + 22, 1);
    put_u32(h + 24, kUacSampleRate);  put_u32(h + 28, kUacSampleRate * 2);
    put_u16(h + 32, 2);  put_u16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);  put_u32(h + 40, data_bytes);
    if (std::fseek(f, 0, SEEK_SET) != 0) return false;
    if (std::fwrite(h, 1, sizeof(h), f) != sizeof(h)) return false;
    std::fflush(f);
    fsync(fileno(f));  // fflush alone does not update the FAT directory entry (file size)
    return std::fseek(f, 0, SEEK_END) == 0;
}

uint32_t next_session_id() {
    uint32_t max_id = 0;
    DIR *d = opendir(kDir);
    if (!d) return 1;
    while (dirent *e = readdir(d)) {
        unsigned id = 0;
        char tail[8] = {};
        if (std::sscanf(e->d_name, "R%4u%7s", &id, tail) == 2) max_id = std::max<uint32_t>(max_id, id);
    }
    closedir(d);
    return max_id + 1;
}

bool start_session() {
    if (!mount_sd()) { s_state = kErrNoSd; return false; }
    mkdir(kDir, 0777);  // fine if it already exists
    uint32_t id = next_session_id();
    char path[48];
    std::snprintf(path, sizeof(path), "%s/R%04uME.WAV", kDir, (unsigned)id);
    s_f_me = std::fopen(path, "wb");
    std::snprintf(path, sizeof(path), "%s/R%04uOT.WAV", kDir, (unsigned)id);
    s_f_ot = std::fopen(path, "wb");
    if (!s_f_me || !s_f_ot) {
        ESP_LOGE(TAG, "cannot create files in %s", kDir);
        if (s_f_me) std::fclose(s_f_me);
        if (s_f_ot) std::fclose(s_f_ot);
        s_f_me = s_f_ot = nullptr;
        s_state = kErrWrite;
        return false;
    }
    s_bytes_me = s_bytes_ot = 0;
    if (!write_wav_header(s_f_me, 0) || !write_wav_header(s_f_ot, 0)) { s_state = kErrWrite; return false; }
    for (Stream *st : {&s_me, &s_ot}) {
        st->fifo.discard(st->fifo.available());
        st->t0 = 0;
        st->timeline = 0;
        st->gap = 0;
        st->events = 0;
        st->high_water = 0;
    }
    s_drops = 0;
    s_me_samples = 0;
    s_write_max_ms = 0;
    s_sync_max_ms = 0;
    s_stalls = 0;
    s_start_us = esp_timer_get_time();
    s_session = id;
    s_feed = true;
    ESP_LOGI(TAG, "recording session %u", (unsigned)id);
    return true;
}

void note_latency(int64_t us, std::atomic<uint32_t> &max_ms) {
    uint32_t ms = static_cast<uint32_t>(us / 1000);
    if (ms > max_ms.load()) max_ms.store(ms);
    if (us > kSlowUs) s_stalls.fetch_add(1);
}

// Move FIFO contents to the files. `final` also flushes the partial last chunk.
bool drain(bool final) {
    struct Out { Stream *stream; FILE *f; uint32_t *bytes; };
    Out outs[2] = {{&s_me, s_f_me, &s_bytes_me}, {&s_ot, s_f_ot, &s_bytes_ot}};
    for (auto &o : outs) {
        while (true) {
            size_t avail = o.stream->fifo.available();
            if (avail < (final ? 1 : kChunk)) break;
            size_t n = o.stream->fifo.read(s_chunk, std::min(avail, kChunk));
            int64_t t = esp_timer_get_time();
            size_t written = std::fwrite(s_chunk, sizeof(int16_t), n, o.f);
            note_latency(esp_timer_get_time() - t, s_write_max_ms);
            if (written != n) {
                ESP_LOGE(TAG, "SD write failed");
                return false;
            }
            *o.bytes += n * sizeof(int16_t);
            if (o.stream == &s_me) s_me_samples.fetch_add(n);
        }
    }
    return true;
}

void stop_session(bool failed) {
    s_feed = false;
    s_stop_us = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(30));  // let the I2S tasks finish the block they are in
    bool ok = !failed && drain(true);
    if (s_f_me) { ok = write_wav_header(s_f_me, s_bytes_me) && ok; std::fclose(s_f_me); }
    if (s_f_ot) { ok = write_wav_header(s_f_ot, s_bytes_ot) && ok; std::fclose(s_f_ot); }
    s_f_me = s_f_ot = nullptr;
    char path[48];
    std::snprintf(path, sizeof(path), "%s/R%04u.TXT", kDir, (unsigned)s_session.load());
    if (FILE *f = std::fopen(path, "w")) {
        std::fprintf(f, "rate=%u\nformat=s16le mono\nme=R%04uME.WAV (microphone)\nothers=R%04uOT.WAV (host playback)\n",
                     (unsigned)kUacSampleRate, (unsigned)s_session.load(), (unsigned)s_session.load());
        std::fprintf(f, "me_samples=%u\nothers_samples=%u\ndropped_samples=%u\n", (unsigned)(s_bytes_me / 2),
                     (unsigned)(s_bytes_ot / 2), (unsigned)s_drops.load());
        std::fprintf(f, "t0_me_us=%lld\nt0_others_us=%lld\n", (long long)s_me.t0.load(), (long long)s_ot.t0.load());
        // Wall clock of the session, to compare with the sample counts (clock drift, lost time).
        std::fprintf(f, "wall_us=%lld\n", (long long)(s_stop_us - s_start_us));
        std::fprintf(f, "fifo_samples=%u\nfifo_high_water_me=%u\nfifo_high_water_others=%u\n",
                     (unsigned)s_me.fifo.capacity(), (unsigned)s_me.high_water.load(), (unsigned)s_ot.high_water.load());
        std::fprintf(f, "write_max_ms=%u\nheader_sync_max_ms=%u\nstalls_over_200ms=%u\nheader_sync_s=%d\n",
                     (unsigned)s_write_max_ms.load(), (unsigned)s_sync_max_ms.load(), (unsigned)s_stalls.load(),
                     CONFIG_MYPLAUD_HEADER_SYNC_SECONDS);
        for (auto &pair : {std::make_pair("me", &s_me), std::make_pair("others", &s_ot)}) {
            const Stream &st = *pair.second;
            std::fprintf(f, "drop_events_%s=%u\n", pair.first, (unsigned)st.events);
            for (uint32_t i = 0; i < std::min<uint32_t>(st.events, kMaxEvents); ++i)
                std::fprintf(f, "drop_%s=%u,%u\n", pair.first, (unsigned)st.log[i].start, (unsigned)st.log[i].length);
        }
        std::fclose(f);
    }
    ESP_LOGI(TAG, "session %u done: me=%u others=%u samples, dropped=%u", (unsigned)s_session.load(),
             (unsigned)(s_bytes_me / 2), (unsigned)(s_bytes_ot / 2), (unsigned)s_drops.load());
    s_state = ok ? kIdle : kErrWrite;
}

void writer_task(void *) {
    int64_t last_header_us = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int st = s_state.load();
        if (s_toggle.exchange(false)) {
            if (st == kIdle || st == kErrNoSd || st == kErrWrite) {
                s_state = kStarting;
                if (start_session()) { s_state = kRecording; last_header_us = esp_timer_get_time(); }
            } else if (st == kRecording) {
                s_state = kStopping;
                stop_session(false);
            }
            continue;
        }
        if (st != kRecording) continue;
        if (!drain(false)) { stop_session(true); continue; }
        int64_t now = esp_timer_get_time();
        if (now - last_header_us > kHeaderUpdateUs) {  // keep the files valid if power is lost
            last_header_us = now;
            bool ok = write_wav_header(s_f_me, s_bytes_me) && write_wav_header(s_f_ot, s_bytes_ot);
            note_latency(esp_timer_get_time() - now, s_sync_max_ms);
            if (!ok) stop_session(true);
        }
    }
}

// Samples that do not fit are counted as a gap and replaced by zeros as soon as there is room,
// so the file keeps the length of the elapsed time instead of silently getting shorter.
void feed(Stream &st, const int16_t *samples, size_t count) {
    if (!s_feed.load(std::memory_order_relaxed)) return;
    if (st.t0.load() == 0) st.t0.store(esp_timer_get_time());
    static const int16_t zeros[256] = {};
    while (st.gap > 0) {
        size_t n = std::min<size_t>(st.gap, sizeof(zeros) / sizeof(zeros[0]));
        size_t w = st.fifo.write(zeros, n);
        st.gap -= w;
        st.timeline += w;
        if (w < n) break;
    }
    size_t written = 0;
    if (st.gap == 0) {
        written = st.fifo.write(samples, count);
        st.timeline += written;
    }
    if (written < count) {
        size_t lost = count - written;
        if (st.gap == 0) {  // a new event; later losses before the gap is filled extend it
            if (st.events < kMaxEvents) st.log[st.events] = {st.timeline, 0};
            ++st.events;
        }
        if (st.events <= kMaxEvents) st.log[st.events - 1].length += lost;
        st.gap += lost;
        s_drops.fetch_add(lost);
    }
    uint32_t level = st.fifo.available();
    if (level > st.high_water.load(std::memory_order_relaxed)) st.high_water.store(level, std::memory_order_relaxed);
}

}  // namespace

void recorder_init() {
    // The buffers are allocated once, as large as the free RAM allows (no PSRAM on this board).
    size_t a = s_me.fifo.init(CONFIG_MYPLAUD_FIFO_LOG2, 13);
    size_t b = s_ot.fifo.init(CONFIG_MYPLAUD_FIFO_LOG2, 13);
    ESP_LOGI(TAG, "FIFO %u + %u samples (%u ms)", (unsigned)a, (unsigned)b,
             (unsigned)(std::min(a, b) * 1000 / kUacSampleRate));
    xTaskCreate(writer_task, "recorder", 6144, nullptr, 4, nullptr);
}

void recorder_toggle() { s_toggle.store(true); }

void recorder_feed_me(const int16_t *samples, size_t count) { feed(s_me, samples, count); }
void recorder_feed_others(const int16_t *samples, size_t count) { feed(s_ot, samples, count); }

void recorder_status(char *out, size_t out_size) {
    switch (s_state.load()) {
        case kIdle: std::snprintf(out, out_size, "REC OFF"); break;
        case kStarting: std::snprintf(out, out_size, "REC START"); break;
        case kStopping: std::snprintf(out, out_size, "REC STOP"); break;
        case kErrNoSd: std::snprintf(out, out_size, "NO SD"); break;
        case kErrWrite: std::snprintf(out, out_size, "SD ERR"); break;
        default: {
            unsigned secs = s_me_samples.load() / kUacSampleRate;
            // W = slowest SD write, Y = slowest header sync (ms), D = lost samples
            std::snprintf(out, out_size, "REC %04u %uS W%u Y%u D%u", (unsigned)s_session.load(), secs,
                          (unsigned)s_write_max_ms.load(), (unsigned)s_sync_max_ms.load(), (unsigned)s_drops.load());
        }
    }
}
