"""fix_musynx_keysounds / fix_sbx_bga: pairing, tempo map, grid placement, BGA move."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import fix_musynx_keysounds as mx  # noqa: E402
import fix_sbx_bga as sbx  # noqa: E402


class Raw:
    def __init__(self, bpm, tempo):
        self.bpm, self.tempo = bpm, tempo


# Tempo: a change at 0 replaces the header BPM; later ones start new segments.
t = mx.Tempo(Raw(150.0, [(0.0, 80.0), (3.0, 160.0)]))
assert t.pts[0] == (0.0, 0.0, 80.0) and len(t.pts) == 2
assert abs(t.beat(3.0)[0] - 4.0) < 1e-9                  # 3 s at 80 BPM = 4 beats
assert abs(t.beat(3.75)[0] - 6.0) < 1e-9                 # then 160 BPM: 2 beats in 0.75 s

# place(): exact slots, coarsest grid, stacked events split into layers.
lines = mx.place([(0.0, "01", "AA"), (0.75, "01", "BB"), (0.75, "01", "CC")], mx.Tempo(Raw(120.0, [])))
assert lines == ["#00001:AA0000BB00000000", "#00001:000000CC00000000"], lines   # 3/8 of a 2 s measure

# LANES: 6T keeps six distinct columns (the first conversion had squeezed them onto four), in
# left-to-right order; 4T's four lanes land on keys 1, 2, 4, 5.
six = [mx.LANES["6T"][l] for l in range(2, 8)]
assert len(set(six)) == 6 and six == [1, 2, 3, 4, 5, 8], six
assert [mx.LANES["4T"][l] for l in (3, 4, 6, 7)] == [1, 2, 4, 5]

# fix_sbx_bga: the BGA moves to the song's slot and a second pass changes nothing.
chart = "#WAV01 Song.mp3\r\n#WAV02 silent.wav\r\n#BMP01 bga.mp4\r\n#00004:01\r\n#00101:00000100\r\n"
out = sbx.fix(chart)
assert "#00104:00000100" in out and "#00004:" not in out
assert sbx.fix(out) is None
print("fix_musynx_test OK")
