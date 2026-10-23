"""py tests\\import_3s_test.py -- 3S chart -> BMS conversion of tools\\import_3s.py."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import import_3s as im

# Two objects on one tick of one channel need two lines; positions reduce to the coarsest grid.
lines = im.measure_lines("01", {0: [(0, "01"), (96, "02"), (0, "03")], 2: [(48, "04")]})
assert lines == ["#00001:0102", "#00001:03", "#00201:00040000"], lines

samples = {0: "kick.wav", 1: "snare.wav"}
events = [
    (3, 0, 1, 0, 6),        # key 1 tap, measure 0
    (3, 0, 1, 1, 6),        # same lane and tick again: dropped
    (8, 96, 1, 1, 6),       # pedal
    (11, 192 + 48, 1, 0, 96),   # turntable long note, measure 1 beat 2 -> ends measure 1 beat 4
    (25, 0, 1, 1, 6),       # background track
    (40, 0, 1, 7, 6),       # unknown sample id: ignored
]
text, used = im.chart_to_bms("T", "", samples, "bgm.wav", events, {0: 150.0, 384: 75.5}, im.LANES_KIND1)
body = text.split("\r\n")
assert "#BPM 150" in body and "#ARTIST Sabin Sound Star" in body and "#LNTYPE 1" in body
assert "#WAV01 kick.wav" in body and "#WAV02 snare.wav" in body and "#WAVZZ bgm.wav" in body
assert "#00011:01" in body                      # the duplicate did not add a second line
assert "#00018:0002" in body                    # pedal at half measure
assert "#00156:0001000100" not in body and "#00156:0101" not in body
assert "#00156:00010001" in body, body          # LN head at 1/4 and tail at 3/4 of measure 1
assert "#00001:02ZZ" not in body and "#00001:02" in body and "#00001:ZZ" in body
assert "#BPM01 75.5" in body and "#00208:01" in body
assert used == {"kick.wav", "snare.wav", "bgm.wav"}
print("import_3s_test OK")

files = {n.lower(): n for n in ["synth1++.wav", "kick_snare1.wav", "SCR017_.wav", "BGM.wav"]}
assert im.find_sample(files, "synth1''.wav") == "synth1++.wav"
assert im.find_sample(files, "kick&snare1.wav") == "kick_snare1.wav"
assert im.find_sample(files, "scr017'.wav") == "SCR017_.wav"
assert im.find_sample(files, "bgm.wav") == "BGM.wav"
assert im.find_sample(files, "nohting.wav") is None
print("find_sample OK")
