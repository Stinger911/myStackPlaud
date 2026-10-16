#include "recorder.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "config.hpp"
#include "sample_fifo.hpp"
#include "wav_header.hpp"

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

// SD write size: a multiple of the 512-byte sector, up to one 16 KiB FAT cluster. The audio starts at
// byte 512 of the file (see wav_header.hpp), so every write is sector-aligned and goes to the card as
// one multi-block transfer. Chosen at start-up as a quarter of the FIFO at most, so a FIFO never has
// to be completely full before a write can happen.
constexpr size_t kMinChunk = 2048;  // samples (4 KiB)
constexpr size_t kMaxChunk = 8192;  // samples (16 KiB)
constexpr unsigned kMinFifoLog2 = 13;
constexpr int64_t kHeaderUpdateUs = CONFIG_MYPLAUD_HEADER_SYNC_SECONDS * 1'000'000LL;
constexpr size_t kMaxEvents = 32;
constexpr size_t kMaxSlow = 32;
constexpr int64_t kSlowUs = 200'000;  // an SD operation slower than this counts as a stall
constexpr int16_t kSilentPeak = 8;    // lost samples this quiet count as lost silence, not lost audio
constexpr uint32_t kWriterTickMs = 20;

enum State { kIdle, kStarting, kRecording, kStopping, kErrNoSd, kErrWrite };

// A span of samples that did not fit in the FIFO; it is replaced by zeros to keep the timeline.
struct DropEvent {
    uint32_t start;   // position in the stream, in samples
    uint32_t length;  // samples
    uint16_t peak;    // loudest lost sample (0 = only silence was lost)
};

// One recorded stream. The I2S task is the only producer; the writer task is the only consumer.
struct Stream {
    DynSampleFifo fifo;
    std::atomic<int64_t> t0{0};
    uint32_t timeline = 0;          // samples accepted into the FIFO (zeros included)
    uint32_t gap = 0;               // samples lost and not yet replaced by zeros
    uint32_t events = 0;            // number of drop events (may exceed kMaxEvents)
    DropEvent log[kMaxEvents] = {};
    std::atomic<uint32_t> lost{0};        // samples lost in this session
    std::atomic<uint32_t> high_water{0};  // most samples ever waiting in the FIFO
    std::atomic<uint32_t> second_high{0}; // same, for the current second (reset by the writer)
};

// A second in which something looked slow: kept for the .TXT (bounded, the first kMaxSlow).
struct SlowSecond {
    uint32_t t_s;        // seconds since the start of the session
    uint16_t write_ms;   // slowest SD write
    uint16_t late_ms;    // how late the writer task woke up
    uint16_t drain_ms;   // slowest pass over both FIFOs
    uint16_t kib;        // data written during the second
    uint8_t me_pct;      // highest ME FIFO fill
    uint8_t ot_pct;      // highest OT FIFO fill
    uint32_t lost;       // samples lost during the second (both streams)
};

Stream s_me;
Stream s_ot;
std::atomic<uint32_t> s_write_max_ms{0};  // slowest SD write
std::atomic<uint32_t> s_sync_max_ms{0};   // slowest header rewrite + fsync
std::atomic<uint32_t> s_stalls{0};        // SD operations slower than kSlowUs
uint32_t s_drain_max_ms = 0;              // slowest pass over both FIFOs (writer task only)
uint32_t s_late_max_ms = 0;               // latest wake-up of the writer task (writer task only)
SlowSecond s_slow[kMaxSlow] = {};
uint32_t s_slow_count = 0;                // may exceed kMaxSlow
int64_t s_start_us = 0;
int64_t s_stop_us = 0;
std::atomic<bool> s_feed{false};
std::atomic<int> s_state{kIdle};
std::atomic<bool> s_toggle{false};
std::atomic<uint32_t> s_drops{0};          // all lost samples (silence included)
std::atomic<uint32_t> s_drops_audible{0};  // lost samples that were not silence
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_me_samples{0};

bool s_alloc_ok = false;
size_t s_heap_free = 0;     // free internal heap before the buffers were allocated
size_t s_heap_largest = 0;  // largest free block at the same moment
uint32_t s_sd_khz = 0;      // SPI clock the card was mounted with
bool s_mounted = false;
int s_fd_me = -1;
int s_fd_ot = -1;
uint32_t s_bytes_me = 0;
uint32_t s_bytes_ot = 0;
int16_t *s_chunk = nullptr;
size_t s_chunk_samples = 0;

// Try the configured SPI clock first and fall back to 20 MHz, which every card accepts.
bool mount_sd() {
    if (s_mounted) return true;
    const int freqs[] = {CONFIG_MYPLAUD_SD_FREQ_KHZ, 20000};
    for (size_t i = 0; i < std::size(freqs); ++i) {
        if (i > 0 && freqs[i] == freqs[0]) break;
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
        host.max_freq_khz = freqs[i];
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.gpio_cs = kSdCs;
        slot.host_id = kSdHost;
        esp_vfs_fat_mount_config_t mnt = {};
        mnt.format_if_mount_failed = false;
        mnt.max_files = 4;
        mnt.allocation_unit_size = 16 * 1024;
        sdmmc_card_t *card = nullptr;
        err = esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &mnt, &card);
        if (err == ESP_OK) {
            sdmmc_card_print_info(stdout, card);
            s_sd_khz = static_cast<uint32_t>(card->real_freq_khz);
            ESP_LOGI(TAG, "SD mounted at %u kHz (asked for %d)", (unsigned)s_sd_khz, freqs[i]);
            s_mounted = true;
            return true;
        }
        ESP_LOGE(TAG, "SD mount at %d kHz: %s", freqs[i], esp_err_to_name(err));
        spi_bus_free(kSdHost);
    }
    return false;
}

// Rewrite the header for `data_bytes` of PCM, then return to the end of the audio.
bool write_wav_header(int fd, uint32_t data_bytes) {
    uint8_t h[kWavHeaderBytes];
    build_wav_header(h, data_bytes, kUacSampleRate);
    if (lseek(fd, 0, SEEK_SET) < 0) return false;
    if (write(fd, h, sizeof(h)) != static_cast<ssize_t>(sizeof(h))) return false;
    fsync(fd);  // updates the FAT directory entry (file size) as well
    return lseek(fd, static_cast<off_t>(kWavHeaderBytes + data_bytes), SEEK_SET) >= 0;
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
    if (!s_alloc_ok) { ESP_LOGE(TAG, "no buffers"); s_state = kErrWrite; return false; }
    if (!mount_sd()) { s_state = kErrNoSd; return false; }
    mkdir(kDir, 0777);  // fine if it already exists
    uint32_t id = next_session_id();
    char path[48];
    std::snprintf(path, sizeof(path), "%s/R%04uME.WAV", kDir, (unsigned)id);
    s_fd_me = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    std::snprintf(path, sizeof(path), "%s/R%04uOT.WAV", kDir, (unsigned)id);
    s_fd_ot = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (s_fd_me < 0 || s_fd_ot < 0) {
        ESP_LOGE(TAG, "cannot create files in %s", kDir);
        if (s_fd_me >= 0) close(s_fd_me);
        if (s_fd_ot >= 0) close(s_fd_ot);
        s_fd_me = s_fd_ot = -1;
        s_state = kErrWrite;
        return false;
    }
    s_bytes_me = s_bytes_ot = 0;
    if (!write_wav_header(s_fd_me, 0) || !write_wav_header(s_fd_ot, 0)) { s_state = kErrWrite; return false; }
    for (Stream *st : {&s_me, &s_ot}) {
        st->fifo.discard(st->fifo.available());
        st->t0 = 0;
        st->timeline = 0;
        st->gap = 0;
        st->events = 0;
        st->lost = 0;
        st->high_water = 0;
        st->second_high = 0;
    }
    s_drops = 0;
    s_drops_audible = 0;
    s_me_samples = 0;
    s_write_max_ms = 0;
    s_sync_max_ms = 0;
    s_stalls = 0;
    s_drain_max_ms = 0;
    s_late_max_ms = 0;
    s_slow_count = 0;
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
// `write_max_ms` is the slowest single write seen since the caller last reset it (per-second telemetry).
bool drain(bool final, uint32_t &write_max_ms) {
    struct Out { Stream *stream; int fd; uint32_t *bytes; };
    Out outs[2] = {{&s_me, s_fd_me, &s_bytes_me}, {&s_ot, s_fd_ot, &s_bytes_ot}};
    for (auto &o : outs) {
        while (true) {
            size_t avail = o.stream->fifo.available();
            if (avail < (final ? 1 : s_chunk_samples)) break;
            size_t n = o.stream->fifo.read(s_chunk, std::min(avail, s_chunk_samples));
            int64_t t = esp_timer_get_time();
            ssize_t written = write(o.fd, s_chunk, n * sizeof(int16_t));
            int64_t us = esp_timer_get_time() - t;
            note_latency(us, s_write_max_ms);
            write_max_ms = std::max<uint32_t>(write_max_ms, static_cast<uint32_t>(us / 1000));
            if (written != static_cast<ssize_t>(n * sizeof(int16_t))) {
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
    uint32_t unused = 0;
    bool ok = !failed && drain(true, unused);
    if (s_fd_me >= 0) { ok = write_wav_header(s_fd_me, s_bytes_me) && ok; close(s_fd_me); }
    if (s_fd_ot >= 0) { ok = write_wav_header(s_fd_ot, s_bytes_ot) && ok; close(s_fd_ot); }
    s_fd_me = s_fd_ot = -1;
    char path[48];
    std::snprintf(path, sizeof(path), "%s/R%04u.TXT", kDir, (unsigned)s_session.load());
    if (FILE *f = std::fopen(path, "w")) {
        std::fprintf(f, "rate=%u\nformat=s16le mono, data from byte %u\nme=R%04uME.WAV (microphone)\nothers=R%04uOT.WAV (host playback)\n",
                     (unsigned)kUacSampleRate, (unsigned)kWavHeaderBytes, (unsigned)s_session.load(), (unsigned)s_session.load());
        std::fprintf(f, "me_samples=%u\nothers_samples=%u\n", (unsigned)(s_bytes_me / 2), (unsigned)(s_bytes_ot / 2));
        // dropped_samples counts everything that did not fit; dropped_audible leaves out lost silence
        // (typically the host stream while nothing is playing).
        std::fprintf(f, "dropped_samples=%u\ndropped_audible=%u\ndropped_me=%u\ndropped_others=%u\n",
                     (unsigned)s_drops.load(), (unsigned)s_drops_audible.load(), (unsigned)s_me.lost.load(),
                     (unsigned)s_ot.lost.load());
        std::fprintf(f, "t0_me_us=%lld\nt0_others_us=%lld\n", (long long)s_me.t0.load(), (long long)s_ot.t0.load());
        // Wall clock of the session, to compare with the sample counts (clock drift, lost time).
        std::fprintf(f, "wall_us=%lld\n", (long long)(s_stop_us - s_start_us));
        std::fprintf(f, "sd_khz=%u\nchunk_samples=%u\nheap_free_at_init=%u\nheap_largest_block_at_init=%u\n",
                     (unsigned)s_sd_khz, (unsigned)s_chunk_samples, (unsigned)s_heap_free, (unsigned)s_heap_largest);
        std::fprintf(f, "fifo_samples=%u\nfifo_me_samples=%u\nfifo_others_samples=%u\n",
                     (unsigned)s_me.fifo.capacity(), (unsigned)s_me.fifo.capacity(), (unsigned)s_ot.fifo.capacity());
        std::fprintf(f, "fifo_high_water_me=%u\nfifo_high_water_others=%u\n", (unsigned)s_me.high_water.load(),
                     (unsigned)s_ot.high_water.load());
        std::fprintf(f, "write_max_ms=%u\nheader_sync_max_ms=%u\nstalls_over_200ms=%u\nheader_sync_s=%d\n",
                     (unsigned)s_write_max_ms.load(), (unsigned)s_sync_max_ms.load(), (unsigned)s_stalls.load(),
                     CONFIG_MYPLAUD_HEADER_SYNC_SECONDS);
        // drain_max_ms: slowest pass over both FIFOs; writer_late_max_ms: how late the writer task woke
        // up (a large value with fast writes means the task was starved, not the card).
        std::fprintf(f, "drain_max_ms=%u\nwriter_late_max_ms=%u\n", (unsigned)s_drain_max_ms, (unsigned)s_late_max_ms);
        std::fprintf(f, "slow_seconds=%u\n", (unsigned)s_slow_count);
        for (uint32_t i = 0; i < std::min<uint32_t>(s_slow_count, kMaxSlow); ++i) {
            const SlowSecond &s = s_slow[i];
            std::fprintf(f, "slow_sec=t%u write_ms%u late_ms%u drain_ms%u kib%u me%u%% ot%u%% lost%u\n", (unsigned)s.t_s,
                         (unsigned)s.write_ms, (unsigned)s.late_ms, (unsigned)s.drain_ms, (unsigned)s.kib,
                         (unsigned)s.me_pct, (unsigned)s.ot_pct, (unsigned)s.lost);
        }
        for (auto &pair : {std::make_pair("me", &s_me), std::make_pair("others", &s_ot)}) {
            const Stream &st = *pair.second;
            std::fprintf(f, "drop_events_%s=%u\n", pair.first, (unsigned)st.events);
            for (uint32_t i = 0; i < std::min<uint32_t>(st.events, kMaxEvents); ++i)
                std::fprintf(f, "drop_%s=%u,%u,%u\n", pair.first, (unsigned)st.log[i].start, (unsigned)st.log[i].length,
                             (unsigned)st.log[i].peak);
        }
        std::fclose(f);
    }
    ESP_LOGI(TAG, "session %u done: me=%u others=%u samples, dropped=%u (audible %u)", (unsigned)s_session.load(),
             (unsigned)(s_bytes_me / 2), (unsigned)(s_bytes_ot / 2), (unsigned)s_drops.load(),
             (unsigned)s_drops_audible.load());
    s_state = ok ? kIdle : kErrWrite;
}

// Per-second telemetry of the writer task; it keeps the seconds in which something looked slow.
struct Telemetry {
    int64_t second_start_us = 0;
    uint32_t write_ms = 0;
    uint32_t late_ms = 0;
    uint32_t drain_ms = 0;
    uint32_t bytes = 0;
    uint32_t lost_at_start = 0;

    void reset(int64_t now) {
        second_start_us = now;
        write_ms = late_ms = drain_ms = bytes = 0;
        lost_at_start = s_drops.load();
    }

    void tick(int64_t now) {
        if (now - second_start_us < 1'000'000) return;
        uint32_t me_hw = s_me.second_high.exchange(0), ot_hw = s_ot.second_high.exchange(0);
        uint32_t me_cap = std::max<size_t>(1, s_me.fifo.capacity()), ot_cap = std::max<size_t>(1, s_ot.fifo.capacity());
        uint32_t me_pct = me_hw * 100 / me_cap, ot_pct = ot_hw * 100 / ot_cap;
        uint32_t lost = s_drops.load() - lost_at_start;
        bool slow = write_ms >= 50 || late_ms >= 60 || drain_ms >= 150 || me_pct >= 60 || ot_pct >= 60 || lost > 0;
        if (slow) {
            if (s_slow_count < kMaxSlow)
                s_slow[s_slow_count] = {static_cast<uint32_t>((now - s_start_us) / 1'000'000),
                                        static_cast<uint16_t>(std::min<uint32_t>(write_ms, 65535)),
                                        static_cast<uint16_t>(std::min<uint32_t>(late_ms, 65535)),
                                        static_cast<uint16_t>(std::min<uint32_t>(drain_ms, 65535)),
                                        static_cast<uint16_t>(std::min<uint32_t>(bytes / 1024, 65535)),
                                        static_cast<uint8_t>(std::min<uint32_t>(me_pct, 100)),
                                        static_cast<uint8_t>(std::min<uint32_t>(ot_pct, 100)), lost};
            ++s_slow_count;
        }
        reset(now);
    }
};

void writer_task(void *) {
    int64_t last_header_us = 0;
    bool header_me_next = true;
    Telemetry tel;
    while (true) {
        int64_t before = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(kWriterTickMs));
        int64_t woke = esp_timer_get_time();
        int st = s_state.load();
        if (s_toggle.exchange(false)) {
            if (st == kIdle || st == kErrNoSd || st == kErrWrite) {
                s_state = kStarting;
                if (start_session()) {
                    s_state = kRecording;
                    last_header_us = esp_timer_get_time();
                    tel.reset(last_header_us);
                }
            } else if (st == kRecording) {
                s_state = kStopping;
                stop_session(false);
            }
            continue;
        }
        if (st != kRecording) continue;
        int64_t late_us = (woke - before) - static_cast<int64_t>(kWriterTickMs) * 1000;
        uint32_t late_ms = late_us > 0 ? static_cast<uint32_t>(late_us / 1000) : 0;
        tel.late_ms = std::max(tel.late_ms, late_ms);
        s_late_max_ms = std::max(s_late_max_ms, late_ms);

        uint32_t bytes_before = s_bytes_me + s_bytes_ot;
        if (!drain(false, tel.write_ms)) { stop_session(true); continue; }
        int64_t now = esp_timer_get_time();
        uint32_t drain_ms = static_cast<uint32_t>((now - woke) / 1000);
        tel.drain_ms = std::max(tel.drain_ms, drain_ms);
        s_drain_max_ms = std::max(s_drain_max_ms, drain_ms);
        tel.bytes += s_bytes_me + s_bytes_ot - bytes_before;
        tel.tick(now);

        // Keep the files valid if power is lost. One file per round, alternating: an fsync of both at once
        // stalled the writer for ~450 ms, almost the whole FIFO.
        if (now - last_header_us > kHeaderUpdateUs / 2) {
            last_header_us = now;
            bool ok = header_me_next ? write_wav_header(s_fd_me, s_bytes_me) : write_wav_header(s_fd_ot, s_bytes_ot);
            header_me_next = !header_me_next;
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
        int peak = 0;
        for (size_t i = written; i < count; ++i) peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
        if (st.gap == 0) {  // a new event; later losses before the gap is filled extend it
            if (st.events < kMaxEvents) st.log[st.events] = {st.timeline, 0, 0};
            ++st.events;
        }
        if (st.events <= kMaxEvents) {
            DropEvent &e = st.log[st.events - 1];
            e.length += lost;
            e.peak = static_cast<uint16_t>(std::max<int>(e.peak, peak));
        }
        st.gap += lost;
        st.lost.fetch_add(lost);
        s_drops.fetch_add(lost);
        if (peak > kSilentPeak) s_drops_audible.fetch_add(lost);
    }
    uint32_t level = st.fifo.available();
    if (level > st.high_water.load(std::memory_order_relaxed)) st.high_water.store(level, std::memory_order_relaxed);
    if (level > st.second_high.load(std::memory_order_relaxed)) st.second_high.store(level, std::memory_order_relaxed);
}

// Two FIFOs of the same size (the biggest the free RAM allows, there is no PSRAM on this board), so
// neither stream is left with a sliver. Then the write buffer: internal, DMA-capable and 16-byte aligned,
// otherwise the SD driver would copy and send it one sector at a time.
bool alloc_buffers() {
    s_heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    unsigned log2 = 0;
    for (unsigned l = CONFIG_MYPLAUD_FIFO_LOG2; l >= kMinFifoLog2; --l) {
        if (s_me.fifo.alloc(l)) {
            if (s_ot.fifo.alloc(l)) { log2 = l; break; }
            s_me.fifo.release();
        }
    }
    if (log2 == 0) return false;
    size_t chunk = std::clamp<size_t>(s_me.fifo.capacity() / 4, kMinChunk, kMaxChunk);
    while (chunk >= kMinChunk) {
        s_chunk = static_cast<int16_t *>(
            heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (s_chunk) break;
        chunk /= 2;
    }
    if (!s_chunk) return false;
    s_chunk_samples = chunk;
    return true;
}

}  // namespace

void recorder_init() {
    s_alloc_ok = alloc_buffers();
    ESP_LOGI(TAG, "buffers %s: FIFO %u + %u samples (%u ms each), chunk %u samples, heap free %u largest %u",
             s_alloc_ok ? "ok" : "FAILED", (unsigned)s_me.fifo.capacity(), (unsigned)s_ot.fifo.capacity(),
             (unsigned)(s_me.fifo.capacity() * 1000 / kUacSampleRate), (unsigned)s_chunk_samples,
             (unsigned)s_heap_free, (unsigned)s_heap_largest);
    xTaskCreate(writer_task, "recorder", 6144, nullptr, 4, nullptr);
}

void recorder_toggle() { s_toggle.store(true); }

void recorder_feed_me(const int16_t *samples, size_t count) { feed(s_me, samples, count); }
void recorder_feed_others(const int16_t *samples, size_t count) { feed(s_ot, samples, count); }

RecorderInfo recorder_info() {
    RecorderInfo info{};
    switch (s_state.load()) {
        case kStarting: info.state = RecorderState::Starting; break;
        case kRecording: info.state = RecorderState::Recording; break;
        case kStopping: info.state = RecorderState::Stopping; break;
        case kErrNoSd: info.state = RecorderState::NoSd; break;
        case kErrWrite: info.state = RecorderState::WriteError; break;
        default: info.state = RecorderState::Idle; break;
    }
    info.session = s_session.load();
    info.elapsed_s = s_me_samples.load() / kUacSampleRate;
    info.drops = s_drops_audible.load();  // lost silence is not worth a warning
    return info;
}

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
