"""How far a music video is from its song: video cuts/motion against audio onsets.

    py tools\\bga_sync.py <video> <audio> [--max 4]

Prints the lag in seconds: positive means the video must start that much LATER than the audio to
line up (its events come early). Two envelopes at 30 steps a second -- the frame-to-frame change of
a 48x27 grey copy of the video, and the spectral flux of the audio -- are normalised and
cross-correlated over +/- --max seconds. Music videos cut and pulse on the beat, so the peak is the
offset; `conf` is how far that peak stands above the rest (below ~3 means no clear answer).
"""
import subprocess
import sys

import numpy as np

FFMPEG = r"C:\EZ2AC\EZ2CustomMod_v25_category_gate_silent_load_fix\EZ2AC_FNEX\ffmpeg-modern.exe"
RATE = 30


def video_envelope(path):
    w, h = 48, 27
    raw = subprocess.run([FFMPEG, "-v", "error", "-i", path, "-vf", "fps=%d,scale=%d:%d" % (RATE, w, h),
                          "-pix_fmt", "gray", "-f", "rawvideo", "-"], capture_output=True).stdout
    f = np.frombuffer(raw, np.uint8).reshape(-1, h, w).astype(np.float32)
    d = np.abs(np.diff(f, axis=0)).mean(axis=(1, 2))
    return np.concatenate([[0.0], d])


def audio_envelope(path):
    sr = 11025
    raw = subprocess.run([FFMPEG, "-v", "error", "-i", path, "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"],
                         capture_output=True).stdout
    a = np.frombuffer(raw, np.float32)
    hop = sr // RATE
    n = len(a) // hop - 1
    win = np.hanning(hop * 2)
    frames = np.stack([a[i * hop:i * hop + hop * 2] * win for i in range(n)])
    mag = np.log1p(np.abs(np.fft.rfft(frames, axis=1)))
    flux = np.maximum(np.diff(mag, axis=0), 0).sum(axis=1)
    return np.concatenate([[0.0], flux])


def norm(x):
    x = x - np.convolve(x, np.ones(RATE) / RATE, mode="same")     # remove the slow trend
    x = np.maximum(x, 0)
    return (x - x.mean()) / (x.std() + 1e-9)


def lag(video, audio, max_s=4.0):
    v, a = norm(video_envelope(video)), norm(audio_envelope(audio))
    m = int(max_s * RATE)
    scores = []
    for k in range(-m, m + 1):
        # video frame i corresponds to audio frame i - k when the video starts k frames later
        if k >= 0:
            x, y = v[:len(a) - k] if len(a) - k > 0 else v[:0], a[k:k + len(v)]
        else:
            x, y = v[-k:], a[:len(v) + k]
        n = min(len(x), len(y))
        scores.append(float(np.dot(x[:n], y[:n]) / max(n, 1)))
    s = np.array(scores)
    best = int(np.argmax(s))
    others = np.concatenate([s[:max(0, best - 5)], s[best + 6:]])
    conf = (s[best] - others.mean()) / (others.std() + 1e-9)
    return (best - m) / RATE, conf


if __name__ == "__main__":
    mx = float(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 4.0
    l, c = lag(sys.argv[1], sys.argv[2], mx)
    print("lag %+.3f s  conf %.1f" % (l, c))
