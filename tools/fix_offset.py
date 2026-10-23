"""Shift charts whose notes sit early or late against their own music.

    py tools\\fix_offset.py --sync sync.csv               # report, write nothing
    py tools\\fix_offset.py --sync sync.csv --apply       # rewrite the charts (originals backed up)
    py tools\\fix_offset.py --packs DEE-1.1 --apply       # measure these packs now instead of a CSV

audio_sync.py measures, per chart, how far the notes are from the attacks in the music. Charts of
one song agree to the frame, so the song's median is its offset. The detector has a bias of its own
(onsets peak a little after they start); it is measured every run on CHUNITHM charts that were
checked note by note against their .c2s, and taken off. A song more than --threshold ms off after
that has its NOTES moved (never the music, never the BGA, which follows the music), respecting BPM
changes, stops and measure lengths, and is written only if a fresh measurement then puts it on the
music.
"""
import argparse
import collections
import csv
import os
import re
import shutil
import statistics
import sys
from fractions import Fraction

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_sync as sync
import bms_audit as audit

BMSDATA = audit.ROOT
BACKUP = os.path.normpath(os.path.join(BMSDATA, "..", "..", "BMSData_backup_offset"))
DATA = re.compile(r"^#(\d{3})([0-9A-Za-z]{2}):(\S*)")
CALIBRATION_PACKS = ["CHU.1", "CHU.2"]


def refined_lag(env, times):
    """Lag in seconds (positive = notes late), sub-frame by parabolic interpolation, and confidence."""
    max_lag = int(sync.MAX_LAG_S * sync.RATE / sync.HOP)
    frames = np.round(np.asarray(times) * sync.RATE / sync.HOP).astype(np.int64)
    lags = np.arange(-max_lag, max_lag + 1)
    scores = np.zeros(len(lags))
    for i, lag in enumerate(lags):
        f = frames + lag
        f = f[(f >= 0) & (f < len(env))]
        if len(f):
            scores[i] = env[f].mean()
    k = int(scores.argmax())
    off = 0.0
    if 0 < k < len(scores) - 1:
        a, b, c = scores[k - 1], scores[k], scores[k + 1]
        den = a - 2 * b + c
        if den < 0:
            off = 0.5 * (a - c) / den
    conf = float((scores[k] - np.median(scores)) / (scores.std() + 1e-9))
    return -(lags[k] + off) * sync.HOP / sync.RATE, conf


def song_measure(sdir):
    """{chart: (lag s, conf)} for the charts of one song that line up with a main BGM."""
    files = {n.lower(): n for n in os.listdir(sdir)}
    out, envs = {}, {}
    for c in sorted(f for f in os.listdir(sdir) if f.lower().endswith(audit.CHART_EXT)):
        try:
            times, bgm = sync.chart_times(os.path.join(sdir, c))
        except Exception:
            continue
        best = None
        for start, name in bgm:
            p = audit.resolve_audio(sdir, name or "", files) if name else None
            d = audit.audio_seconds(p) if p else None
            if d and (best is None or d > best[2]):
                best = (start, p, d)
        if not best or best[2] < 30.0 or len(times) < 30:
            continue
        if best[1] not in envs:
            try:
                envs[best[1]] = sync.onset_envelope(best[1])
            except Exception:
                envs[best[1]] = None
        env = envs[best[1]]
        if env is None:
            continue
        rel = [t - best[0] for t in times if 0 <= t - best[0] <= best[2]]
        if len(rel) >= 30:
            out[c] = refined_lag(env, rel)
    return out


def measure_job(sdir):
    try:
        return sdir, song_measure(sdir)
    except Exception:
        return sdir, {}


# ---- moving notes in time ---------------------------------------------------------------------

def tempo_map(path):
    """(seconds(measure, frac), inverse(seconds) -> (measure, Fraction frac))."""
    head, scale, ev = audit.parse(path)
    seconds, changes = audit.timeline(head, scale, ev)
    last = max([e[0] for e in ev] + [0]) + 64
    starts = [0.0]
    for m in range(last):
        starts.append(starts[-1] + 4.0 * scale.get(m, 1.0))

    def beat_to_pos(beat):
        m = max(0, min(last - 1, int(np.searchsorted(starts, beat, side="right") - 1)))
        length = 4.0 * scale.get(m, 1.0)
        return m, max(0.0, min(1.0, (beat - starts[m]) / length))

    # seconds -> beat, walking the same changes the timeline uses.
    def sec_to_beat(t):
        cur_t, cur_b, bpm = 0.0, 0.0, head["bpm"]
        for cb, nb, stop in changes:
            seg = (cb - cur_b) * 60.0 / bpm if bpm > 0 else 0.0
            if cur_t + seg > t:
                break
            cur_t += seg
            cur_b = cb
            if nb is not None:
                bpm = nb
            elif bpm > 0:
                pause = stop * 60.0 / bpm
                if cur_t + pause > t:
                    return cur_b                      # inside a stop: the stop's position
                cur_t += pause
        return cur_b + (t - cur_t) * bpm / 60.0 if bpm > 0 else cur_b

    return seconds, lambda t: beat_to_pos(sec_to_beat(t))


NOTE_BASES = set(audit.KEYS) | {0x16, 0x17, 0x26, 0x27}


TEMPO_CHANNELS = {0x02, 0x03, 0x08, 0x09}


def shift_notes(path, delta_s):
    """File bytes with every note (play lanes, scratch, long notes) moved by -delta_s seconds.
    Only note lines are rewritten (they are ASCII); every other line is kept byte for byte, so a
    UTF-8 or Shift-JIS title survives."""
    return shift_events(path, -delta_s, lambda base: base in NOTE_BASES)


def shift_music(path, delta_s):
    """The same correction the other way round: the music (channel 01), BGA and every other
    non-note, non-tempo channel moved LATER by delta_s. For a chart whose first note is too close
    to the start to move the notes earlier (107 WJMAX songs, ~110 ms late, notes from 0 s)."""
    return shift_events(path, delta_s, lambda base: base not in NOTE_BASES and base not in TEMPO_CHANNELS)


def shift_events(path, delta_s, moving):
    """File bytes with the events of the channels `moving(base)` accepts moved by +delta_s seconds,
    on the chart's own tempo map; None if one would land before the start."""
    raw = open(path, "rb").read()
    nl = b"\r\n" if b"\r\n" in raw else b"\n"
    seconds, to_pos = tempo_map(path)
    RES = 960
    moved = collections.defaultdict(list)            # (measure, channel) -> [(slot, value)]
    kept = []
    for line in raw.splitlines():
        m = DATA.match(line.strip().decode("ascii", "replace"))
        if not m or not re.fullmatch(r"[0-9A-Fa-f]{2}", m.group(2)):
            kept.append(line)
            continue
        ch = int(m.group(2), 16)
        base = ch - 0x40 if 0x51 <= ch <= 0x69 else ch
        if not moving(base):
            kept.append(line)
            continue
        meas, data = int(m.group(1)), m.group(3)
        n = len(data) // 2
        for i in range(n):
            v = data[2 * i:2 * i + 2]
            if v == "00":
                continue
            t = seconds(meas, i / n) + delta_s
            if t < 0:
                return None                            # would go before the start
            nm, frac = to_pos(t)
            slot = int(round(frac * RES))
            if slot >= RES:
                nm, slot = nm + 1, 0
            moved[(nm, ch)].append((slot, v))
    lines = []
    for (meas, ch), items in sorted(moved.items()):
        layers = []
        for slot, v in sorted(items):
            for layer in layers:
                if slot not in layer:
                    layer[slot] = v
                    break
            else:
                layers.append({slot: v})
        for layer in layers:
            g = RES
            for sl in layer:
                if sl:
                    g = int(np.gcd(g, sl))
            cells = ["00"] * (RES // g)
            for sl, v in layer.items():
                cells[sl // g] = v
            lines.append(("#%03d%02X:%s" % (meas, ch, "".join(cells))).encode("ascii"))
    return nl.join(kept + lines) + nl


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sync", help="audio_sync.py CSV to pick candidates from")
    ap.add_argument("--packs", nargs="*")
    ap.add_argument("--threshold", type=float, default=20.0, help="ms off (after bias) before a song is moved")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    a = ap.parse_args()
    from concurrent.futures import ProcessPoolExecutor

    with ProcessPoolExecutor(max_workers=a.workers) as pool:
        # Bias: CHUNITHM charts checked note for note against their .c2s.
        cal = []
        for pack in CALIBRATION_PACKS:
            pdir = os.path.join(BMSDATA, pack)
            dirs = [os.path.join(pdir, s) for s in sorted(os.listdir(pdir))[:120]]
            for _, res in pool.map(measure_job, dirs, chunksize=4):
                cal += [lag for lag, conf in res.values() if conf >= 4]
        bias = statistics.median(cal)
        print("detector bias %.1f ms (from %d verified CHUNITHM charts)" % (bias * 1000, len(cal)))

        # Candidate songs.
        songs = set()
        if a.sync:
            per = collections.defaultdict(list)
            for r in csv.DictReader(open(a.sync, encoding="utf-8")):
                if float(r["conf"]) >= 4:
                    per[(r["pack"], r["song"])].append(float(r["lag_ms"]) / 1000.0)
            for (pack, song), lags in per.items():
                if abs(statistics.median(lags) - bias) * 1000 > a.threshold * 0.75:
                    songs.add(os.path.join(BMSDATA, pack, song))
        for pack in a.packs or []:
            pdir = os.path.join(BMSDATA, pack)
            songs |= {os.path.join(pdir, s) for s in os.listdir(pdir) if os.path.isdir(os.path.join(pdir, s))}
        songs = sorted(s for s in songs if not os.path.relpath(s, BMSDATA).startswith(tuple(CALIBRATION_PACKS)))

        fixed = skipped = fine = 0
        for sdir, res in pool.map(measure_job, songs, chunksize=2):
            sure = [lag for lag, conf in res.values() if conf >= 4]
            if not sure:
                continue
            off = statistics.median(sure) - bias
            if abs(off) * 1000 <= a.threshold:
                fine += 1
                continue
            rel = os.path.relpath(sdir, BMSDATA)
            results = []
            # Always measured and moved from the chart as it is now, so a second run corrects what
            # is left instead of re-applying a shift; the first run's backup keeps the original.
            # Every chart of the song shares its music, so every chart moves -- not only the ones
            # with enough notes to be measured. Notes go earlier; if one of them would land before
            # the start, the music (and the BGA with it) goes later instead, by the same amount.
            charts = sorted(f for f in os.listdir(sdir) if f.lower().endswith(audit.CHART_EXT))
            how = "notes"
            for chart in charts:
                target = os.path.join(sdir, chart)
                results.append((target, os.path.join(BACKUP, rel, chart), target, shift_notes(target, off)))
            if any(n is None for *_, n in results):
                how = "music"
                results = [(t, bk, s, shift_music(t, off) if off > 0 else None) for t, bk, s, _ in results]
            if any(n is None for *_, n in results):
                skipped += 1
                print("  %s: %+.0f ms, but neither the notes nor the music can move that far; left alone" %
                      (rel, off * 1000))
                continue
            # Check: the shifted charts must now sit on the music.
            # A fresh folder per song: a fixed one could not always be removed in time (a file still
            # held open), and the next copy into it stopped the whole run halfway through.
            import tempfile
            tmpdir = os.path.join(tempfile.mkdtemp(prefix="offset_check_"), "song")
            shutil.copytree(sdir, tmpdir, ignore=shutil.ignore_patterns("*.bms", "*.bme", "*.bml", "*.pms"))
            for target, bk, source, new in results:
                open(os.path.join(tmpdir, os.path.basename(target)), "wb").write(new)
            after = [lag for lag, conf in song_measure(tmpdir).values() if conf >= 4]
            shutil.rmtree(os.path.dirname(tmpdir), ignore_errors=True)
            residual = statistics.median(after) - bias if after else None
            if residual is None or abs(residual) * 1000 > 8:
                skipped += 1
                print("  %s: %+.0f ms, but after moving it measures %s; left alone" %
                      (rel, off * 1000, "%+.0f ms" % (residual * 1000) if residual is not None else "nothing"))
                continue
            fixed += 1
            print("  %s %s: notes %+.0f ms against the music -> %+.0f ms (moved the %s)" %
                  ("fixed" if a.apply else "would fix", rel, off * 1000, residual * 1000, how))
            if a.apply:
                for target, bk, source, new in results:
                    os.makedirs(os.path.dirname(bk), exist_ok=True)
                    if not os.path.exists(bk):
                        shutil.copy2(target, bk)
                    open(target, "wb").write(new)
    print("\n%d songs already on the music, %d %s, %d left alone" %
          (fine, fixed, "moved" if a.apply else "to move", skipped))


if __name__ == "__main__":
    sys.exit(main())
