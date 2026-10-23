"""Measure how well each chart sits on its own music, for any pack, without the raw chart.

    py tools\\audio_sync.py --packs CHU.1            # some packs
    py tools\\audio_sync.py --csv sync.csv           # the whole library, one row per chart

A chart whose notes are early or late against the music shows it in the audio itself: notes land
on the song's attacks. For every chart with a single main BGM this takes the music's onset
envelope (spectral flux, ~6 ms steps), lays the chart's note times over it and slides them +/-300 ms:
the lag with the strongest match is how far the chart is off. Done separately on each half of the
song it also shows drift (a wrong BPM walking away from the music).

  lag_ms     positive = the notes come LATE against the music (move them earlier to fix)
  conf       how much the best lag stands out (peak over the spread of all lags); < 4 is noise
  drift_ms   lag of the second half minus the first

Charts with segmented or keysound-only music are skipped: there is no one track to line up with.
"""
import argparse
import collections
import csv
import os
import statistics
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_audit as audit

HOP = 256                      # samples at 44.1 kHz ~ 5.8 ms
RATE = 44100
MAX_LAG_S = 0.30


def onset_envelope(path):
    import soundfile as sf
    data, sr = sf.read(path, dtype="float32", always_2d=True)
    x = data.mean(axis=1)
    if sr != RATE:                                           # nearest resample is enough for onsets
        idx = (np.arange(int(len(x) * RATE / sr)) * sr / RATE).astype(np.int64)
        x = x[np.clip(idx, 0, len(x) - 1)]
    n = 1024
    frames = 1 + max(0, (len(x) - n) // HOP)
    if frames < 10:
        return None
    win = np.hanning(n).astype(np.float32)
    env = np.zeros(frames, dtype=np.float32)
    prev = None
    for start in range(0, frames, 2048):                     # in blocks, to bound memory
        stop = min(frames, start + 2048)
        idx = np.arange(start, stop)[:, None] * HOP + np.arange(n)[None, :]
        mag = np.log1p(np.abs(np.fft.rfft(x[idx] * win, axis=1)))
        if prev is not None:
            mag_prev = np.vstack([prev[None, :], mag[:-1]])
        else:
            mag_prev = np.vstack([mag[:1], mag[:-1]])
        env[start:stop] = np.maximum(mag - mag_prev, 0).sum(axis=1)
        prev = mag[-1]
    env -= np.convolve(env, np.ones(64) / 64, mode="same")  # take out the loudness trend
    env = np.maximum(env, 0)
    return env / (env.std() + 1e-9)


def best_lag(env, times, max_lag):
    """(lag in frames, confidence) for note times (seconds, in the audio's timeline)."""
    frames = np.round(np.asarray(times) * RATE / HOP).astype(np.int64)
    lags = np.arange(-max_lag, max_lag + 1)
    scores = np.zeros(len(lags))
    for i, lag in enumerate(lags):
        f = frames + lag
        f = f[(f >= 0) & (f < len(env))]
        if len(f):
            # A little tolerance: the best of the frame and its neighbours.
            scores[i] = np.maximum(np.maximum(env[f], env[np.clip(f - 1, 0, len(env) - 1)]),
                                   env[np.clip(f + 1, 0, len(env) - 1)]).mean()
    k = int(scores.argmax())
    spread = scores.std() + 1e-9
    return int(lags[k]), float((scores[k] - np.median(scores)) / spread)


def chart_times(path):
    """Note-head times relative to the main BGM start, and that BGM's file (None if segmented)."""
    head, scale, ev = audit.parse(path)
    seconds, _ = audit.timeline(head, scale, ev)
    lnobj = head["lnobj"]
    held, times = set(), []
    bgm = []
    for m, f, ch, v in sorted(ev, key=lambda e: (e[0], e[1])):
        if ch == 1:
            bgm.append((seconds(m, f), head["wav"].get(audit.b36(v))))
            continue
        base = ch - 0x40 if 0x51 <= ch <= 0x69 else ch
        if base not in audit.KEYS:
            continue
        if 0x51 <= ch <= 0x69:
            if base in held:
                held.discard(base)
                continue
            held.add(base)
        elif lnobj and audit.b36(v) == lnobj:
            continue
        times.append(seconds(m, f))
    return times, bgm


def song_job(args):
    sdir, charts = args
    files = {n.lower(): n for n in os.listdir(sdir)}
    rows, env_cache = [], {}
    for c in charts:
        try:
            times, bgm = chart_times(os.path.join(sdir, c))
        except Exception:
            continue
        if len(times) < 30 or not bgm:
            continue
        # The main BGM: the longest channel-01 sample; it must carry the song on its own.
        best = None
        for start, name in bgm:
            p = audit.resolve_audio(sdir, name or "", files) if name else None
            d = audit.audio_seconds(p) if p else None
            if d and (best is None or d > best[2]):
                best = (start, p, d)
        if not best or best[2] < 30.0 or max(times) - best[0] > best[2] + 2.0:
            continue
        start, audio, dur = best
        if audio not in env_cache:
            try:
                env_cache[audio] = onset_envelope(audio)
            except Exception:
                env_cache[audio] = None
        env = env_cache[audio]
        if env is None:
            continue
        rel = sorted(t - start for t in times if 0.0 <= t - start <= dur)
        if len(rel) < 30:
            continue
        max_lag = int(MAX_LAG_S * RATE / HOP)
        lag, conf = best_lag(env, rel, max_lag)
        half = len(rel) // 2
        lag1, c1 = best_lag(env, rel[:half], max_lag)
        lag2, c2 = best_lag(env, rel[half:], max_lag)
        ms = 1000.0 * HOP / RATE
        rows.append({"song": os.path.basename(sdir), "chart": c, "notes": len(rel),
                     "lag_ms": round(-lag * ms, 1), "conf": round(conf, 1),
                     "drift_ms": round(-(lag2 - lag1) * ms, 1) if min(c1, c2) >= 4 else None})
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=audit.ROOT)
    ap.add_argument("--packs", nargs="*")
    ap.add_argument("--csv")
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    a = ap.parse_args()
    packs = sorted(p for p in os.listdir(a.root) if os.path.isdir(os.path.join(a.root, p)))
    if a.packs:
        want = {p.lower() for p in a.packs}
        packs = [p for p in packs if p.lower() in want]
    writer = None
    if a.csv:
        fh = open(a.csv, "w", newline="", encoding="utf-8")
        writer = csv.DictWriter(fh, fieldnames=["pack", "song", "chart", "notes", "lag_ms", "conf", "drift_ms"])
        writer.writeheader()
    print("%-10s %6s %8s %8s %8s %8s" % ("pack", "charts", "median", "|lag|>25", "|lag|>50", "drift>30"))
    with ProcessPoolExecutor(max_workers=a.workers) as pool:
        for pack in packs:
            pdir = os.path.join(a.root, pack)
            jobs = []
            for song in sorted(os.listdir(pdir)):
                sdir = os.path.join(pdir, song)
                if os.path.isdir(sdir):
                    charts = [f for f in os.listdir(sdir) if f.lower().endswith(audit.CHART_EXT)]
                    if charts:
                        jobs.append((sdir, charts))
            rows = [r for rs in pool.map(song_job, jobs, chunksize=4) for r in rs]
            sure = [r for r in rows if r["conf"] >= 4]
            if writer:
                for r in rows:
                    writer.writerow(dict(r, pack=pack))
            if not sure:
                print("%-10s %6d   (no chart with a clear match)" % (pack, len(rows)), flush=True)
                continue
            lags = [r["lag_ms"] for r in sure]
            drift = [r for r in sure if r["drift_ms"] is not None and abs(r["drift_ms"]) > 30]
            print("%-10s %6d %+8.1f %8d %8d %8d" %
                  (pack, len(sure), statistics.median(lags), sum(abs(l) > 25 for l in lags),
                   sum(abs(l) > 50 for l in lags), len(drift)), flush=True)


if __name__ == "__main__":
    sys.exit(main())
