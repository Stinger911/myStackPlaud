# Plan: myPlaud

- Derived from: `INTENT.md` (draft, 2026-10-15)
- Status: draft, 2026-10-16
- Success (from intent): a Meet/Zoom meeting held through the device as USB headset becomes a speaker-labelled text transcript in iCloud, readable on computer and phone.

## Where we are

| Intent item | State |
|---|---|
| USB headset (UAC speaker + mic) on Mac | Done, works with `CONFIG_UAC_SUPPORT_MACOS=y` |
| Playback to headphones (3.5 mm / speaker) | Done, DAC capped at -20 dB, host volume/mute follow |
| Two clean streams recorded on SD | Done (`R<nnnn>ME/OT.WAV` + `.TXT`) |
| Local STT with speaker labels | Done for RU (`stt/transcribe.py`, whisper.cpp + Silero VAD) |
| Real Zoom call, 32 min (R0007) | Works end to end, but 11.5 s of samples dropped, ~30 min to transcribe |
| Runs from M5Stack Launcher | Works (image from SD `/downloads`) |
| Device screen UX (M4) | Designed (spec v2); screen refactor in progress |
| PL / EN / mixed speech | Not tested |
| Phone as host | Not tested |
| Remote ASR fallback, summary, Obsidian note | Not started |

## Milestones

### M1. Reliable long recordings (blocks MVP)
Goal: a 1-2 h meeting records with 0 dropped samples and aligned streams.
- Find the cause of drops in R0007 (SD write stalls vs. ring too small): log max write latency and ring high-water mark, show on HUD / in `.TXT`.
- Enlarge ring buffers (PSRAM if available), larger write blocks, pre-allocate file space, test a faster/other SD card.
- Make drops visible and harmless: on overflow insert silence for the dropped span so file length equals wall-clock time (timestamps stay valid); record drop count and positions in `.TXT`.
- Measure USB-vs-I2S clock drift over 1 h (compare OT length with wall time); add resampling/skip-insert only if drift is audible or shifts timing > 0.5 s.
- Acceptance: 60-min Zoom call, `dropped_samples=0`, ME/OT lengths within 100 ms, cable-pull still leaves a valid file.

### M2. Transcript quality and speed
Goal: usable transcript within minutes after the meeting, fewer fake lines.
- Stop "Я" hallucinations ("Thank you.", "Fire!"): echo/bleed gate (ignore ME speech while OT is loud and ME level is low), extend the phrase filter, raise VAD threshold for ME.
- Speed: R0007 took 30 min for 32 min of audio. Try parallel whisper runs, greedy decoding, smaller model for ME, skip long silent spans; target < 0.5x real time.
- Compare on the same recording: large-v3-turbo q5 vs. a Gemma audio model (INTENT open question).
- Test and tune on Polish, English and mixed RU/PL/EN speech (record short samples of each; add as test fixtures).
- Acceptance: R0007 transcribed in < 15 min, no isolated English filler lines in "Я", PL/EN sample reads correctly.

### M3. Hands-off workflow
Goal: insert card, get a transcript; no manual commands.
- Auto-run on card mount (launchd / folder watcher) for new sessions, skip already processed.
- Output to iCloud Drive `myPlaud/` (exists) plus optional copy into the Obsidian vault note.
- Optional summary (local or remote model) as a separate, off-by-default step.
- Acceptance: card inserted -> notification -> `.md` in iCloud within the M2 time budget.

### M4. Device UX
Spec: `specs/M4-device-ux.md` (v2, 2026-10-16, screen design agreed).
Goal: the screen tells the truth and the device fails visibly; one-glance state from half a metre.
- Screen redesign: header (one link indicator `USB AUDIO` / `BT AUDIO` (reserved) / `USB FILES` / `LINK OFF`, battery), status row (REC, SOUND/SILENCE/NO HOST, warnings), 40 px elapsed time, ME/OT levels, footer (session, SD time left, drops). Debug HUD moves under `Tab`.
- Stop by 1 s hold of G0 (short press only starts); short press while recording shows `HOLD TO STOP`.
- States S0-S11 incl. NO SD, SD ERROR, LOW SPACE, DROP, CARD LOST, LOW BATTERY, LINK LOST; emergency stops close files validly first.
- Beeps on start/stop/warnings (DAC only, never into ME); headphone volume with `+` / `-` (11 levels, ceiling at the current -20 dB cap, NVS).
- File mode (`F`): SD card exposed to the computer as a USB disk, from READY only.
- Power (battery while USB audio is active, runtime), composite USB feasibility (audio + serial, audio + disk), card housekeeping.
- Order (see spec): 1 screen refactor into zones + state model (started 2026-10-16), 2 data sources (levels, SD space, battery, drops), 3 normal states + stop by hold, 4 emergency states with fixtures, 5 volume, 6 link indicator + file mode after the composite-USB decision, 7 photos in `firmware/README.md`.
- Acceptance: A1-A7 in the spec.

### M5. Other scenarios
- Phone (Android/iPhone) as USB host for calls: adapter, enumeration, volume/mute behaviour.
- Dictation without host (mic only) and doctor-visit mode: record ME only, no USB needed.
- Remote ASR fallback when the local model is too slow or unavailable.
- Line-in / real mic limits of the ADV (intent open question).

## Order and dependencies
M1 first (the MVP is not trustworthy with drops). M2 and M3 can follow in parallel. M4 is small and can be mixed in. M5 only after M1-M3 pass on the Mac.

## Risks
- SD write latency on a given card may need a different card or FAT cluster size; mitigate with PSRAM buffer and pre-allocation.
- Local Whisper may be too slow on this Mac for long meetings; mitigate with parallelism or remote fallback.
- Echo of the host's audio into the microphone (speakers instead of headphones) corrupts the "me" stream; mitigate with the bleed gate and by recommending headphones.
- Phone support depends on the phone's UAC host behaviour and adapter.
- Recording others requires their consent (own responsibility, per intent).

## Out of scope (per intent)
Mobile app, multi-user, publishing to Launcher Hub, anything beyond personal use.
