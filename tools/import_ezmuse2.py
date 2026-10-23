"""Import Ez-Muse II songs (DXArchive .emb) as a BMSData pack the mod loads as customs.

    py tools\\import_ezmuse2.py                 # every song
    py tools\\import_ezmuse2.py --only 010 AXION # a few, by archive name
    py tools\\import_ezmuse2.py --dry-run        # report only, write nothing

The archives are opened read-only with the user's extractor (extract_emb_dxa.py) and never changed.
Each song becomes BMSData\\5KeyMix\\EZM2\\<archive name>\\ holding:

  - the game's standard charts, <prefix>_5K_<Only|Normal>_<Origin|Advance|Nightmare>.bms. These are
    the six columns musiclist.csv has levels for; "dummy", "- copy" and event/course charts are left
    out. The mod fills at most five difficulty slots, so a song with all six loses Only-Nightmare
    (the Normal one, with scratch, is the native 5K style and stays).
  - only the files those charts reference (#WAV/#BMP), <id>_pre.wav for the preview and the
    1280x720 title card as jacket.png, which the mod turns into the disc art.

#TITLE in the archives is the folder id ("010"), so #TITLE/#ARTIST in the extracted COPY are replaced
with the names from the game's own musiclist.csv, written as UTF-8 (the mod decodes UTF-8 first).
Files already present with the same size are skipped, so an interrupted run can simply be repeated.
"""
import argparse
import csv
import io
import mmap
import os
import re
import sys
import importlib.util

DEFAULT_ROOT = r"I:\ARCADES\EZ2\EZm2"
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "EZ2AC_FNEX", "BMSData", "5KeyMix", "EZM2")
CHART = re.compile(r"^(.+?)_5K_(Only|Normal)_(Origin|Advance|Nightmare)\.bm[se]$", re.I)
REF = re.compile(rb"^#(?:WAV|BMP)[0-9A-Z]{2}[ \t]+(.+?)\s*$", re.I | re.M)
DELETED = "\u524a\u9664"   # "削除": the list's marker for a removed entry, not an artist


def load_dxa(path):
    spec = importlib.util.spec_from_file_location("extract_emb_dxa", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["extract_emb_dxa"] = mod
    spec.loader.exec_module(mod)
    key = mod.DXA_KEY

    # The script XORs byte by byte in Python, which takes minutes for one 175 MB video. Same
    # transform on whole integers: identical output, C speed.
    def key_xor(data, position=0):
        n = len(data)
        if n == 0:
            return b""
        p = position % len(key)
        stream = (key[p:] + key[:p]) * (n // len(key) + 1)
        return (int.from_bytes(data, "little") ^ int.from_bytes(stream[:n], "little")).to_bytes(n, "little")

    mod.key_xor = key_xor
    return mod


def read_archive(dxa, path):
    """(mmap, header, {lowercase path: entry}); caller closes the mmap."""
    fh = open(path, "rb")
    raw = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
    fh.close()
    header, entries = dxa.read_archive_index(raw)
    return raw, header, {e.path.lower(): e for e in entries}


def load_musiclist(dxa, system_emb):
    raw, header, entries = read_archive(dxa, system_emb)
    try:
        text = dxa.extract_entry(raw, header, entries["musiclist.csv"]).decode("cp932", "replace")
    finally:
        raw.close()
    rows = [r for r in csv.reader(io.StringIO(text)) if len(r) > 9 and not r[0].startswith("#")]
    # pass (col 8) is the folder/archive name, bmspass (col 9) the chart file prefix.
    return {r[8].strip().lower(): r for r in rows}, {r[9].strip().lower(): r for r in rows}


def pick_charts(entries):
    """{(style, diff): entry path} for the standard charts, at most five."""
    found = {}
    for path in sorted(entries):
        m = CHART.match(os.path.basename(path))
        if m and "/" not in path:
            found.setdefault((m.group(2).lower(), m.group(3).lower()), path)
    if len(found) > 5:
        found.pop(("only", "nightmare"), None)
    return found


def set_header(data, key, value):
    """Replace the #KEY line of a chart (bytes); prepend it if absent."""
    line = b"#" + key + b" " + value.encode("utf-8")
    pat = re.compile(rb"^#" + key + rb"(?:[ \t][^\r\n]*)?(?=\r?$)", re.I | re.M)   # CRLF charts
    if pat.search(data):
        return pat.sub(lambda _m: line, data, count=1)
    return line + b"\r\n" + data


def placed_missing(data, exists):
    """#WAV files the chart actually plays (BGM 01, P1/P2 lanes, long notes) that exists() rejects.

    Declared-but-unused #WAVs and the BGA frame lists the arcade replaced with its .ogv are
    everywhere in these charts and cost nothing, so only placed samples count.
    """
    text = data.decode("cp932", "replace")
    wav = {m.group(1).upper(): m.group(2).strip()
           for m in re.finditer(r"^#WAV([0-9A-Z]{2})[ \t]+(.+?)\s*$", text, re.M | re.I)}
    used = set()
    for m in re.finditer(r"^#\d{3}([0-9A-Z]{2}):(\S+)", text, re.M | re.I):
        ch = m.group(1).upper()
        if ch == "01" or ch[0] in "1256":
            s = m.group(2)
            used |= {s[i:i + 2].upper() for i in range(0, len(s) - 1, 2)}
    return sorted(wav[i] for i in used if i in wav and not exists(wav[i]))


def resolve(entries, ref):
    """Archive entry for a #WAV/#BMP reference: exact name, then same stem (the engine does too)."""
    name = ref.replace("\\", "/").lower()
    if name in entries:
        return entries[name]
    stem = os.path.splitext(os.path.basename(name))[0]
    for path, e in entries.items():
        if os.path.splitext(os.path.basename(path))[0] == stem:
            return e
    return None


def import_song(dxa, emb, out_root, by_pass, by_bms, dry):
    stem = os.path.splitext(os.path.basename(emb))[0]
    raw, header, entries = read_archive(dxa, emb)
    try:
        charts = pick_charts(entries)
        if not charts:
            return "skip", stem, set(), "no standard 5K chart (%d .bms/.bme are event/course charts)" % sum(
                p.endswith((".bms", ".bme")) for p in entries)
        prefix = CHART.match(os.path.basename(next(iter(charts.values())))).group(1).lower()
        row = by_pass.get(stem.lower()) or by_bms.get(prefix)
        title = row[6].strip() if row else ""
        artist = row[7].strip() if row and row[7].strip() != DELETED else ""

        dst = os.path.join(out_root, stem)
        wanted = {}          # output name -> entry
        missing = set()
        chart_data = {}
        for path in charts.values():
            data = dxa.extract_entry(raw, header, entries[path])
            for ref in REF.findall(data):
                e = resolve(entries, ref.decode("cp932", "replace").strip())
                if e is not None:
                    wanted[os.path.basename(e.path)] = e
            missing.update(placed_missing(data, lambda n: resolve(entries, n) is not None))
            if title:
                data = set_header(data, b"TITLE", title)
            if artist:
                data = set_header(data, b"ARTIST", artist)
            chart_data[os.path.basename(entries[path].path)] = data
        for path, e in entries.items():
            base = os.path.basename(path)
            if "/" in path:
                continue
            if re.search(r"_pre\.(wav|ogg)$", base) and not any(k.lower() == base for k in wanted):
                wanted[os.path.basename(e.path)] = e
        jacket = next((e for p, e in sorted(entries.items()) if p.endswith("_title.png") and "/" not in p), None)
        if jacket is not None:
            wanted["jacket.png"] = jacket

        size = sum(e.data_size for e in wanted.values())
        info = "%d charts, %d files, %.0f MB, title=%r artist=%r%s" % (
            len(charts), len(wanted), size / 2 ** 20, title or "(from chart)", artist or "(from chart)",
            ", missing in the archive too: %s" % ", ".join(sorted(missing)) if missing else "")
        if dry:
            return "ok", stem, missing, info

        os.makedirs(dst, exist_ok=True)
        for name, data in chart_data.items():
            with open(os.path.join(dst, name), "wb") as f:
                f.write(data)
        for name, e in wanted.items():
            target = os.path.join(dst, name)
            if os.path.exists(target) and os.path.getsize(target) == e.data_size:
                continue
            payload = dxa.extract_entry(raw, header, e)
            with open(target + ".part", "wb") as f:
                f.write(payload)
            os.replace(target + ".part", target)
        return "ok", stem, missing, info
    finally:
        raw.close()


def self_check(out_root, allowed):
    """Placed samples missing from an imported folder, beyond those the archive lacked as well."""
    bad = []
    for song in sorted(os.listdir(out_root)):
        d = os.path.join(out_root, song)
        if not os.path.isdir(d):
            continue
        have = {n.lower() for n in os.listdir(d)}
        stems = {os.path.splitext(n)[0] for n in have}

        def exists(ref):
            r = os.path.basename(ref.replace("\\", "/")).lower()
            return r in have or os.path.splitext(r)[0] in stems

        for n in have:
            if n.endswith((".bms", ".bme")):
                with open(os.path.join(d, n), "rb") as f:
                    lost = {x.lower() for x in placed_missing(f.read(), exists)} - allowed.get(song.lower(), set())
                bad += ["%s/%s: %s" % (song, n, x) for x in sorted(lost)]
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=DEFAULT_ROOT, help="folder holding Ez-MuseII\\ and extract_emb_dxa.py")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--only", nargs="*", help="archive names (without .emb) to import")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    dxa = load_dxa(os.path.join(a.root, "extract_emb_dxa.py"))
    data = os.path.join(a.root, "Ez-MuseII", "Data")
    by_pass, by_bms = load_musiclist(dxa, os.path.join(data, "SystemFile.emb"))
    music = os.path.join(data, "Music")
    embs = sorted(f for f in os.listdir(music) if f.lower().endswith(".emb"))
    if a.only:
        want = {o.lower() for o in a.only}
        embs = [f for f in embs if os.path.splitext(f)[0].lower() in want]

    done = skipped = failed = 0
    allowed = {}
    for i, f in enumerate(embs, 1):
        try:
            status, stem, lacking, info = import_song(dxa, os.path.join(music, f), a.out, by_pass, by_bms, a.dry_run)
            allowed[stem.lower()] = {x.lower() for x in lacking}
        except Exception as ex:          # one bad archive must not stop the rest
            status, stem, info = "FAIL", f, "%s: %s" % (type(ex).__name__, ex)
        print("[%3d/%d] %-4s %-22s %s" % (i, len(embs), status, stem, info), flush=True)
        done += status == "ok"; skipped += status == "skip"; failed += status == "FAIL"
    print("\nimported %d, skipped %d, failed %d  ->  %s" % (done, skipped, failed, a.out))
    if not a.dry_run and os.path.isdir(a.out):
        bad = self_check(a.out, allowed)
        print("self-check: %s" % ("every placed sample is there" if not bad else "%d placed samples lost:" % len(bad)))
        for b in bad[:20]:
            print("   " + b)
        failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
