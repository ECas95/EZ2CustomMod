"""Re-letter the game's Korean UI textures in English.

    py tools\\texture_translate.py build      # translation\\spec.py -> EZ2AC_FNEX\\Translation\\system\\...
    py tools\\texture_translate.py preview    # the same, plus before/after sheets in translation\\preview

The originals are only read. Each rewritten .abm goes to EZ2AC_FNEX\\Translation\\system\\<same path>,
which the mod serves in place of system\\<same path> (EnglishTextures=1 in BMSMod.ini).

A spec entry names a file and a list of text boxes. For each box the Korean is found inside it
(content rows against the box's own background), wiped by interpolating the background across it
row by row, and the English is drawn in the colour and size the original used:

    ("SongSelect\\Effector\\effector_4d_off.abm", [Box(58, 0, 512, 64, "No effect")])

Box(x0, y0, x1, y1, text, align="left"|"center"|"right", lines=None, size=None, colors=None)
  text     one line, or several separated by "\\n" (drawn on the rows the Korean occupied)
  colors   per line, or per "|"-separated segment; default = the colour sampled from the original
Everything is in the texture's own pixels.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abm_tool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYSTEM = os.path.join(ROOT, "EZ2AC_FNEX", "system")
OUT = os.path.join(ROOT, "EZ2AC_FNEX", "Translation", "system")
FONT = r"C:\Windows\Fonts\bahnschrift.ttf"


class Box:
    """ink: "auto" (differs from the box's median), "white" (bright, unsaturated: text on a busy
    picture). text "" only erases. weight "bold"/"regular"; lines "|"-segments may start with "~"
    to be drawn regular in a bold line."""
    def __init__(self, x0, y0, x1, y1, text, align="left", size=None, colors=None, threshold=48, pad=2,
                 ink="auto", weight="bold", stroke=None, grow=2, fill="rect", glow=False):
        self.x0, self.y0, self.x1, self.y1 = x0, y0, x1, y1
        self.text, self.align, self.size, self.colors = text, align, size, colors
        self.threshold, self.pad, self.ink, self.weight, self.stroke = threshold, pad, ink, weight, stroke
        self.grow, self.fill, self.glow = grow, fill, glow


def lum(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def ink_mask(px, box):
    reg = px[box.y0:box.y1, box.x0:box.x1].astype(np.float32)
    if box.ink == "white":
        return (reg.min(axis=2) > 165) & (reg.max(axis=2) - reg.min(axis=2) < 60)
    bg = np.median(lum(reg))
    return np.abs(lum(reg) - bg) > box.threshold


def bands(px, box):
    """Rows of the box that hold content, grouped into text lines: [(y0, y1, x0, x1)]."""
    ink = ink_mask(px, box)
    rows = np.where(ink.any(axis=1))[0]
    out = []
    if not len(rows):
        return out
    start = prev = rows[0]
    for r in list(rows[1:]) + [None]:
        if r is not None and r - prev <= 2:
            prev = r
            continue
        cols = np.where(ink[start:prev + 1].any(axis=0))[0]
        out.append((box.y0 + start, box.y0 + prev + 1, box.x0 + cols[0], box.x0 + cols[-1] + 1))
        if r is not None:
            start = prev = r
    return out


def wipe(px, y0, y1, x0, x1, box):
    if box.fill == "rect":
        wipe_rect(px, y0, y1, x0, x1)
        return
    wipe_diffuse(px, y0, y1, x0, x1, box)


def wipe_rect(px, y0, y1, x0, x1):
    """Fill the rectangle by interpolating, per row, between the pixels just outside it: clean on
    the flat and gradient plates almost all of this text sits on."""
    h, w, _ = px.shape
    y0, y1, x0, x1 = max(0, y0), min(h, y1), max(0, x0), min(w, x1)
    for y in range(y0, y1):
        left = px[y, x0 - 1].astype(np.float32) if x0 > 0 else None
        right = px[y, x1].astype(np.float32) if x1 < w else None
        if left is None: left = right if right is not None else np.zeros(3, np.float32)
        if right is None: right = left
        t = np.linspace(0, 1, x1 - x0, dtype=np.float32)[:, None]
        px[y, x0:x1] = (left * (1 - t) + right * t).astype(np.uint8)


def wipe_diffuse(px, y0, y1, x0, x1, box):
    """Erase the text inside the rectangle: only its pixels (and a 2 px halo for antialiasing and
    glow) are replaced, by diffusing the surrounding background into them -- a picture behind the
    text keeps its detail between the letters."""
    h, w, _ = px.shape
    y0, y1, x0, x1 = max(0, y0), min(h, y1), max(0, x0), min(w, x1)
    sub = Box(x0, y0, x1, y1, "", threshold=box.threshold, ink=box.ink)
    m = ink_mask(px, sub)
    for _ in range(box.grow):                                  # dilate
        m = m | np.roll(m, 1, 0) | np.roll(m, -1, 0) | np.roll(m, 1, 1) | np.roll(m, -1, 1)
    # A margin of known pixels around the rectangle to diffuse from.
    Y0, Y1, X0, X1 = max(0, y0 - 3), min(h, y1 + 3), max(0, x0 - 3), min(w, x1 + 3)
    reg = px[Y0:Y1, X0:X1].astype(np.float32)
    hole = np.zeros(reg.shape[:2], bool)
    hole[y0 - Y0:y1 - Y0, x0 - X0:x1 - X0] = m
    known = ~hole
    val = np.where(hole[..., None], 0, reg)
    for _ in range(400):
        acc = np.zeros_like(val); cnt = np.zeros(val.shape[:2], np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            k = np.roll(known, (dy, dx), (0, 1)); v = np.roll(val, (dy, dx), (0, 1))
            acc += v * k[..., None]; cnt += k
        fill = hole & (cnt > 0)
        val[fill] = acc[fill] / cnt[fill][:, None]
        newly = fill & ~known
        known = known | newly
        if known.all():
            # a few smoothing passes so the filled area is not blocky
            for _ in range(8):
                acc = sum(np.roll(val, s, a) for s, a in ((1, 0), (-1, 0), (1, 1), (-1, 1))) / 4
                val[hole] = acc[hole]
            break
    px[Y0:Y1, X0:X1] = np.clip(val, 0, 255).astype(np.uint8)


def ink_color(px, y0, y1, x0, x1, box):
    """The text's colour: the pixels that differ most from the background, which is read from a
    ring just outside the text (inside a tight text box the letters themselves are the majority)."""
    h, w, _ = px.shape
    Y0, Y1, X0, X1 = max(0, y0 - 3), min(h, y1 + 3), max(0, x0 - 3), min(w, x1 + 3)
    ring = np.concatenate([px[Y0, X0:X1], px[Y1 - 1, X0:X1], px[Y0:Y1, X0], px[Y0:Y1, X1 - 1]]).astype(np.float32)
    bg = np.median(lum(ring))
    reg = px[y0:y1, x0:x1].reshape(-1, 3).astype(np.float32)
    d = np.abs(lum(reg) - bg)
    if box.ink == "white":
        d = np.where(reg.max(axis=1) - reg.min(axis=1) < 60, d, 0)
    if d.max() <= 0:
        return (255, 255, 255)
    top = reg[d >= np.percentile(d, 95)]
    return tuple(int(v) for v in top.mean(axis=0))


_FONTS = {}


def glow_colors(px, y0, y1, x0, x1):
    """Glowing lettering: the fill is its brightest pixels, the glow its most saturated ones."""
    reg = px[max(0, y0):y1, max(0, x0):x1].reshape(-1, 3).astype(np.float32)
    l = lum(reg)
    fill = reg[l >= np.percentile(l, 96)].mean(axis=0)
    sat = reg.max(axis=1) - reg.min(axis=1)
    cand = reg[(sat >= np.percentile(sat, 90)) & (l > 40)]
    glow = cand.mean(axis=0) if len(cand) else fill * 0.5
    return tuple(int(v) for v in fill), tuple(int(v) for v in glow)


def font(size, weight, width=100):
    """Bahnschrift, the DIN the game's own Latin lettering resembles, at a weight and a width
    (100 normal .. 75 condensed). Build time only: the result is baked into the texture."""
    key = (size, weight, width)
    if key not in _FONTS:
        f = ImageFont.truetype(FONT, size)
        f.set_variation_by_axes([700 if weight == "bold" else 400, width])
        _FONTS[key] = f
    return _FONTS[key]


def seg_len(size, weight, width, line):
    return sum(font(size, "regular" if s.startswith("~") else weight, width).getlength(s.lstrip("~"))
               for s in line.split("|"))


def fit_font(text, height, avail, weight):
    """Largest (size, width) that fits: the Korean's ink height sets the size (Latin caps drawn as
    tall as the Hangul was), then the letters condense before they shrink."""
    size = max(8, int(round(height * 1.12)))
    while size > 7:
        for width in (100, 90, 80, 75):
            if seg_len(size, weight, width, text) <= avail:
                return size, width
        size -= 1
    return 7, 75


def render(src_rel, boxes):
    data = open(os.path.join(SYSTEM, src_rel), "rb").read()
    img = abm_tool.decode(data)
    px = np.array(img)
    w, h = img.size
    draws = []
    for b in boxes:
        found = bands(px, b)
        if not found:
            raise ValueError("%s: no text inside %s" % (src_rel, (b.x0, b.y0, b.x1, b.y1)))
        lines = b.text.split("\n")
        # One English line per Korean line; if the counts differ, spread over the whole block.
        if len(lines) != len(found):
            y0 = min(f[0] for f in found); y1 = max(f[1] for f in found)
            step = (y1 - y0) / len(lines)
            hgt = max(f[1] - f[0] for f in found)
            slots = [(int(y0 + i * step + (step - hgt) / 2), int(y0 + i * step + (step + hgt) / 2),
                      min(f[2] for f in found), max(f[3] for f in found)) for i in range(len(lines))]
        else:
            slots = found
        colors = [ink_color(px, f[0], f[1], f[2], f[3], b) for f in found]
        if b.glow:
            fills = [glow_colors(px, f[0] - 3, f[1] + 3, f[2] - 3, f[3] + 3) for f in found]
            colors = [c[0] for c in fills]
            b.stroke = fills[0][1]
            b.grow = max(b.grow, 3)
        for f in found:
            wipe(px, f[0] - b.pad, f[1] + b.pad, f[2] - b.pad, f[3] + b.pad, b)
        if not b.text:
            continue                                   # erase only
        for i, (line, slot) in enumerate(zip(lines, slots)):
            col = colors[min(i, len(colors) - 1)]
            if b.colors:
                col = b.colors[min(i, len(b.colors) - 1)]
            height = slot[1] - slot[0]
            if b.align == "left":
                avail = b.x1 - slot[2]
            elif b.align == "right":
                avail = slot[3] - b.x0
            else:
                avail = b.x1 - b.x0
            fit = fit_font(line, b.size or height, avail - 2, b.weight)
            draws.append([line, slot, fit, col, b])
        mine = [dd for dd in draws if dd[4] is b]
        common = min(dd[2] for dd in mine)
        for dd in mine:
            dd[2] = common
    out = Image.fromarray(px)
    d = ImageDraw.Draw(out)
    for line, slot, (size, width), col, b in draws:
        fb, fr = font(size, b.weight, width), font(size, "regular", width)
        cy = (slot[0] + slot[1]) / 2
        segs = line.split("|")
        cols = list(col) if isinstance(col, list) else [col]
        cols += [cols[-1]] * (len(segs) - len(cols))
        total = seg_len(size, b.weight, width, line)
        if b.align == "left":
            x = slot[2]
        elif b.align == "right":
            x = slot[3] - total
        else:
            x = (b.x0 + b.x1) / 2 - total / 2
        for s, c in zip(segs, cols):
            f = fr if s.startswith("~") else fb
            s = s.lstrip("~")
            kw = {"stroke_width": 2 if b.glow and size >= 14 else 1, "stroke_fill": tuple(b.stroke)} if b.stroke else {}
            d.text((x, cy), s, font=f, fill=tuple(c), anchor="lm", **kw)
            x += f.getlength(s)
    return img, out, data


def build(preview=False):
    sys.path.insert(0, os.path.join(ROOT, "translation"))
    import spec
    made = 0
    pairs = []
    for rel, boxes in spec.SPEC:
        before, after, data = render(rel, boxes)
        dst = os.path.join(OUT, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(abm_tool.encode(after, data))
        made += 1
        pairs.append((rel, before, after))
    if preview:
        pdir = os.path.join(ROOT, "translation", "preview")
        os.makedirs(pdir, exist_ok=True)
        page, y, n = None, 0, 0
        for rel, a, b in pairs:
            s = min(1.0, 600 / a.width)
            a2 = a.resize((max(1, int(a.width * s)), max(1, int(a.height * s))))
            b2 = b.resize(a2.size)
            hh = a2.height + 16
            if page is None or y + hh > 1000:
                if page is not None:
                    page.save(os.path.join(pdir, "preview_%03d.png" % n)); n += 1
                page, y = Image.new("RGB", (1250, 1000), (60, 0, 60)), 0
            page.paste(a2, (0, y + 14)); page.paste(b2, (640, y + 14))
            ImageDraw.Draw(page).text((2, y + 1), rel, fill=(255, 255, 0))
            y += hh + 2
        if page is not None:
            page.save(os.path.join(pdir, "preview_%03d.png" % n))
    print(made, "textures written to", OUT)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("build", "preview"):
        build(preview=sys.argv[1] == "preview")
    else:
        print(__doc__)
