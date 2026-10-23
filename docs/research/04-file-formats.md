# 4. File formats

[← Engine architecture](03-engine-architecture.md) · [Index](../RESEARCH.md) · [Next: timing, judgement and scoring →](05-timing-judgement-scoring.md)

EZ2AC's data is about 290,000 files: `system/` (39,383), `bg/` (35,595), `video/` (455) and `sound/`
(215,552). Most formats descend from Amuse World's EZ2DJ tools of the late 1990s, and some still
carry their original names: `EZFF`, `EZTR`, `STRM`, `SCR0`. Earlier community work by freem
(`ez2stuff/formats.md`) documented the container layouts. This chapter adds what the binary and
whole-library sweeps established, and corrects several points where the earlier descriptions turned
out to be incomplete.

Unless noted, every structure is little-endian.

## 4.1 File encryption

Four functions in the binary decrypt data, and they are selected **by call site, not by file
extension**:

| Function | Address | Used for |
|---|---|---|
| `EzChart::decryptFile` | `0x410A40` | charts (`.ez`) |
| `DecryptFile2` | `0x410C90` | keysound indexes (`.ezi`), unpacked to a plaintext `.ezz` |
| `KParser::slurpFile` | `0x4135F0` | configuration and per-chart `.ini` files, `EZ2AC.ini` |
| `SongTable::loadBinFile` | `0x469C30` | the song database (`song.bin`) |

The extension does not decide which table applies. One shipped `.ezi` file
(`5radiomix1p-10harpe-rd3.ezi`) holds an `EZFF` chart inside.

### Structure of the transform

All of them are the same transform with different 512-byte keys:

1. The data is walked in blocks of alternating length, 8 bytes then 16 bytes. The final block takes
   whatever is left.
2. Inside each block, every byte gets the next key byte added modulo 256, and the block is written
   back in reverse order. The key position advances by the block length and wraps at 512.
3. The whole buffer is then reversed.

Decryption undoes the final reversal, subtracts the key and un-reverses each block. The game
decrypts an `.ini` into a sibling file with the extension `.int` and reads that.

### Strength

This is obfuscation, not encryption:

- It is an additive keystream with a period of 512 bytes, combined with a fixed permutation.
- There is no IV and no per-file variation, so every file of a kind is encrypted with the same
  keystream.
- Any 512 bytes of known plaintext at a known position recover a key completely, and every
  format here starts with a known header (`EZFF`, `[General]`).
- Without any known plaintext, the difference of two ciphertexts of the same kind equals the
  difference of their plaintexts.

The keys are not reproduced in this study. The mod reads the chart key from the running
executable's memory (`0x4A74B0`) when it learns from native charts, the same "bring your own
executable" approach EZ2DECOMP takes.

## 4.2 `.ez` — charts ("EZFF")

| Version | Game |
|---|---|
| v4 | EZ2DJ 1st Tracks (BPMs must be scaled by about 0.99723) |
| v5 | 1st Trax SE |
| v6 | 2nd Trax, written by the EZSEQ editor |
| v7 | EZ2AC Evolve: the keysound index widened to 16 bits |
| v8 | EZ2AC Final, and this game |

Charts are encrypted since 7th Trax v1.5. Across this game's library, **12,259 charts are v8 and
100 are v7**.

**Header**:

| Offset | Field |
|---|---|
| `0x00` | `EZFF\0` |
| `0x05` | version byte |
| `0x06` | first name, 64 bytes |
| `0x46` | second name, 64 bytes |
| `0x86` | ticks per measure (u16) |
| `0x88` | BPM (float) |
| `0x8C` | track count (u16) |
| `0x8E` | total ticks (u32) |
| `0x92` | length in seconds (float) |

Earlier descriptions knew a single name at `0x06` and left `0x46`–`0x85` unaccounted for; the
loader copies two 64-byte names.

**Tracks**: 64 track blocks follow from `0x96`. Each block is `EZTR`, a 64-byte name, a tick count
at `+0x46` and a data size at `+0x4A`, then its records. A native chart's lane is the **index of its
block** (0..63); the block's own name field is empty in shipped charts. A healthy chart contains
`EZTR` exactly 64 times, and walking the blocks lands exactly on the end of the file. That check is
what identified the one corrupt song in the game.

**Records**: a u32 tick, a u8 type and a payload of 5 (v4), 6 (v5/v6) or 8 (v7/v8) bytes, so a v8
record is 13 bytes. For a note (type 1) in v7/v8:

| Offset | Field |
|---|---|
| `+5` | keysound index (u16) |
| `+7` | velocity |
| `+8` | pan |
| `+9` | an unknown byte |
| `+10` | length (u16) |

The game never reads the record's last byte (`+0x0C`).

**The v7 widening is the format's one real trap.** Widening the key index by one byte pushed
velocity, pan and length along by one. Reading a v8 chart at v6 offsets picks the length up shifted
by a byte, so 6 becomes `0x0600` = 1,536, and every note in the game turns into an endless hold. The
chart still parses and still looks plausible, which is what makes the mistake dangerous.

**Note semantics**:

- **Length is biased by 6.** A length of 0 or 6 is a tap, and anything larger is a hold lasting
  `length − 6` ticks. 1,988 of the 2,067 notes of one representative chart carry exactly 6.
- **Velocity defaults to 127, pan to 64** (0 = hard left, 127 = hard right). The engine applies
  both through DirectSound volume and pan before playing the sample.
- **There are more record types than the four documented before.** `NoteConvert` (`0x410EF0`)
  converts on-disk types to runtime types:

  | Disk type | Meaning | Runtime type |
  |---|---|---|
  | 1 | note | 1 |
  | 2 | volume | 4 |
  | 3 | BPM change | 5 |
  | 4 | beats per measure | 9, which nothing handles |
  | 5 | no payload | 6 |
  | 6, 7 | one word | 7, 8 |
  | ≥ 8 | two words | — |

  A parser that stops at type 4 does not lose its alignment, because records are fixed width, but
  it silently drops events.
- **The tick grid.** Every modern chart declares 192 ticks per measure (48 per beat), and the
  loader rescales ×4 at load time, which is the unit the judge works in
  ([chapter 5](05-timing-judgement-scoring.md)).

**Scale of the library**: 12,359 of 12,361 charts parse, containing 27,473,420 notes, of which
557,431 are holds. The densest chart has 9,372 notes over 52 used tracks.

### Track roles (v6 and later)

| Tracks | Role |
|---|---|
| 0 | control |
| 1–2 | background left/right |
| 3–7 | player-1 keys 1–5 |
| 8, 9 | effectors 1, 2 (keys 6 and 7 in 7-key layouts) |
| 10 | player-1 scratch |
| 11 | player-1 pedal |
| 12, 13 | effectors 3, 4 |
| 14–18 | player-2 keys |
| 19 | player-2 scratch |
| 20 | player-2 pedal |
| 21 | lights |
| 22 and up | background notes |

A mode does not use every track as a lane, and which tracks it uses is data
([section 4.6](#46-gds--mode-descriptors)). Notes on tracks a mode does not claim simply play as
backing audio. A wrong lane set therefore never crashes: a lane quietly goes missing. Measured over
1,150 ClubMix charts, its right hand is tracks 12–16, while tracks 17 and 18 carry notes in **zero**
charts. CV2Mix has no single lane set at all; its 112 charts are a superposition of several layouts.

## 4.3 `.ezi` / `.ezz` — keysound indexes

A plain text list, `<note> <mode> <filename>` per line, encrypted on disk as `.ezi`. Old games
used note names (`C#0` … `D#21`, at most 256 sounds); this one uses numeric indexes (at most 2,048).

Behaviours of the game's loader (`EziLoader::loadEZI`, `0x411090`) that a compatible reader must
copy:

- **Mode-2 lines carry a second filename**, which the loader reads and discards. Missing it shifts
  every following line by one token, silently, because the shifted tokens still parse.
- **Modes other than 1 and 2 read one filename.** There is no default branch: mode 0, which is
  common, falls through to the mode-1 path.
- **A note number outside [0, 0x800) aborts the whole file.** The loader logs, unloads every sample
  it had already created and fails. It does not skip the line.
- **The `;` comment branch is dead code**: none of the 12,359 shipped files contains a comment.
- **Names may point into other songs' folders** with Windows relative paths
  (`..\..\sound\stay\p_MR.wav`). 4,703 references do. With relative paths, both separators and the
  `.wav` → `.ssf` swap resolved, 4,197,115 of 4,202,081 references resolve. The remaining 0.118%
  dangle in the shipped data.

## 4.4 `.ssf` / `.ezw` — samples

An 18-byte header followed by raw PCM. The header is a `WAVEFORMATEX` tail plus a size: channels,
sample rate, byte rate, block alignment, bits per sample, data size. There is no magic number. A
reader validates the header by its redundancy instead: the block alignment must equal
channels × bits/8, and the byte rate must equal rate × alignment.

178,470 of 178,474 shipped samples (38.7 hours) validate. The library is not uniform: about 7% are
not 44.1 kHz, and a few are mono or 8-bit, so a player must resample. The four failures are two
duplicated files that declare 3 channels at 24 bits.

## 4.5 Per-chart `.ini` — judgement and gauge

Each chart has a sibling `.ini`, encrypted with the configuration table:

```ini
[General]          Level, MeasureScale
[JudgmentDelta]    Kool, Cool, Good, Miss         (chart ticks, not milliseconds)
[GaugeUpDownRate]  Cool, Good, Miss, Fail         (gauge percent per judgement)
```

Measured over all 12,342 chart `.ini` files, there are **327 distinct window sets and 498
distinct gauge sets**, and every file has a `[JudgmentDelta]` section. The most common windows
(6 / 24 / 50 / 70) cover only 45% of the charts. A reimplementation that hard-codes one set is not
playing the same game on the rest.

The section reader (`0x4206E0`) **reads values by position, not by key name**. It matches the
section name and then takes the entries in order. Two consequences follow:

- `[GaugeUpDownRate]` has no `Kool` key: KOOL's gauge rate is COOL's.
- The negative rates land on the grades opposite to what their key names suggest:
  - `Fail` is what an **unpressed** note costs.
  - `Miss` is what a press in the outermost window band costs.

  Read that way the file is self-consistent: `[JudgmentDelta] Miss` is the outermost window, and
  `[GaugeUpDownRate] Miss` is what landing in it costs.

Defaults when the file is missing: windows Miss 72, Good 36, Cool 24, Kool 6; level 99; gauge
+0.2 / +0.1 / −1.8 / −4.8.

The game adjusts these values after reading them ([chapter 5](05-timing-judgement-scoring.md)).
`MeasureScale` in particular is overwritten with 1.6 in all modes except the four course modes and
mode 12.

## 4.6 `.gds` — mode descriptors

The `.gds` files are **plain text and not encrypted**. There is one per mode under
`system/<mode>/`, 23 in all. They are brace-structured rather than flat INI, which is why the game
tokenizes them instead of splitting lines:

```text
[General]
NumberOfSlot=2
...
[Slot1]
NumberOfTrack=7
Track1 =
{
    Key=15,16
    SongTrack=10
}
```

The reader (`0x421190`) dispatches on section names: `General`, `Slot<n>`, `Background`, `Panel`,
`Gauge`, `ShowCredit`, `JudgmentLink` and `InGameSongSelect`. `loadSlotSection` (`0x420F60`) fills
a per-player track list at `director + 0x380 + player × 0x1C4`, with 16-byte entries of
{input key, second key, chart track, copy of the track}.

**Lane order is data, and it is the order of the entries in the file.** The `N` in `TrackN` is read
as a token and never used. For example, every 5-key-family mode lists `10 3 4 5 6 7 11`: the scratch
(track 10) is lane 0, then the keys, then the pedal (track 11). A two-value `Key=` maps two inputs
onto one lane; `Key=15,16` is the turntable's two directions.

Input key numbers are 6–9 for the effectors, 10–14 for the player-1 keys, 15/16 for the player-1
scratch, 17 for the player-1 pedal, and 18–25 for the same on the player-2 side.

## 4.7 `song.bin` and `Stage.ini` — the song database

`song.bin` is decrypted by `SongTable::loadBinFile` (`0x469C30`) and holds one record per song per
mode. In twelve of the thirteen mode tables the record's **name field is empty**, because nothing
reads it: a song is identified by a short key, and that key names the song's folder, its art and its
ranking file ([chapter 6](06-song-select.md)). CV2Mix is the exception. Its key is a number and the
name field carries a chart name instead.

`Stage.ini` lists each mode's categories and their songs. Its parser copies every category's song
ids into a **fixed table of 436 ids per category**, with no bounds check. A `Stage.ini` that lists
more songs in a category overruns the table into neighbouring globals; this was confirmed with a
hardware write breakpoint, when the parser's copy loop at `0x469E3E` overwrote the global at
`0x37DCECC`. This is one reason the mod never feeds the game a modified `Stage.ini`.

## 4.8 `.abm` — textures

An `.abm` is a Windows BMP with the magic `AW` in place of `BM` and four header fields XOR-masked
with constants that change with every game era:

| Field (offset) | Old | Evolve | NT | TT | Final | Final EX |
|---|---|---|---|---|---|---|
| data offset (`0x0A`) | `56FE` | `45AE` | `85BE` | `95AB` | `23FF` | `109A` |
| width (`0x12`) | `0831` | `9AF1` | `96EC` | `45BB` | `BDC9` | `CFA1` |
| height (`0x16`) | `1019` | `1D1B` | `FDEB` | `AE12` | `1F01` | `51AE` |
| bits per pixel (`0x1C`) | `1120` | `67BE` | `67AE` | `78EF` | `A97F` | `B18F` |

(Constants from freem's documentation; the Final EX column matches the binary.)

Quirks the 62,000-texture sweep found:

- **The palette** is whatever lies between the header and the pixel data, exactly as BMP defines it.
  Some files carry 255 entries rather than 256.
- **Rows are not always padded.** 31 files store packed rows: a 498-pixel 24-bit row is 1,494 bytes,
  not 1,496.
- **Some files misstate their depth.** 25 declare a paletted depth, carry no palette and hold
  24-bit pixels.
- **35,376 files carry two spare bytes at the end.** Sizes alone could not tell whether they were
  at the start or the end. Decoding a greyscale mask both ways settled it: 100% grey pixels one way,
  93% the other. The wrong choice still produces a recognisable picture, shifted by one pixel, which
  is what makes it dangerous.
- **16-bit art is 1:5:5:5, not 5:6:5.** Read as 5:6:5, green steals a bit from red and the picture
  becomes noise.
- **A 32-bit `.abm` whose alpha never varies is 24-bit art padded to 32.** Its alpha is usually all
  zero and must be ignored, or the sprite vanishes.

## 4.9 `.str` and `.scr` — animated backgrounds

Native songs without a video animate their background with layered 2D scenes, authored in the
Ez2Visual tool.

**`.str` is a scene ("STRM")**. All 10,593 shipped files parse, and they come in three variants:

- **v148** (10,579 files): `STRM`, version 148, a 36-byte header (fps at `+8` — always 60 — and the
  layer count at `+0x10`), then per layer a texture count, that many 128-byte texture names, a
  keyframe count and that many **124-byte keyframes**.
- **ASCII header** (1 file): the 12-byte string `STRM 1.0.0` in place of magic and version.
- **Legacy** (13 files): no magic, a 12-byte header and 88-byte keyframes that store the draw quad
  before the UV rectangle.

A v148 keyframe:

| Offset | Field |
|---|---|
| `+0` | frame (60 Hz) |
| `+4` | type |
| `+8` | x |
| `+12` | y |
| `+48` | the four quad corners: 8 floats, all x then all y, going round the quad (TL, TR, BR, BL) |
| `+80` | animation frame |
| `+88` | texture delay |
| `+92` | rotation |
| `+96` | RGBA (four floats, 0–255) |
| `+112` | source blend factor |
| `+116` | destination blend factor |

Further rules:

- **Only type-0 keyframes are poses.** Type-1 entries are zeroed placeholders: across 231,764 of
  them, their count matches the all-zero blend pairs exactly. Interpolating through them draws the
  clip black.
- **Rotation is an index into a 1,024-entry sine/cosine table** (`0x4A5228` / `0x4A6228`), not
  radians.
- **The UV rectangle is an offset and a size**, not two corners.
- Of 895,214 keyframes, 98.2% have axis-aligned quads, and a quarter of the real keys rotate.

**`.scr` is a timeline ("SCR0")**. It is a table of 136-byte entries: a start time (u32), a
**plane** number (u32) and a 128-byte `.str` name.

- **Planes play at the same time.** Each plane advances through its own scenes. All 297 parseable
  files are time-ordered within every plane, but only 152 are ordered globally.
- **Times are 60 Hz frames, not milliseconds.** One overlay clip declares 60 fps and keys at frames
  0 and 30, half a second for a cymbal flash. Across 3,251 consecutive entry pairs, the gap the
  timeline leaves for a scene divided by that scene's own length has a median of exactly 1.00.
  Reading the times as milliseconds plays every scene 16.7 times too slowly.
- **The table's start offset varies** (`0x14`, `0x1C` and `0x24` all occur), so a reader locates
  it by checking which offset yields names that exist on disk.

The shipped data has loose ends of its own: **6% of `.str` texture references point at files that
do not exist** (6,704 of 108,159 under `bg/`). A loader must tolerate them, as the game evidently
does.

## 4.10 `.spv` — videos

Despite the extension, an `.spv` is an **ASF / Windows Media** file: it begins with the ASF header
GUID. All 162 shipped `.spv` files are 256×256 with no audio track, and their codecs are WMV3 (112),
WMV2 (30) and WMV1 (20). The binary builds `%s\Video\%s\%s.spv` for a song's video and references
`.spv` ten times. It never references `.ogv`, although 162 `.ogv` files sit in the tree: something
else put them there. A placeholder texture, `Video_Blank_256.bmp`, stands in while no frame exists.

## 4.11 Other formats

| Format | What it is |
|---|---|
| `.pvi` | panel visuals: track rectangles, colours and alpha functions, note bombs, judgement sprites (with the original's own typo, `GoolStr`), combo fonts, the target bar |
| `.txg` (`TXG0`) | texture atlas: sub-rectangles sharing one surface (`loadTXG`, `0x408F70`) |
| `.ets` / `.pst` | track sets of the EZSEQ / ptSequencer chart editors |
| `.ezv` (`EVFF0.91`–`0.95`) | Ez2Visual's text source for `.str` scenes; its 16 multitexture presets are the engine's 16 combiner recipes |
| `.rep` | replays (`EZREPLAY_V1` / `V1.1`), used by 6th and 7th Trax and removed in Evolve |
| `music.ini` | the per-game song settings of 1st Tracks / 1st SE, replaced by per-chart `.ini` files from 2nd Trax |
| `rank_<mode>_<id>[-hd\|-shd\|-ex].bin` | a song's Top-5 per difficulty: five 8-byte names, then five 32-bit scores |
