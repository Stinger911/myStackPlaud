# M4. Device UX

Status: draft, 2026-10-16

## Goal
The screen tells the truth about what the device is doing, and the device fails visibly instead of silently.

## Requirements
1. **R4.1 Status screen.** Show: USB connected, host audio present (sound / silence from the OT level), REC state with elapsed time (mm:ss or h:mm:ss), free SD space as remaining minutes, drop warning (red) when M1 reports a drop. The waveform and debug counters become a secondary view toggled by a key.
2. **R4.2 Warnings.** No card, card full (< 5 min left stops with a clear message), write error, low battery (stop cleanly at a threshold read from the PMIC/ADC if available, else document as not possible).
3. **R4.3 Start/stop feedback.** A short beep or screen flash on start/stop of recording so it is clear without looking.
4. **R4.4 Power.** Determine whether the battery charges while the USB audio device is active, and runtime on battery with recording; document in `firmware/README.md`.
5. **R4.5 Composite device (investigation).** Feasibility of audio + serial console on the single USB-C port; result and decision recorded, implement only if cheap.
6. **R4.6 Card housekeeping.** `stt/` or a small tool lists sessions on the card, flags empty ones (R0001*, R0003*) and offers deletion with confirmation; old `myPlaud_screen_test.bin` removed from `/downloads` (with confirmation).

## Acceptance tests
- A1: each state in R4.1/R4.2 reproduced on the device and photographed or described in `firmware/README.md`.
- A2: 2-h run on battery and on USB power; results documented.
- A3: housekeeping tool dry-run lists exactly the empty sessions; nothing is deleted without confirmation.

## Out of scope
Settings menus, Wi-Fi, a file browser (Launcher already has one).
