# firmware: USB audio test (Cardputer ADV)

Step 1 of the USB-headset approach (see `../INTENT.md`): the device enumerates as a
USB audio device. The screen shows CONNECTED / DISCONNECTED and, while the host plays
audio to it, the waveform of that stream. The mic stream is silence for now.

## Status

Builds cleanly with ESP-IDF v5.4 (first build, 2026-10-15). **Not run on hardware yet.**
Behaviour on the device is unverified; in particular:

- `espp::M5StackCardputer::get()`, `initialize_lcd()`, `write_lcd_frame()` from
  `espp/m5stack-cardputer` - check names and that `write_lcd_frame` works without
  `initialize_display()`; check RGB565 byte order if colours look wrong.
- `uac_device_init()` and its callbacks from `espressif/usb_device_uac`. Channel count and
  sample rate are pinned in `sdkconfig.defaults` (1 ch, 48 kHz, 16-bit) and read from
  `CONFIG_UAC_*` in `main/config.hpp`.
- `tud_mounted()` from TinyUSB as the CONNECTED signal.

## Build

Requires ESP-IDF >= 5.1 (tested with v5.4: `. ~/Work/esp/esp-idf/export.sh`).

    cd firmware
    idf.py set-target esp32s3
    idf.py menuconfig        # USB Device UAC: set speaker/mic channels and sample rate
    idf.py build flash monitor

If flashing fails because USB is now owned by the UAC device, hold G0 while connecting
to enter download mode.

## Next steps

2. Real microphone to the host (`on_mic_data` <- `initialize_microphone`).
3. Speaker output to the 3.5 mm jack (`play_audio`).
4. Record both streams to SD with timestamps.
