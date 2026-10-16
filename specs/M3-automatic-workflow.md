# M3. Automatic workflow

Status: draft, 2026-10-16

## Goal
Insert the SD card into the Mac; a few minutes later the transcript is in iCloud Drive (and optionally an Obsidian note) without typing a command.

## Requirements
1. **R3.1 Trigger.** A launchd agent (or folder watcher) detects a mounted volume with a `MYPLAUD` directory and starts processing. Volume name is not hard-coded.
2. **R3.2 Idempotence.** Each session is processed once. State is a marker in the output directory (`Rnnnn.json` exists and is newer than the WAV files); `--force` reprocesses. Sessions with empty files (< 1 s) are skipped silently.
3. **R3.3 Safety.** The card is only read, never modified or ejected. The job copies/streams audio as needed and stops cleanly if the card disappears.
4. **R3.4 Notification.** A macOS notification on start, success (with file path) and failure (with reason). Log file in `~/Library/Logs/myplaud.log`.
5. **R3.5 Output.** Default output stays iCloud Drive `myPlaud/`. Optional `--obsidian-dir` also writes a note with frontmatter (`type: meeting`, date, duration, source) and a link back; default off.
6. **R3.6 Summary (optional, off by default).** `--summary` produces a short summary and action items with a configurable local or remote model; the transcript is always written first and never blocked by the summary. Remote use requires an explicit flag per run or a setting, because the audio/text leaves the machine.
7. **R3.7 Install.** One command installs/uninstalls the agent (`stt/install_agent.sh`); no root; documented in `stt/README.md`.

## Acceptance tests
- A1: insert a card with new session R0008: notification, `.md` and `.json` appear, log has timings.
- A2: re-insert the same card: nothing is reprocessed.
- A3: eject the card mid-run: job exits with a failure notification, no partial `.md` left behind.
- A4: uninstall removes the agent and leaves outputs.

## Out of scope
Wi-Fi/BLE transfer from the device, mobile app.

## Open questions
- Which summary model/language, and is a remote model acceptable for work conversations (personal use only per intent)?
