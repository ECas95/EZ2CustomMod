"""Give StepMania-sourced charts (NeonFM) their full tempo map back.

    py tools\\fix_sm_tempo.py              # report, write nothing
    py tools\\fix_sm_tempo.py --apply      # fix the BMSData copies (originals backed up)

What went wrong: the converter wrote a BMS tempo change only for BPMs that differ from the song's
base BPM, so a change BACK to the base was dropped. A Bride In Dream goes 116 -> 58 -> 116 -> 29 ->
116; its chart went 116 -> 58 and stayed there, twice as slow, 100 s past the end of the music.
The notes themselves sit on the right beats. The fix rebuilds the tempo map (BPM changes, stops,
offset) from the .sm and leaves the notes alone; a chart is written only if every note then lands
on its .sm note to the millisecond.
"""
import argparse
import math
from fractions import Fraction
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_audit as audit
import raw_compare as rc
import fix_bangdream as fb

GAMES = {"NEO": r"E:\.done\NeonFM"}
TEMPO_LINE = re.compile(rb"^#(\d{3})(03|08|09):", re.I)
TEMPO_DEF = re.compile(rb"^#(BPM[0-9A-Za-z]{2}|STOP[0-9A-Za-z]{2})\s", re.I)
B36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def sm_tag(text, tag):
    m = re.search(r"#" + tag + r":([^;]*);", text, re.I)
    return m.group(1).strip() if m else ""


def sm_pairs(value):
    out = []
    for part in value.split(","):
        if "=" in part:
            a, b = part.split("=", 1)
            try:
                out.append((float(a), float(b)))
            except ValueError:
                pass
    return sorted(out)


def read_sm(path):
    """(offset s, [(beat, bpm)], [(beat, seconds)], [chart dict]) of a .sm file."""
    text = open(path, encoding="utf-8", errors="replace").read()
    offset = float(sm_tag(text, "OFFSET") or 0)
    bpms = sm_pairs(sm_tag(text, "BPMS")) or [(0.0, 120.0)]
    stops = sm_pairs(sm_tag(text, "STOPS"))
    charts = []
    for block in re.findall(r"#NOTES:([^;]*);", text, re.S):
        fields = block.split(":")
        if len(fields) < 6:
            continue
        kind, desc, diff, meter = (f.strip() for f in fields[:4])
        beats, cells = [], []                     # cells: (measure, Fraction in measure, column, char)
        measures = fields[5].split(",")
        for mi, measure in enumerate(measures):
            rows = [r.strip() for r in measure.strip().splitlines() if r.strip() and not r.strip().startswith("//")]
            for ri, row in enumerate(rows):
                if any(c in "124" for c in row):
                    beats.append(mi * 4 + 4.0 * ri / len(rows))
                for col, ch in enumerate(row):
                    if ch in "1234":
                        cells.append((mi, Fraction(ri, len(rows)), col, ch))
        charts.append({"kind": kind, "difficulty": diff.lower(), "meter": meter, "beats": beats, "cells": cells})
    return offset, bpms, stops, charts


def beat_to_seconds(beat, offset, bpms, stops):
    t, cur, bpm = 0.0, 0.0, bpms[0][1]
    for b, v in bpms[1:]:
        if b >= beat:
            break
        t += (b - cur) * 60.0 / bpm
        cur, bpm = b, v
    t += (beat - cur) * 60.0 / bpm
    t += sum(s for b, s in stops if b < beat)
    return t - offset


def chart_for(charts, bms_name):
    """The .sm chart this BMS came from: "01 - Hard 84.bms" -> difficulty Hard, meter 84."""
    stem = os.path.splitext(bms_name)[0].lower()
    words = re.findall(r"[a-z]+", stem)
    nums = re.findall(r"\d+", stem)
    meter = nums[-1] if len(nums) >= 2 else None
    for c in charts:
        if c["difficulty"] in words and (meter is None or c["meter"] == meter):
            return c
    return None


def tempo_lines(bpms, stops, offset):
    """BMS header definitions and channel lines for this .sm tempo map (4 beats a measure)."""
    defs, lines, events = [], [], {}
    ids = {}
    for b, v in bpms[1:]:
        key = ("%.6f" % v).rstrip("0").rstrip(".")
        if key not in ids:
            ids[key] = B36[(len(ids) + 1) // 36] + B36[(len(ids) + 1) % 36]
            defs.append(("#BPM%s %s" % (ids[key], key)).encode())
        events.setdefault((int(b // 4), 0x08), []).append((Fraction(b % 4 / 4.0).limit_denominator(3840), ids[key]))
    sids = {}
    for b, s in stops:
        bpm = next((v for bb, v in reversed(bpms) if bb <= b), bpms[0][1])
        # 1/192 of a whole note; decimals kept, since a rounded stop shifts everything after it
        # by up to half a unit (~4 ms at 150 BPM), and the mod reads the value as a double.
        units = s * bpm / 60.0 * 48
        if units <= 0:
            continue
        key = ("%.4f" % units).rstrip("0").rstrip(".")
        if key not in sids:
            sids[key] = B36[(len(sids) + 1) // 36] + B36[(len(sids) + 1) % 36]
            defs.append(("#STOP%s %s" % (sids[key], key)).encode())
        events.setdefault((int(b // 4), 0x09), []).append((Fraction(b % 4 / 4.0).limit_denominator(3840), sids[key]))
    # Each line on the finest grid its events need (a change at beat 37.3333 needs thirds), not a
    # fixed 192 that moves off-grid changes and lets the error pile up over the song.
    for (meas, ch), items in sorted(events.items()):
        res = 1
        for frac, _ in items:
            res = res * frac.denominator // math.gcd(res, frac.denominator)
        res = min(res, 3840)
        cells = ["00"] * res
        for frac, v in items:
            cells[min(res - 1, int(round(frac * res)))] = v
        lines.append(("#%03d%02X:%s" % (meas, ch, "".join(cells))).encode())
    base = ("#BPM " + ("%.6f" % bpms[0][1]).rstrip("0").rstrip(".")).encode()
    return base, defs, lines


def retempo(data, bpms, stops, offset):
    nl = data.count(bytes([13, 10])) and bytes([13, 10]) or bytes([10])
    base, defs, lines = tempo_lines(bpms, stops, offset)
    keep = []
    for l in data.splitlines():
        s = l.strip()
        if TEMPO_LINE.match(s) or TEMPO_DEF.match(s):
            continue
        if re.match(rb"^#BPM\s", s, re.I):
            keep.append(base)
            continue
        keep.append(l)
    at = next((i for i, l in enumerate(keep) if re.match(rb"^#\d{3}[0-9A-Za-z]{2}:", l.strip())), len(keep))
    return nl.join(keep[:at] + defs + lines + keep[at:]) + nl


NOTE_LINE = re.compile(rb"^#(\d{3})(1[0-9A-Za-z]|5[0-9A-Za-z]):", re.I)
MUSIC_LINE = re.compile(rb"^#(\d{3})01:", re.I)


def rebuild(data, chart, bpms, stops, offset):
    """The chart with its tempo map AND its notes taken from the .sm (for the few conversions
    whose notes were placed off the beats, not just off the tempo): column n -> key n, holds and
    rolls as #LNOBJ long notes, mines dropped. The .sm's #OFFSET becomes a stop at the start
    (negative: beat 0 comes after the music starts) or a later music trigger (positive)."""
    m = re.search(rb"^#LNOBJ\s+(\S\S)", data, re.M | re.I)
    lnobj = m.group(1).decode() if m else "ZZ"
    sound = "02"
    stops = list(stops)
    music_at = None
    if offset < 0:
        stops = [(0.0, -offset)] + stops
    elif offset > 0:
        music_at = offset
    out = retempo(data, bpms, stops, 0.0)
    nl = out.count(bytes([13, 10])) and bytes([13, 10]) or bytes([10])
    lines = [l for l in out.splitlines() if not NOTE_LINE.match(l.strip())]
    if music_at is not None:
        beat = music_at * bpms[0][1] / 60.0
        frac = Fraction(beat / 4.0).limit_denominator(3840)
        fixed = []
        for l in lines:
            mm = MUSIC_LINE.match(l.strip())
            if mm and mm.group(1) == b"000":
                cells = re.sub(rb"^#\d{3}01:", b"", l.strip())
                ids = [cells[i:i + 2] for i in range(0, len(cells), 2) if cells[i:i + 2] != b"00"]
                if ids:
                    fixed.append(("#00001:%s" % "".join(
                        ids[0].decode() if k == int(frac * frac.denominator) else "00"
                        for k in range(frac.denominator))).encode())
                    continue
            fixed.append(l)
        lines = fixed
    events = {}
    for mi, pos, col, ch in chart["cells"]:
        if col > 4:
            continue
        v = lnobj if ch == "3" else sound
        events.setdefault((mi, 0x11 + col), []).append((pos, v))
    notes = []
    for (meas, ch), items in sorted(events.items()):
        res = 1
        for pos, _ in items:
            res = res * pos.denominator // math.gcd(res, pos.denominator)
        cells = ["00"] * res
        for pos, v in items:
            cells[int(pos * res)] = v
        notes.append(("#%03d%02X:%s" % (meas, ch, "".join(cells))).encode())
    return nl.join(lines + notes) + nl


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()
    good = lambda c: c and c["spread_ms"] < 3 and abs(c["offset_ms"]) < 3   # tiny spread leaves no room for drift
    check = os.path.join(os.environ.get("TEMP", "."), "sm_fix_check.bms")
    for pack, done_dir in GAMES.items():
        backup_root = os.path.normpath(os.path.join(audit.ROOT, "..", "..", "BMSData_backup_sm_tempo"))
        sms = {}
        for song in os.listdir(done_dir):
            sdir = os.path.join(done_dir, song, "Charts Raw")
            if not os.path.isdir(sdir):
                continue
            files = [os.path.join(sdir, f) for f in os.listdir(sdir) if f.lower().endswith(".sm")]
            if files:
                name = re.sub(r"\s*\[[^\]]*\]\s*$", "", song)
                sms.setdefault(fb.norm(name), files[0])
                sms.setdefault(fb.norm(fb.romanize(name)), files[0])
        ok = fixed = unmatched = failed = 0
        pdir = os.path.join(audit.ROOT, pack)
        for song in sorted(os.listdir(pdir)):
            sdir = os.path.join(pdir, song)
            sm = sms.get(fb.norm(song))
            if not os.path.isdir(sdir):
                continue
            if not sm:
                unmatched += 1
                print("  no .sm: %s" % song)
                continue
            offset, bpms, stops, charts = read_sm(sm)
            for chart in sorted(f for f in os.listdir(sdir) if f.lower().endswith((".bms", ".bme"))):
                target = os.path.join(sdir, chart)
                c = chart_for(charts, chart)
                if not c:
                    unmatched += 1
                    print("  no .sm chart for %s/%s" % (song, chart))
                    continue
                raw = sorted(beat_to_seconds(b, offset, bpms, stops) for b in c["beats"])
                if good(rc.compare(rc.bms_times(target), raw)):
                    ok += 1
                    continue
                data = open(target, "rb").read()
                new = retempo(data, bpms, stops, offset)
                open(check, "wb").write(new)
                after = rc.compare(rc.bms_times(check), raw)
                if not good(after):
                    # Notes off the beats too: rebuild them from the .sm as well.
                    new = rebuild(data, c, bpms, stops, offset)
                    open(check, "wb").write(new)
                    after = rc.compare(rc.bms_times(check), raw)
                if not good(after):
                    failed += 1
                    print("  not fixed from the .sm, left alone: %s/%s (%s)" % (song, chart, after))
                    continue
                fixed += 1
                print("  %s %s/%s/%s" % ("fixed" if a.apply else "would fix", pack, song, chart))
                if a.apply:
                    bk = os.path.join(backup_root, pack, song, chart)
                    os.makedirs(os.path.dirname(bk), exist_ok=True)
                    if not os.path.exists(bk):
                        shutil.copy2(target, bk)
                    open(target, "wb").write(new)
        print("%s: %d charts already right, %d %s, %d unmatched, %d left alone" %
              (pack, ok, fixed, "fixed" if a.apply else "to fix", unmatched, failed))


if __name__ == "__main__":
    sys.exit(main())
