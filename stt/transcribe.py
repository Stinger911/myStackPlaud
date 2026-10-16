#!/usr/bin/env python3
"""Turn a myPlaud recording (two WAV files) into a speaker-labelled transcript.

The device writes two aligned mono WAV files per session:
    R<nnnn>ME.WAV  - the microphone ("me")
    R<nnnn>OT.WAV  - what the host played ("others")
Each stream is transcribed on its own (so the speaker label is free, no diarization needed),
silent stretches are dropped (Whisper hallucinates on silence), and the two are merged by time
into a Markdown note.

Local by default: whisper.cpp (`whisper-cli`) + ffmpeg. Python standard library only.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from dataclasses import dataclass, asdict
from pathlib import Path

DEFAULT_MODEL = "large-v3-turbo-q5_0"
MODEL_DIR = Path(os.environ.get("MYPLAUD_MODELS", Path.home() / ".cache" / "myplaud" / "models"))
MODEL_URL = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-{name}.bin"
VAD_NAME = "silero-v5.1.2"
VAD_URL = "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-{name}.bin"
DEFAULT_OUT = Path(os.environ.get(
    "MYPLAUD_OUT", Path.home() / "Library" / "Mobile Documents" / "com~apple~CloudDocs" / "myPlaud"))
CARD_DIRS = [Path("/Volumes/M5CARD/MYPLAUD")]

FRAME_S = 0.1  # analysis frame for the activity detector


@dataclass
class Segment:
    start: float
    end: float
    text: str
    speaker: str


# --------------------------------------------------------------------------- input files

def find_pair(arg: str | None, directory: Path | None) -> tuple[Path, Path, str]:
    """Return (me_path, others_path, session_name) for `R0004`, `R0004ME.WAV` or a path to either."""
    search = [directory] if directory else [d for d in CARD_DIRS if d.exists()] + [Path.cwd()]
    if arg is None:
        # newest session that has both files
        found = {}
        for d in search:
            for p in d.glob("R[0-9][0-9][0-9][0-9]ME.WAV"):
                ot = p.with_name(p.name.replace("ME.WAV", "OT.WAV"))
                if ot.exists() and p.stat().st_size > 512 and ot.stat().st_size > 512:
                    found[p.name[:5]] = (p, ot)
        if not found:
            raise SystemExit("no recordings found; pass a session name like R0004 or --dir")
        name = sorted(found)[-1]
        return (*found[name], name)
    p = Path(arg)
    if p.suffix.lower() == ".wav":
        base = re.sub(r"(ME|OT)\.WAV$", "", p.name, flags=re.I)
        return p.with_name(base + "ME.WAV"), p.with_name(base + "OT.WAV"), base
    m = re.fullmatch(r"R?(\d{1,4})", arg, flags=re.I)
    if not m:
        raise SystemExit(f"cannot understand session {arg!r}; use R0004 or a path to a .WAV")
    name = f"R{int(m.group(1)):04d}"
    for d in search:
        me, ot = d / f"{name}ME.WAV", d / f"{name}OT.WAV"
        if me.exists() and ot.exists():
            return me, ot, name
    raise SystemExit(f"{name}ME.WAV / {name}OT.WAV not found in: {', '.join(map(str, search))}")


# --------------------------------------------------------------------------- audio analysis

def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=True, **kw)


def duration_s(path: Path) -> float:
    out = run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
               "-of", "default=nw=1:nk=1", str(path)]).stdout.strip()
    return float(out)


def frame_levels(path: Path) -> list[float]:
    """RMS level in dBFS of consecutive 100 ms frames (digital silence -> -120)."""
    n = int(16000 * FRAME_S)
    af = f"aresample=16000,asetnsamples=n={n}:p=0,astats=metadata=1:reset=1," \
         "ametadata=print:key=lavfi.astats.Overall.RMS_level:file=-"
    res = subprocess.run(["ffmpeg", "-hide_banner", "-v", "error", "-i", str(path), "-af", af,
                          "-f", "null", "-"], capture_output=True, text=True, check=True)
    levels = []
    for line in res.stdout.splitlines():
        if "RMS_level=" in line:
            v = line.split("=", 1)[1].strip()
            levels.append(-120.0 if v in ("-inf", "nan") else max(float(v), -120.0))
    return levels


def noise_adaptive_threshold(levels: list[float], margin_db: float = 8.0) -> float:
    """Speech threshold: 10th percentile of frame levels plus a margin, kept in a sane range."""
    if not levels:
        return -50.0
    s = sorted(levels)
    p10 = s[int(0.10 * (len(s) - 1))]
    return min(max(p10 + margin_db, -65.0), -35.0)


def active_regions(levels: list[float], threshold_db: float, min_speech_s: float = 0.2,
                   merge_gap_s: float = 0.6, pad_s: float = 0.3) -> list[tuple[float, float]]:
    """Time ranges (start, end) in seconds where the level exceeds the threshold."""
    regions: list[list[float]] = []
    start = None
    for i, lv in enumerate(levels + [-120.0]):
        t = i * FRAME_S
        if lv > threshold_db and start is None:
            start = t
        elif lv <= threshold_db and start is not None:
            if t - start >= min_speech_s:
                regions.append([start, t])
            start = None
    merged: list[list[float]] = []
    for r in regions:
        if merged and r[0] - merged[-1][1] <= merge_gap_s:
            merged[-1][1] = r[1]
        else:
            merged.append(r)
    total = len(levels) * FRAME_S
    return [(max(0.0, a - pad_s), min(total, b + pad_s)) for a, b in merged]


def overlap(a0: float, a1: float, b0: float, b1: float) -> float:
    return max(0.0, min(a1, b1) - max(a0, b0))


def keep_voiced(segments: list[Segment], regions: list[tuple[float, float]],
                min_fraction: float = 0.4) -> list[Segment]:
    """Drop segments that do not overlap detected speech (typical Whisper hallucinations)."""
    kept = []
    for s in segments:
        dur = max(s.end - s.start, 1e-6)
        covered = sum(overlap(s.start, s.end, a, b) for a, b in regions)
        if covered / dur >= min_fraction:
            kept.append(s)
    return kept


def clip_to_regions(segments: list[Segment], regions: list[tuple[float, float]]) -> list[Segment]:
    """Trim each segment to the speech actually present. whisper.cpp tends to start a segment at
    the edge of its 30 s window, long before the first word."""
    out = []
    for s in segments:
        hits = [(a, b) for a, b in regions if overlap(s.start, s.end, a, b) > 0]
        if not hits:
            continue
        out.append(Segment(max(s.start, hits[0][0]), min(s.end, hits[-1][1]), s.text, s.speaker))
    return out


def build_chunks(regions: list[tuple[float, float]], levels: list[float], max_s: float = 28.0,
                 join_gap_s: float = 1.5, min_s: float = 0.4) -> list[tuple[float, float]]:
    """Group speech regions into pieces of at most `max_s` for the ASR. Long pieces are cut at the
    quietest frame near the limit so a word is rarely split."""
    joined: list[list[float]] = []
    for a, b in regions:
        if joined and a - joined[-1][1] <= join_gap_s:
            joined[-1][1] = b
        else:
            joined.append([a, b])
    chunks: list[tuple[float, float]] = []
    for a, b in joined:
        while b - a > max_s:
            lo, hi = int((a + max_s - 6.0) / FRAME_S), int((a + max_s) / FRAME_S)
            window = levels[lo:hi] or [0.0]
            cut = (lo + window.index(min(window))) * FRAME_S
            chunks.append((a, cut))
            a = cut
        chunks.append((a, b))
    return [(a, b) for a, b in chunks if b - a >= min_s]


# --------------------------------------------------------------------------- ASR backends

# Phrases Whisper tends to invent out of noise. Dropped only when the segment is very short, so a
# real "bye" at the end of a call (a short segment too) is the price; a longer sentence is never touched.
HALLUCINATIONS = {"bye", "thank you", "thanks", "thanks for watching", "you", "okay", "ok", "so",
                  "продолжение следует", "спасибо", "пока", "до свидания"}
SHORT_SEGMENT_S = 2.0


def is_hallucination(text: str, duration_s: float) -> bool:
    plain = re.sub(r"[^\w\s]", "", text.lower()).strip()
    return duration_s <= SHORT_SEGMENT_S and plain in HALLUCINATIONS


NON_SPEECH = re.compile(r"^\s*[\[\(\*].*[\]\)\*]\s*$")


def prepare_for_asr(src: Path, dst: Path, normalize: bool) -> None:
    af = "highpass=f=80"
    if normalize:
        af += ",loudnorm=I=-20:LRA=11:TP=-3"
    run(["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(src), "-af", af,
         "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", str(dst)])


def find_whisper_cli() -> str:
    for cand in (os.environ.get("WHISPER_CLI"), "whisper-cli", "whisper-cpp"):
        if cand and shutil.which(cand):
            return cand
    raise SystemExit("whisper-cli not found. Install it with: brew install whisper-cpp")


def model_path(name_or_path: str) -> Path:
    p = Path(name_or_path).expanduser()
    if p.exists():
        return p
    p = MODEL_DIR / f"ggml-{name_or_path}.bin"
    if not p.exists():
        raise SystemExit(f"model {name_or_path!r} not found at {p}; download it with:\n"
                         f"  python3 {Path(sys.argv[0]).name} --download-model {name_or_path}")
    return p


def download_model(name: str) -> Path:
    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    dst = MODEL_DIR / f"ggml-{name}.bin"
    if dst.exists():
        print(f"already there: {dst}")
        return dst
    url = (VAD_URL if name.startswith("silero") else MODEL_URL).format(name=name)
    print(f"downloading {url}")
    tmp = dst.with_suffix(".part")
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length", 0))
        got = 0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            got += len(chunk)
            if total:
                print(f"\r  {got / 1e6:7.1f} / {total / 1e6:.1f} MB", end="", flush=True)
    tmp.rename(dst)
    print(f"\nsaved {dst}")
    return dst


def parse_whisper_cpp_json(data: dict, speaker: str) -> tuple[list[Segment], str]:
    segs = []
    for item in data.get("transcription", []):
        text = item.get("text", "").strip()
        if not text or NON_SPEECH.match(text) or not any(ch.isalnum() for ch in text):
            continue
        off = item["offsets"]
        start, end = off["from"] / 1000.0, off["to"] / 1000.0
        if is_hallucination(text, end - start):
            continue
        segs.append(Segment(start, end, text, speaker))
    return segs, data.get("result", {}).get("language", "")


def transcribe_whisper_cpp(wav: Path, chunks: list[tuple[float, float]], speaker: str, model: str, lang: str,
                           threads: int, work: Path, vad_threshold: float = 0.15) -> tuple[list[Segment], str]:
    """Cut `wav` (16 kHz mono) into the given chunks and transcribe them in ONE whisper-cli run
    (the model is loaded once; the language is detected per chunk)."""
    cli = find_whisper_cli()
    files = []
    for k, (a, b) in enumerate(chunks):
        f = work / f"{speaker}_{k:04d}.wav"
        run(["ffmpeg", "-hide_banner", "-v", "error", "-y", "-ss", f"{a:.3f}", "-t", f"{b - a:.3f}",
             "-i", str(wav), "-c:a", "pcm_s16le", str(f)])
        files.append(f)
    cmd = [cli, "-m", str(model_path(model)), "-l", lang, "-oj", "-np", "-t", str(threads), "-sns", "-mc", "0"]
    vad = MODEL_DIR / f"ggml-{VAD_NAME}.bin"
    if vad.exists():  # Silero VAD: tones, clicks and room noise are not sent to the decoder
        cmd += ["--vad", "--vad-model", str(vad), "-vt", str(vad_threshold)]
    else:
        print(f"  note: no VAD model; run --download-model {VAD_NAME} to suppress hallucinations on non-speech")
    for f in files:
        cmd += ["-f", str(f)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit(f"whisper-cli failed:\n{res.stderr[-2000:]}")
    segs: list[Segment] = []
    weight: dict[str, int] = {}
    for (a, _b), f in zip(chunks, files):
        out = next((c for c in (Path(str(f) + ".json"), f.with_suffix(".json")) if c.exists()), None)
        if out is None:  # the VAD found no speech in this chunk
            continue
        part, chunk_lang = parse_whisper_cpp_json(json.loads(out.read_text(encoding="utf-8")), speaker)
        for x in part:
            x.start += a
            x.end += a
        segs += part
        if chunk_lang:
            weight[chunk_lang] = weight.get(chunk_lang, 0) + sum(len(x.text) for x in part)
    lang_out = max(weight, key=weight.get) if weight else ""
    return segs, lang_out


def transcribe_stub(regions: list[tuple[float, float]], speaker: str) -> tuple[list[Segment], str]:
    """No ASR: one placeholder per detected speech region. For testing the pipeline."""
    return [Segment(a, b, "[speech]", speaker) for a, b in regions], "und"


# --------------------------------------------------------------------------- merge / output

def merge_turns(segments: list[Segment], max_gap_s: float = 2.0) -> list[Segment]:
    """Sort by start; join consecutive segments of the same speaker when the gap is small."""
    out: list[Segment] = []
    for s in sorted(segments, key=lambda x: (x.start, x.end)):
        prev = out[-1] if out else None
        if prev and prev.speaker == s.speaker and s.start - prev.end <= max_gap_s:
            prev.end = max(prev.end, s.end)
            prev.text = f"{prev.text} {s.text}"
        else:
            out.append(Segment(s.start, s.end, s.text, s.speaker))
    return out


def fmt_time(t: float) -> str:
    t = int(t)
    return f"{t // 3600}:{t % 3600 // 60:02d}:{t % 60:02d}" if t >= 3600 else f"{t // 60:02d}:{t % 60:02d}"


def render_markdown(turns: list[Segment], name: str, date: str, duration: float, language: str,
                    names: dict[str, str]) -> str:
    lines = ["---", f"title: Запись {name}", f"date: {date}", f"duration: {fmt_time(duration)}",
             f"language: {language or 'unknown'}", f"source: {name}", "tags: [myplaud, transcript]", "---", "",
             f"# Запись {name}", ""]
    if not turns:
        lines.append("_Речь не обнаружена._")
    for t in turns:
        lines.append(f"**[{fmt_time(t.start)}] {names[t.speaker]}:** {t.text}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


# --------------------------------------------------------------------------- main

def process_stream(path: Path, speaker: str, args, work: Path) -> tuple[list[Segment], str, list]:
    levels = frame_levels(path)
    # With the VAD the level gate only has to skip silence, so it can sit closer to the noise floor.
    have_vad = args.backend != "stub" and (MODEL_DIR / f"ggml-{VAD_NAME}.bin").exists()
    thr = args.threshold_db if args.threshold_db is not None else \
        noise_adaptive_threshold(levels, margin_db=4.0 if have_vad else 8.0)
    regions = active_regions(levels, thr)
    speech_s = sum(b - a for a, b in regions)
    print(f"  {speaker}: threshold {thr:.1f} dBFS, {len(regions)} speech regions, {speech_s:.1f} s of speech")
    if not regions:
        return [], "", regions
    if args.backend == "stub":
        segs, lang = transcribe_stub(regions, speaker)
    else:
        wav = work / f"{speaker}.wav"
        prepare_for_asr(path, wav, normalize=not args.no_normalize)
        chunks = build_chunks(regions, levels)
        segs, lang = transcribe_whisper_cpp(wav, chunks, speaker, args.model, args.lang, args.threads, work,
                                            args.vad_threshold)
        before = len(segs)
        segs = clip_to_regions(keep_voiced(segs, regions), regions)
        if before != len(segs):
            print(f"  {speaker}: dropped {before - len(segs)} segment(s) outside detected speech")
        if not segs:
            lang = ""  # whatever Whisper detected on non-speech is not a language
    return segs, lang, regions


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session", nargs="?", help="R0004, R0004ME.WAV or a path; default: newest on the card")
    ap.add_argument("--dir", type=Path, help="folder with the WAV files (default: the SD card, then cwd)")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"output folder (default: {DEFAULT_OUT})")
    ap.add_argument("--backend", choices=["whisper-cpp", "stub"], default="whisper-cpp")
    ap.add_argument("--model", default=DEFAULT_MODEL, help="ggml model name or path (default: %(default)s)")
    ap.add_argument("--lang", default="auto", help="auto, ru, pl, en ... (default: auto)")
    ap.add_argument("--threads", type=int, default=max(os.cpu_count() or 4, 4))
    ap.add_argument("--threshold-db", type=float, help="fixed speech threshold in dBFS (default: adaptive)")
    ap.add_argument("--vad-threshold", type=float, default=0.15,
                    help="Silero VAD sensitivity, lower = more sensitive (default: %(default)s)")
    ap.add_argument("--no-normalize", action="store_true", help="skip loudness normalisation before ASR")
    ap.add_argument("--me-name", default="Я")
    ap.add_argument("--others-name", default="Остальные")
    ap.add_argument("--date", default=dt.date.today().isoformat(), help="date for the note (the device has no clock)")
    ap.add_argument("--download-model", metavar="NAME", help="download a ggml model and exit")
    args = ap.parse_args(argv)

    if args.download_model:
        download_model(args.download_model)
        return 0
    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            raise SystemExit(f"{tool} not found (brew install ffmpeg)")

    me, others, name = find_pair(args.session, args.dir)
    print(f"{name}: {me} + {others.name}")
    duration = max(duration_s(me), duration_s(others))
    names = {"me": args.me_name, "others": args.others_name}

    all_segments: list[Segment] = []
    lang_weight: dict[str, int] = {}
    with tempfile.TemporaryDirectory() as td:
        for path, speaker in ((me, "me"), (others, "others")):
            segs, lang, _ = process_stream(path, speaker, args, Path(td))
            all_segments += segs
            if lang and lang != "und":  # weight by amount of text: the stream with more words wins
                lang_weight[lang] = lang_weight.get(lang, 0) + sum(len(x.text) for x in segs)
    turns = merge_turns(all_segments)
    language = max(lang_weight, key=lang_weight.get) if lang_weight else ""

    args.out.mkdir(parents=True, exist_ok=True)
    md = args.out / f"{args.date} {name}.md"
    md.write_text(render_markdown(turns, name, args.date, duration, language, names), encoding="utf-8")
    (args.out / f"{name}.json").write_text(
        json.dumps({"session": name, "duration": duration, "language": language,
                    "turns": [asdict(t) for t in turns]}, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"wrote {md}  ({len(turns)} turns)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
