"""Re-time CHUNITHM conversions from their raw .c2s charts.

    py tools\\fix_chunithm.py              # report what would change, write nothing
    py tools\\fix_chunithm.py --apply      # rewrite the affected charts in BMSData (originals backed up)

What went wrong: a .c2s "measure" is always RESOLUTION ticks -- MET only moves the bar lines the
game draws, and can even change halfway through a measure. The converter turned every MET into a
BMS measure length (#xxx02), so a chart with a time signature other than 4/4 drifts away from its
music: Doughnut Hole (MET 4 8) played at half length, L'episode ran 50 s past its audio.

The fix keeps everything the converter decided -- which notes, which lanes, which sounds -- and only
puts each note back at its .c2s time. The converter kept note count and order (its metadata.json
says notes_source == notes_output), so the n-th note of the chart is the n-th note of the .c2s.
Tempo comes from the .c2s; the bogus measure lengths go. A chart is written only if the result
then matches the .c2s to the millisecond, and the original is copied to a backup folder first.

The copies the game plays live in BMSData under other folder names; each is found by its note data.
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_audit as audit
import raw_compare as rc

DONE_GAME = r"E:\.done\CHUNITHM"
BMSDATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "EZ2AC_FNEX", "BMSData", "5KeyMix")
PACK_PREFIX = "CHU."
DATA = re.compile(rb"^#(\d{3})([0-9A-Za-z]{2}):(\S*)")
DROP_CHANNELS = {0x02, 0x03, 0x08, 0x09}          # measure length, tempo, stops: rebuilt from .c2s


def read_c2s_ticks(path):
    """(resolution, sorted note ticks, [(tick, bpm)]) with measures of RESOLUTION ticks."""
    res, bpms, notes = 384, [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        try:
            if f[0] == "RESOLUTION":
                res = int(f[1])
            elif f[0] == "BPM":
                bpms.append((int(f[1]) * res + int(f[2]), float(f[3])))
            elif f[0] in ("TAP", "CHR", "FLK", "HLD", "SLD", "SXD", "MNE"):
                notes.append(int(f[1]) * res + int(f[2]))
        except (ValueError, IndexError):
            continue
    bpms = sorted(bpms) or [(0, 120.0)]
    if bpms[0][0] != 0:
        bpms.insert(0, (0, bpms[0][1]))
    return res, sorted(notes), bpms


def read_c2s_notes(path):
    """[(tick, cell, width)] of every note the converter kept (its policy: heads of TAP, CHR, FLK,
    HLD, SLD and SXD; each SLD segment line counts), measures of RESOLUTION ticks."""
    res, out = 384, []
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        try:
            if f[0] == "RESOLUTION":
                res = int(f[1])
            elif f[0] in ("TAP", "CHR", "FLK", "HLD", "SLD", "SXD", "MNE"):
                out.append((int(f[1]) * res + int(f[2]), int(f[3]), int(f[4])))
        except (ValueError, IndexError):
            continue
    return sorted(out)


def chunithm_lanes(notes):
    """[(tick, cell, width)] -> [(tick, lane 1..5)], the converter's own rule.

    Measured on 207434 single notes (no exception): the lane is where the note's centre falls when
    the 16 cells are cut in five, floor((cell + width/2) * 5/16) + 1. Two notes of one chord on one
    lane: the one further right (by centre) moves, toward the side its exact position leans within
    the lane, left on a tie -- (6,4)+(6,4) -> 2,3; (5,3)+(8,3) -> 3,4; (12,4)+(12,4) -> 4,5.
    """
    out = []
    by_tick = {}
    for t, cell, w in notes:
        by_tick.setdefault(t, []).append(cell + w / 2.0)
    for t in sorted(by_tick):
        taken = set()
        for centre in sorted(by_tick[t]):
            p = centre * 5.0 / 16.0                      # 0..5, continuous
            lane = min(5, max(1, int(p) + 1))
            if lane in taken:
                lean = 1 if p > (lane - 0.5) + 1e-9 else -1
                for step in (lean, -lean, 2 * lean, -2 * lean, 3 * lean, -3 * lean, 4 * lean, -4 * lean):
                    c = lane + step
                    if 1 <= c <= 5 and c not in taken:
                        lane = c
                        break
                else:
                    continue                             # a chord wider than five: dropped
            taken.add(lane)
            out.append((t, lane))
    return out


def tick_to_seconds(tick, res, bpms):
    sec, prev, bpm = 0.0, 0, bpms[0][1]
    for t, b in bpms:
        if t > tick:
            break
        sec += (t - prev) / (res / 4.0) * 60.0 / bpm
        prev, bpm = t, b
    return sec + (tick - prev) / (res / 4.0) * 60.0 / bpm


def seconds_to_tick(sec, res, bpms):
    acc, prev, bpm = 0.0, 0, bpms[0][1]
    for t, b in bpms[1:]:
        seg = (t - prev) / (res / 4.0) * 60.0 / bpm
        if acc + seg > sec:
            break
        acc += seg
        prev, bpm = t, b
    return prev + round((sec - acc) * bpm / 60.0 * (res / 4.0))


def note_signature(data):
    """What identifies a chart across copies: its note-lane data lines, header lines ignored."""
    lines = []
    for raw in data.splitlines():
        m = DATA.match(raw.strip())
        if m:
            ch = int(m.group(2), 36) if not re.fullmatch(rb"[0-9A-Fa-f]{2}", m.group(2)) else int(m.group(2), 16)
            base = ch - 0x40 if 0x51 <= ch <= 0x69 else ch
            if base in audit.KEYS:
                lines.append(raw.strip())
    return hashlib.sha1(b"\n".join(sorted(lines))).hexdigest()


def index_bmsdata():
    idx = {}
    for pack in os.listdir(BMSDATA):
        if not pack.startswith(PACK_PREFIX):
            continue
        for root, _, files in os.walk(os.path.join(BMSDATA, pack)):
            for f in files:
                if f.lower().endswith((".bms", ".bme")):
                    p = os.path.join(root, f)
                    idx.setdefault(note_signature(open(p, "rb").read()), []).append(p)
    return idx


def retime(bms_path, res, raw_notes, bpms):
    """New file bytes, or (None, reason). The notes are regenerated from the .c2s (time and lane);
    header, sounds and every other channel come from the converter's file."""
    data = open(bms_path, "rb").read()
    nl = b"\r\n"
    head, scale, ev = audit.parse(bms_path)
    seconds, _ = audit.timeline(head, scale, ev)
    # Every data event with its time on the converter's own (wrong) timeline.
    events = []                                   # [time, order, channel, value, is_note]
    order = 0
    for raw in data.splitlines():
        m = DATA.match(raw.strip())
        if not m:
            continue
        meas = int(m.group(1))
        chs = m.group(2).decode()
        if not re.fullmatch(r"[0-9A-Fa-f]{2}", chs):
            continue
        ch = int(chs, 16)
        if ch in DROP_CHANNELS:
            continue
        payload = m.group(3).decode("ascii", "replace")
        n = len(payload) // 2
        for i in range(n):
            v = payload[2 * i:2 * i + 2]
            if v == "00":
                continue
            base = ch - 0x40 if 0x51 <= ch <= 0x69 else ch
            events.append([seconds(meas, i / n), order, ch, v, base in audit.KEYS])
            order += 1
    notes = [e for e in events if e[4]]
    if len(notes) != len(raw_notes):
        return None, "note count %d vs .c2s %d" % (len(notes), len(raw_notes))
    values = {}
    for e in notes:
        values[e[3]] = values.get(e[3], 0) + 1
    sound = max(values, key=values.get)           # CHUNITHM charts play one silent sample
    placed = [(t, 0x10 + lane, sound) for t, lane in chunithm_lanes(raw_notes)]
    for e in events:
        if not e[4]:
            placed.append((max(0, seconds_to_tick(e[0], res, bpms)), e[2], e[3]))
    # Tempo on channel 08 with #BPMxx definitions.
    bpm_defs = {}
    for t, b in bpms[1:]:
        key = ("%.6f" % b).rstrip("0").rstrip(".")
        if key not in bpm_defs:
            bpm_defs[key] = audit_b36(len(bpm_defs) + 1)
        placed.append((t, 0x08, bpm_defs[key]))

    # Header: everything that is not a data line, minus the old tempo definitions.
    header = []
    for raw in data.splitlines():
        s = raw.strip()
        if not s or DATA.match(s):
            continue
        u = s.upper()
        if u.startswith(b"#BPM") and len(s) > 4 and s[4:5] not in (b" ", b"\t"):
            continue                              # #BPMxx
        if u.startswith(b"#BPM ") or u.startswith(b"#BPM\t"):
            s = b"#BPM " + (("%.6f" % bpms[0][1]).rstrip("0").rstrip(".")).encode()
        header.append(s)
    header += [("#BPM%s %s" % (k, v)).encode() for v, k in bpm_defs.items()]

    # Data lines, one per (measure, channel) layer, on the reduced grid.
    by = {}
    for t, ch, v in placed:
        by.setdefault((t // res, ch), []).append((t % res, v))
    lines = []
    for (meas, ch), items in sorted(by.items()):
        layers = []
        for pos, v in sorted(items):
            for layer in layers:
                if pos not in layer:
                    layer[pos] = v
                    break
            else:
                layers.append({pos: v})
        for layer in layers:
            g = res
            for pos in layer:
                g = math.gcd(g, pos)
            slots = ["00"] * (res // g)
            for pos, v in layer.items():
                slots[pos // g] = v
            lines.append(("#%03d%02X:%s" % (meas, ch, "".join(slots))).encode())
    return nl.join(header) + nl + nl + nl.join(lines) + nl, None


def stacked_notes(path):
    """Notes sharing a key and a moment with another -- none may survive a rebuild."""
    head, scale, ev = audit.parse(path)
    seconds, _ = audit.timeline(head, scale, ev)
    seen, stacked = set(), 0
    for m, f, ch, v in ev:
        if 0x11 <= ch <= 0x15:
            key = (ch, round(seconds(m, f), 6))
            stacked += key in seen
            seen.add(key)
    return stacked


def audit_b36(n):
    digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    return digits[n // 36] + digits[n % 36]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()
    backup_root = os.path.normpath(os.path.join(os.path.dirname(BMSDATA), "..", "BMSData_backup_chunithm_retime"))
    idx = index_bmsdata()
    # A chart fixed on an earlier run is found through its backed-up original, and the original is
    # what it is rebuilt from, so running this again gives the same result instead of compounding.
    if os.path.isdir(backup_root):
        for root, _, files in os.walk(backup_root):
            for f in files:
                bk = os.path.join(root, f)
                target = os.path.join(BMSDATA, os.path.relpath(bk, backup_root))
                if os.path.exists(target):
                    idx.setdefault(note_signature(open(bk, "rb").read()), []).append(target)
    songs = sorted(d for d in os.listdir(DONE_GAME) if os.path.isdir(os.path.join(DONE_GAME, d)))
    if a.limit:
        songs = songs[:a.limit]
    ok = fixed = unmatched = failed = 0
    for song in songs:
        sdir = os.path.join(DONE_GAME, song)
        try:
            meta = json.load(open(os.path.join(sdir, "metadata.json"), encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for c in meta.get("charts", []):
            raw_path = os.path.join(sdir, "Charts Raw", c.get("source", ""))
            done_bms = os.path.join(sdir, "Charts", c.get("output", ""))
            if not (os.path.exists(raw_path) and os.path.exists(done_bms)):
                continue
            res, ticks, bpms = read_c2s_ticks(raw_path)
            raw_notes = read_c2s_notes(raw_path)
            raw_sec = [tick_to_seconds(t, res, bpms) for t in ticks]
            before = rc.compare(rc.bms_times(done_bms), raw_sec)
            if before and before["spread_ms"] < 2 and abs(before["offset_ms"]) < 2 and abs(before["drift_ms_per_min"]) < 2:
                ok += 1
                continue
            targets = idx.get(note_signature(open(done_bms, "rb").read()), [])
            if not targets:
                unmatched += 1
                print("  no BMSData copy: %s / %s" % (song, c["output"]))
                continue
            for target in sorted(set(targets)):
                bk = os.path.join(backup_root, os.path.relpath(target, BMSDATA))
                source = bk if os.path.exists(bk) else target
                new, why = retime(source, res, raw_notes, bpms)
                if new is None:
                    failed += 1
                    print("  skipped %s: %s" % (target, why))
                    continue
                check = os.path.join(os.environ.get("TEMP", "."), "chu_retime_check.bms")
                open(check, "wb").write(new)
                after = rc.compare(rc.bms_times(check), raw_sec)
                if not after or after["spread_ms"] > 2 or abs(after["offset_ms"]) > 2:
                    failed += 1
                    print("  result does not match .c2s, left alone: %s (%s)" % (target, after))
                    continue
                stacked = stacked_notes(check)
                if stacked:
                    failed += 1
                    print("  result stacks %d notes on one key, left alone: %s" % (stacked, target))
                    continue
                fixed += 1
                print("  %s %s: spread %.0f ms, drift %+.0f ms/min -> 0" %
                      ("fixed" if a.apply else "would fix", os.path.relpath(target, BMSDATA),
                       before["spread_ms"], before["drift_ms_per_min"]))
                if a.apply:
                    rel = os.path.relpath(target, BMSDATA)
                    bk = os.path.normpath(os.path.join(backup_root, rel))
                    os.makedirs(os.path.dirname(bk), exist_ok=True)
                    if not os.path.exists(bk):
                        shutil.copy2(target, bk)
                    open(target, "wb").write(new)
    print("\n%d charts already right, %d %s, %d without a BMSData copy, %d not fixable" %
          (ok, fixed, "fixed" if a.apply else "to fix", unmatched, failed))


if __name__ == "__main__":
    sys.exit(main())
