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
  The microphone stream to the host is still silence.
- HUD (top line) shows `F` frames, `A` speaker callbacks, `M` mic callbacks. A watchdog restarts the
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
