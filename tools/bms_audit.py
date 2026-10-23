"""Audit the converted BMS library for timing and structure problems, without touching any file.

    py tools\\bms_audit.py                     # every pack under BMSData\\5KeyMix
    py tools\\bms_audit.py --packs CHU.1 PIU.2 # some packs
    py tools\\bms_audit.py --csv out.csv       # also one row per chart

Per chart it computes real note times (BPM changes on 03/08, #STOP on 09, measure length on 02)
and looks for what a bad conversion leaves behind:

  past_audio   notes after the song's audio has ended (the main BGM sample: the longest one
               placed on channel 01, from where it is triggered). A converter that got the BPM or
               the tick base wrong stretches the chart past the music -- the clearest offsync sign.
  short        the last note well before the audio ends (< 60%): the opposite stretch, or a
               truncated conversion.
  bgm_late     the main BGM starts late (> 15 s). Informational only: many packs start the music a
               few measures in, so this is only a lead, not a finding.
  overlaps     notes stacked on one key (same tick, or inside a held long note).
  open_ln      LNTYPE 1 long notes with no end.
  bad_bpm      a BPM of 0, negative, or above 1000.
  empty        no playable note.

Keysound charts with no single BGM have no audio to compare against; they only get the
structure checks.
"""
import argparse
import collections
import csv
import os
import re
import sys

try:
    import soundfile
except ImportError:
    soundfile = None

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "EZ2AC_FNEX", "BMSData", "5KeyMix")
CHART_EXT = (".bms", ".bme", ".bml", ".pms")
AUDIO_EXT = (".wav", ".ogg", ".mp3", ".flac")
LINE = re.compile(r"^#(\d{3})([0-9A-Za-z]{2}):(\S*)")
KEYS = {0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x19, 0x21, 0x22, 0x23, 0x24, 0x25, 0x28, 0x29}


def b36(s):
    try:
        return int(s, 36)
    except ValueError:
        return 0


_durations = {}


def audio_seconds(path):
    if path in _durations:
        return _durations[path]
    d = None
    if soundfile is not None:
        try:
            d = soundfile.info(path).duration
        except Exception:
            d = None
    _durations[path] = d
    return d


def resolve_audio(song_dir, name, files):
    """#WAV name -> real file: exact, then the same stem with any audio extension (as players do)."""
    low = name.replace("\\", "/").split("/")[-1].lower()
    if low in files:
        return os.path.join(song_dir, files[low])
    stem = os.path.splitext(low)[0]
    for ext in AUDIO_EXT:
        if stem + ext in files:
            return os.path.join(song_dir, files[stem + ext])
    return None


def parse(path):
    text = open(path, "rb").read().decode("cp932", "replace")
    head = {"bpm": 130.0, "wav": {}, "bpmx": {}, "stop": {}, "lnobj": 0}
    scale = {}
    ev = []                                      # (measure, frac, channel, value)
    for raw in text.splitlines():
        s = raw.strip()
        if not s.startswith("#"):
            continue
        u = s.upper()
        m = LINE.match(s)
        if m:
            meas, ch, data = int(m.group(1)), int(m.group(2), 16) if re.fullmatch(r"[0-9A-Fa-f]{2}", m.group(2)) else -1, m.group(3)
            if ch == 2:
                try:
                    scale[meas] = float(data)
                except ValueError:
                    pass
                continue
            if ch < 0 or len(data) < 2:
                continue
            n = len(data) // 2
            for i in range(n):
                v = data[2 * i:2 * i + 2]
                if v == "00":
                    continue
                ev.append((meas, i / n, ch, v))
            continue
        if u.startswith("#BPM ") or (u.startswith("#BPM") and len(u) > 4 and u[4] in " \t"):
            try:
                head["bpm"] = float(s[4:].strip())
            except ValueError:
                pass
        elif u.startswith("#BPM") and len(s) > 6:
            try:
                head["bpmx"][b36(s[4:6])] = float(s[6:].strip())
            except ValueError:
                pass
        elif u.startswith("#WAV") and len(s) > 6:
            head["wav"][b36(s[4:6])] = s[6:].strip()
        elif u.startswith("#STOP") and len(s) > 7:
            try:
                head["stop"][b36(s[5:7])] = float(s[7:].strip())
            except ValueError:
                pass
        elif u.startswith("#LNOBJ"):
            head["lnobj"] = b36(s[6:].strip()[:2])
    return head, scale, ev


def timeline(head, scale, ev):
    """Seconds at a (measure, frac) position, honouring BPM changes and stops."""
    beat_of = {}
    acc = 0.0
    last = max([e[0] for e in ev] + [0]) + 1
    for m in range(last + 1):
        beat_of[m] = acc
        acc += 4.0 * scale.get(m, 1.0)

    def beat(m, f):
        return beat_of.get(m, acc) + f * 4.0 * scale.get(m, 1.0)

    changes = []                                 # (beat, bpm or None, stop_beats)
    for m, f, ch, v in ev:
        if ch == 3:
            changes.append((beat(m, f), float(int(v, 16)), 0.0))
        elif ch == 8:
            bpm = head["bpmx"].get(b36(v))
            if bpm:
                changes.append((beat(m, f), bpm, 0.0))
        elif ch == 9:
            st = head["stop"].get(b36(v))
            if st:
                changes.append((beat(m, f), None, st / 48.0))   # 192ths of a whole note -> beats
    changes.sort(key=lambda c: c[0])

    def seconds(m, f):
        b = beat(m, f)
        t, cur_b, bpm = 0.0, 0.0, head["bpm"]
        for cb, nb, stop in changes:
            if cb > b or (cb == b and nb is None):
                break
            if bpm > 0:
                t += (cb - cur_b) * 60.0 / bpm
            cur_b = cb
            if nb is not None:
                bpm = nb
            elif bpm > 0:
                t += stop * 60.0 / bpm
        if bpm > 0:
            t += (b - cur_b) * 60.0 / bpm
        return t

    return seconds, changes


def audit_chart(path, song_dir, files):
    head, scale, ev = parse(path)
    seconds, changes = timeline(head, scale, ev)
    issues = []
    bpms = [head["bpm"]] + [c[1] for c in changes if c[1] is not None]
    if any(b <= 0 or b > 1000 for b in bpms):
        issues.append("bad_bpm")

    notes = []                                   # (time, key, pos) heads
    held = {}
    open_ln = 0
    busy = collections.defaultdict(lambda: -1.0)
    overlaps = 0
    lnobj = head["lnobj"]
    for m, f, ch, v in sorted(ev, key=lambda e: (e[0], e[1])):
        base = ch - 0x40 if 0x51 <= ch <= 0x69 else ch
        if base not in KEYS:
            continue
        t = seconds(m, f)
        if 0x51 <= ch <= 0x69:
            if base in held:                     # tail
                busy[base] = t
                del held[base]
                continue
            held[base] = t
        elif lnobj and b36(v) == lnobj:
            busy[base] = t
            continue
        if t <= busy[base] + 1e-6:
            overlaps += 1
        busy[base] = t
        notes.append(t)
    open_ln = len(held)
    if not notes:
        issues.append("empty")
    if overlaps:
        issues.append("overlaps")
    if open_ln:
        issues.append("open_ln")

    # The background: every sample placed on channel 01. The music ends where the last of them
    # does -- a song cut into 24 s segments (Ez-Muse II) is still one song -- and the main BGM is
    # the longest one, which only says something about the whole chart when it covers most of it.
    best, audio_end = None, 0.0
    for m, f, ch, v in ev:
        if ch != 1:
            continue
        name = head["wav"].get(b36(v))
        if not name:
            continue
        p = resolve_audio(song_dir, name, files)
        d = audio_seconds(p) if p else None
        if not d:
            continue
        start = seconds(m, f)
        audio_end = max(audio_end, start + d)
        if best is None or d > best[1]:
            best = (start, d)
    last = max(notes) if notes else 0.0
    first = min(notes) if notes else 0.0
    row = {"notes": len(notes), "first": round(first, 2), "last": round(last, 2),
           "overlaps": overlaps, "open_ln": open_ln}
    if best and audio_end >= 20.0:
        start, dur = best
        row.update(bgm_start=round(start, 2), bgm_end=round(audio_end, 2))
        if last > audio_end + 2.0:
            issues.append("past_audio")
        elif notes and dur >= 0.5 * audio_end and last < start + dur * 0.6:
            issues.append("short")
        if start > 15.0 and dur >= 0.5 * audio_end:
            issues.append("bgm_late")
    row["issues"] = issues
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--packs", nargs="*")
    ap.add_argument("--csv")
    a = ap.parse_args()
    if soundfile is None:
        print("note: soundfile not installed; the audio checks are skipped")

    packs = sorted(p for p in os.listdir(a.root) if os.path.isdir(os.path.join(a.root, p)))
    if a.packs:
        want = {p.lower() for p in a.packs}
        packs = [p for p in packs if p.lower() in want]
    writer = None
    if a.csv:
        fh = open(a.csv, "w", newline="", encoding="utf-8")
        writer = csv.writer(fh)
        writer.writerow(["pack", "song", "chart", "notes", "first", "last", "bgm_start", "bgm_end",
                         "overlaps", "open_ln", "issues"])
    summary = []
    for pack in packs:
        pdir = os.path.join(a.root, pack)
        counts = collections.Counter()
        charts = 0
        for song in sorted(os.listdir(pdir)):
            sdir = os.path.join(pdir, song)
            if not os.path.isdir(sdir):
                continue
            try:
                names = os.listdir(sdir)
            except OSError:
                continue
            files = {n.lower(): n for n in names}
            for c in names:
                if not c.lower().endswith(CHART_EXT):
                    continue
                charts += 1
                try:
                    row = audit_chart(os.path.join(sdir, c), sdir, files)
                except Exception as ex:
                    row = {"issues": ["unreadable:%s" % type(ex).__name__]}
                for i in row["issues"]:
                    counts[i] += 1
                if writer and row["issues"]:
                    writer.writerow([pack, song, c, row.get("notes"), row.get("first"), row.get("last"),
                                     row.get("bgm_start"), row.get("bgm_end"), row.get("overlaps"),
                                     row.get("open_ln"), " ".join(row["issues"])])
        summary.append((pack, charts, counts))
        print("%-10s %5d charts  %s" % (pack, charts,
              "  ".join("%s=%d" % kv for kv in sorted(counts.items())) or "clean"), flush=True)
    print("\nworst packs (share of charts that do not fit their audio: past_audio + short):")
    ranked = sorted(summary, key=lambda s: -(s[2]["past_audio"] + s[2]["short"]) / max(1, s[1]))
    for pack, charts, c in ranked[:30]:
        timing = c["past_audio"] + c["short"]
        if timing:
            print("  %-10s %5.1f%%  past_audio=%d short=%d  bad_bpm=%d overlaps=%d  (of %d)" %
                  (pack, 100.0 * timing / max(1, charts), c["past_audio"], c["short"], c["bad_bpm"],
                   c["overlaps"], charts))


if __name__ == "__main__":
    sys.exit(main())
