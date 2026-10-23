"""Put Bang Dream's music back at the start of its charts.

    py tools\\fix_bangdream.py              # report, write nothing
    py tools\\fix_bangdream.py --apply      # fix the BMSData copies (originals backed up)

What went wrong: a Bang Dream chart (itself BMS, "Charts Raw\\Originales\\*.txt") that carries its
music puts it at the very start, #00001:01. Some carry none -- their channel 01 holds only the
fever cues (cmd_fever_ready, ...) -- and for those the converter hung song.mp3 on the FIRST
channel-01 event instead, the fever cue dozens of measures in: 'FIGHT' ADDICT started its music
47 s late, every note ahead of it. The fix moves the music's trigger to the start and leaves every
note alone. Some also lost the raw's BPM changes; those get the raw's tempo map back. A chart is
written only if it then matches one of its song's raw charts to the millisecond.
"""
import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_audit as audit
import raw_compare as rc
import fix_chunithm as fc

DONE_GAME = r"E:\.done\Bang Dream"
PACK_PREFIX = "BD."
DATA = re.compile(rb"^#(\d{3})01:(\S*)")


def song_wav_ids(data):
    """#WAV ids that are the music (song.* / bgm*), as 2-char strings."""
    ids = set()
    for raw in data.splitlines():
        m = re.match(rb"^#WAV([0-9A-Za-z]{2})\s+(\S.*)$", raw.strip(), re.I)
        if m:
            name = os.path.basename(m.group(2).decode("utf-8", "replace").strip()).lower()
            if name.startswith(("song.", "bgm")):
                ids.add(m.group(1).decode().upper())
    return ids


def move_music_to_start(data):
    """Bytes with every channel-01 trigger of the music removed and one placed at measure 0."""
    ids = song_wav_ids(data)
    if not ids:
        return None
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    out, first_id = [], None
    for raw in data.splitlines():
        m = DATA.match(raw.strip())
        if not m:
            out.append(raw)
            continue
        payload = m.group(2).decode("ascii", "replace")
        cells = [payload[i:i + 2] for i in range(0, len(payload) - 1, 2)]
        kept = []
        for c in cells:
            if c.upper() in ids:
                first_id = first_id or c.upper()
                kept.append("00")
            else:
                kept.append(c)
        if any(c != "00" for c in kept):
            out.append(("#%s01:%s" % (m.group(1).decode(), "".join(kept))).encode())
    if not first_id:
        return None
    # Right after the header, where a data section starts.
    idx = next((i for i, l in enumerate(out) if re.match(rb"^#\d{3}[0-9A-Za-z]{2}:", l.strip())), len(out))
    out.insert(idx, ("#00001:%s" % first_id).encode())
    return nl.join(out) + nl


TEMPO = re.compile(rb"^#(\d{3})(02|03|08|09):", re.I)
TEMPO_DEF = re.compile(rb"^#(BPM[0-9A-Za-z]{2}|STOP[0-9A-Za-z]{2})\s", re.I)


def copy_tempo(data, raw_path):
    """The chart with its tempo map replaced by the raw chart's: measure lengths (02), BPM
    changes (03, 08 with #BPMxx) and stops (09 with #STOPxx). Some conversions dropped the raw's
    BPM changes entirely -- ALIVE runs 176/180/174/176/178, its chart a flat 176 -- and every note
    sits on the raw's own measure grid, so the raw's map is the right one for them."""
    raw = open(raw_path, "rb").read()
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    base = next((l.strip() for l in raw.splitlines() if re.match(rb"^#BPM\s", l.strip(), re.I)), None)
    keep = []
    for l in data.splitlines():
        s = l.strip()
        if TEMPO.match(s) or TEMPO_DEF.match(s):
            continue
        if base and re.match(rb"^#BPM\s", s, re.I):
            keep.append(base)
            continue
        keep.append(l)
    defs = [l.strip() for l in raw.splitlines() if TEMPO_DEF.match(l.strip())]
    lines = [l.strip() for l in raw.splitlines() if TEMPO.match(l.strip())]
    at = next((i for i, l in enumerate(keep) if re.match(rb"^#\d{3}[0-9A-Za-z]{2}:", l.strip())), len(keep))
    return nl.join(keep[:at] + defs + lines + keep[at:]) + nl


def norm(name):
    return re.sub(r"[^0-9a-z]+", "", name.lower())


def romanize(text):
    """The organiser's romanisation (E:\\.done\\run.py, pykakasi hepburn), so names match."""
    try:
        from pykakasi import kakasi
    except ImportError:
        return text
    if not hasattr(romanize, "k"):
        romanize.k = kakasi()
    try:
        return "".join(i.get("hepburn", i.get("orig", "")) for i in romanize.k.convert(text))
    except Exception:
        return text



def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()
    backup_root = os.path.normpath(os.path.join(os.path.dirname(fc.BMSDATA), "..", "BMSData_backup_bangdream_bgm"))
    # The game's copies were converted separately from the ones next to the raw charts, so they are
    # found by song name (normalised, and romanised as BMSData names are) rather than by content.
    done = {}
    for song in os.listdir(DONE_GAME):
        sdir = os.path.join(DONE_GAME, song)
        if os.path.isdir(sdir):
            raws = []
            for root, _, files in os.walk(os.path.join(sdir, "Charts Raw")):
                raws += [os.path.join(root, f) for f in files
                         if f.lower().endswith(".txt") and "_command" not in f.lower()]
            done.setdefault(norm(song), raws)
            done.setdefault(norm(romanize(song)), raws)
    good = lambda c: c and c["spread_ms"] < 2 and abs(c["offset_ms"]) < 2 and abs(c["drift_ms_per_min"]) < 2
    check = os.path.join(os.environ.get("TEMP", "."), "bd_fix_check.bms")
    ok = fixed = unmatched = failed = 0
    for pack in sorted(p for p in os.listdir(fc.BMSDATA) if p.startswith(PACK_PREFIX)):
        for song in sorted(os.listdir(os.path.join(fc.BMSDATA, pack))):
            sdir = os.path.join(fc.BMSDATA, pack, song)
            if not os.path.isdir(sdir):
                continue
            cands = done.get(norm(song)) or []
            raw_times = [(r, rc.read_bms_raw(r)) for r in cands]
            for chart in sorted(f for f in os.listdir(sdir) if f.lower().endswith((".bms", ".bme"))):
                target = os.path.join(sdir, chart)
                if not raw_times:
                    unmatched += 1
                    print("  no raw chart: %s / %s" % (song, chart))
                    continue
                bk = os.path.join(backup_root, os.path.relpath(target, fc.BMSDATA))
                source = bk if os.path.exists(bk) else target
                # Which raw difficulty this chart is: the game's copies keep four of up to five
                # (01 Easy is often the raw "normal", 03 Hard the raw "special"), so every raw
                # chart of the song is tried, as it is and with the music moved to the start.
                cur = rc.bms_times(source)
                if any(good(rc.compare(cur, t)) for _, t in raw_times):
                    ok += 1
                    continue
                data = open(source, "rb").read()
                hit = new = None
                for r, t in raw_times:
                    for attempt in (move_music_to_start(data), move_music_to_start(copy_tempo(data, r))):
                        if attempt is None:
                            continue
                        open(check, "wb").write(attempt)
                        if good(rc.compare(rc.bms_times(check), t)):
                            hit, new = r, attempt
                            break
                    if hit:
                        break
                if not hit:
                    failed += 1
                    print("  matches no raw difficulty, left alone: %s/%s/%s" % (pack, song, chart))
                    continue
                fixed += 1
                print("  %s %s/%s/%s (raw %s)" % ("fixed" if a.apply else "would fix", pack, song, chart,
                                                os.path.basename(hit)))
                if a.apply:
                    os.makedirs(os.path.dirname(bk), exist_ok=True)
                    if not os.path.exists(bk):
                        shutil.copy2(target, bk)
                    open(target, "wb").write(new)
    print("\n%d charts already right, %d %s, %d without a raw chart, %d left alone" %
          (ok, fixed, "fixed" if a.apply else "to fix", unmatched, failed))


if __name__ == "__main__":
    sys.exit(main())
