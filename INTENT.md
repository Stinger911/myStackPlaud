# Intent: myPlaud — personal recorder with speaker-labelled transcripts

- Author: Andrey Abramov
- Status: draft
- Date: 2026-10-15

## Problem
Recording from a phone is inconvenient for the situations that matter most: phone calls, and Meet/Zoom meetings where I wear headphones. I cannot get a clean, searchable record of what was said, and existing recorders (Plaud, my own `MyPlaud/ots.py` pipeline) do not fit this workflow.

## Proposed outcome
A pocket device (M5Stack Cardputer ADV) that sits between the audio source (Mac or phone) and my headphones and acts as a USB headset. The host sends the other participants' audio to it as the USB "speaker"; the device records that stream ("others"), plays it to my headphones through its 3.5 mm jack, and records my voice from its own microphone ("me"), which it also sends back to the host as the USB "microphone". Two clean streams give speaker labels without diarization, with a common clock and no loopback driver on the Mac. The result is a labelled text transcript. The transcript is a plain file available from both computer and phone (iCloud Drive), and can be dropped into the Obsidian vault.

## Users
Only me (single user). Publishing to M5Stack Launcher Hub is possible in theory but has no assessed value and is out of scope.

## Scenarios
1. Meet/Zoom meetings (I use headphones).
2. Phone calls.
3. Dictating notes on the go.
4. Doctor visits.

## Success (MVP)
A Meet/Zoom meeting held through the device as USB headset becomes a text transcript with speaker labels, stored in iCloud, readable on computer and phone. Phone calls via a USB-connected phone are a stretch goal for the same mechanism.

## Scope
- In: Cardputer ADV firmware (record my voice with timestamps, store on SD); USB Audio Class device (speaker + microphone) with recording of both streams on the device; playback to headphones; transfer to computer via SD card (baseline), Wi-Fi and/or BLE (later); speech-to-text with speaker diarization; output file in iCloud.
- Later / optional: summary; automatic Obsidian note; phone-call capture over USB; fallback of capturing the other side on the Mac (system audio loopback) if USB audio proves unreliable.
- Out: mobile app, multi-user, publishing, anything beyond personal use.

## Constraints
- Hardware: Cardputer ADV (ESP32-S3: BLE only, no Bluetooth Classic, so no A2DP/HFP at the hardware level; custom firmware cannot add it). Hence the other side is received over USB audio (UAC) instead. The ESP32-S3 has native USB device mode; Espressif provides a TinyUSB-based UAC component (`usb_device_uac`), but it is less mature than the Bluetooth stacks. Arduino-ESP32 does not enable the TinyUSB audio class, so the firmware uses ESP-IDF. The ADV codec (ES8311) runs speaker and microphone in full duplex on one I2S bus at a shared sample rate.
- Speech-to-text: local model preferred; remote model as fallback. Candidates: Whisper (whisper.cpp / faster-whisper) or Google Gemma (audio-capable variant, e.g. Gemma 3n — to be verified).
- Languages: Russian, Polish, English, including mixed speech within one recording.
- Privacy: personal use only; recordings are not published or shared; consent of other parties is my responsibility.
- Output must be a file reachable from computer and phone (iCloud).

## Open questions
- Does the ADV enumerate reliably as a USB audio device and keep UAC/I2S clocks in sync (asynchronous feedback, no clicks or drift)? First test: `firmware/` shows CONNECTED/DISCONNECTED and the waveform of the stream the host plays to it.
- The single USB-C port carries power, flashing and audio. Can it be a composite device (audio + serial) and can the battery charge during use?
- Do phones (Android, iPhone) accept it as a USB headset for calls, and with what adapter?
- Fallback if USB audio fails: capture the other side on the Mac (system audio loopback), my voice on the device; align streams by time.
- Does the Cardputer ADV have a usable line-in / what are the real mic and storage limits?
- Recording format (WAV/PCM 16 kHz mono vs compressed) and SD capacity.
- Transfer path to the Mac: SD card vs Wi-Fi upload vs BLE.
- Diarization approach for local use (e.g. pyannote or equivalent) and whether Whisper or Gemma handles RU/PL/EN mixed speech better — compare on real recordings.
