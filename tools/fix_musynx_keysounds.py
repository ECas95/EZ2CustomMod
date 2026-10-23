"""Rebuild the MUSYNX pack (BMSData\\5KeyMix\\MUS) from the game's own charts, with its real sound.

    py tools\\fix_musynx_keysounds.py [--apply] [--only 13vis ...] [--jobs 8]

MUSYNX is fully keysounded: every note plays its own sample and the rest of the song is a stream of
automatic samples (MusicNote); there is no song file. The converted charts had one 0.26 s sample
(Song.mp3, the first stream of the song's .acb) on every note and no background, and in the ~100
charts with tempo changes (BPMChanger) the converter's timing was wrong too -- Gooddream ran 226 s
against the game's 153.

So each chart is written again from the raw one (textassets\\<chart>.txt, times in 100 ns):

  - timing: the raw BPM and BPMChanger events (4/4 measures, #BPMxx on channel 08), and every
    event is placed from its raw time, exact to half a millisecond;
  - notes: raw Note / LongNote with their own keysounds, on the game's own lanes (LANES: 4T on
    keys 1,2,(3),4,5; 6T on six keys, which the 5-key mode folds onto five); long notes are
    LNTYPE 1 pairs on 5x;
  - background: every MusicNote on #xxx01;
  - #WAVxx: the raw sample names as .ogg, extracted from StreamingAssets\\song_<id>.acb with
    vgmstream and encoded with ffmpeg; the official preview (songPreview.acb) is written as
    preview.wav -- the largest file in the folder, which is what the mod cuts previews from;
  - #ARTIST (the converter wrote "#BPM 129" there) and #PLAYLEVEL come from composersInfo_en.txt.

The raw files are only read. Originals go to EZ2AC_FNEX\\BMSData_backup_musynx first.
"""
import argparse
import bisect
import concurrent.futures as cf
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "EZ2AC_FNEX", "BMSData", "5KeyMix", "MUS")
BACKUP = os.path.join(ROOT, "EZ2AC_FNEX", "BMSData_backup_musynx")
RAW = r"I:\.BMS\.raw\MUSYNX"
TEXT = os.path.join(RAW, "textassets")
STREAMS = os.path.join(RAW, "nsp", "01007B6006092000", "romfs", "Data", "StreamingAssets")
VGM = os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages"
                         r"\vgmstream.vgmstream_Microsoft.Winget.Source_8wekyb3d8bbwe\vgmstream-cli.exe")
FFMPEG = os.path.join(ROOT, "EZ2AC_FNEX", "ffmpeg-modern.exe")
CHART = re.compile(r"^(.+?)(4T|6T)_(\w+)$", re.I)
B36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
DENOMS = (1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 384, 768, 1536, 3072)


# ---- raw -------------------------------------------------------------------------------------------
class RawChart:
    def __init__(self, path):
        self.wav, self.notes, self.music, self.tempo = {}, [], [], []
        self.bpm = None
        for line in open(path, encoding="utf-8", errors="replace"):
            p = [x.strip() for x in line.rstrip("\r\n").split("\t")]
            k = p[0]
            if k == "BPM" and len(p) >= 2 and self.bpm is None:
                self.bpm = float(p[1])
            elif k == "BPMChanger" and len(p) >= 3:
                self.tempo.append((int(p[1]) / 1e7, float(p[2])))
            elif k == "WAV" and len(p) >= 3:
                self.wav[p[1].upper()] = p[2]
            elif k == "Note" and len(p) >= 4 and p[2] != "100":
                self.notes.append((int(p[1]) / 1e7, int(p[2]), p[3].upper(), None))
            elif k == "LongNote" and len(p) >= 5 and p[2] != "100":
                self.notes.append((int(p[1]) / 1e7, int(p[2]), p[3].upper(), int(p[4]) / 1e7))
            elif k == "MusicNote" and len(p) >= 4:
                self.music.append((int(p[1]) / 1e7, p[3].upper()))
        self.notes.sort(key=lambda n: (n[0], n[1]))
        self.tempo.sort()


def read_composers():
    info = {}
    text = open(os.path.join(TEXT, "composersInfo_en.txt"), encoding="utf-8", errors="replace").read()
    for b in text.split("--++--"):
        lines = [l.rstrip("\r") for l in b.strip("\r\n").split("\n")]
        if not lines or not lines[0].startswith("SongInfo/songInfo_"):
            continue
        comp = vocal = ""
        levels = {}
        for l in lines:
            s = l.strip()
            if s.lower().startswith("composer:"):
                comp = s.split(":", 1)[1].strip()
            elif s.lower().startswith("vocal:"):
                vocal = s.split(":", 1)[1].strip()
            m = re.match(r"^(_\w+)\s+LV\s*(\d+)", s)
            if m:
                levels[m.group(1)] = int(m.group(2))
        info[lines[0][len("SongInfo/songInfo_"):].strip().lower()] = (comp, vocal, levels)
    return info


# ---- BMS timing (used by tools/bms_render_check.py) ------------------------------------------------
class BmsTimeline:
    """Seconds for a BMS position: measure lengths (#xxx02) and BPM changes (#xxx03, #xxx08)."""

    def __init__(self, text):
        self.base = float(re.search(r"^#BPM\s+([\d.]+)", text, re.M | re.I).group(1))
        ext = {m.group(1).upper(): float(m.group(2))
               for m in re.finditer(r"^#BPM([0-9A-Z]{2})\s+([\d.]+)", text, re.M | re.I)}
        self.lens = {int(m.group(1)): float(m.group(2)) for m in re.finditer(r"^#(\d{3})02:([\d.]+)", text, re.M)}
        last = max([int(m.group(1)) for m in re.finditer(r"^#(\d{3})", text, re.M)] + [0])
        self.start = [0.0]
        for k in range(last + 2):
            self.start.append(self.start[-1] + 4.0 * self.lens.get(k, 1.0))
        changes = []
        for m in re.finditer(r"^#(\d{3})(03|08):(\S+)", text, re.M):
            meas, ch, d = int(m.group(1)), m.group(2), m.group(3)
            n = len(d) // 2
            for i in range(n):
                tok = d[2 * i:2 * i + 2].upper()
                v = None if tok == "00" else (int(tok, 16) if ch == "03" else ext.get(tok))
                if v:
                    changes.append((self.beat(meas, Fraction(i, n)), float(v)))
        self.pts = [(0.0, 0.0, self.base)]
        for b, v in sorted(changes):
            b0, t0, bpm = self.pts[-1]
            self.pts.append((b, t0 + (b - b0) * 60.0 / bpm, v))
        self.pb = [p[0] for p in self.pts]

    def beat(self, measure, frac):
        return self.start[measure] + float(frac) * 4.0 * self.lens.get(measure, 1.0)

    def time(self, beat):
        b0, t0, bpm = self.pts[bisect.bisect_right(self.pb, beat) - 1]
        return t0 + (beat - b0) * 60.0 / bpm


# ---- the new timeline, from the raw tempo --------------------------------------------------------------
class Tempo:
    """Raw BPM + BPMChanger as a beat map; 4 beats to the measure."""

    def __init__(self, raw):
        self.pts = [(0.0, 0.0, raw.bpm)]               # (seconds, beat, bpm from here)
        for t, v in raw.tempo:
            t0, b0, bpm = self.pts[-1]
            if t <= t0 + 1e-9:
                self.pts[-1] = (t0, b0, v)
            else:
                self.pts.append((t, b0 + (t - t0) * bpm / 60.0, v))
        self.pt = [p[0] for p in self.pts]

    def beat(self, t):
        t0, b0, bpm = self.pts[bisect.bisect_right(self.pt, t) - 1]
        return b0 + (t - t0) * bpm / 60.0, bpm


def place(events, tempo):
    """{(measure, channel): [(fraction, token)]} -> BMS lines, each on the coarsest grid that keeps
    every event within half a millisecond; simultaneous events on one channel get extra lines."""
    per = {}
    for t, ch, tok in events:
        b, bpm = tempo.beat(t)
        m = int(b // 4 + 1e-9)
        per.setdefault((m, ch), []).append(((b - 4 * m) / 4.0, tok, bpm))
    lines = []
    for (m, ch) in sorted(per):
        evs = per[(m, ch)]
        for den in DENOMS:
            if all(abs(round(f * den) - f * den) * 4.0 * 60.0 / bpm <= 0.0005 for f, _, bpm in evs):
                break
        layers = []
        for f, tok, _ in sorted(evs):
            slot = min(den - 1, int(round(f * den)))
            for lay in layers:
                if lay[slot] == "00":
                    lay[slot] = tok
                    break
            else:
                lay = ["00"] * den
                lay[slot] = tok
                layers.append(lay)
        lines += ["#%03d%s:%s" % (m, ch, "".join(l)) for l in layers]
    return lines


def free_id(taken):
    for a in reversed(B36):
        for b in reversed(B36):
            if a + b != "00" and a + b not in taken:
                return a + b
    return None


# The game's lanes, as BMS keys (the digit after the 1x / 5x channel). Counted over every raw chart:
# 4T plays lanes 3, 4, 6, 7 (and 24 notes on the centre lane 5); 6T plays 2-7 (and 2 notes on 8).
# 6T keeps all six columns -- 11-15 plus the BME sixth key 18 -- and the 5-key mode folds them onto
# five at load (src/lane_fold.h). The first conversion had squeezed 6T onto four keys, and the first
# pass of this tool copied that from the old charts.
LANES = {"4T": {3: 1, 4: 2, 5: 3, 6: 4, 7: 5},
         "6T": {2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 8, 8: 9}}


def rebuild(bms_path, raw_path, level, artist, have, style):
    """`have`: the lowercase stream names the song's .acb really holds. Some songs are not
    keysounded at all -- Gooddream's .acb is one 2:47 stream, BGM1, which its chart starts as a
    MusicNote at 3.0 s, while the chart still names 560 per-part samples that do not exist. Only
    samples that exist are declared; a note naming a missing one stays, silent."""
    raw_bytes = open(bms_path, "rb").read()
    text = raw_bytes.decode("utf-8-sig", errors="replace")
    raw = RawChart(raw_path)
    raw.wav = {k: v for k, v in raw.wav.items() if v.lower() in have}
    lanes = LANES[style.upper()]
    tail = free_id(set(raw.wav) | {n[2] for n in raw.notes})
    tempo = Tempo(raw)
    events = []
    for t, rl, wid, end in raw.notes:
        k = lanes.get(rl)
        if k is None:
            if wid in raw.wav:
                events.append((t, "01", wid))      # a lane the style does not have: keep its sound
            continue
        if end is None:
            events.append((t, "1%d" % k, wid))
        else:
            events.append((t, "5%d" % k, wid))
            events.append((end, "5%d" % k, tail or wid))
    events += [(t, "01", w) for t, w in raw.music if w in raw.wav]
    bpms = sorted({v for _, _, v in tempo.pts[1:]} | set())
    bpm_id = {v: B36[(n + 1) // 36] + B36[(n + 1) % 36] for n, v in enumerate(bpms)}
    events += [(t, "08", bpm_id[v]) for t, _, v in tempo.pts[1:]]
    used = sorted({w for _, ch, w in events if ch not in ("08",) and w in raw.wav})

    head = []
    for line in text.splitlines():
        u = line.upper()
        if re.match(r"^#\d{3}", line) or u.startswith(("#WAV", "#BPM", "#LNOBJ", "#ARTIST", "#PLAYLEVEL")):
            continue
        if line.strip():
            head.append(line)
    title_at = next((i for i, l in enumerate(head) if l.upper().startswith("#TITLE")), len(head) - 1)
    head.insert(title_at + 1, "#ARTIST %s" % artist)
    head.append("#BPM %s" % ("%g" % tempo.pts[0][2]))     # a BPMChanger at 0 replaces the header BPM
    if level:
        head.append("#PLAYLEVEL %d" % level)
    head += ["#BPM%s %s" % (bpm_id[v], "%g" % v) for v in bpms]
    head += ["#WAV%s %s.ogg" % (w, raw.wav[w]) for w in used]
    if tail:
        head.append("#WAV%s _silent.wav" % tail)
    body = place(events, tempo)
    data = ("\r\n".join(head + [""] + body) + "\r\n").encode("utf-8")
    if raw_bytes.startswith(b"\xef\xbb\xbf"):
        data = b"\xef\xbb\xbf" + data
    stats = "%d notes, %d background, %d samples, %d tempo changes" % (
        len(raw.notes), len(raw.music), len(used), len(tempo.pts) - 1)
    return data, {raw.wav[w] for w in used}, stats


# ---- audio -----------------------------------------------------------------------------------------
def unpack(acb):
    """Every stream of an .acb as a .wav in a temporary folder: (folder, {lowercase name: path})."""
    tmp = tempfile.mkdtemp(prefix="musynx_")
    subprocess.run([VGM, "-S", "0", "-o", os.path.join(tmp, "?n.wav"), acb],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    return tmp, {f[:-4].lower(): os.path.join(tmp, f) for f in os.listdir(tmp) if f.lower().endswith(".wav")}


def encode(have, names, dst_dir, jobs):
    def enc(n):
        out = os.path.join(dst_dir, n + ".ogg")
        if not (os.path.exists(out) and os.path.getsize(out) > 0):
            subprocess.run([FFMPEG, "-v", "error", "-y", "-i", have[n.lower()], "-c:a", "libvorbis",
                            "-q:a", "5", out], check=True)
    with cf.ThreadPoolExecutor(jobs) as ex:
        list(ex.map(enc, names))


def extract_previews():
    tmp = tempfile.mkdtemp(prefix="musynx_prev_")
    subprocess.run([VGM, "-S", "0", "-o", os.path.join(tmp, "?n.wav"), os.path.join(STREAMS, "songPreview.acb")],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    return tmp, {f[:-4].lower(): os.path.join(tmp, f) for f in os.listdir(tmp)}


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args(argv)
    comp = read_composers()
    prev_dir, previews = (extract_previews() if a.apply else (None, {}))
    songs = charts = failed = 0
    try:
        for folder in sorted(os.listdir(PACK)):
            sdir = os.path.join(PACK, folder)
            found = [(c, CHART.match(os.path.basename(c)[:-4])) for c in sorted(glob.glob(os.path.join(glob.escape(sdir), "*.bms")))]
            found = [(c, m) for c, m in found if m]
            if not found:
                continue
            sid = found[0][1].group(1)
            if a.only and sid.lower() not in {x.lower() for x in a.only}:
                continue
            acb = os.path.join(STREAMS, "song_%s.acb" % sid)
            if not os.path.exists(acb):
                # A few songs' charts and audio are named apart ("8th" charts, song_8thPlanet.acb).
                near = glob.glob(os.path.join(STREAMS, "song_%s*.acb" % sid))
                if len(near) != 1:
                    print("  %s: no song_%s.acb, left alone" % (folder, sid))
                    continue
                acb = near[0]
            audio_id = os.path.basename(acb)[len("song_"):-len(".acb")]
            c_, v_, levels = comp.get(sid.lower()) or comp.get(audio_id.lower(), ("", "", {}))
            artist = (c_ + (" feat. " + v_ if v_ else "")) or "MUSYNX"
            tmp, have = unpack(acb)
            try:
                names, results, ok = set(), [], True
                bdir = os.path.join(BACKUP, folder)
                for c, m in found:
                    raw = os.path.join(TEXT, os.path.basename(c)[:-4] + ".txt")
                    if not os.path.exists(raw):
                        print("  %s: no raw chart" % os.path.basename(c))
                        ok = False
                        continue
                    key = "_" + m.group(3).lower() + ("6T" if m.group(2).upper() == "6T" else "")
                    # Keys always come from the converter's chart: the backup once one was made.
                    src = os.path.join(bdir, os.path.basename(c))
                    src = src if os.path.exists(src) else c
                    data, used, st = rebuild(src, raw, levels.get(key), artist, have, m.group(2))
                    if data is None:
                        print("  %s: %s -- song left alone" % (os.path.basename(c), st))
                        ok = False
                        continue
                    names |= used
                    results.append((c, data))
                if not ok or not names:
                    if ok:
                        print("  %s: the .acb holds none of its samples -- left alone" % folder)
                    failed += 1
                    continue               # all charts of a song or none: they share the samples
                songs += 1
                charts += len(results)
                if not a.apply:
                    continue
                os.makedirs(bdir, exist_ok=True)
                for c, _ in results:
                    if not os.path.exists(os.path.join(bdir, os.path.basename(c))):
                        shutil.copy2(c, bdir)
                song = os.path.join(sdir, "Song.mp3")
                if os.path.exists(song):
                    if not os.path.exists(os.path.join(bdir, "Song.mp3")):
                        shutil.copy2(song, bdir)
                    os.remove(song)
                encode(have, sorted(names), sdir, a.jobs)
                p = previews.get("preview_%s" % audio_id.lower()) or previews.get("preview_%s" % sid.lower())
                if p:
                    shutil.copyfile(p, os.path.join(sdir, "preview.wav"))
                for c, data in results:
                    open(c, "wb").write(data)
                print("%-28s %d charts, %d samples" % (sid, len(results), len(names)), flush=True)
            finally:
                shutil.rmtree(tmp, ignore_errors=True)
    finally:
        if prev_dir:
            shutil.rmtree(prev_dir, ignore_errors=True)
    print("songs %d (%d charts) %s, %d songs left alone"
          % (songs, charts, "rebuilt" if a.apply else "to rebuild", failed))


if __name__ == "__main__":
    main(sys.argv[1:])
