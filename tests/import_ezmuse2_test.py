"""py tests\\import_ezmuse2_test.py -- header rewrite and placed-sample check of the Ez-Muse II importer."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import import_ezmuse2 as im

# CRLF chart: the existing lines are replaced in place, never duplicated; #TITLEX is not #TITLE.
d = b"#PLAYER 1\r\n#TITLE 010\r\n#ARTIST eoll\r\n#TITLEX y\r\n#00111:01\r\n"
o = im.set_header(im.set_header(d, b"TITLE", "水晶世界"), b"ARTIST", "Eoll")
assert o == "#PLAYER 1\r\n#TITLE 水晶世界\r\n#ARTIST Eoll\r\n#TITLEX y\r\n#00111:01\r\n".encode("utf-8"), o
assert im.set_header(b"#PLAYER 1\r\n", b"TITLE", "x") == b"#TITLE x\r\n#PLAYER 1\r\n"

# Only samples placed on BGM/P1/P2/long-note channels count; declared-only and BGA (04) do not.
c = (b"#WAV01 a.ogg\r\n#WAV02 b.ogg\r\n#WAV03 c.ogg\r\n#WAV04 d.ogg\r\n"
     b"#00101:0100\r\n#00111:0200\r\n#00104:0300\r\n")
assert im.placed_missing(c, lambda n: False) == ["a.ogg", "b.ogg"]
assert im.placed_missing(c, lambda n: n != "b.ogg") == ["b.ogg"]
print("import_ezmuse2_test OK")
