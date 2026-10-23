# 7. From BMS to FNEX

[← Song select](06-song-select.md) · [Index](../RESEARCH.md) · [Next: performance on the cabinet →](08-performance-on-xp.md)

BMS is the open chart format of the rhythm-game community: plain text, one folder per song, with
keysounds as ordinary audio files. FNEX is the game's own format ([chapter 4](04-file-formats.md)).
This chapter describes how a BMS song becomes something the game plays, and the measurements that
decided each rule. The conversion happens in memory when a song starts; the BMS files are never
modified.

## 7.1 The library

The cabinet's library holds **23,587 songs** in 106 category folders, scanned from
`BMSData\<mode>\<category>\<song>\`. AutoChart expands them to **212,283 catalogue entries**: one
per song per playable mode. About **84,000 charts** are rated from their note data at start-up.

Songs get **stable ids** (`bms000123`) from a persistent map keyed by the song's relative path
(`CustomCache\idmap.txt`), so adding or removing folders never renumbers existing songs. A song's
scores and favourites are tied to that id.

A **scan cache** (`CustomCache\scan.cache`, 33 MB for this library) stores what a scan learned about
each song. On a normal start, 23,587 songs are reused and only new or changed folders are parsed
again; the cached scan loads in about a second.

## 7.2 Channels to tracks

BMS numbers its channels in base 36: `11`–`19` for player-1 keys, `21`–`29` for player 2, `51`–`69`
for long notes, `01` for background music. The mapping to FNEX tracks was checked against decrypted
native charts of every mode:

| BMS channel | FNEX track | Note |
|---|---|---|
| `11`–`15` | 3–7 | player-1 keys 1–5 |
| `16` | 10, or 8 in 7-key modes | scratch in 5-key layouts; the 6th key in 7-key layouts |
| `17` | 10, or 9 in 7-key modes | rarely used outside 7-key charts |
| `18`, `19` | 11 | pedal |
| `21`–`27` | 12–18 | player-2 keys |
| `28`, `29` | 19 | player-2 pedal |
| `51`–`59`, `61`–`69` | as their play channel | long notes |
| `01` | 22–31 | background music, spread over the ten background tracks by sample id, as native charts do |

Channel 16 is the only mode-dependent case: the same channel is the scratch in 5-key layouts and
the 6th key in 7-key layouts.

Some songs ship charts written for another layout. A survey of the library on 2026-09-26 found:

| Layout | Songs | Played in |
|---|---|---|
| 7 keys (channels 11–15 + 18/19) | 503 | 7keyMix |
| doubles, 5 + 5 | 97 | ClubMix |
| doubles with a 6th/7th key | 51 | SpaceMix |

Those charts are mapped exactly as written in the matching mode instead of being generated.

## 7.3 The tick grid

Shipped charts use 192 ticks per measure, and the first converter did too. BMS charts are not
authored on that grid:

- **Measured over 45,707 notes from 80 charts**, the dominant BMS subdivision is **1/384 of a
  measure**.
- **On a 192 grid, every odd 1/384 position falls exactly between two ticks** and has to be rounded.
  **24% of all notes were displaced, by a mean of 4.17 ms**, almost always by the full half-tick.
  Native charts sit exactly on their grid, which is why converted charts felt harder to time than
  native ones of the same level.

The converter therefore writes **384 ticks per measure**. The engine honours a chart's declared
resolution rather than assuming 192: it reads the resolution from the chart header and derives
ticks per beat from it. The dominant case becomes exact and the worst case for everything else is
halved.

BMS positions are converted through an integer grid of 57,657,600 per measure, which every real BMS
subdivision divides exactly (powers of two up to 256, times 3, 5, 7, 9, 11 and 13), so no rounding
error accumulates before the final conversion.

## 7.4 Notes, long notes and timing events

- **Taps carry a length of 6.** Every shipped tap carries a length, and it is never zero: in one
  decrypted native chart, 1,988 of 2,067 notes are taps of length 6. The converter writes 6 for every
  tap, and `length + 6` for holds.
- **Long notes come in two BMS forms**, both supported:
  - **`LNTYPE 1`** marks a hold with two events on a long-note channel, head then tail. One
    missing or extra event inverts every hold after it on that lane, and a library audit found this
    in hundreds of charts. The pairing is solved per lane as a small dynamic programme: each event
    is either paired with the next one or played alone as a tap, with penalties for pairs that would
    swallow an ordinary note. A well-formed lane comes out as plain alternation would read it; a
    broken one loses one event instead of everything after it.
  - **`#LNOBJ`** ends a hold with a marker note on the play channel.
- **Holds at the end of a chart** are padded with a tempo record, so the stage does not end while
  the last hold is still held.
- **`#STOP`** is supported: stops are resolved after the whole file has been read, because a stop's
  position depends on measure-length changes that may appear after it.
- **Tempo changes** become FNEX BPM records, which the engine applies when its clock reaches them
  ([chapter 5](05-timing-judgement-scoring.md#51-the-song-clock)).
- **The dominant BPM.** The wheel and the constant-speed option use the BPM the chart **spends the
  most time at**, not the declared `#BPM`. One chart declares 22.875 and jumps to 183 in its first
  measure; dividing by the declared value pinned the scroll speed at its 999% maximum.

Engine limits shape the conversion:

- **2,048 events per track.** The engine's per-track queue holds at most 2,048 events.
- **2,048 keysounds per song.** Keysound ids must be below `0x800`.
- **Chords wider than the lanes.** A chord wider than the available lanes keeps its extra notes as
  background sound rather than dropping them.

## 7.5 Five keys from anything

The 5-key mode maps only channels 11–15 to its keys, but packs ship 7-key, pop'n and double charts
beside their 5-key ones. Some "5-key" charts even use the five centre keys of the 7-key layout. The
converter folds every chart onto five keys:

- **Five lanes or fewer**, just not 11–15: kept in their left-to-right order on keys 1–5.
- **More than five lanes**: each lane goes to its proportional place on the five keys.
- **Scratch lanes**: the 5-key mode has no turntable (the shipped 5-key charts hold 0 scratch notes
  in 168,643), so a scratch note takes the free key nearest key 1.
- **Overlaps**: a note that lands on a key that is already busy (same tick, or inside a hold) takes
  the nearest free key; notes that cannot fit anywhere keep sounding as background.

A chart already on the five keys with nothing stacked comes out exactly as written.

## 7.6 AutoChart: every mode from 5-key charts

Most BMS songs only have 5-key or 7-key charts, while the game has thirteen modes. AutoChart
generates the missing ones from the 5-key source, with a **statistical model learned from the
game's own charts**. It uses no neural network and no external model file. The model is
deterministic (seeded) and is learned at start-up from the installation itself.

**Learning.** Up to 192 native charts per mode are read. Encrypted charts are decrypted in memory
with the key table of the running executable. For each mode the model accumulates:

- chart and note counts, and long-note counts;
- which physical tracks the mode really uses for keys, scratch, pedal and the second side, ranked
  by share of notes (hard-coded layouts had several modes wrong: RubyMix uses five lanes, not
  four, and Ez2Catch and CV2Mix use a second side);
- the share of scratch, pedal and second-side notes;
- a 16-position histogram of where in the beat scratch and pedal notes fall;
- the mean gap between consecutive scratch notes and between pedal notes;
- how often scratch and pedal notes take part in chords and long notes.

The learned model is cached (`native_model.cache`), because walking the whole native `SOUND` tree on
a cold cabinet disk takes minutes.

**Generation**, per song and target mode:

1. Convert the BMS to FNEX events.
2. Resolve `LNTYPE` / `LNOBJ` into single events with a length.
3. Group notes by tick.
4. Choose the ticks that become scratch or pedal notes, from the model's phase histogram, density
   and spacing, and from the keysound's name.
5. Redistribute the remaining notes over the mode's valid lanes.
6. Lock each lane until its long note ends.
7. Never put two generated notes on the same control at the same tick.
8. Send leftover simultaneous audio to background tracks when the mode has too few controls.

The source mode's own charts are left as the author wrote them; the model only adds scratch and pedal
notes there (`EnrichSourceMode`). `Intensity` scales the density of the generated controls (100 =
the native model, 0 = none, 200 = double).

## 7.7 Rating charts from their notes

BMS charts declare their own level (`#PLAYLEVEL`), and at the top of the scale the declaration stops
meaning anything. **46% of songs called their hardest chart 18**, and inside that single level the
busiest two seconds ran from 13 to 83 notes per second. The mod rates every chart from its note data
instead (`LevelFromNotes`):

1. **Measure** each chart: average notes per second, the **peak** (the busiest two seconds), chord
   and jack percentages, and a density profile.
2. **Score** it as a blend: `0.8 × peak + 0.2 × (2.6 × average)`.
   - Peak alone ranks the library well, but cannot tell a song's charts apart, because a song's Easy
     and Expert usually share the same climax.
   - Average density separates them. Across the library, `2.6 × average` sits on the same scale as
     the peak (the ratio holds at 2.58–2.62 from level 5 to 18), so the blend does not shift the
     curve.
3. **Map the score to a level 1–20** through a curve fitted to the mean peak of every chart that
   declared levels 2–17 (1,379 charts). Level 18 is left out of the fit, because it is where packs put
   everything they consider hard. The curve is smoothed to be monotone with a real step per level,
   then continued past 17 over the range the declared 18s actually occupy.
4. **Add one level for heavy jacks** (12% or more): repeated notes in one lane are harder than the
   same density spread out, and the measured jack rate climbs from 0% at level 2 to 15.5% at 18.
5. **Order a song's charts by the continuous score**, not the rounded level, so two charts that both
   read 13 still sort correctly. Fewer notes breaks real ties.

The result, measured over 400 songs: the level of each song's hardest chart, which used to pile
45.8% of the library onto 18, now spreads over 14 levels with 14.0% in the fullest one. 84,210 of
84,291 charts are rated this way; the other 81 have too few notes to measure and keep their
declared level.

## 7.8 Difficulties

A song's charts are sorted by their rating and fill the four native slots: NM, HD, SHD, EX.
A fifth chart goes to the **EZ column**, the dormant leftmost difficulty column
([chapter 6](06-song-select.md#65-input)): with five charts, the easiest becomes EZ and the other
four fill NM–EX; with more than five, EZ takes the easiest and EX the hardest. Difficulty tags in
file names (`[EASY 5K]`, `[APPEND 5K]`) are recognised, and the lane-count suffix is ignored.

## 7.9 Text, audio and video

- **Titles and artists.** BMS declares no text encoding. Newer packs are UTF-8, classic Japanese
  ones Shift-JIS, Korean ones EUC-KR. The converter tries UTF-8 first, then tells Shift-JIS from
  EUC-KR by which reading spells valid characters, and falls back to the system code page. Text is
  drawn with the wide-character API and a bundled CJK font (WenQuanYi Micro Hei), so a title never
  passes through the code page and comes out unreadable.
- **Keysounds.** WAV is copied straight into the game's sample buffers. MP3 and OGG are decoded
  inside the process (minimp3, stb_vorbis). Other formats go through Media Foundation where it
  exists, and through an XP-compatible `ffmpeg.exe` otherwise. Compressed keysounds of the song
  under the cursor are decoded in the background before the song starts.
- **Previews.** A song's audio preview is cut from its longest audio file: 40 seconds are decoded so
  the 20-second window (the median length of the shipped previews) can start past the intro.
- **Video.** A song's background is the video its chart references, played through DirectShow, or
  through ffmpeg when DirectShow cannot open it. About 980 songs use **image BGA** instead (a
  picture, or a sequence of pictures, on channel 04), played through the same 640×480 path. Songs
  with neither get a random clip from the game's own background pool.
