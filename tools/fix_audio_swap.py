"""Replace a song's music with the original's when the charts plainly belong to the original.

    py tools\\fix_audio_swap.py --mismatch mismatch.csv            # report
    py tools\\fix_audio_swap.py --mismatch mismatch.csv --apply    # swap (originals backed up)

audio_mismatch.py lists songs whose music differs in length from the original's (a trimmed file,
a preview, a different master). A name match alone is not proof -- the same title exists in
several games -- so each candidate is TRIED: the song is copied aside with the original music
under the same file name and the same trigger, and both versions are measured against the notes
(fix_offset.song_measure). The swap is kept only when the charts clearly sit on the original
(confidence >= 5, within 40 ms after the detector bias) and did not on the current file
(confidence < 4, or more than 60 ms off). Saika (Cytus 1) is the model case: +169 ms at
confidence 2.9 on its trimmed file, -17 ms at 5.2 on the original.
"""
import argparse
import csv
import os
import shutil
import statistics
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_audit as audit
import fix_offset as fo

BACKUP = os.path.normpath(os.path.join(audit.ROOT, "..", "..", "BMSData_backup_audio_swap"))
BIAS = 0.0116    # detector bias, measured on the verified CHUNITHM charts (fix_offset prints it)
# Pack prefix -> the E:\.done game its songs came from, where it is certain.
PACK_GAMES = {"CHU": "CHUNITHM", "CYT": None, "MUSE": "Muse Dash", "PSK": "Project Sekai", "ARC": "Arcaea",
              "DEE": None, "BD": "Bang Dream", "DJM": "DJMANIAX", "KAL": "KALPA", "SEVENS": "Seven's Code"}


def summary(measure):
    sure = [(lag, conf) for lag, conf in measure.values()]
    if not sure:
        return None, 0.0
    return statistics.median(l for l, _ in sure) - BIAS, statistics.median(c for _, c in sure)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mismatch", required=True)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()
    swapped = kept = 0
    for r in csv.DictReader(open(a.mismatch, encoding="utf-8")):
        sdir = os.path.join(audit.ROOT, r["pack"], r["song"])
        mine, original = r["mine"], r["original"]
        if not (os.path.exists(mine) and os.path.exists(original)):
            continue
        # Only a file from the pack's own game. A song that also exists elsewhere (Project Sekai has
        # many CHUNITHM titles) can line up with that game's master by chance -- taiyoukeidesuko
        # did, 154.6 s against its own 147.2 s -- and swapping in another game's mix is not a fix.
        game = PACK_GAMES.get(r["pack"].split(".")[0].split("-")[0].rstrip("0123456789"))
        if game and r.get("game") != game:
            kept += 1
            continue
        cur_lag, cur_conf = summary(fo.song_measure(sdir))
        tmp = os.path.join(tempfile.gettempdir(), "audio_swap_try")
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.copytree(sdir, tmp)
        shutil.copy2(original, os.path.join(tmp, os.path.basename(mine)))
        new_lag, new_conf = summary(fo.song_measure(tmp))
        fmt = lambda lag, conf: ("%+.0f ms @%.1f" % (lag * 1000, conf)) if lag is not None else "no match"
        current_bad = cur_lag is None or abs(cur_lag) > 0.060 or (cur_conf < 4 and abs(cur_lag) > 0.025)
        new_good = new_lag is not None and new_conf >= 5 and abs(new_lag) <= 0.040
        if not (current_bad and new_good):
            kept += 1
            continue
        swapped += 1
        print("  %s %s/%s: current %s, original music %s" % ("swapped" if a.apply else "would swap",
              r["pack"], r["song"], fmt(cur_lag, cur_conf), fmt(new_lag, new_conf)))
        if a.apply:
            bk = os.path.join(BACKUP, r["pack"], r["song"], os.path.basename(mine))
            os.makedirs(os.path.dirname(bk), exist_ok=True)
            if not os.path.exists(bk):
                shutil.copy2(mine, bk)
            shutil.copy2(original, mine)
    print("\n%d songs %s, %d left as they are" % (swapped, "swapped" if a.apply else "to swap", kept))


if __name__ == "__main__":
    sys.exit(main())
