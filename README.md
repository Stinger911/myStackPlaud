# myPlaud

A personal recorder with speaker-labelled transcripts, built on the M5Stack Cardputer ADV (ESP32-S3).

The device sits between a computer and a headset. To the host it is a USB headset (speaker + microphone). It plays the host's audio to the headphones, records two separate streams to a microSD card, and a local pipeline turns them into one Markdown transcript:

    host audio (the other participants)  ->  R<nnnn>OT.WAV  \
                                                              > ASR per stream -> merge by time -> note.md
    device microphone (me)               ->  R<nnnn>ME.WAV  /

Which file a phrase comes from tells who said it, so no diarization is needed, and both streams share one clock.

Personal project, single user. Recording other people requires their consent, which is the user's responsibility.

## Status

| Part | State |
|---|---|
| USB headset on macOS, playback to headphones, two-stream recording to SD | Works |
| Reliable long recordings (FIFO sized to free RAM, drop accounting in `R<nnnn>.TXT`) | Works; 1 h+ session not yet measured |
| Device UX: screen states, hold-to-stop, volume keys, beeps and flash, battery, link indicator, file mode | Mostly done, see `specs/M4-device-ux.md` |
| Transcript for Russian speech (whisper.cpp + Silero VAD) | Works; Polish, English and mixed speech not tested |
| Desktop companion app (card housekeeping, transcription, updates) | Intent only: `INTENT-desktop.md` |
| Phone as host, ME-only dictation mode, remote ASR | Not started |

The detailed plan is in `PLAN.md`; requirements and acceptance tests are in `specs/`.

## Repository layout

| Path | What it is |
|---|---|
| `INTENT.md` | Problem, outcome, scope and open questions of the whole project |
| `INTENT-desktop.md` | Intent of the desktop companion app (draft) |
| `PLAN.md` | Milestones M1-M5 derived from the intent |
| `specs/` | One spec per milestone: requirements, design notes, acceptance tests |
| `firmware/` | ESP-IDF firmware for the Cardputer ADV; its own `README.md` has hardware notes and build variants |
| `stt/` | `transcribe.py`: sessions -> speaker-labelled Markdown; its own `README.md` has setup and options |
| `launcher-backup.bin` | Flash backup of the stock M5Stack Launcher (ignored by git) |

## Quick start

### Firmware

Requires ESP-IDF 5.4 (`. ~/Work/esp/esp-idf/export.sh`).

    cd firmware
    idf.py build                 # output: build/myplaud_usb_audio_test.bin

Flash over USB (`idf.py flash`, hold G0 while connecting if the port is taken by the UAC device), or copy the image to the card under `/downloads` and start it from the M5Stack Launcher. Host unit tests are in `firmware/test/`. Details, debug build variants and the board quirks: `firmware/README.md`.

### Use

1. Connect the device to the computer and select it as the sound input and output.
2. Short press of G0 starts a recording; a 1 s hold stops it. Files land in `/MYPLAUD` on the card.
3. Press `F` in the idle state to expose the card to the computer as a USB disk (file mode); `F` or G0 returns to recording mode.

### Transcripts

    brew install whisper-cpp
    python3 stt/transcribe.py --download-model large-v3-turbo-q5_0
    python3 stt/transcribe.py              # newest session on the card

The note goes to iCloud Drive `myPlaud/` by default. Full options: `stt/README.md`.

## Hardware notes

- Cardputer ADV (Stamp-S3A, ESP32-S3FN8, 8 MB flash, no PSRAM). The ESP32-S3 has no Bluetooth Classic, hence USB audio instead of A2DP/HFP.
- One USB-C port carries power, flashing and audio; the battery charges while USB audio is active.
- Audio is mono, 16-bit, 48 kHz. 192 000 bytes per second for both streams.

## Roadmap

Next: the remaining M4 items (runtime on battery, card housekeeping, state photos), a 1 h+ recording check, transcript quality on Polish, English and mixed speech (M2), and the desktop app (`INTENT-desktop.md`) that replaces the planned launchd agent of M3.

## Contributing and rules

Repository content is in English. Nothing is committed, pushed or released without an explicit command from the owner.
