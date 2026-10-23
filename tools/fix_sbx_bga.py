"""SuperBeat Xonic (BMSData\\5KeyMix\\SBX): start the BGA with the music, as the game does.

    py tools\\fix_sbx_bga.py [--apply]

The game starts a song's video together with its audio. Measured with tools\\bga_sync.py over all
89 raw songs (video cuts against audio onsets, summed so the beat-periodic peaks cancel): the video
leads the audio by 0.00-0.07 s. The converter put the BGA (#xxx04) at the start of the chart and the
song (#xxx01) a measure or so later, so every video ran 1.3-3.8 s ahead of its music.

The BGA event is moved to exactly where the song starts -- the same measure and the same slot of
the same grid -- so it also follows any later offset correction made to the music. Charts are
backed up to EZ2AC_FNEX\\BMSData_backup_sbx_bga first.
"""
import glob
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "EZ2AC_FNEX", "BMSData", "5KeyMix", "SBX")
BACKUP = os.path.join(ROOT, "EZ2AC_FNEX", "BMSData_backup_sbx_bga")
LINE = re.compile(r"^#(\d{3})(\d\d):(\S+)\s*$")


def fix(text):
    """The chart with its BGA at the song's position, or None when it already is / cannot tell."""
    lines = text.splitlines()
    wav = {m.group(1).upper(): m.group(2).strip()
           for m in (re.match(r"^#WAV([0-9A-Z]{2})\s+(.+)$", l, re.I) for l in lines) if m}
    songs = [k for k, v in wav.items() if not v.lower().startswith("silent")]
    song_pos = bga_tok = None
    for l in lines:
        m = LINE.match(l)
        if not m:
            continue
        meas, ch, d = int(m.group(1)), m.group(2), m.group(3)
        n = len(d) // 2
        for i in range(n):
            tok = d[2 * i:2 * i + 2].upper()
            if tok == "00":
                continue
            if ch == "01" and tok in songs and (song_pos is None or (meas, i / n) < song_pos[:2]):
                song_pos = (meas, i / n, n, i)
            if ch == "04" and bga_tok is None:
                bga_tok = tok
    if song_pos is None or bga_tok is None:
        return None
    meas, _, n, i = song_pos
    new = "#%03d04:%s" % (meas, "".join(bga_tok if k == i else "00" for k in range(n)))
    out, placed = [], False
    for l in lines:
        m = LINE.match(l)
        if m and m.group(2) == "04":
            if not placed:
                out.append(new)
                placed = True
            continue
        out.append(l)
    if out == lines:
        return None
    return "\r\n".join(out) + "\r\n"


def main(apply):
    changed = skipped = 0
    for p in sorted(glob.glob(os.path.join(PACK, "*", "*.bms"))):
        raw = open(p, "rb").read()
        text = raw.decode("utf-8", errors="surrogateescape")
        new = fix(text)
        if new is None:
            skipped += 1
            continue
        changed += 1
        if apply:
            rel = os.path.relpath(p, PACK)
            bk = os.path.join(BACKUP, rel)
            os.makedirs(os.path.dirname(bk), exist_ok=True)
            if not os.path.exists(bk):
                shutil.copy2(p, bk)
            open(p, "wb").write(new.encode("utf-8", errors="surrogateescape"))
    print("%d charts %s, %d left as they were" % (changed, "fixed" if apply else "to fix", skipped))


if __name__ == "__main__":
    main("--apply" in sys.argv)
