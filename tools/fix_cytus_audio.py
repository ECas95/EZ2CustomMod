"""Give Cytus II charts that have their own music per difficulty that music back.

    py tools\\fix_cytus_audio.py            # report
    py tools\\fix_cytus_audio.py --apply    # copy the audio in and point the charts at it (backed up)

Three Cytus II songs play a different cut of their music per difficulty (song_hard.mp3,
song_chaos.mp3, song_glitch.mp3 next to song.mp3). The copies in BMSData got one file for all of
them -- LAST Re;SORT got the 20 s preview, so every chart ran two minutes past its music. Each
BMSData chart is paired with the converter's chart it shares most note times with (not by rank:
LAST Re;SORT has CHAOS as its 04 and GLITCH as its 03), and takes that chart's music file.
"""
import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_audit as audit
import fix_bangdream as fb

DONE = r"E:\.done\Cytus 2"
PACKS = ("CYT-2.1", "CYT-2.2", "CYT-2.3")
BACKUP = os.path.normpath(os.path.join(audit.ROOT, "..", "..", "BMSData_backup_cytus_audio"))
MUSIC = re.compile(rb"^#WAV([0-9A-Za-z]{2})\s+(\S.*?)\s*$", re.M | re.I)


def music_of(path):
    """(wav id, file) of the chart's music: the #WAV named song*."""
    for m in MUSIC.finditer(open(path, "rb").read()):
        name = m.group(2).decode("utf-8", "replace")
        if os.path.basename(name).lower().startswith("song"):
            return m.group(1).decode(), name
    return None, None


def note_times(path):
    head, scale, ev = audit.parse(path)
    seconds, _ = audit.timeline(head, scale, ev)
    return {round(seconds(m, f), 3) for m, f, ch, v in ev if 0x11 <= ch <= 0x19 or 0x51 <= ch <= 0x59}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()
    done = {}
    for s in os.listdir(DONE):
        sd = os.path.join(DONE, s)
        if os.path.isdir(sd) and any(re.match(r"song_.+\.(mp3|ogg|wav)$", f, re.I) for f in os.listdir(sd)):
            done[fb.norm(re.sub(r"\s*\[[^\]]*\]\s*$", "", s))] = sd
    for pack in PACKS:
        pdir = os.path.join(audit.ROOT, pack)
        for song in sorted(os.listdir(pdir)):
            src = done.get(fb.norm(song))
            if not src:
                continue
            sdir = os.path.join(pdir, song)
            mine = sorted(f for f in os.listdir(sdir) if f.lower().endswith(".bms"))
            theirs = sorted(f for f in os.listdir(src) if f.lower().endswith(".bms"))
            # Paired by shared note times, not by rank: LAST Re;SORT's "04 Expert" is the CHAOS
            # chart and its "03 Hard" the GLITCH one.
            their_times = {t: note_times(os.path.join(src, t)) for t in theirs}
            pairs = []
            for m in mine:
                mt = note_times(os.path.join(sdir, m))
                best = max(theirs, key=lambda t: len(mt & their_times[t])) if theirs else None
                if best and len(mt & their_times[best]) >= 0.5 * max(1, len(mt)):
                    pairs.append((m, best))
            for m, t in pairs:
                _, want = music_of(os.path.join(src, t))
                wid, have = music_of(os.path.join(sdir, m))
                if not want or not wid:
                    continue
                want_file = os.path.basename(want)
                source_audio = os.path.join(src, want_file)
                if not os.path.exists(source_audio):
                    source_audio = os.path.join(src, "Cancion", want_file)
                target_audio = os.path.join(sdir, want_file)
                same_file = os.path.exists(target_audio) and os.path.getsize(target_audio) == os.path.getsize(source_audio)
                if have.lower() == want_file.lower() and same_file:
                    continue
                print("  %s %s/%s/%s: music %s -> %s" % ("fixed" if a.apply else "would fix", pack, song, m,
                                                        have, want_file))
                if not a.apply:
                    continue
                for f in (m, have):
                    p = os.path.join(sdir, f)
                    bk = os.path.join(BACKUP, pack, song, f)
                    if os.path.exists(p) and not os.path.exists(bk):
                        os.makedirs(os.path.dirname(bk), exist_ok=True)
                        shutil.copy2(p, bk)
                if not same_file:
                    shutil.copy2(source_audio, target_audio)
                data = open(os.path.join(sdir, m), "rb").read()
                data = re.sub(rb"^#WAV" + wid.encode() + rb"\s+\S.*?(\r?)$",
                              lambda mm: b"#WAV" + wid.encode() + b" " + want_file.encode() + mm.group(1),
                              data, count=1, flags=re.M | re.I)
                open(os.path.join(sdir, m), "wb").write(data)


if __name__ == "__main__":
    sys.exit(main())
