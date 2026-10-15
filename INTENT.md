# Intent: myPlaud — personal recorder with speaker-labelled transcripts

- Author: Andrey Abramov
- Status: draft
- Date: 2026-10-15

## Problem
Recording from a phone is inconvenient for the situations that matter most: phone calls, and Meet/Zoom meetings where I wear headphones. I cannot get a clean, searchable record of what was said, and existing recorders (Plaud, my own `MyPlaud/ots.py` pipeline) do not fit this workflow.

## Proposed outcome
A pocket device (M5Stack Cardputer ADV) that records speech on demand and, after recording, produces a text transcript with speakers labelled. The transcript is a plain file available from both computer and phone (iCloud Drive), and can be dropped into the Obsidian vault.

## Users
Only me (single user). Publishing to M5Stack Launcher Hub is possible in theory but has no assessed value and is out of scope.

## Scenarios
1. Meet/Zoom meetings (I use headphones).
2. Phone calls.
3. Dictating notes on the go.
4. Doctor visits.

## Success (MVP)
A meeting recording becomes a text transcript with speaker labels, stored in iCloud, readable on computer and phone.

## Scope
- In: Cardputer ADV firmware (record, store on SD); transfer to computer via SD card (baseline), Wi-Fi and/or BLE (later); speech-to-text with speaker diarization; output file in iCloud.
- Later / optional: summary; automatic Obsidian note.
- Out: mobile app, multi-user, publishing, anything beyond personal use.

## Constraints
- Hardware: Cardputer ADV (ESP32-S3: BLE only, no Bluetooth Classic HFP/A2DP — it cannot act as an ordinary BT headset).
- Speech-to-text: local model preferred; remote model as fallback. Candidates: Whisper (whisper.cpp / faster-whisper) or Google Gemma (audio-capable variant, e.g. Gemma 3n — to be verified).
- Languages: Russian, Polish, English, including mixed speech within one recording.
- Privacy: personal use only; recordings are not published or shared; consent of other parties is my responsibility.
- Output must be a file reachable from computer and phone (iCloud).

## Open questions
- How to capture the other party when I wear headphones (Meet/Zoom, calls): ambient mic, wired inline split, or only my own voice plus a separate computer-side recording? To be decided on a prototype.
- Does the Cardputer ADV have a usable line-in / what are the real mic and storage limits?
- Recording format (WAV/PCM 16 kHz mono vs compressed) and SD capacity.
- Transfer path to the Mac: SD card vs Wi-Fi upload vs BLE.
- Diarization approach for local use (e.g. pyannote or equivalent) and whether Whisper or Gemma handles RU/PL/EN mixed speech better — compare on real recordings.
