"""Which of the game's UI textures could carry text, as contact sheets to review for Korean.

    py tools\\texture_inventory.py count
    py tools\\texture_inventory.py sheets <outdir> [--cell 320x240 --cols 3 --rows 3]

Left out: per-song art (discs, eyecatches, song names, channel art), gameplay note panels, digit
fonts and masks (a mask follows its image). An animation (same name + frame number) is shown as
its first, middle and last frame. Every sheet has a numbered cell per texture; index.csv maps
sheet/cell back to the file.
"""
import collections
import csv
import os
import re
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abm_tool

SYSTEM = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "EZ2AC_FNEX", "system")
SKIP_DIRS = {"disc", "discsmall", "resultdisc", "eyecatch", "songname", "channel_disc", "channel_eyecatch",
             "panel", "lvfont", "largelvfont", "font", "timefont", "timefont_course", "preview", "targetname"}
MASK = re.compile(r"(_m|_mask|mask|_m_\w*)$", re.I)
FRAME = re.compile(r"^(.*?)(\d+)$")


def candidates():
    groups = collections.defaultdict(list)
    for root, dirs, files in os.walk(SYSTEM):
        rel = os.path.relpath(root, SYSTEM)
        parts = {p.lower() for p in rel.split(os.sep)}
        if parts & SKIP_DIRS:
            continue
        for f in files:
            if not f.lower().endswith(".abm"):
                continue
            stem = f[:-4]
            if MASK.search(stem):
                continue
            m = FRAME.match(stem)
            key = (rel, m.group(1) if m else stem)
            groups[key].append((int(m.group(2)) if m else -1, os.path.join(rel, f)))
    out = []
    for key in sorted(groups):
        frames = sorted(groups[key])
        pick = {0, len(frames) // 2, len(frames) - 1}
        out += [frames[i][1] for i in sorted(pick)]
    return out


def sheets(outdir, width=1280, height=960, maxw=640):
    """Shelf-packed at native size (wider than maxw scaled down), so small text stays legible."""
    os.makedirs(outdir, exist_ok=True)
    files = candidates()
    idx, n, c = [], 0, 0
    sheet = draw = None
    x = y = shelf = 0

    def flush():
        if sheet is not None and c:
            sheet.save(os.path.join(outdir, "sheet_%04d.png" % n))

    for rel in files:
        try:
            img = abm_tool.decode(open(os.path.join(SYSTEM, rel), "rb").read())
        except Exception:
            continue
        if img.width > maxw:
            img = img.resize((maxw, max(1, img.height * maxw // img.width)))
        if img.height > height - 14:
            img = img.resize((max(1, img.width * (height - 14) // img.height), height - 14))
        w, h = max(img.width, 60), img.height + 14
        if sheet is not None and x + w > width:
            x, y, shelf = 0, y + shelf + 4, 0
        if sheet is None or y + h > height:
            flush()
            if sheet is not None:
                n += 1
            sheet = Image.new("RGB", (width, height), (60, 0, 60))
            draw = ImageDraw.Draw(sheet)
            x = y = shelf = c = 0
        sheet.paste(img, (x, y + 14))
        draw.text((x + 1, y + 1), "%d.%d" % (n, c), fill=(255, 255, 0))
        idx.append((n, c, rel))
        x += w + 4
        shelf = max(shelf, h)
        c += 1
    flush()
    with open(os.path.join(outdir, "index.csv"), "w", newline="", encoding="utf-8") as fh:
        wr = csv.writer(fh)
        wr.writerow(["sheet", "cell", "file"])
        wr.writerows(idx)
    print(len(idx), "textures,", n + 1, "sheets in", outdir)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "count":
        files = candidates()
        print(len(files), "textures to review")
        print(collections.Counter(f.split(os.sep)[0] for f in files).most_common(40))
    elif len(a) >= 2 and a[0] == "sheets":
        sheets(a[1])
    else:
        print(__doc__)
