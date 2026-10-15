# Intent: myPlaud — personal recorder with speaker-labelled transcripts

- Author: Andrey Abramov
- Status: draft
- Date: 2026-10-15

## Problem
Recording from a phone is inconvenient for the situations that matter most: phone calls, and Meet/Zoom meetings where I wear headphones. I cannot get a clean, searchable record of what was said, and existing recorders (Plaud, my own `MyPlaud/ots.py` pipeline) do not fit this workflow.

## Proposed outcome
A pocket device (M5Stack Cardputer ADV) that records my voice on demand, combined with a computer-side recording of the other participants, producing a text transcript with speakers labelled. In the MVP the two sources are separate streams: the device mic is "me", the computer capture is "others". The transcript is a plain file available from both computer and phone (iCloud Drive), and can be dropped into the Obsidian vault.

## Users
Only me (single user). Publishing to M5Stack Launcher Hub is possible in theory but has no assessed value and is out of scope.

## Scenarios
1. Meet/Zoom meetings (I use headphones).
2. Phone calls.
3. Dictating notes on the go.
4. Doctor visits.

## Success (MVP)
A Meet/Zoom meeting becomes a text transcript with speaker labels, stored in iCloud, readable on computer and phone. Phone calls are not covered by the MVP.

## Scope
- In: Cardputer ADV firmware (record my voice with timestamps, store on SD); computer-side capture of the other participants (system audio on the Mac, e.g. via a loopback driver); merging the two streams by time; transfer to computer via SD card (baseline), Wi-Fi and/or BLE (later); speech-to-text with speaker diarization; output file in iCloud.
- Later / optional: summary; automatic Obsidian note; phone-call capture; A2DP/HFP sink on a classic-ESP32 device (Core2 etc.) as a second stream source.
- Out: mobile app, multi-user, publishing, anything beyond personal use.

## Constraints
- Hardware: Cardputer ADV (ESP32-S3: BLE only, no Bluetooth Classic, so no A2DP/HFP at the hardware level; custom firmware cannot add it). Hence the MVP captures the other side on the computer, not on the device.
- Speech-to-text: local model preferred; remote model as fallback. Candidates: Whisper (whisper.cpp / faster-whisper) or Google Gemma (audio-capable variant, e.g. Gemma 3n — to be verified).
- Languages: Russian, Polish, English, including mixed speech within one recording.
- Privacy: personal use only; recordings are not published or shared; consent of other parties is my responsibility.
- Output must be a file reachable from computer and phone (iCloud).

## Open questions
- Decided for MVP: other participants are captured on the Mac (system audio), my voice on the device. Open: how to align the two streams in time (clock sync over Wi-Fi/NTP, start marker, or cross-correlation) and which loopback tool to use.
- Phone calls: no capture path yet (A2DP carries media, calls use HFP). Candidates: classic-ESP32 device, external BT receiver into line-in, or speakerphone.
- Does the Cardputer ADV have a usable line-in / what are the real mic and storage limits?
- Recording format (WAV/PCM 16 kHz mono vs compressed) and SD capacity.
- Transfer path to the Mac: SD card vs Wi-Fi upload vs BLE.
- Diarization approach for local use (e.g. pyannote or equivalent) and whether Whisper or Gemma handles RU/PL/EN mixed speech better — compare on real recordings.
