"""Import Sabin Sound Star (3S) songs as a BMSData pack the mod loads as customs.

    py tools\\import_3s.py                    # every song
    py tools\\import_3s.py --only Anyway      # a few, by folder name
    py tools\\import_3s.py --dry-run          # report only, write nothing

Sources are read-only. Each song becomes BMSData\\5KeyMix\\3S\\<folder>\\ with one .bms per chart and
the keysounds it plays.

.sss charts: 3 header bytes, then the payload minus a repeating 128-byte key (the key from the
user's decrypt_sss_batch.py). The payload is a text sample table ("D0 0 kick.wav"; the note name is
the sample id, a line without one is the BGM) followed by the same EZFF/EZTR layout FNEX uses, with
11-byte events instead of 13: u32 tick, u8 type, u8 sample, u8 vel, u8 pan, u8 flags, u16 length.
192 ticks per 4/4 measure. Type 1 is a sound, a length above 6 a long note. Tempo is type 3 on
track 0 with the BPM as f32 at event byte 5; the last one on a tick wins.

Tracks use the FNEX numbering, and the digit in name_<d><diff>.sss is the play style:
  1 (390 charts): T3-T7 five keys, T8 pedal, T11 turntable  -> 5KeyMix, imported here
  2 (226): both sides, 13 lanes; 3 (280): T8-T10 + T17-T19. No matching mode yet, left out.

.wav keysounds: only the first 256 bytes are masked, with the same key read 35 bytes earlier and
four bytes changed (WAV_KEY_FIX). The rest is plain PCM and is copied as it is.

Titles: 3s.exe keeps (title, folder key, artist) as consecutive 4-aligned strings; "none" = no
artist. A title that is not plain ASCII falls back to the folder name (the mod's font is Latin).
"""
import argparse
import importlib.util
import math
import os
import re
import struct
import sys
from collections import defaultdict

DEFAULT_ROOT = r"I:\ARCADES\EZ2\3S"
GAME = "Sabin Sound Star - Renascence Burst"
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "EZ2AC_FNEX", "BMSData", "5KeyMix", "3S")
TICKS_PER_MEASURE = 192
TAP_LEN = 6
WAV_MASKED = 256
WAV_KEY_SHIFT = -35
WAV_KEY_FIX = {96: 0x14, 115: 0x46, 120: 0x46, 124: 0x14}
# 3S track -> BMS channel (hex text). Keys 1-5, turntable, pedal: the FNEX 5K layout.
LANES_KIND1 = {3: "11", 4: "12", 5: "13", 6: "14", 7: "15", 11: "16", 8: "18"}
BGM_ID = 1295          # "ZZ": clear of the 1..256 sample ids
B36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def b36(n):
    return B36[n // 36] + B36[n % 36]


def load_key(root):
    spec = importlib.util.spec_from_file_location("decrypt_sss_batch", os.path.join(root, "decrypt_sss_batch.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.KEY, mod


def unmask(data, stream):
    n = len(data)
    rep = (stream * (n // len(stream) + 1))[:n]
    return bytes((a - b) & 0xFF for a, b in zip(data, rep))


def wav_mask(key):
    k = bytearray(key)
    for i, d in WAV_KEY_FIX.items():
        k[i] = (k[i] + d) & 0xFF
    return bytes(k[(i + WAV_KEY_SHIFT) % 128] for i in range(128))


def titles_from_exe(exe, folders):
    b = open(exe, "rb").read()
    strs, o = [], 0
    while o < len(b):
        e = b.find(b"\0", o)
        if e < 0:
            break
        if e > o:
            strs.append((o, b[o:e]))
        o = (e + 4) & ~3
    index = defaultdict(list)
    for i, (_, s) in enumerate(strs):
        if 1 < len(s) < 64:
            index[s.lower()].append(i)
    out = {}
    for f in folders:
        for i in index.get(f.lower().encode("latin-1", "replace"), []):
            if not (0 < i < len(strs) - 1):
                continue
            if strs[i + 1][1].lower() == strs[i][1].lower():
                continue                       # title == key: the key is the next one
            if strs[i][0] - strs[i - 1][0] > 0x40 or strs[i + 1][0] - strs[i][0] > 0x40:
                continue
            title, artist = strs[i - 1][1], strs[i + 1][1]
            if title == b"none":
                continue
            t = title.decode("latin-1").strip()
            a = artist.decode("latin-1").strip()
            out[f] = (t if t.isascii() and t.isprintable() else f,
                      a if a != "none" and a.isascii() and a.isprintable() else "")
            break
    return out


def parse_chart(dec):
    """(samples {id: file}, bgm file or None, events [(track, tick, type, sample, length)], tempo {tick: bpm})"""
    ez = dec.find(b"EZFF")
    if ez < 0:
        raise ValueError("no EZFF")
    samples, bgm = {}, None
    for line in dec[:ez].decode("latin-1").splitlines():
        parts = line.split()
        if len(parts) >= 3:
            m = re.fullmatch(r"([A-G])(#?)(\d+)", parts[0])
            if m:
                semi = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}[m.group(1)] + bool(m.group(2))
                samples[int(m.group(3)) * 12 + semi] = " ".join(parts[2:])
                continue
        if len(parts) == 2 and parts[1].lower().endswith(".wav"):
            bgm = parts[1]
    b = dec[ez:]
    if struct.unpack_from("<H", b, 0x86)[0] != TICKS_PER_MEASURE:
        raise ValueError("resolution %d" % struct.unpack_from("<H", b, 0x86)[0])
    tempo = {0: struct.unpack_from("<f", b, 0x88)[0]}
    events, off = [], 0x96
    for track in range(struct.unpack_from("<H", b, 0x8c)[0]):
        if b[off:off + 4] != b"EZTR":
            raise ValueError("bad EZTR at %d" % off)
        size = struct.unpack_from("<I", b, off + 74)[0]
        off += 78
        for i in range(0, size - size % 11, 11):
            tick, typ, sample, _v, _p, _f, length = struct.unpack_from("<IBBBBBH", b, off + i)
            if typ == 1:
                events.append((track, tick, typ, sample, length))
            elif typ == 3 and track == 0:
                bpm = struct.unpack_from("<f", b, off + i + 5)[0]
                if 1.0 <= bpm <= 2000.0:
                    tempo[tick] = bpm
        off += size
    return samples, bgm, events, tempo


def measure_lines(channel, items):
    """{measure: [(pos_tick, id)]} -> BMS lines; several lines when two objects share a tick."""
    out = []
    for measure in sorted(items):
        layers = []
        for pos, oid in sorted(items[measure]):
            for layer in layers:
                if pos not in layer:
                    layer[pos] = oid
                    break
            else:
                layers.append({pos: oid})
        for layer in layers:
            g = TICKS_PER_MEASURE
            for pos in layer:
                g = math.gcd(g, pos)
            slots = ["00"] * (TICKS_PER_MEASURE // g)
            for pos, oid in layer.items():
                slots[pos // g] = oid
            out.append("#%03d%s:%s" % (measure, channel, "".join(slots)))
    return out


def chart_to_bms(title, artist, samples, bgm, events, tempo, lanes):
    """(bms text, set of wav files it plays)"""
    chans = defaultdict(lambda: defaultdict(list))
    used, seen = {}, set()

    def put(ch, tick, oid):
        chans[ch][tick // TICKS_PER_MEASURE].append((tick % TICKS_PER_MEASURE, oid))

    for track, tick, _typ, sample, length in events:
        f = samples.get(sample)
        if f is None:
            continue
        oid = b36(sample + 1)
        used[oid] = f
        ch = lanes.get(track)
        if ch is None:                         # background, or a lane this mode has no key for
            put("01", tick, oid)
            continue
        if (ch, tick) in seen:
            continue
        seen.add((ch, tick))
        if length > TAP_LEN:
            ln = "5" + ch[1]
            put(ln, tick, oid)
            put(ln, tick + length, oid)
        else:
            put(ch, tick, oid)
    if bgm:
        used[b36(BGM_ID)] = bgm
        put("01", 0, b36(BGM_ID))

    bpms = {}
    first = tempo.get(0, 120.0)
    for tick, bpm in sorted(tempo.items()):
        if tick == 0:
            continue
        key = "%.4f" % bpm
        if key not in bpms:
            bpms[key] = b36(len(bpms) + 1)
        put("08", tick, bpms[key])

    head = ["", "*---------------------- HEADER FIELD", "",
            "#PLAYER 1", "#GENRE 3S", "#TITLE " + title, "#ARTIST " + (artist or "Sabin Sound Star"),
            "#BPM %s" % ("%.4f" % first).rstrip("0").rstrip("."), "#RANK 3", "#LNTYPE 1", ""]
    head += ["#WAV%s %s" % (oid, f) for oid, f in sorted(used.items())]
    head += ["#BPM%s %s" % (oid, v.rstrip("0").rstrip(".")) for v, oid in bpms.items()]
    body = ["", "*---------------------- MAIN DATA FIELD", ""]
    for ch in sorted(chans):
        body += measure_lines(ch, chans[ch])
    return "\r\n".join(head + body) + "\r\n", set(used.values())


def find_sample(files, name):
    """Folder file for a sample-table name. The shipped files had ' and & replaced when they were
    saved -- "synth1''.wav" is synth1++.wav, "kick&snare1.wav" kick_snare1.wav, "scr017'.wav"
    SCR017_.wav -- so those spellings are tried too."""
    low = name.lower()
    for cand in (low, low.replace("'", "_").replace("&", "_"), low.replace("'", "+").replace("&", "_")):
        if cand in files:
            return files[cand]
    return None


def import_song(folder, src, dst, key, mask, meta, dry):
    charts = sorted(f for f in os.listdir(src) if re.search(r"_1[a-z]+\.sss$", f, re.I))
    if not charts:
        return "skip", "no style-1 chart", set()
    title, artist = meta.get(folder, (folder, ""))
    files = {f.lower(): f for f in os.listdir(src)}
    wavs, missing, out = set(), set(), {}
    for c in charts:
        raw = open(os.path.join(src, c), "rb").read()
        samples, bgm, events, tempo = parse_chart(unmask(raw[3:], key))
        # The chart names each sample by the file it will be copied as.
        samples = {k: find_sample(files, v) or v for k, v in samples.items()}
        bgm = bgm and (find_sample(files, bgm) or bgm)
        text, used = chart_to_bms(title, artist, samples, bgm, events, tempo, LANES_KIND1)
        out[os.path.splitext(c)[0] + ".bms"] = text
        for w in used:
            (wavs if w.lower() in files else missing).add(w.lower())
    info = "%d charts, %d sounds, title=%r artist=%r%s" % (
        len(charts), len(wavs), title, artist, ", missing: " + ", ".join(sorted(missing)) if missing else "")
    if dry:
        return "ok", info, missing
    os.makedirs(dst, exist_ok=True)
    for name, text in out.items():
        with open(os.path.join(dst, name), "w", encoding="utf-8", newline="") as f:
            f.write(text)
    for w in sorted(wavs):
        target = os.path.join(dst, files[w])
        size = os.path.getsize(os.path.join(src, files[w]))
        if os.path.exists(target) and os.path.getsize(target) == size:
            continue
        with open(os.path.join(src, files[w]), "rb") as f:
            data = f.read()
        data = unmask(data[:WAV_MASKED], mask) + data[WAV_MASKED:]
        if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
            raise ValueError("%s did not unmask to a RIFF/WAVE file" % files[w])
        with open(target + ".part", "wb") as f:
            f.write(data)
        os.replace(target + ".part", target)
    return "ok", info, missing


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=DEFAULT_ROOT, help="folder with decrypt_sss_batch.py and the game")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    key, _ = load_key(a.root)
    mask = wav_mask(key)
    game = os.path.join(a.root, GAME, "Game Data")
    data = os.path.join(game, "data")
    folders = sorted((f for f in os.listdir(data) if os.path.isdir(os.path.join(data, f))), key=str.lower)
    if a.only:
        want = {o.lower() for o in a.only}
        folders = [f for f in folders if f.lower() in want]
    meta = titles_from_exe(os.path.join(game, "3s.exe"), folders)

    done = skipped = failed = 0
    for i, f in enumerate(folders, 1):
        try:
            status, info, _ = import_song(f, os.path.join(data, f), os.path.join(a.out, f), key, mask, meta, a.dry_run)
        except Exception as ex:                # one bad song must not stop the rest
            status, info = "FAIL", "%s: %s" % (type(ex).__name__, ex)
        print("[%3d/%d] %-4s %-14s %s" % (i, len(folders), status, f, info), flush=True)
        done += status == "ok"; skipped += status == "skip"; failed += status == "FAIL"
    print("\nimported %d, skipped %d, failed %d  ->  %s" % (done, skipped, failed, a.out))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
