"""Render a keysounded BMS to audio and find a reference clip inside it.

    py tools\\bms_render_check.py <chart.bms> <reference.wav>

Every sounding event (#xxx01 background, 1x notes, 5x long-note heads) is mixed at the time the
chart's own tempo map gives it, from the files its #WAVxx lines name. The reference -- for MUSYNX
the game's official preview, cut from the real song -- is then located in the mix by normalised
cross-correlation. A correct chart shows the reference at a clean peak (corr well above 0.5); wrong
timing or wrong samples leave it near zero.
"""
import os
import re
import subprocess
import sys
from fractions import Fraction

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fix_musynx_keysounds import BmsTimeline, FFMPEG  # noqa: E402

SR = 11025


def decode(path):
    raw = subprocess.run([FFMPEG, "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True).stdout
    return np.frombuffer(raw, np.float32)


def render(bms):
    text = open(bms, encoding="utf-8-sig", errors="replace").read()
    folder = os.path.dirname(bms)
    wav = {m.group(1).upper(): m.group(2).strip()
           for m in re.finditer(r"^#WAV([0-9A-Z]{2})\s+(.+)$", text, re.M | re.I)}
    tl = BmsTimeline(text)
    events = []
    pending = {}
    for m in re.finditer(r"^#(\d{3})(01|1[1-9]|5[1-9]):(\S+)", text, re.M):
        meas, ch, d = int(m.group(1)), m.group(2), m.group(3)
        n = len(d) // 2
        for i in range(n):
            tok = d[2 * i:2 * i + 2].upper()
            if tok == "00":
                continue
            events.append((tl.time(tl.beat(meas, Fraction(i, n))), ch, tok))
    events.sort()
    cache, out = {}, np.zeros(int(SR * (events[-1][0] + 30)), np.float32)
    for t, ch, tok in events:
        if ch[0] == "5":                                    # 5x: heads sound, tails do not
            if pending.get(ch):
                pending[ch] = False
                continue
            pending[ch] = True
        f = wav.get(tok)
        if not f:
            continue
        if f not in cache:
            p = os.path.join(folder, f)
            cache[f] = decode(p) if os.path.exists(p) else np.zeros(0, np.float32)
        s = cache[f]
        a = int(t * SR)
        out[a:a + len(s)] += s[:max(0, len(out) - a)]
    return out


def locate(mix, ref):
    n = len(ref)
    f = 1 << int(np.ceil(np.log2(len(mix) + n)))
    corr = np.fft.irfft(np.fft.rfft(mix, f) * np.conj(np.fft.rfft(ref, f)), f)[:len(mix) - n + 1]
    local = np.sqrt(np.convolve(mix ** 2, np.ones(n), "valid"))[:len(corr)]
    c = corr / (local * np.sqrt((ref ** 2).sum()) + 1e-9)
    c[local < 0.1 * local.max()] = 0.0            # near-silent windows say nothing
    k = int(np.argmax(c))
    return k / SR, float(c[k])


if __name__ == "__main__":
    mix, ref = render(sys.argv[1]), decode(sys.argv[2])
    at, c = locate(mix, ref)
    print("reference found at %.3f s, corr %.3f" % (at, c))
