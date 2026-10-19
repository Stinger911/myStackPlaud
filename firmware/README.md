# firmware: USB audio test (Cardputer ADV)

Step 1 of the USB-headset approach (see `../INTENT.md`): the device enumerates as a
USB audio device. The screen shows CONNECTED / DISCONNECTED and, while the host plays
audio to it, the waveform of that stream. The mic stream is silence for now.

## Status

Builds with ESP-IDF v5.4. Verified on a Cardputer ADV (2026-10-15):

- Screen test build (no USB audio): LCD, backlight, banner and waveform drawing work.
- USB audio build: enumerates on macOS as "usb uac" (1 in / 1 out, 48 kHz) and becomes the default
  output; CONNECTED banner and the waveform of the host stream work (about 1000 speaker callbacks/s).
  `CONFIG_UAC_SUPPORT_MACOS=y` is required, otherwise macOS sends audio in one short burst.
  The device microphone reaches the host as the USB microphone: 10 s recorded on macOS with ffmpeg
  (`ffmpeg -f avfoundation -i ":usb uac"`) had no dropouts and no clipping.
- The espp microphone task delivered no data on this board (its errors are swallowed), so capture
  uses own I2S/ES8311 code in `main/audio_in.cpp`; `espp` is only used for the LCD. GPIO43 is both
  the I2S word select and UART0 TX, so the UART console is off in this build.
- Playback to the codec DAC (headphone jack / built-in speaker) works together with the microphone:
  a 60 s quiet tone had no clicks and no dropouts. The host stream goes through a FIFO (40 ms
  pre-buffer, trimmed above 100 ms) read by a task paced by the I2S clock. DAC volume follows the
  host slider but is capped at -20 dB (starts at -30 dB) because the headphone output is loud;
  host mute silences it. USB and I2S clocks are independent, so a very long session may need more
  buffer management (not observed in a 60 s test).
- HUD (top line) shows `F` frames, `A` speaker callbacks, `M` mic callbacks, `U` mic underruns,
  `L` mic peak (thousands), `P` playback re-buffer count.
- Old HUD note: A watchdog restarts the
  device if the main loop stalls for 2 s and the bottom line then shows `STUCK S<stage> F<frame> A<cb>`.
- A pure test tone looks frozen because the 40 ms window advances by about half a period per frame.

Things that are not obvious about this board / the `espp/m5stack-cardputer` component:

- The component's LEDC backlight stays dark on the ADV; `main.cpp` drives GPIO38 directly.
- One SPI DMA transfer is limited to 32 KiB, so a frame is sent in 27-row strips.
- The driver byte-swaps the buffer in place, so buffers hold native RGB565 and alternate between two frames.
- The panel comes up colour-inverted; `screen.cpp` inverts pixels to compensate.
- A synchronous `clear()` before the first frame is needed, otherwise the first async write never completes.

`CONFIG_UAC_*` in `sdkconfig.defaults` (1 ch, 48 kHz, 16-bit) is read by `main/config.hpp`.

## Build

Requires ESP-IDF >= 5.1 (tested with v5.4: `. ~/Work/esp/esp-idf/export.sh`).

    cd firmware
    idf.py set-target esp32s3
    idf.py menuconfig        # USB Device UAC: set speaker/mic channels and sample rate
    idf.py build flash monitor

If flashing fails because USB is now owned by the UAC device, hold G0 while connecting
to enter download mode. After flashing from download mode, release G0 and replug the cable.

## Debug build variants

Overlay configs, built into separate directories (not committed):

    idf.py -B build-screentest -D SDKCONFIG=sdkconfig.screentest \
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.screentest.defaults" build

- `sdkconfig.screentest.defaults`: no USB audio, console on the native USB serial/JTAG port
  (logs via `idf.py monitor`), fake CONNECTED toggle and a moving sine, panic goes to gdbstub.
  This variant can be re-flashed over USB without entering download mode.
- `sdkconfig.bootonly.defaults`: `app_main` only logs; used to bisect boot problems.

Running from the Launcher is not required: flashing directly replaces Launcher in flash.
Take a backup first: `python -m esptool --chip esp32s3 -p PORT read_flash 0 0x800000 launcher-backup.bin`
(restore with `write_flash 0 launcher-backup.bin`).

## Next steps

2. Real microphone to the host (`on_mic_data` <- `initialize_microphone`).
3. Speaker output to the 3.5 mm jack (`play_audio`).
4. Record both streams to SD with timestamps.

## SD recording

Short press on G0 starts a recording; stopping needs a 1 s hold (progress ring and `HOLD TO STOP`; releasing earlier
cancels, and a short press while recording only shows the hint). Logic in `main/g0_button.hpp`, host test in
`test/g0_button_test.cpp`. The screen shows `READY`, `REC` with the elapsed time, `NO SD CARD`, `SD WRITE ERROR`.
Files land in `/MYPLAUD` on the microSD card: `R<nnnn>ME.WAV` (microphone), `R<nnnn>OT.WAV` (what the
host played; zeros while the host is silent), `R<nnnn>.TXT` (metadata, written on a clean stop). Mono, s16le,
48 kHz, 8.3 names. Both streams are fed from the I2S tasks on the same clock, so they are aligned to one
10 ms block. WAV headers are refreshed with `fsync` every 2 s, so a recording cut by power loss stays
playable (verified: cable pulled mid-recording, 12 s recovered, header exact).

Since 2026-10-16 (M1 diagnostics and write path):
- The WAV header is padded to 512 bytes with a `JUNK` chunk, so the audio starts on a sector boundary
  (`main/wav_header.hpp`, host test `test/wav_header_test.cpp`; ffprobe, ffmpeg and Python `wave` read it).
  Writes are 4-16 KiB (a quarter of the FIFO at most), sector-aligned, from a DMA-capable buffer, through
  plain `write()` so FATFS can send them as multi-block transfers. Older files keep their 44-byte header.
- Both FIFOs get the same size (the biggest that fits); the SD clock is `MYPLAUD_SD_FREQ_KHZ` (default 20 MHz; 40 MHz is opt-in, it did not mount on the test card,
  falls back to 20 MHz).
- `R<nnnn>.TXT` now also has: `dropped_audible` (lost samples that were not silence, shown on the screen),
  `dropped_me` / `dropped_others`, `drop_*=start,length,peak` (peak 0 = only silence was lost), `sd_khz`,
  `chunk_samples`, `fifo_me_samples` / `fifo_others_samples`, heap figures at start-up, `drain_max_ms`,
  `writer_late_max_ms` (large with fast writes = the writer task was starved, not the card) and up to 32
  `slow_sec=` lines (seconds with a slow write, late wake-up, a filling FIFO or any loss).

Verified 2026-10-15 with a 26 s session: the host tone came back bit-exact (amplitude 1599 of 1600), speech
in the microphone file lined up with it, no dropped samples. Not yet verified: long sessions (SD stalls,
USB/I2S clock drift), a real Meet/Zoom call.

## Keyboard (M4)
- `keyboard.cpp` reads the TCA8418 (0x34) on the codec's I2C bus; the espp keyboard driver is not used because it opens the same I2C port itself. Decoding, key map, actions and auto-repeat are in `keys.hpp` (host test `test/keys_test.cpp`).
- `Tab`: status / debug view (an error screen always returns to status). `+` / `=` and `-`: headphone level 0..10, 3 dB per step below the host volume (`volume.hpp`, host test `test/volume_test.cpp`), auto-repeat on hold, 1.5 s overlay, stored in NVS after the overlay has gone. Level 0 mutes playback only; ME and OT recordings are unaffected.
- `F` is decoded and logged, file mode is not implemented yet. Not done: the 1 kHz volume tick, the `HOST MUTE` overlay.

## Free space, battery and emergency states (M4)
- The card is mounted at boot and polled every 2 s while idle: inserting or pulling it switches `NO SD CARD` / READY by itself. Free space comes from FATFS (`esp_vfs_fat_info`, cached after the first query) and is shown as recording time left (192 000 bytes per second for both streams); while recording it is checked every 5 s.
- Below 15 minutes the status row shows `SD < 15 MIN` (orange border); below 5 minutes the recording stops cleanly and the `SD FULL` screen waits for G0.
- Battery: GPIO10 through a 2:1 divider (espp `battery_voltage()`), 8 reads averaged every 5 s, percentage from the 1S Li-ion curve in `power_policy.hpp`. A voltage that is not a battery's (below 2.5 V or above 4.6 V) hides the field. `BATT LOW` at 15% or less; at 5% or less for three readings in a row the recording stops cleanly (`BATTERY LOW` screen). **Unverified on hardware: whether this pin and divider are right on the ADV.**
- A write error ends the recording with the files closed; if the card no longer answers (CMD13) the screen is `CARD REMOVED`, otherwise `SD WRITE ERROR`. Error screens stay until G0 is pressed; G0 does not start a recording from them.
- The reason of the stop is in `R<nnnn>.TXT` as `stop_reason=normal|write_error|sd_full|battery`.
- Host tests: `test/power_policy_test.cpp` (space levels, battery curve, stop confirmation).


## File mode (M4, R4.5 / R4.10)

`F` in READY (card present) restarts the device as a USB flash drive: the SD card appears on the computer and
nothing else runs (no recording, no headset). Leave with `F`, G0, or by ejecting the disk on the computer; the
device restarts into the normal mode. Mode switching is a restart on purpose (one owner of the card, see
`specs/M4-device-ux.md`, R4.5). Implementation: `main/file_mode.cpp` (RTC flag, mass-storage callbacks over raw
`sdmmc` sectors, descriptors swapped with the linker's `--wrap`), the TinyUSB MSC class is enabled in the top
`CMakeLists.txt`. The free-space field shows `--` in file mode.
