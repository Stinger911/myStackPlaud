# stt: recordings -> speaker-labelled transcript

`transcribe.py` turns the two WAV files of a myPlaud session into one Markdown note.

    R0004ME.WAV  (microphone, "Я")      \
                                          > per-stream ASR -> merge by time -> "2026-10-15 R0004.md"
    R0004OT.WAV  (host audio, "Остальные")/

The speaker label comes from which file a phrase is in, so no diarization is needed. Standard library
only; external tools: `ffmpeg` and `whisper-cli` (whisper.cpp).

## Setup (once)

    brew install whisper-cpp                                   # provides whisper-cli (Metal on Apple silicon)
    python3 stt/transcribe.py --download-model large-v3-turbo-q5_0   # ~570 MB into ~/.cache/myplaud/models
    python3 stt/transcribe.py --download-model silero-v5.1.2         # ~1 MB voice-activity model (recommended)

## Use

    python3 stt/transcribe.py              # newest session on the SD card (/Volumes/M5CARD/MYPLAUD)
    python3 stt/transcribe.py R0004        # a given session
    python3 stt/transcribe.py R0004 --lang ru --out ~/Documents/notes

The note goes to `~/Library/Mobile Documents/com~apple~CloudDocs/myPlaud/` (iCloud Drive, readable on the
phone); change it with `--out` or `MYPLAUD_OUT`. The device has no clock, so the note date is today's
(`--date` to override). `--backend stub` skips the ASR and prints a placeholder per detected speech
region: it exercises everything else and needs no model.

## How it works

1. Per stream, the level of every 100 ms frame is measured; the speech threshold is the 10th percentile
   (the room noise) plus 8 dB (4 dB when the VAD model is present), bounded to -65...-35 dBFS. Frames above it become speech regions.
2. Each stream is high-passed, loudness-normalised, resampled to 16 kHz and given to `whisper-cli`
   (language auto-detect unless `--lang`).
3. Each speech region (merged into pieces of at most 28 s) is cut out and all pieces go to one
   `whisper-cli` run, so timestamps are anchored to real speech and the language is detected per piece.
   With the Silero VAD model present, tones, key clicks and room noise are not decoded at all; without it
   Whisper invents text on non-speech ("Thank you.", "Okay."). Text without a letter or digit and
   segments outside detected speech are dropped as well. `--vad-threshold` (default 0.15) trades missing
   quiet speech against letting noise through.
4. Segments of the two streams are merged by start time; consecutive segments of one speaker within 2 s
   are joined into one turn.

## Limits (known)

- Tried on a handful of short Russian recordings only (large-v3-turbo q5, Apple silicon: about 18 s for
  26 s of two-stream audio). Not tried on Polish, English or mixed speech, nor on a real call.
  Unit tests: `python3 -m unittest discover -s stt/tests`.
- The language is detected per piece (up to 28 s), so mixing inside one piece will be uneven.
- Streams are aligned to one 10 ms block on the device; acoustic leakage of the host audio into the
  microphone (speaker instead of headphones) can produce duplicate lines in the "me" stream.
- Remote ASR fallback (see `INTENT.md`) is not implemented yet.
