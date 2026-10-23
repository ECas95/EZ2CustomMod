# Research: how EZ2AC FNEX works

A study of the arcade rhythm game **EZ2AC FNEX** (Final EX): its executable, engine, file formats,
timing and scoring rules, and song select. The study is what made EZ2CustomMod possible: loading a
library of 23,587 BMS songs into the game, on its original Windows XP hardware, without modifying
the game's files. Chapters 1–8 characterise the game and the conversion; chapters 9–13 document
every feature the mod adds and the machinery behind it.

> **Private material.** Parts of this study draw on the private EZ2DECOMP project and on analysis of
> a commercial game. Keep this repository private. The study reproduces no bytes of the original
> executable, no assets and no key tables. It describes the licence protection only as far as
> needed to explain why the analysis targets an unpacked image, and gives no procedure for removing
> it.

## Chapters

| # | Chapter | What it covers |
|---|---|---|
| 1 | [Method](research/01-method.md) | The five lines of evidence (four decompilers, a byte-matching decompilation, the mod as a probe, library-wide sweeps, cabinet measurements) and how claims were verified |
| 2 | [The binary and its protection](research/02-binary-and-protection.md) | The retail protector at the level of analysis, the unpacked image, its sections, the compiler (Visual C++ 7.1) and what follows from it, and how 2EZConfig launches the game |
| 3 | [Engine architecture](research/03-engine-architecture.md) | Directors and engine services; the Direct3D 7 renderer, textures and fonts; DirectSound; input and the turntable; configuration and memory arenas; key globals |
| 4 | [File formats](research/04-file-formats.md) | The file obfuscation's structure, charts (`.ez` v4–v8), keysound indexes, samples, per-chart `.ini`, mode descriptors, the song database, textures, animated backgrounds and videos |
| 5 | [Timing, judgement and scoring](research/05-timing-judgement-scoring.md) | The tick clock, how a press becomes a judgement, judgement windows, hold notes, the gauge, both scoring models and the note-order options |
| 6 | [Song select](research/06-song-select.md) | The song-select object and its 884-byte song entry, titles as pictures, the 47 categories, the category seed, per-mode input and the hover preview |
| 7 | [From BMS to FNEX](research/07-bms-to-fnex.md) | Channel mapping, the 384-tick grid, long notes, folding charts onto five keys, AutoChart's learned model, rating charts from their notes, difficulties, text, audio and video |
| 8 | [Performance on the cabinet](research/08-performance-on-xp.md) | The cabinet's limits, the start-up timeline, the two song-select freezes and their causes, graphics-resource budgets and the rules that came out of it |
| 9 | [Song select: what the mod adds](research/09-song-select-extras.md) | Categories for 23,587 songs and their automatic split, the RANDOM row, FAVORITES / RECENT / MOST PLAYED, sorting and jumps, custom Top-5, the EZ column, the notice card, the detail panels, constant speed, custom courses, category backgrounds |
| 10 | [Lane cover and play options](research/10-lane-cover-and-play-options.md) | The SUDDEN+ cover: where it is drawn, the sliding picture, sharing E1 with bass boost, the turntable and upload bugs found on the cabinet, the picker; the judge offset and its statistics; the frame limiter |
| 11 | [Backgrounds, previews and audio](research/11-bga-previews-and-audio.md) | Choosing a background, DirectShow and ffmpeg playback, frame pacing, image BGA, the disc and audio previews, keysound decoding and prefetch |
| 12 | [Generated art, text and graphics resources](research/12-art-text-and-resources.md) | The art roles, titles on one or two lines, Shift-JIS / EUC-KR detection and the bundled CJK font, the art thread, the RAM store, the texture ring, panel rendering rules, English menus |
| 13 | [Scan, cache, configuration and diagnostics](research/13-scan-cache-config-diagnostics.md) | Start-up, the scan and its cache, permanent ids, every configuration key and the memory tiers, the window, the log, the diagnostics, the maintenance tools and the tests |

A condensed version is available as a technical report: [paper.pdf](paper/paper.pdf)
([HTML](paper/paper.html); rebuilt with `py -3 docs/paper/build_paper.py --pdf`).

## Main findings

- **The retail executable is AES-protected** with a licence-derived key, so all analysis targets an
  unpacked image. That image's fidelity to the retail build can be checked without the key, because
  AES-ECB maps every 16-byte block one-to-one (31,983 blocks, zero conflicts).
- **The program has fixed addresses and a 53.7 MB static data section.** Most of its state lives in
  fixed global tables, and most of its limits are the sizes of those tables.
- **Judgement is done in chart ticks of 1/192 of a beat, not in milliseconds.** Windows are
  therefore beat-relative, every chart ships its own (327 distinct sets), and the game widens them by
  3 ticks and subtracts a fixed 5-tick latency compensation from every press.
- **A hold note is graded at both ends but counted once**; the average of the two grades is computed
  and then discarded. Holds pay out in instalments, and the counter that sets the maximum score and
  the function that pays instalments disagree for one note kind.
- **The key modes score a flat 300 / 150 / 41 per KOOL / COOL / GOOD**, with a maximum of
  `notes × 300`. Ez2Catch uses a different, logarithmic combo model.
- **The `.ini` gauge rates are read by position**, so their names are crossed relative to the grades
  they apply to, and KOOL's rate is COOL's.
- **Song titles are pictures**, not text, and song select was built around about 430 songs: one
  385 KB table on the stack per category, and a linear search per song.
- **File "encryption" is a 512-byte additive keystream with a fixed permutation**, the same for every
  file of a kind.
- **BMS charts need a finer grid than the game's**: on 192 ticks per measure, 24% of BMS notes land
  between ticks.
- **Declared BMS levels are unreliable at the top** (46% of songs call their hardest chart 18);
  rating charts from peak and average density spreads them over 14 levels.
- **On the cabinet, start-up and song select were dominated by work invisible on a development PC**;
  every fix in chapter 8 came from a cabinet log.
- **The texture pool is a bump allocator keyed by file name, rebuilt silently after every song.**
  Everything the mod draws in song select (the texture ring, the panels, the disc preview, the lane
  cover) follows from that: fixed budgets, unique names, and a check that each texture still points
  at the mod's own surface.
- **The engine's derived input is shaped for its own screens.** The turntable's running total is
  zeroed at a per-screen limit, so the lane cover reads the raw 8-bit position instead.
- **A background needs its own clock.** Frames from ffmpeg arrive at decode speed. Pacing them against
  the 30 fps stream, and gating the first frame on the chart's BGA time, is what keeps them in sync.
- **A BMS title's encoding can be inferred from what its bytes spell**: kana and kanji for Shift-JIS,
  the 2,350 common Hangul syllables for EUC-KR.

## Conventions

- Addresses are virtual addresses in the unpacked image (base `0x400000`). Offsets written `+0x…` are
  relative to the object being discussed.
- "Matched" means reconstructed by EZ2DECOMP and verified byte-identical to the original function.
- Counts over "the library" refer to the shipped game data unless the text says the BMS library.

## Sources and acknowledgements

- **EZ2DECOMP**, a private byte-matching decompilation of EZ2AC, whose matched reconstructions and
  documentation underpin chapters 2 to 6.
- **freem**'s `ez2stuff` (`formats.md` and tools), the earliest public description of the containers.
- **2EZConfig-V2** by kasaski (GPL-3.0), a rewrite of ben-rnd's 2EZConfig: the launcher, I/O emulation
  and patch library the game runs under.
- The community converters and song-database editor in `ExternalTools` / `EZ2Tools`.
- The mod's own logs, tests and source comments, which record most measurements quoted here.
