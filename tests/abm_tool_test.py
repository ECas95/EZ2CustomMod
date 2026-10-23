"""abm_tool: the XOR header round-trips, and decode(encode(img)) is the image (24- and 16-bit)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import abm_tool  # noqa: E402
from PIL import Image  # noqa: E402

img = Image.new("RGB", (5, 3))
img.putdata([(i * 16, 255 - i * 16, (i * 40) % 256) for i in range(15)])
data24 = abm_tool.encode(img)
assert data24[:2] == b"AW"
assert abm_tool.header(data24)[1:] == (5, 3, 24)
assert abm_tool.decode(data24).tobytes() == img.tobytes()

# 16-bit 1:5:5:5 keeps the top five bits of each channel.
like16 = bytearray(data24)
v = abm_tool.xor_header(like16)
v[28:30] = (16).to_bytes(2, "little")
like16 = bytes(abm_tool.xor_header(v))
data16 = abm_tool.encode(img, like16)
assert abm_tool.header(data16)[3] == 16
want = [tuple((c >> 3) << 3 for c in p) for p in img.getdata()]
assert list(abm_tool.decode(data16).getdata()) == want
print("abm_tool_test OK")
