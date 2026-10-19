# Intent: myPlaud Desktop — companion app for Mac, Windows and Linux

- Author: Andrey Abramov
- Status: draft
- Date: 2026-10-19
- Extends: `INTENT.md` (device and transcripts), supersedes the "launchd agent" idea in `specs/M3-automatic-workflow.md`

## Problem
Today the loop around the device is a set of shell commands: mount the card, run `stt/transcribe.py`, delete old sessions by hand, flash a new image by copying a file to the card. It works for me on one Mac, but it is easy to forget a step, it breaks when the card is not mounted, it needs Homebrew and Python, and it does not exist at all on Windows or Linux. The device now has a file mode (`F`: the SD card appears as a USB disk), so the computer can talk to it without taking the card out, but nothing uses that yet.

## Proposed outcome
A small desktop app (Mac, Windows, Linux) that sits in the tray, notices the device in file mode (or a mounted card), and does three things:

1. **Card housekeeping.** Lists sessions on the card (`MYPLAUD/R<nnnn>ME/OT.WAV` + `.TXT`) with duration, size, drops and whether a transcript exists. Flags empty or broken sessions, shows free space, and deletes or archives sessions only on explicit confirmation. Never writes to the card while the device records (file mode excludes recording, so this is safe by design).
2. **Transcription.** Runs the existing pipeline (`stt/transcribe.py` logic: per-stream ASR, merge by time, speaker labels from the file name) on new sessions and writes the Markdown note to the output folder (iCloud Drive `myPlaud/` by default; optional Obsidian vault folder). Progress, queue and retry are visible; a session is processed once.
3. **Updates.** Checks GitHub releases for a newer firmware image and a newer app version, shows what changed, downloads the image with a checksum, and puts it on the card in `downloads/` under a new name so the M5Stack Launcher can start it. Never overwrites an existing image and never installs anything without a click.

## Users
Only me (single user, single device). Cross-platform is a requirement of convenience and longevity, not of a user base.

## Scenarios
1. Plug the device in file mode: the app shows the sessions, I tick the new ones, it transcribes them and tells me where the notes are.
2. Insert the bare SD card into the computer: the same, without the device.
3. After a month: free space is low, the app lists old sessions that already have a transcript and offers to archive and delete them.
4. A new firmware release appears on GitHub: the app tells me, I confirm, the image lands on the card, I start it from the Launcher.

## Success (MVP)
On the Mac, from a clean install of the app: connect the device in file mode, click one button, and get the speaker-labelled note for a new session in iCloud, with the card left consistent and ejected safely. Windows and Linux builds do the same with the same sessions.

## Scope
- In: detection of the card/device and its mount point on three platforms; read-only listing and parsing of the session files; transcription through a local whisper.cpp backend with the model download managed by the app; output folder settings; deletion/archiving with confirmation; GitHub release check, download, SHA-256 verification, copy to the card; safe eject; tray icon and notifications; log file.
- Later / optional: summary step (off by default); Obsidian note with frontmatter; remote ASR fallback with a per-run opt-in; flashing the device over USB serial (`esptool`) without the Launcher; Wi-Fi transfer.
- Out: mobile app, multi-user, accounts, cloud sync of its own, telemetry, publishing, auto-install of updates, anything beyond personal use.

## Constraints
- Device side: file mode already exposes the card as a standard USB mass-storage disk with FAT; the app must not need a device protocol. If the device later gets a control channel (serial or HID), it is an extension, not a prerequisite.
- The card is the single source of truth: no database of the app is required for correctness; any app state (processed marker, settings) lives beside the output, not on the card.
- Privacy: recordings and transcripts stay on the machine; the only network calls are GitHub (release metadata and assets) and the one-time model download, both visible in the UI. Remote ASR is opt-in per run.
- Languages: Russian, Polish, English and mixed speech, as in the main intent.
- Reuse over rewrite: the transcription pipeline already exists in Python (`stt/`), tested on R0007; the app must not fork its logic.
- Packaging: one installable per platform, no Homebrew or system Python required from the user. `ffmpeg` and `whisper-cli` are bundled or fetched by the app.
- Never commit, push, publish or release without an explicit command (project rule); the update feature only reads releases.

## Open questions
- **Stack.** Tauri (small, Rust shell + web UI, can spawn the Python/whisper sidecar)
- **Transcription engine.** Keep the Python pipeline as a sidecar now, later port the pipeline to the app language (Rust)
- **Card detection.** Mac: `/Volumes/*/MYPLAUD`; Windows: drive letters with a `MYPLAUD` folder; Linux: `/run/media/$USER/*`, `/media/*`. Polling for volumes is enough now. Eject on Windows/Linux without admin rights.
- **Updates.** Releases live on GitHub repo is public https://github.com/Stinger911/myStackPlaud.git. Which is asset naming, checksum file, signature? How does the app learn the installed firmware version (a `VERSION` file the device writes to the card, or the image name)?
- **Deletion policy.** Archive to a local folder first, then delete from the card. What counts as "empty" (< 2 s, as in M3)?
- **Models.** Which model for which hardware (large-v3-turbo q5 is 570 MB and slow on CPU-only Windows/Linux machines); is GPU support (CUDA/Vulkan) needed at all for the first release?
- **Signing and distribution.** Unsigned builds for personal use are acceptable on Mac (right-click open) and Linux; Windows SmartScreen warnings are tolerated. Confirm.
- **Relation to M3.** The app replaces the launchd agent: confirm that the agent is dropped, and that the CLI `stt/transcribe.py` stays as the engine and as a scriptable fallback.
