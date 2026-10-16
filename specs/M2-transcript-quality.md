# M2. Transcript quality and speed

Status: draft, 2026-10-16

## Goal
A usable speaker-labelled transcript within about half the meeting length after the card is read, with no isolated fake lines, for Russian, Polish, English and mixed speech.

## Evidence (why)
R0007 (32 min): 197 turns, 30 min 39 s wall time (338 s CPU user, 19% CPU, so mostly waiting on a single whisper run), 318 ME and 274 OT speech regions. "Я" contains isolated English lines ("Thank you.", "Fire!", "I'm going to go.") that were not said. PL/EN/mixed speech was never tested.

## Requirements
1. **R2.1 Bleed gate.** A ME region is dropped or merged when the OT stream is loud in the same span and the ME level is below a margin over the ME noise floor (microphone picking up speakers). Margin and window are CLI options with defaults tuned on R0007.
2. **R2.2 Hallucination filter.** Extend the short-phrase filter and make it language-aware; add per-stream VAD threshold (`--vad-threshold-me`, `--vad-threshold-others`). Filtered lines go to `Rnnnn.json` with a `filtered: true` flag instead of being lost (so tuning is auditable).
3. **R2.3 Speed.** Target full transcription of a 32-min recording in under 15 min on the author's Mac. Measures, by evidence: several `whisper-cli` processes in parallel (threads split), skipping long non-speech spans, merging pieces to fill the 28 s window, a smaller model for ME. Print per-stage timing.
4. **R2.4 Languages.** Recording samples of 1-2 min each in Polish, English and mixed RU/PL/EN are added as fixtures (outside git if large; a small script generates them from real WAV and a manifest with expected key phrases). Language is chosen per piece; mixed pieces are not forced into one language. Report the language of each turn in `.json`.
5. **R2.5 Model comparison.** On the same fixtures compare `large-v3-turbo q5_0` against at least one other local model (candidate: an audio-capable Gemma, to be verified available); record word error on key phrases and time in `specs/results/`. Decision is written back to `myPlaud.md` and `INTENT.md` open questions.
6. **R2.6 Drop-aware timing.** Sessions with drops from M1 old format (no zero-fill) are flagged in the Markdown header (`warning: N s lost`).

## Acceptance tests
- A1: R0007: no turn in "Я" shorter than 1.5 s whose text is a known filler; manual spot check of 20 random turns finds no invented text.
- A2: R0007 total time < 15 min.
- A3: PL, EN and mixed fixtures: all key phrases present; language per turn correct for >= 90% of turns.
- A4: unit tests cover bleed gate, filter flags and the new options; all tests pass.

## Out of scope
Diarization within one stream (the device already separates "me" and "others"), summaries (M3).

## Open questions
- Is an audio-capable Gemma runnable locally on this Mac with acceptable speed?
- Is there a second remote participant case (several "others" in one stream) that needs diarization later?
