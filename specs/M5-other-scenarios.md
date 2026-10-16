# M5. Other scenarios

Status: draft, 2026-10-16

## Goal
Extend beyond the Mac meeting: phone as host, recording without a host, remote ASR fallback.

## Requirements
1. **R5.1 Phone as host.** Test an Android phone and an iPhone with the needed adapter: enumeration, call audio both ways, volume/mute behaviour, whether the phone supplies power, and whether the 48 kHz single-channel format is accepted. Result table in `firmware/README.md`; fixes only if cheap (for example a 16 kHz or stereo descriptor variant behind Kconfig).
2. **R5.2 Mic-only mode.** When no USB host is mounted (or by a key), G0 records ME only (no OT file, `.TXT` says `mode=mic-only`), for dictation and doctor visits. `transcribe.py` handles a session with only ME and labels it with a single speaker.
3. **R5.3 Remote ASR fallback.** `--backend remote` sends audio pieces to a configured remote ASR service when the local model is unavailable or too slow; endpoint and key come from environment variables (keys are never read from the repo, never logged). Off by default; the transcript header states which backend produced it.
4. **R5.4 Line-in check.** Establish whether the ADV has a usable line-in or an alternative second audio source and what the real microphone limits are (noise floor, max level); record the result in `firmware/README.md`.

## Acceptance tests
- A1: a 5-min phone call recorded from a phone with both sides audible in separate files, or a documented reason it is impossible.
- A2: a 5-min dictation without a host transcribed into a single-speaker note.
- A3: `--backend remote` against a stub server produces the same Markdown schema; with no key set it fails with a clear message and no network call.

## Out of scope
Making the ESP32-S3 a Bluetooth headset (hardware cannot).

## Open questions
- Which remote ASR service, and what data may leave the machine?
