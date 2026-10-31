#!/usr/bin/env python3
"""Inspect SD card layout at G:\\ against the music-keyboard conventions."""
from __future__ import annotations

import json
import os
import struct
import wave
from collections import Counter

ROOT = r"G:\\"


def walk_tree() -> None:
    print("=== TREE ===")
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d != "System Volume Information"]
        rel = os.path.relpath(dirpath, ROOT)
        depth = 0 if rel == "." else rel.count(os.sep) + 1
        indent = "  " * depth
        name = "G:" if rel == "." else os.path.basename(dirpath)
        print(f"{indent}{name}/  ({len(filenames)} files)")
        show = filenames
        if len(filenames) > 40:
            print(f"{indent}  ... {len(filenames)} files omitted from tree listing")
            continue
        for fn in sorted(show, key=lambda s: s.encode("utf-8", "replace")):
            p = os.path.join(dirpath, fn)
            safe = fn.encode("ascii", "backslashreplace").decode("ascii")
            print(f"{indent}  {safe:32s} {os.path.getsize(p):10d}")


def parse_wav_header(path: str) -> dict:
    info = {
        "ok": False,
        "error": "",
        "rate": 0,
        "channels": 0,
        "width": 0,
        "frames": 0,
        "duration": 0.0,
        "comptype": "",
        "size": os.path.getsize(path),
        "riff_ok": False,
        "data_ok": False,
    }
    try:
        with open(path, "rb") as f:
            head = f.read(12)
            if len(head) < 12 or head[0:4] != b"RIFF" or head[8:12] != b"WAVE":
                info["error"] = "not RIFF/WAVE"
                return info
            info["riff_ok"] = True
            found_fmt = False
            found_data = False
            while True:
                chunk = f.read(8)
                if len(chunk) < 8:
                    break
                cid, csize = struct.unpack("<4sI", chunk)
                data = f.read(csize)
                if csize % 2 == 1:
                    f.read(1)
                if cid == b"fmt ":
                    found_fmt = True
                    audio_format, ch, rate, _br, _ba, bits = struct.unpack_from("<HHIIHH", data)
                    info["channels"] = ch
                    info["rate"] = rate
                    info["width"] = bits
                    info["comptype"] = "PCM" if audio_format == 1 else f"fmt={audio_format}"
                elif cid == b"data":
                    found_data = True
                    info["data_ok"] = True
                    if info["channels"] and info["width"]:
                        bps = info["channels"] * (info["width"] // 8)
                        if bps:
                            info["frames"] = len(data) // bps
                            info["duration"] = info["frames"] / info["rate"] if info["rate"] else 0
        if not found_fmt:
            info["error"] = "missing fmt"
            return info
        if not found_data:
            info["error"] = "missing data"
            return info
        info["ok"] = True
        return info
    except Exception as exc:  # noqa: BLE001
        info["error"] = str(exc)
        return info


def inspect_wav_dir(label: str, folder: str) -> list[dict]:
    print(f"\n=== WAV {label} ({folder}) ===")
    if not os.path.isdir(folder):
        print("  MISSING")
        return []
    rows = []
    files = [fn for fn in os.listdir(folder) if fn.lower().endswith(".wav")]
    files.sort(key=lambda s: (not s[:-4].isdigit(), int(s[:-4]) if s[:-4].isdigit() else s.lower()))
    rates = Counter()
    chans = Counter()
    widths = Counter()
    comps = Counter()
    for fn in files:
        p = os.path.join(folder, fn)
        info = parse_wav_header(p)
        info["name"] = fn
        rows.append(info)
        tag = "OK" if info["ok"] else "BAD"
        extra = info["error"] if not info["ok"] else (
            f"{info['rate']:5d}Hz  {info['channels']}ch  {info['width']:2d}bit  "
            f"{info['comptype']:6s}  {info['duration']:6.2f}s  {info['size']:8d}B"
        )
        safe = fn.encode("ascii", "backslashreplace").decode("ascii")
        print(f"  [{tag}] {safe:16s} {extra}")
        if info["ok"]:
            rates[info["rate"]] += 1
            chans[info["channels"]] += 1
            widths[info["width"]] += 1
            comps[info["comptype"]] += 1
    if files:
        print(f"  summary count={len(files)} rates={dict(rates)} ch={dict(chans)} bits={dict(widths)} fmt={dict(comps)}")
        expected = [f"{i}.wav" for i in range(1, 16)]
        have = set(files)
        missing = [n for n in expected if n not in have]
        extra = sorted(have - set(expected))
        if missing:
            print(f"  missing 1-15: {missing}")
        if extra:
            print(f"  extra names: {extra}")
    else:
        print("  (no wav)")
    return rows


def inspect_puzi() -> None:
    folder = os.path.join(ROOT, "puzi")
    print("\n=== PUZI ===")
    if not os.path.isdir(folder):
        print("  MISSING")
        return
    all_files = [n for n in os.listdir(folder) if n.lower().endswith(".txt")]
    print(f"  txt files={len(all_files)}")
    ok = 0
    fail = 0
    shapes = Counter()
    note_counts = []
    key_styles = Counter()
    for fn in all_files:
        p = os.path.join(folder, fn)
        try:
            with open(p, "r", encoding="utf-8") as f:
                text = f.read()
            doc = json.loads(text)
        except Exception:
            try:
                with open(p, "r", encoding="gbk", errors="replace") as f:
                    text = f.read()
                doc = json.loads(text)
            except Exception:
                fail += 1
                continue
        notes = None
        if isinstance(doc, list) and doc and isinstance(doc[0], dict) and "songNotes" in doc[0]:
            notes = doc[0]["songNotes"]
            shapes["array[0].songNotes"] += 1
        elif isinstance(doc, dict) and "songNotes" in doc:
            notes = doc["songNotes"]
            shapes["object.songNotes"] += 1
        else:
            shapes["other"] += 1
            ok += 1
            continue
        ok += 1
        if isinstance(notes, list):
            note_counts.append(len(notes))
            if notes:
                first = notes[0]
                if isinstance(first, dict):
                    k = str(first.get("key", ""))
                    if k.startswith("Key"):
                        key_styles["KeyN"] += 1
                    elif k.isdigit():
                        key_styles["numeric"] += 1
                    else:
                        key_styles["other:" + k[:20]] += 1
                else:
                    key_styles[f"non-object:{type(first).__name__}"] += 1
    print(f"  json_ok={ok} json_fail={fail} shapes={dict(shapes)} key_styles={dict(key_styles)}")
    if note_counts:
        print(f"  notes min/avg/max={min(note_counts)}/{sum(note_counts)//len(note_counts)}/{max(note_counts)}")

    files = sorted(os.listdir(folder))
    print(f"  files={len(files)}")
    sample_names = [n for n in files if n.lower() in ("11.txt", "always with me.txt", "blue.txt")]
    if not sample_names:
        sample_names = files[:3]
    # also pick a few more representative ones
    extra = []
    for n in files:
        if n.lower().endswith(".txt") and n not in sample_names:
            extra.append(n)
            if len(extra) >= 4:
                break
    files = sample_names + extra
    print("  inspecting sample:", [f.encode('ascii','backslashreplace').decode() for f in files])
    for fn in files:
        p = os.path.join(folder, fn)
        size = os.path.getsize(p)
        print(f"\n  -- {fn} ({size} bytes) --")
        try:
            with open(p, "r", encoding="utf-8") as f:
                text = f.read()
        except UnicodeDecodeError:
            with open(p, "r", encoding="gbk", errors="replace") as f:
                text = f.read()
        preview = text[:400].replace("\r", "")
        print("  preview:", preview[:400])
        try:
            doc = json.loads(text)
        except json.JSONDecodeError as exc:
            print(f"  JSON FAIL: {exc}")
            continue
        notes = None
        if isinstance(doc, list) and doc and isinstance(doc[0], dict) and "songNotes" in doc[0]:
            notes = doc[0]["songNotes"]
            print(f"  shape: array[0].songNotes  extra_keys={list(doc[0].keys())}")
        elif isinstance(doc, dict) and "songNotes" in doc:
            notes = doc["songNotes"]
            print(f"  shape: object.songNotes  extra_keys={list(doc.keys())}")
        else:
            print(f"  shape: unexpected type={type(doc).__name__}")
            if isinstance(doc, dict):
                print("  keys:", list(doc.keys())[:20])
            continue
        print(f"  notes={len(notes)}")
        keys = Counter()
        times = []
        bad = 0
        for n in notes:
            if not isinstance(n, dict) or "key" not in n or "time" not in n:
                bad += 1
                continue
            keys[str(n["key"])] += 1
            times.append(int(n["time"]))
        print(f"  bad_notes={bad} unique_keys={len(keys)} time_range={min(times) if times else None}..{max(times) if times else None}")
        print("  key histogram:")
        for k, c in sorted(keys.items(), key=lambda kv: kv[0]):
            print(f"    {k:16s} {c}")


def main() -> None:
    walk_tree()
    print("\n=== config.json ===")
    cfgp = os.path.join(ROOT, "config.json")
    with open(cfgp, "r", encoding="utf-8") as f:
        raw = f.read()
    print(raw)
    cfg = json.loads(raw)
    print("parsed:", json.dumps(cfg, ensure_ascii=False))

    inspect_wav_dir("piano", os.path.join(ROOT, "piano"))
    inspect_wav_dir("violin", os.path.join(ROOT, "violin"))
    inspect_wav_dir("audio", os.path.join(ROOT, "audio"))
    inspect_wav_dir("audio/piano", os.path.join(ROOT, "audio", "piano"))
    inspect_wav_dir("wav", os.path.join(ROOT, "wav"))
    inspect_wav_dir("record", os.path.join(ROOT, "record"))
    inspect_puzi()


if __name__ == "__main__":
    main()
