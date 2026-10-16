# M1. Reliable long recordings

Status: draft, 2026-10-16

## Goal
A meeting of 1-2 hours is recorded to SD with no lost samples, and the two files stay aligned in wall-clock time, so transcript timestamps and speaker order are trustworthy.

## Evidence (why)
Real Zoom call R0007 (32 min): `dropped_samples=551360` (~11.5 s), `me_samples=94122400`, `others_samples=94216800` (lengths differ by ~2 s), `t0_me_us`/`t0_others_us` differ by 163 us. Short tests (< 1 min) had 0 drops.

## Current design (relevant facts)
- Both streams are fed from the I2S tasks into two `SampleFifo<16384>` (~340 ms each) in `firmware/main/recorder.cpp`; one writer task (stack 6144, priority 4) drains in 4 KiB chunks to FAT on SPI3 at 20 MHz.
- On FIFO overflow `feed()` discards the new samples and counts them in `s_drops`; the file just becomes shorter (time shifts).
- Every 2 s the WAV headers of both files are rewritten and `fsync`ed. An SD stall (FAT update, card garbage collection) longer than ~340 ms overflows the FIFO. The header sync is the first suspect.

## Requirements
1. **R1.1 Diagnose.** Record per session: max SD write latency, max `fsync` latency, FIFO high-water mark per stream, and drop events (time since start, length). Write them to `R<nnnn>.TXT`; show max latency and drop count on the HUD.
2. **R1.2 Timeline integrity.** When samples are lost, write the same number of zero samples at the right position, so that each WAV length equals elapsed wall time within 100 ms and ME/OT stay aligned. `dropped_samples` still counts them; `.TXT` lists drop positions.
3. **R1.3 Buffering.** FIFO capacity covers at least 2 s per stream (use PSRAM if the board has it, otherwise the largest internal buffer that leaves heap for USB and screen). Capacity is a Kconfig value.
4. **R1.4 Fewer stalls.** Reduce the cause found in R1.1. Candidate measures, to be applied by evidence: less frequent header sync (every 10-30 s), a larger write block (8-16 KiB), pre-allocating (growing in big steps) the files, a lower-priority/higher-priority split between writer and the audio tasks, a different SD card or FAT cluster size.
5. **R1.5 Clock drift.** Measure USB vs. I2S drift over 60 min as `OT length - wall time` and `ME length - wall time`. If either exceeds 0.5 s per hour, add correction (drop or duplicate one sample every N); otherwise document the number only.
6. **R1.6 Power loss.** A cable pull or power loss during a session leaves both WAV files playable, losing at most the last header interval.
7. **R1.7 Stop behaviour.** Clean stop (G0) always writes final headers and `.TXT`, also when the card is slow.

## Acceptance tests
- A1: 60-min Zoom/Meet call via Launcher-started image: `dropped_samples=0` and no drop events in `.TXT`; ME and OT lengths each within 100 ms of wall time (measured with a stopwatch log on the Mac) and of each other.
- A2: Same test with a deliberately slow card or SD stress (copy a large file to the card meanwhile is not possible; use a card known to stall): drops, if any, are zero-filled; transcript timestamps of an injected beep every 5 min stay within 0.5 s of real time.
- A3: Cable pull at minute 5: both files open in `ffmpeg`, length >= elapsed - 2 header intervals.
- A4: `python3 -m unittest discover -s stt/tests` still passes; the existing `.TXT` format only gains keys (older sessions still parse).

## Out of scope
Compression, encryption, Wi-Fi upload.

## Open questions
- Does this Cardputer ADV have PSRAM that the build can use (check `CONFIG_SPIRAM` and board variant)?
- Is the stall the card (garbage collection) or the FAT layer? R1.1 decides.
