"""Find BMSData songs whose music file differs from the original's (trimmed, cut short, a preview).

    py tools\\audio_mismatch.py --csv out.csv

Every song under E:\\.done is indexed by its name (the "[id]" tail dropped, also romanised, as the
BMSData folder names are). For each BMSData song the main BGM (the longest channel-01 sample) is
compared with the original's music (Cancion\\*, or song.* at the song root); a difference of more
than --tolerance seconds is listed with both durations. Nothing is changed.
"""
import argparse
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_audit as audit
import fix_bangdream as fb
import audio_sync as sync

DONE = r"E:\.done"
AUDIO = (".mp3", ".ogg", ".wav", ".flac")


def done_index():
    idx = {}
    for game in os.listdir(DONE):
        gdir = os.path.join(DONE, game)
        if not os.path.isdir(gdir):
            continue
        for song in os.listdir(gdir):
            sdir = os.path.join(gdir, song)
            if not os.path.isdir(sdir):
                continue
            music = []
            cdir = os.path.join(sdir, "Cancion")
            if os.path.isdir(cdir):
                music += [os.path.join(cdir, f) for f in os.listdir(cdir) if f.lower().endswith(AUDIO)]
            music += [os.path.join(sdir, f) for f in os.listdir(sdir)
                      if f.lower().endswith(AUDIO) and f.lower().startswith(("song", "bgm", "music"))]
            if not music:
                continue
            name = re.sub(r"\s*\[[^\]]*\]\s*$", "", song)
            for key in {fb.norm(name), fb.norm(fb.romanize(name))}:
                if key:
                    idx.setdefault(key, []).append((game, sdir, music))
    return idx


def main_bgm(sdir):
    files = {n.lower(): n for n in os.listdir(sdir)}
    best = None
    for c in os.listdir(sdir):
        if not c.lower().endswith(audit.CHART_EXT):
            continue
        try:
            _, bgm = sync.chart_times(os.path.join(sdir, c))
        except Exception:
            continue
        for start, name in bgm:
            p = audit.resolve_audio(sdir, name or "", files) if name else None
            d = audit.audio_seconds(p) if p else None
            if d and (best is None or d > best[1]):
                best = (p, d)
        if best:
            break
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv")
    ap.add_argument("--tolerance", type=float, default=3.0)
    ap.add_argument("--packs", nargs="*")
    a = ap.parse_args()
    idx = done_index()
    rows = []
    packs = sorted(p for p in os.listdir(audit.ROOT) if os.path.isdir(os.path.join(audit.ROOT, p)))
    if a.packs:
        packs = [p for p in packs if p in a.packs]
    for pack in packs:
        n = bad = unmatched = 0
        songs = [s for s in sorted(os.listdir(os.path.join(audit.ROOT, pack)))
                 if os.path.isdir(os.path.join(audit.ROOT, pack, s))]
        # The pack's game: the one most of its song names are found in. A name shared with another
        # game ("conflict" is in Arcaea, ChainBeet and Cytus) must not be compared across games.
        votes = {}
        for s in songs:
            for g in {c[0] for c in idx.get(fb.norm(s), [])}:
                votes[g] = votes.get(g, 0) + 1
        game_of_pack = max(votes, key=votes.get) if votes else None
        for song in songs:
            sdir = os.path.join(audit.ROOT, pack, song)
            cands = [c for c in idx.get(fb.norm(song), []) if c[0] == game_of_pack]
            if not cands:
                unmatched += 1
                continue
            mine = main_bgm(sdir)
            if not mine:
                continue
            n += 1
            game, ddir, music = cands[0]
            theirs = [(p, audit.audio_seconds(p)) for p in music]
            theirs = [t for t in theirs if t[1]]
            if not theirs:
                continue
            # The original's closest file in length is the one this song was made from.
            p, d = min(theirs, key=lambda t: abs(t[1] - mine[1]))
            if abs(d - mine[1]) > a.tolerance:
                bad += 1
                rows.append({"pack": pack, "song": song, "game": game, "mine": mine[0], "mine_s": round(mine[1], 1),
                             "original": p, "original_s": round(d, 1)})
        print("%-10s %5d songs compared, %4d with a different music length, %4d not in E:\\.done" %
              (pack, n, bad, unmatched), flush=True)
    if a.csv:
        with open(a.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=["pack", "song", "game", "mine", "mine_s", "original", "original_s"])
            w.writeheader()
            w.writerows(rows)
    print("\n%d songs with a music file of another length than the original's" % len(rows))


if __name__ == "__main__":
    sys.exit(main())
