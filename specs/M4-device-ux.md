# M4. Device UX

Status: draft v2, 2026-10-16 (screen design agreed; hardware-dependent items marked open)

## Goal
The screen tells the truth about what the device is doing, and the device fails visibly instead of silently.

## Design principles
1. **The screen tells the truth.** `REC` is shown only while a file is actually being written.
2. **Readable in one second from half a metre.** The main state is carried by the border colour plus one large word or number. Colour is never the only carrier: every state also has a word.
3. **Errors are louder than success.** A problem recolours the border/header red or orange and stays until acknowledged.
4. **A meeting cannot be lost by accident.** Starting is light, stopping is deliberate (hold G0).
5. **Minimal input.** G0 covers the main use; the keyboard is for secondary actions only.
6. **No menus or settings** (see Out of scope).

## Hardware constraints for the UI
| Item | Value | Consequence |
|---|---|---|
| Screen | 240x135, ST7789, landscape, colours inverted, backlight GPIO38 | Frame is sent in strips (32 KB DMA limit) |
| Fonts | Monospace sizes: 11 / 13 px (header, footer, labels), 16 px (state), 24-32 px (READY, messages), 40 px (elapsed time) | At most ~6 text lines; ~30 characters per line at 13 px |
| Colours | RGB565: black, white, green, red, cyan, grey (as in `screen.cpp`) plus orange (warnings), blue (BT), violet (file mode) | Verify on the real inverted panel |
| Input | G0 (BOOT) and the 56-key keyboard | G0 = main action; keys = secondary |
| Audio out | Speaker and 3.5 mm via ES8311 | Beeps and ticks go to the DAC only, never into the ME stream |
| Port | One USB-C for power, audio, files; no console | Diagnostics go to the screen and the session `.TXT`, not to a log |

## Controls
| Action | Input | Works in | Feedback |
|---|---|---|---|
| Start recording | G0, short press | READY | Beep 880 Hz / 80 ms, green screen flash 150 ms |
| Stop recording | G0, **hold 1 s** (decided 2026-10-16) | Recording | Progress ring and `HOLD TO STOP`; on completion two beeps (880 -> 440 Hz) and a white flash |
| Cancel stop | Release before 1 s | Recording | Ring rolls back, recording continues |
| Short G0 press while recording | - | Recording | No action; the status row briefly shows `HOLD TO STOP` |
| Status / Debug view | `Tab` | Any | Immediate |
| Headphone volume | `+` (`=`) and `-` | Any except FILES | `VOL` overlay for 1.5 s, short tick at the new level |
| File mode on/off | `F` | READY, FILES | Violet border, beep 660 Hz |
| Acknowledge error | G0, short press | Error screens (not while recording) | Returns to READY |

Stop-by-hold replaces the current "short press toggles" behaviour: the stop takes 1 s longer, but a touch in a pocket cannot end a recording.

## Screen layout
```
240 px
+----------------------------------------------+ -+
| * USB AUDIO                 [==] 82%         |  | Header 26 px (13 px text)
+----------------------------------------------+ -+
| * REC                          SOUND         |  | Status row (16 / 13 px)
|                                              |  |
|                0:32:14                       |  | Elapsed time 40 px
| ME ||||||.....   OT ||||||||..               |  | Levels (labels 11 px)
+----------------------------------------------+ -+
| R0008        SD 3h 40m           DROP 0      |  | Footer 26 px (13 px text)
+----------------------------------------------+ -+
                                                  135 px
```
Three zones keep fixed places: header = link and power, centre = recording, footer = resources.

### Header: link indicator and battery
One indicator for the host link. The modes are mutually exclusive:

| Mode | Text | Dot | Meaning |
|---|---|---|---|
| USB headset | `USB AUDIO` | green | Host sees a speaker and microphone; recording possible |
| Bluetooth (future) | `BT AUDIO` | blue | Reserved. ESP32-S3 is BLE-only, so unavailable on this hardware |
| File transfer | `USB FILES` | violet | SD card exposed to the computer as a disk (USB mass storage) |
| No link | `LINK OFF` | grey ring | No host; "disconnected" is spelled out, not an absent icon |

Battery: icon and percentage, orange below 15%. If the PMIC/ADC is not readable the field is hidden, not faked.

### Status row and elapsed time
- Left: `REC` with a dot blinking at 1 Hz; while G0 is held a ring and `HOLD TO STOP`.
- Right: `SOUND` (cyan) / `SILENCE` (grey, OT below threshold for more than 3 s) / `NO HOST` (grey, no link). During a warning the same slot shows a short text instead: `SD < 15 MIN`, `BATT LOW`, `DROPPED`.
- Elapsed time: 40 px, `M:SS` below one hour, `H:MM:SS` after.
- Levels ME / OT: 10 segments, dBFS scale, fast attack and slow decay, last segment red on clipping. In READY both are shown idle.

### Footer: resources
Session number (`R0008`), remaining space as recording time (`SD 3h 40m`; two 48 kHz s16 streams are ~11.5 MB per minute), `DROP N` (red when N > 0). In file mode: free space and `MOUNTED`.

## States
```
 BOOT -> READY --G0--> RECORDING --hold G0--> SAVING -> READY
           |  ^             |  ^                          ^
         F |  | F           |  +- DROP (stay in RECORDING)
           v  |             v
         FILES        SD_ERROR / CARD_LOST / LOW_SPACE / LOW_BATT -> SAVING (emergency stop)
           NO_SD (READY blocked)
```

| # | State | Border / header | Centre | Sound | Entered when |
|---|---|---|---|---|---|
| S0 | **FILES** | Violet border | `FILES`, "SD is a disk on the computer", "eject there, then F" | beep 660 Hz | `F` in READY |
| S1 | **READY** | Green border | `READY`, `G0 = REC`, idle levels | - | Card present, space available |
| S2 | **RECORDING** | Red border, blinking dot | `REC`, elapsed time, levels | start beep | G0 short press |
| S3 | **STOPPING** | Red border | Hold ring, `HOLD TO STOP` | - | G0 held |
| S4 | **SAVING** | Grey border | `SAVING...` (up to 1-2 s, `.TXT` written) | stop beeps | Hold completed |
| S5 | **NO SD** | Red header | `NO SD CARD`, `INSERT CARD`, start disabled | 3 short | No card in READY |
| S6 | **SD ERROR** | Red border and header | `SD WRITE ERROR`, `RECORDING STOPPED`, `G0 = OK` | 1 s tone | Write error; files closed valid first |
| S7 | **LOW SPACE** | Orange border | `SD < 15 MIN`; below 5 min stops: `STOPPING: SD FULL` | 2 beeps at 15 min; tone on stop | R4.2 threshold |
| S8 | **DROP** | Red border blinking 2 s | Recording continues, `DROPPED`, `DROP N` red | 1 quiet beep, first drop of the session | M1 reports a drop |
| S9 | **CARD LOST** | Red border and header | `CARD REMOVED`, `RECORDING STOPPED` | 1 s tone | Card removed while recording |
| S10 | **LOW BATTERY** | Orange, then red | `BATT LOW`; at the stop threshold `STOPPING: BATTERY` | 2 beeps at 15%; tone on stop | PMIC/ADC readable |
| S11 | **LINK LOST** | Header `LINK OFF` (grey ring) | Recording **continues** (mic is written), status row `NO HOST`, OT shown grey | - | Host disconnected |

Priority: emergency stops (S6, S9, S10, S7 below 5 min) > warnings (S7, S8) > normal states. Any emergency first closes the files validly, then shows the error; the error screen stays until G0 is pressed. S11 is not an error: dictation and doctor-visit scenarios (M5) record ME only.

## Secondary view: Debug (`Tab`)
The current HUD moves here: `F/A/M/U` counters, waveform of the host stream, max SD write latency and ring high-water mark (from M1), temperature and voltage. Does not change recording state. `Tab` returns to Status; an error automatically switches back to Status.

## Headphone volume
- 11 levels: 0 (`MUTE`) to 10. One step per press of `+` / `-`, auto-repeat on hold.
- **Ceiling unchanged:** level 10 equals the current DAC cap of -20 dB; the control never goes above it. Steps are logarithmic (about 3 dB per step) so each press feels equal.
- **With host volume:** effective level = host volume x device level. Host mute has priority and the overlay shows `HOST MUTE`.
- **Overlay:** for 1.5 s it replaces the ME/OT levels row (`VOL`, 10 segments, number; at 0 an orange `MUTE`), then disappears. Header and elapsed time stay visible. Works in READY too.
- **Feedback:** 1 kHz tick at the new level (silent at 0), sent to the DAC only, never into the ME stream.
- Stored in NVS and restored after reboot. Does not affect the recordings: ME and OT are written without volume processing.
- In FILES there is no audio; the keys do nothing.

## File mode (USB flash drive)
- Entered with `F` from READY; left with `F` or by ejecting the disk on the computer. Not available while recording.
- While active the SD card belongs to the host: no recording, no USB audio (one USB-C port, hence the modes are exclusive). G0 and volume keys do nothing.
- Before handing the card over the firmware flushes and unmounts its FAT; on return it remounts and re-scans the session numbers.
- Screen: violet border, header `USB FILES`, centre `FILES`, footer free space and `MOUNTED`.
- Purpose: collect recordings without taking the card out of the Cardputer.

## Sounds
| Event | Pattern |
|---|---|
| Start | 1 x 880 Hz, 80 ms |
| Stop | 880 Hz then 440 Hz, 80 ms each |
| Warning (space, battery, drop) | 2 x 660 Hz, 60 ms each |
| Error / emergency stop | 1 x 330 Hz, 1 s |
| Volume tick | 1 x 1 kHz, short |

All sounds are capped by the same -20 dB ceiling as playback. If a beep cannot be kept out of the ME stream, it falls back to visual-only feedback.

## Requirements
1. **R4.1 Status screen.** Layout, link indicator, status row, elapsed time, levels and footer as above: link mode, host audio present (sound / silence from the OT level), REC state with elapsed time (`M:SS` or `H:MM:SS`), free SD space as remaining time, drop warning (red) when M1 reports a drop. Debug HUD under `Tab`.
2. **R4.2 Warnings.** No card, card full (< 5 min left stops with a clear message), write error, card removed, low battery (stop cleanly at a threshold read from the PMIC/ADC if available, else document as not possible).
3. **R4.3 Start/stop feedback.** Beep and screen flash on start and stop (see Controls and Sounds). **Done 2026-10-19, verified on R0014.** The start beep plays before the recording begins (the recording starts 120 ms after the press) and the stop beep after the files are closed, so the microphone never hears either; R0014 shows no 880/440 Hz trace in ME or OT. The error tone (330 Hz, 1 s) plays after the files are closed. Volume ticks and warning beeps are silent while recording (visual only) until `MYPLAUD_BEEPS_WHILE_RECORDING` is shown not to leak into ME; they play when nothing is recording.
4. **R4.4 Power.** Determine whether the battery charges while the USB audio device is active, and runtime on battery with recording; document in `firmware/README.md`. **Charging confirmed 2026-10-19:** the battery charges while the USB audio device is active (user observation on the hardware). Runtime on battery with recording is still not measured.
5. **R4.5 Composite device (investigation).** Feasibility of audio + serial console on the single USB-C port, and of audio + mass storage (file mode) as one composite device versus restarting the USB stack on mode switch; result and decision recorded, implement only if cheap.
   **Decision (2026-10-19): restart per mode.** One image, two USB personalities, chosen at boot. `F` stores a flag in RTC memory and restarts; the file-mode boot presents a single mass-storage interface (the audio descriptors are swapped for it at link time with `--wrap`, `main/file_mode.cpp`) and never starts the recorder, the headset or the FAT mount, so the card has exactly one owner. Leaving (`F`, G0, or an eject on the computer) restarts into the normal mode. Why not composite: the computer would see a disk during recording, and two owners of one FAT is the failure we cannot afford; a composite device also needs the component's descriptors and TinyUSB configuration replaced by our own. Why not a USB stack restart without a reboot: `usb_device_uac` has no deinit. The cost is a 2-3 s boot per switch, which is fine for a rare action. Audio plus a serial console on the one port: not done (the console shares GPIO43 with the codec's I2S, see `sdkconfig.defaults`); diagnostics stay on the screen and in the session `.TXT`.
6. **R4.6 Card housekeeping.** `stt/` or a small tool lists sessions on the card, flags empty ones (R0001*, R0003*) and offers deletion with confirmation; old `myPlaud_screen_test.bin` removed from `/downloads` (with confirmation).
7. **R4.7 Stop by hold.** G0 starts with a short press; stopping needs a 1 s hold with a progress ring; a short press during recording shows `HOLD TO STOP`.
8. **R4.8 Link indicator.** One mutually exclusive indicator: `USB AUDIO` / `BT AUDIO` (reserved) / `USB FILES` / `LINK OFF`.
9. **R4.9 Headphone volume.** `+`/`-` keys, 11 levels, ceiling at the current DAC cap, overlay, NVS persistence, no effect on recordings.
10. **R4.10 File mode.** `F` toggles USB mass storage for the SD card from READY, with safe unmount and remount. **Implemented 2026-10-19** (raw sector access to the card over USB mass storage; free space is not shown in FILES because the FAT is not mounted there; the footer shows `--`). Verified on hardware 2026-10-19 (disk appears on the Mac, files copy, eject returns to the normal mode).

## Implementation order
1. Refactor `screen.cpp` into zone drawing functions (header / centre / footer) with a state table; debug view under `Tab`. Needs fonts for 11/13/16/24/32/40 px.
2. Data sources: ME/OT levels (already in the audio tasks), SD free space, battery (check the PMIC), `drops` (from M1).
3. Normal states S1-S4 with beeps; stop by hold (R4.7). **Done** (beeps and flash: R4.3).
4. Emergency states S5-S10 with fixtures: remove the card, fill the card, pull USB. **Implemented (free space, battery, S5-S7, S9, S10); not yet exercised on hardware.** The error tone of these states is in (R4.3).
5. Volume control (R4.9) after checking how it combines with host volume in `audio.cpp`. **Keyboard, `Tab` and volume keys with overlay and NVS are in** (own TCA8418 driver on the codec's I2C bus); the confirmation tick and the `HOST MUTE` overlay are not; `F` is read but does nothing until file mode exists.
6. Link indicator (R4.8) and file mode (R4.10) after the R4.5 decision.
7. Photograph or describe every state in `firmware/README.md` (A1).

## Acceptance tests
- A1: each state in R4.1/R4.2 reproduced on the device and photographed or described in `firmware/README.md`.
- A2: 2-h run on battery and on USB power; results documented.
- A3: housekeeping tool dry-run lists exactly the empty sessions; nothing is deleted without confirmation.
- A4: a short G0 press during recording does not stop it; a 1 s hold does; releasing earlier cancels.
- A5: volume steps change the headphone level, never exceed the -20 dB ceiling, persist across reboot, and leave the ME/OT recordings unchanged (compare the files with volume at 0 and 10).
- A6: file mode: the card appears on the Mac as a disk, files copy intact, after eject and `F` the next session continues the numbering and records normally.
- A7: all four link modes render correctly, including `LINK OFF` when the cable is pulled mid-recording (recording continues).

## Open questions
- Is the battery readable on the ADV (PMIC/ADC)? If not, the battery field is hidden and S10 is not implementable.
- Composite USB (audio + disk) or a USB restart per mode (R4.5).
- Does the DAC volume combine with host volume without becoming too quiet at low levels? Check in `audio.cpp`.
- Is an ME-only dictation mode a separate on-screen state already in M4, or part of M5?
- Colours on the inverted panel: the palette was drawn in a browser prototype and must be checked on the real screen.
- Meeting title or label: not planned (no text input before recording; against principle 5).

## Out of scope
Settings menus, Wi-Fi, an on-device file browser (Launcher already has one), BT audio itself (only the indicator slot is reserved).

## References
- Interactive prototype of the screens (Artifact, session 2026-10-16).
- Vault design note: `lab18/AI-Zone/myPlaud-UX-design-2026-10-16.md`.
