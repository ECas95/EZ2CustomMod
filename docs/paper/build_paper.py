#!/usr/bin/env python3
"""Builds docs/paper/paper.html (and, with --pdf, paper.pdf through headless Edge).

The two figures are computed here from the data quoted in the text, so the numbers in the plots and
the numbers in the tables cannot drift apart:
  Figure 1  judgement windows in milliseconds against tempo: window_ticks * 312.5 / BPM
  Figure 2  the cabinet's start-up timeline, from its log of 2026-10-19 (v36.0.0)

    py -3 docs/paper/build_paper.py          # HTML only
    py -3 docs/paper/build_paper.py --pdf    # HTML + PDF
"""
import html
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Default .ini windows (Kool, Cool, Good, Miss) widened by the engine's +3 ticks.
WINDOWS = [("KOOL", 9), ("COOL", 27), ("GOOD", 39), ("MISS", 75)]
SERIES_COLOURS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]   # validated categorical slots 1-4
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e2e1dc"

# Start-up steps (ms), consecutive timestamps of the cabinet log.
STARTUP = [
    ("Load scan cache (23,587 songs)", 0, 1063),
    ("Load AutoChart model", 1063, 1078),
    ("Rate 84,210 charts from notes", 1078, 4563),
    ("AutoChart: 188,696 virtual charts", 4563, 6860),
    ("Verify catalogue order", 6860, 14344),
    ("Index catalogue", 14344, 15438),
    ("Size song select", 15438, 22641),
    ("Remaining set-up, window released", 22641, 26875),
]


def ms(ticks, bpm):
    return ticks * 312.5 / bpm


def figure_windows():
    w, h, ml, mr, mt, mb = 640, 300, 52, 70, 16, 40
    x0, x1, y0, y1 = 80, 240, 0, 300
    def X(b): return ml + (b - x0) / (x1 - x0) * (w - ml - mr)
    def Y(v): return h - mb - (v - y0) / (y1 - y0) * (h - mt - mb)
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="Judgement windows in milliseconds against tempo">']
    for v in range(0, 301, 50):
        out.append(f'<line x1="{ml}" x2="{w-mr}" y1="{Y(v):.1f}" y2="{Y(v):.1f}" stroke="{GRID}" stroke-width="1"/>')
        out.append(f'<text x="{ml-6}" y="{Y(v)+4:.1f}" text-anchor="end" font-size="11" fill="{INK2}">{v}</text>')
    for b in range(80, 241, 20):
        out.append(f'<text x="{X(b):.1f}" y="{h-mb+16}" text-anchor="middle" font-size="11" fill="{INK2}">{b}</text>')
    out.append(f'<text x="{(ml + w - mr)/2:.0f}" y="{h-6}" text-anchor="middle" font-size="11" fill="{INK2}">Tempo (BPM)</text>')
    out.append(f'<text x="14" y="{(mt + h - mb)/2:.0f}" text-anchor="middle" font-size="11" fill="{INK2}" '
               f'transform="rotate(-90 14 {(mt + h - mb)/2:.0f})">Half-width of window (ms)</text>')
    for (name, ticks), colour in zip(WINDOWS, SERIES_COLOURS):
        pts = " ".join(f"{X(b):.1f},{Y(ms(ticks, b)):.1f}" for b in range(x0, x1 + 1, 2))
        out.append(f'<polyline points="{pts}" fill="none" stroke="{colour}" stroke-width="2" stroke-linejoin="round"/>')
        end = ms(ticks, x1)
        out.append(f'<circle cx="{X(x1):.1f}" cy="{Y(end):.1f}" r="4" fill="{colour}" stroke="#fcfcfb" stroke-width="2"/>')
        out.append(f'<text x="{X(x1)+8:.1f}" y="{Y(end)+4:.1f}" font-size="11" fill="{INK}">{name} ±{ticks}t</text>')
    out.append("</svg>")
    return "\n".join(out)


def figure_startup():
    w, row, ml, mr, mt = 640, 26, 230, 70, 8
    h = mt + row * len(STARTUP) + 30
    total = STARTUP[-1][2]
    def X(t): return ml + t / total * (w - ml - mr)
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="Start-up timeline of the cabinet">']
    for s in range(0, 27, 5):
        x = X(s * 1000)
        out.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{mt}" y2="{mt + row*len(STARTUP)}" stroke="{GRID}" stroke-width="1"/>')
        out.append(f'<text x="{x:.1f}" y="{mt + row*len(STARTUP) + 16}" text-anchor="middle" font-size="11" fill="{INK2}">{s} s</text>')
    for i, (label, a, b) in enumerate(STARTUP):
        y = mt + i * row + 5
        width = max(X(b) - X(a), 2)
        out.append(f'<text x="{ml-8}" y="{y+12}" text-anchor="end" font-size="11" fill="{INK}">{html.escape(label)}</text>')
        out.append(f'<rect x="{X(a):.1f}" y="{y}" width="{width:.1f}" height="16" rx="3" fill="{SERIES_COLOURS[0]}"/>')
        out.append(f'<text x="{X(b)+6:.1f}" y="{y+12}" font-size="11" fill="{INK2}">{(b-a)/1000:.1f} s</text>')
    out.append("</svg>")
    return "\n".join(out)


def table_windows():
    bpms = [90, 120, 150, 180, 200, 240]
    head = "".join(f"<th>{b} BPM</th>" for b in bpms)
    rows = []
    for name, ticks in WINDOWS:
        cells = "".join(f"<td>±{ms(ticks, b):.0f}</td>" for b in bpms)
        rows.append(f"<tr><th>{name} (±{ticks} ticks)</th>{cells}</tr>")
    tick = "".join(f"<td>{312.5/b:.2f}</td>" for b in bpms)
    rows.append(f"<tr><th>1 tick (ms)</th>{tick}</tr>")
    return f"<table><thead><tr><th>Window</th>{head}</tr></thead><tbody>{''.join(rows)}</tbody></table>"


def main():
    with open(os.path.join(HERE, "paper_template.html"), encoding="utf-8") as f:
        page = f.read()
    page = (page.replace("{{FIGURE_WINDOWS}}", figure_windows())
                .replace("{{TABLE_WINDOWS}}", table_windows())
                .replace("{{FIGURE_STARTUP}}", figure_startup()))
    out = os.path.join(HERE, "paper.html")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(page)
    print("wrote", out)
    if "--pdf" in sys.argv:
        edge = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
        pdf = os.path.join(HERE, "paper.pdf")
        subprocess.run([edge, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                        f"--print-to-pdf={pdf}", "file:///" + out.replace("\\", "/")], check=True)
        print("wrote", pdf)


if __name__ == "__main__":
    main()
