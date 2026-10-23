"""Compare converted BMS charts with the raw charts they came from, song by song.

    py tools\\raw_compare.py CHUNITHM                 # every song of one game under E:\\.done
    py tools\\raw_compare.py CHUNITHM --limit 50      # the first 50 songs
    py tools\\raw_compare.py CHUNITHM --csv out.csv   # one row per chart

Each song folder under E:\\.done\\<game> holds the raw charts ("Charts Raw"), the converter's BMS
("Charts") and a metadata.json naming which raw file became which BMS. For every chart the note
times of both are taken in seconds -- the BMS relative to where its BGM starts, the raw from its
own zero -- and each BMS note is matched with the nearest raw note. What the differences say:

  offset   the median difference: a constant shift, i.e. the chart is early or late against the
           music (a missed or doubled audio offset).
  spread   how far notes stray from that median (p90): a few notes misplaced, or a grid rounded.
  drift    how the difference grows over the song, in ms per minute: a wrong BPM or tick base,
           the chart slowly walking away from the music.

Only formats with a reader here are compared; add one to GAME_READERS to cover another game.
"""
import argparse
import csv
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_audit as audit   # BMS parsing and timing

DONE = r"E:\.done"


# ---- raw readers: path -> sorted note times in seconds ------------------------------------------

def read_c2s(path):
    """CHUNITHM .c2s: RESOLUTION ticks per whole note, BPM/MET changes at (measure, tick)."""
    res = 384
    bpms, mets, notes = [], [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        if not f or not f[0]:
            continue
        t = f[0]
        try:
            if t == "RESOLUTION":
                res = int(f[1])
            elif t == "BPM":
                bpms.append((int(f[1]), int(f[2]), float(f[3])))
            elif t == "MET":
                mets.append((int(f[1]), int(f[2]), int(f[3]), int(f[4])))
            elif t in ("TAP", "CHR", "FLK", "HLD", "SLD", "SXD", "MNE"):
                notes.append((int(f[1]), int(f[2])))
        except (ValueError, IndexError):
            continue
    # A c2s "measure" is always RESOLUTION ticks, whatever MET says: MET only moves the bar lines
    # the game draws. Note offsets never reach RESOLUTION even under MET 4 8 (Doughnut Hole: 382
    # of 384), and 145 such measures at 252 BPM are the song's 138 s. Scaling measures by MET --
    # what the converter did -- halves or doubles the chart against its music.
    def tick(m, t):
        return m * res + t

    changes = sorted((tick(m, t), bpm) for m, t, bpm in bpms) or [(0, 120.0)]

    def seconds(tk):
        sec, prev_t, bpm = 0.0, 0, changes[0][1]
        for ct, cb in changes:
            if ct > tk:
                break
            sec += (ct - prev_t) / (res / 4.0) * 60.0 / bpm
            prev_t, bpm = ct, cb
        return sec + (tk - prev_t) / (res / 4.0) * 60.0 / bpm

    return sorted(seconds(tick(m, t)) for m, t in notes)


def read_bms_raw(path):
    """A raw chart that is itself BMS (Bang Dream's .txt): every note event on a play channel,
    heads and ends alike, from where its BGM starts. Only BMS -> raw matching is done, so the
    extra ends do no harm."""
    head, scale, ev = audit.parse(path)
    seconds, _ = audit.timeline(head, scale, ev)
    bgm = None
    for m, f, ch, v in sorted(ev, key=lambda e: (e[0], e[1])):
        if ch == 1 and head["wav"].get(audit.b36(v), "").lower().startswith("bgm"):
            bgm = seconds(m, f)
            break
    times = [seconds(m, f) for m, f, ch, v in ev
             if 0x11 <= ch <= 0x19 or 0x21 <= ch <= 0x29 or 0x51 <= ch <= 0x59 or 0x61 <= ch <= 0x69]
    return sorted(t - (bgm or 0.0) for t in times)


def read_deemo_json(path):
    """DEEMO .json: notes carry their time in seconds ("time", or "_time" in older files)."""
    j = json.load(open(path, encoding="utf-8"))
    times = []
    for n in j.get("notes", []) if isinstance(j, dict) else []:
        t = n.get("time", n.get("_time"))
        if isinstance(t, (int, float)):
            times.append(float(t))
    return sorted(times)


# Raw formats per game (extension -> reader). A game not listed uses the default.
GAME_READERS = {
    "CHUNITHM": {".c2s": read_c2s},
    "Bang Dream": {".txt": read_bms_raw},
    "DEEMO": {".json": read_deemo_json},
    "DEEMO 2": {".json": read_deemo_json},
    "DEEMO Reborn": {".json": read_deemo_json},
}
DEFAULT_READERS = {".c2s": read_c2s}


def chart_pairs(game, sdir):
    """[(raw path, bms path, reader)] for one song folder.

    With a metadata.json listing charts (source -> output) that is the mapping; otherwise each BMS
    in Charts is matched to the raw file under "Charts Raw" whose name carries its difficulty word
    (expert.bms <-> 289_invadeshow_expert.txt)."""
    readers = GAME_READERS.get(game, DEFAULT_READERS)
    meta_path = os.path.join(sdir, "metadata.json")
    pairs = []
    if os.path.exists(meta_path):
        try:
            meta = json.load(open(meta_path, encoding="utf-8"))
        except ValueError:
            meta = {}
        for ch in meta.get("charts", []) if isinstance(meta, dict) else []:
            src, out = ch.get("source"), ch.get("output") or ch.get("bms")
            reader = readers.get(os.path.splitext(src or "")[1].lower())
            if src and out and reader:
                out = out.replace("\\", os.sep).replace("/", os.sep)
                bms = os.path.join(sdir, out) if os.sep in out else os.path.join(sdir, "Charts", out)
                pairs.append((os.path.join(sdir, "Charts Raw", src), bms, reader))
        if pairs:
            return pairs
    raws = []
    for root, _, files in os.walk(os.path.join(sdir, "Charts Raw")):
        for f in files:
            if os.path.splitext(f)[1].lower() in readers:
                raws.append(os.path.join(root, f))
    cdir = os.path.join(sdir, "Charts")
    if not os.path.isdir(cdir):
        return pairs
    for b in sorted(os.listdir(cdir)):
        if not b.lower().endswith((".bms", ".bme")):
            continue
        words = [w for w in __import__("re").split(r"[^a-z]+", os.path.splitext(b)[0].lower()) if len(w) >= 3]
        for w in words:
            hit = [r for r in raws if w in os.path.basename(r).lower()]
            if hit:
                pairs.append((hit[0], os.path.join(cdir, b), readers[os.path.splitext(hit[0])[1].lower()]))
                break
    return pairs


# ---- BMS side -----------------------------------------------------------------------------------

def bms_times(path):
    """Note head times in seconds from where the chart's BGM starts (0 if it has none)."""
    head, scale, ev = audit.parse(path)
    seconds, _ = audit.timeline(head, scale, ev)
    lnobj = head["lnobj"]
    held, times = set(), []
    bgm = None
    for m, f, ch, v in sorted(ev, key=lambda e: (e[0], e[1])):
        if ch == 1 and bgm is None:
            bgm = seconds(m, f)
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
    base_t = bgm or 0.0
    return sorted(t - base_t for t in times)


def compare(bms, raw):
    if not bms or not raw:
        return None
    import bisect
    diffs = []
    for t in bms:
        i = bisect.bisect_left(raw, t)
        best = min((raw[j] for j in (i - 1, i) if 0 <= j < len(raw)), key=lambda r: abs(r - t))
        diffs.append((t, t - best))
    d = [x[1] for x in diffs]
    med = statistics.median(d)
    dev = sorted(abs(x - med) for x in d)
    p90 = dev[int(0.9 * (len(dev) - 1))]
    # Drift: least-squares slope of difference over time.
    n = len(diffs)
    mt = sum(x[0] for x in diffs) / n
    md = sum(x[1] for x in diffs) / n
    var = sum((x[0] - mt) ** 2 for x in diffs)
    slope = sum((x[0] - mt) * (x[1] - md) for x in diffs) / var if var > 0 else 0.0
    return {"n_bms": len(bms), "n_raw": len(raw), "offset_ms": med * 1000.0,
            "spread_ms": p90 * 1000.0, "drift_ms_per_min": slope * 60000.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--done", default=DONE)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--csv")
    a = ap.parse_args()
    gdir = os.path.join(a.done, a.game)
    songs = sorted(d for d in os.listdir(gdir) if os.path.isdir(os.path.join(gdir, d)))
    if a.limit:
        songs = songs[:a.limit]
    rows, skipped = [], 0
    for song in songs:
        sdir = os.path.join(gdir, song)
        pairs = chart_pairs(a.game, sdir)
        if not pairs:
            skipped += 1
            continue
        for raw_path, bms_path, reader in pairs:
            if not os.path.exists(raw_path) or not os.path.exists(bms_path):
                continue
            try:
                r = compare(bms_times(bms_path), reader(raw_path))
            except Exception as ex:
                r = {"error": "%s: %s" % (type(ex).__name__, ex)}
            if r:
                r.update(song=song, chart=os.path.basename(bms_path))
                rows.append(r)
    ok = [r for r in rows if "error" not in r]
    print("%s: %d charts compared, %d songs with no raw/BMS pair, %d errors" %
          (a.game, len(ok), skipped, len(rows) - len(ok)))
    if ok:
        offs = [r["offset_ms"] for r in ok]
        print("  offset  median %+.1f ms   p10 %+.1f   p90 %+.1f" %
              (statistics.median(offs), sorted(offs)[len(offs) // 10], sorted(offs)[9 * len(offs) // 10]))
        print("  spread  median %.1f ms" % statistics.median(r["spread_ms"] for r in ok))
        print("  drift   median %+.1f ms/min" % statistics.median(r["drift_ms_per_min"] for r in ok))
        bad = [r for r in ok if abs(r["offset_ms"]) > 30 or r["spread_ms"] > 30 or abs(r["drift_ms_per_min"]) > 30]
        print("  charts off by more than 30 ms in offset, spread or drift: %d of %d" % (len(bad), len(ok)))
        for r in sorted(bad, key=lambda r: -max(abs(r["offset_ms"]), r["spread_ms"], abs(r["drift_ms_per_min"])))[:15]:
            print("    %-50.50s %-26.26s off %+7.1f  spread %6.1f  drift %+7.1f  notes %d/%d" %
                  (r["song"], r["chart"], r["offset_ms"], r["spread_ms"], r["drift_ms_per_min"], r["n_bms"], r["n_raw"]))
    if a.csv:
        with open(a.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=["song", "chart", "n_bms", "n_raw", "offset_ms", "spread_ms",
                                               "drift_ms_per_min", "error"])
            w.writeheader()
            for r in rows:
                w.writerow({k: r.get(k) for k in w.fieldnames})


if __name__ == "__main__":
    sys.exit(main())
