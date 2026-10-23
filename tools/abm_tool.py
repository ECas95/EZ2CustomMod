"""The game's .abm textures: a BMP whose header fields at 10/18/22/28 are XORed with fixed keys.

    py tools\\abm_tool.py decode <in.abm> <out.png>
    py tools\\abm_tool.py encode <in.png> <out.abm> [--like original.abm]

decode writes the pixels as a PNG (16-bit 1:5:5:5 and 24/32-bit sources). encode writes a BMP with
the same bit depth and orientation as --like (default 24-bit, bottom-up) and XORs its header.
The XOR is its own inverse, so decode(encode(x)) == x.
"""
import io
import struct
import sys

from PIL import Image

XORS = ((10, 0x109A), (18, 0xCFA1), (22, 0x51AE), (28, 0xB18F))


def xor_header(b):
    b = bytearray(b)
    for off, key in XORS:
        v = struct.unpack_from("<I", b, off)[0] ^ key
        struct.pack_into("<I", b, off, v)
    return b


def abm_to_bmp_bytes(data):
    if data[:2] != b"AW":
        raise ValueError("not an .abm")
    b = xor_header(data)
    b[0:2] = b"BM"
    return bytes(b)


def header(data):
    b = xor_header(data)
    off, = struct.unpack_from("<I", b, 10)
    w, h = struct.unpack_from("<ii", b, 18)
    bpp, = struct.unpack_from("<H", b, 28)
    return off, w, h, bpp


def decode(data):
    """.abm bytes -> RGB PIL image."""
    off, w, h, bpp = header(data)
    if bpp == 16:                      # 1:5:5:5 -- PIL reads 16-bit BMP as 5:6:5, so do it by hand
        ah = abs(h)
        stride = (w * 2 + 3) & ~3
        px = bytearray(w * ah * 3)
        for y in range(ah):
            sy = ah - 1 - y if h > 0 else y
            row = data[off + sy * stride: off + sy * stride + w * 2]
            for x in range(w):
                v = row[2 * x] | (row[2 * x + 1] << 8)
                i = (y * w + x) * 3
                px[i] = ((v >> 10) & 0x1F) << 3
                px[i + 1] = ((v >> 5) & 0x1F) << 3
                px[i + 2] = (v & 0x1F) << 3
        return Image.frombytes("RGB", (w, ah), bytes(px))
    return Image.open(io.BytesIO(abm_to_bmp_bytes(data))).convert("RGB")


def encode(img, like=None):
    """RGB PIL image -> .abm bytes, matching the bit depth / orientation of `like` (.abm bytes)."""
    img = img.convert("RGB")
    w, h = img.size
    bpp, top_down = 24, False
    if like is not None:
        _, _, lh, bpp = header(like)
        top_down = lh < 0
    rows = []
    src = img.tobytes()
    for y in range(h):
        r = src[y * w * 3:(y + 1) * w * 3]
        if bpp == 16:
            out = bytearray()
            for x in range(w):
                R, G, B = r[3 * x], r[3 * x + 1], r[3 * x + 2]
                v = ((R >> 3) << 10) | ((G >> 3) << 5) | (B >> 3)
                out += struct.pack("<H", v)
        elif bpp == 32:
            out = bytearray()
            for x in range(w):
                out += bytes((r[3 * x + 2], r[3 * x + 1], r[3 * x], 0))
        else:
            out = bytearray()
            for x in range(w):
                out += bytes((r[3 * x + 2], r[3 * x + 1], r[3 * x]))
        out += b"\0" * ((-len(out)) % 4)
        rows.append(bytes(out))
    if not top_down:
        rows.reverse()
    pixels = b"".join(rows)
    bmp = bytearray(b"BM")
    bmp += struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54)
    bmp += struct.pack("<IiiHHIIiiII", 40, w, -h if top_down else h, 1, bpp, 0, len(pixels), 2835, 2835, 0, 0)
    bmp += pixels
    out = xor_header(bmp)
    out[0:2] = b"AW"
    return bytes(out)


def main(argv):
    if len(argv) >= 3 and argv[0] == "decode":
        decode(open(argv[1], "rb").read()).save(argv[2])
    elif len(argv) >= 3 and argv[0] == "encode":
        like = open(argv[argv.index("--like") + 1], "rb").read() if "--like" in argv else None
        open(argv[2], "wb").write(encode(Image.open(argv[1]), like))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
