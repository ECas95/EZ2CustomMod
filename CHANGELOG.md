# Changelog

Newest first. Dates are when the change was made. Every version's first log line says which build is
running: `[version] EZ2CustomMod_v…`.

- [v36.2.0](#v3620--2026-10-22) · [v36.1.1](#v3611--2026-10-22) · [v36.1.0](#v3610--2026-10-22)
- [v36.0.0](#v3600--2026-09-26) and its development builds
- [v35.4.0](#v3540--2026-07-12) · [v3 – v25](#v3--v25--june-and-july-2026)

## v36.2.0 — 2026-10-22

Released as [v36.2.0](../../releases/tag/v36.2.0).

**Added**
- Lane cover picker in song select. Hold **PEDAL + E4** to open it; E2/E3 browse `LaneCover\` with a
  thumbnail, E1 keeps the choice (written to `BMSMod.ini`), E4 leaves. While it is open the game is
  shown none of the cabinet's buttons, so the wheel and the category pager stay put.

**Changed**
- A tap of PEDAL + E4 still cycles the sort order, but now on release, so a hold can open the picker.
- The detail panels hide while the picker is open.

## v36.1.1 — 2026-10-22

Deployed to the cabinet for testing; superseded the same day by v36.2.0, which contains it.

**Fixed**
- The lane cover did not follow the turntable. It read the engine's running total
  (`Obj40C180 +0x420238`), which the game zeroes whenever it passes a limit of 5, 10 or 20 counts set
  per screen. A steady turn read back as +3, +3, −3, +3, −3 and the cover shook in place. It now reads
  the raw 8-bit position (`+0x420220`) and wraps it the way the engine does, and drops the turntable
  movement left over from song select.
- The lane cover sometimes did not load. A failed texture upload was ignored and left the cover blank
  or full of garbage for the rest of the song. The upload is now checked, a lost surface is restored,
  and the upload is retried on the next frame.

## v36.1.0 — 2026-10-22

Deployed to the cabinet for testing; superseded the same day by v36.2.0, which contains it.

**Fixed** — song select on the cabinet's Pentium E5800
- Changing category froze the game for 0.7–1.7 s. Every change copied and re-sorted the mode's whole
  catalogue (23,587 songs) only to find the songs already injected. The check now runs first, and the
  copy happens only when there really is something to inject.
- The catalogue sort found each song's category rank with a linear `_stricmp` over ~100 categories,
  twice per comparison, and moved whole records on every swap. It now sorts (rank, index) pairs with
  the ranks in a hash map. This also speeds up start-up and every return to song select after a song.
- Each song the cursor stopped on froze the game for 0.4–0.6 s while its preview was cut with ffmpeg
  on the game thread (86 times in one session of the cabinet's log). The cut now runs on a background
  thread from the 8th frame of rest, and the game's 30-frame preview timer is held until the file is
  ready.
- Some song names never loaded: the art thread published an `.abm` while the game thread was
  absorbing the same file into RAM, and `MoveFileEx` failed with `ERROR_ACCESS_DENIED`. The fallback
  tile failed the same way. Publishing and absorbing are now serialised, and a good copy already in
  RAM is never overwritten by a fallback tile.

## v36.0.0 — 2026-09-26

The build that ran on the cabinet from 2026-09-26 to 2026-10-22, released as
[v36.0.0](../../releases/tag/v36.0.0). The DLL logged `v36_0_0_native_autochart` for the whole
development period, so the intermediate builds are published as `v36.0.0-dev.YYYYMMDD` pre-releases.
The repository's history has one commit per source snapshot kept during that period.

### 2026-09-26 — production

- Custom Radio Mix courses: four songs from one level band, listed after the game's own channels.
- Charts written for a mode's own lanes are played as written instead of generated: 7-key charts in
  7keyMix, doubles in ClubMix/SpaceMix.
- Lane cover (SUDDEN+): hold E1 and turn the turntable in play; a tap of E1 still toggles bass boost.
- Image BGA: charts whose channel 04 shows pictures instead of a movie (980 songs).
- FAVORITES, RECENT and MOST PLAYED virtual categories, kept in `CustomCache\playdata.txt`.
- Custom songs' rank files are kept in `CustomCache\rank` instead of the game's `SOUND\`.
- English menu textures (`Translation\`, built by `tools/texture_translate.py`).
- Titles in UTF-8, Shift-JIS and EUC-KR are detected and drawn wide, so they no longer come out as
  mojibake.
- 7-key and pop'n charts are folded onto five keys, and their scratch channels kept.
- Supersampled panel text, so the mod's panels match the game's smoothed art.
- Texture ring: custom names and discs reuse a fixed set of textures, and slots left over from a
  texture pool rebuild are emptied.
- Scan-cache paths are stored relative to the install folder, so copying the game to the cabinet's
  disk no longer forces a full rescan.

### 2026-09-18 → 09-25 — [v36.0.0-dev.20260925](../../releases/tag/v36.0.0-dev.20260925)

- Levels rated from the notes (`LevelFromNotes`): a blend of the busiest two seconds and sustained
  density, ordered by the continuous score so a song's charts never share a level by rounding.
- Stats and density panels in song select.
- `#STOP` support. The wheel shows the BPM a chart spends most time at.
- BGA start time fixed: it used 48 ticks per beat after the charts had moved to a finer grid, so the
  video ran twice as late as it should.
- The BGA got its own clock, paced against the 30 fps stream instead of the decoder.
- Judge windows scaled to the mod's chart grid, including the engine's fixed 3-tick widening, so a
  custom chart judges exactly like a native one.
- Difficulty tags such as `[EASY 5K]` are recognised; the lane-count suffix is ignored.

### 2026-09-08 → 09-17 — [v36.0.0-dev.20260917](../../releases/tag/v36.0.0-dev.20260917)

- MP3 and OGG keysounds decoded in-process (minimp3, stb_vorbis) instead of one ffmpeg process per
  file. Background decodes can be cancelled when the cursor moves.
- The HUD notice card, built like the game's own panels.
- The EZ column: a song's fifth chart in the dormant leftmost difficulty column.
- Art prefetch thread and render lock: wheel art is rendered off the game thread.
- RAM asset store: generated art lives in memory with an LRU budget, not in `CustomCache`.
- Pixel uploads through `Lock`/`Unlock` instead of GDI (~7 ms saved per upload under dgVoodoo).
- Per-chart judgement `.ini` files served directly, with the values the native charts ship.
- Guard on the game's texture loader, which reads into a static 8 MB buffer without a size check.
- Long-note width fixed.

### 2026-08-30 → 09-07 — [v36.0.0-dev.20260907](../../releases/tag/v36.0.0-dev.20260907)

- Scan cache: launches stop re-parsing every BMS (13–26 s saved per start at the time).
- Song-select capacity sized from the library; the native categories compacted so custom categories
  start right after HOT, NEW, ALL and the 16 game versions (the LV and alphabetical views dropped).
- Wheel texture window: names and discs load only around the cursor, keeping a long browse inside
  Windows' 10,000 GDI object limit.
- RANDOM row at the top of every category.
- BGA preview on the song-select disc, and category backgrounds and icons (`bg.png`, `ico.png`).
- Native `.scr`/`.str` background scenes composited for previews (formats reverse-engineered here).
- AutoChart's catalogue lookup hashed: about 3 billion `_stricmp` calls removed from start-up.
- Wheel BPM per difficulty, minimum note length 6, padding so a chart ending in a long note is not cut.
- Crash fixed entering a native LV category (a negative row index from the renderer's stack).
- Log lines carry milliseconds since load; memory accounting diagnostics.

### 2026-08-25 → 08-29 — [v36.0.0-dev.20260826](../../releases/tag/v36.0.0-dev.20260826), [v36.0.0-dev.20260829](../../releases/tag/v36.0.0-dev.20260829)

- AutoChart: virtual charts for every mode, generated from the 5-key source, with scratch and pedal
  timing learned from the native charts.
- 5keyMix and streetMix treated as the separate modes they are.
- Virtual mode copies share chart and file data instead of deep copies.
- The game's category filter (`sub_435070`) replaced: heap-built list and hashed lookup instead of a
  376 KB stack array, lifting the 436-songs-per-category ceiling.
- Finer chart grid (384 ticks per measure) for BMS timing.
- Windows XP support: costly diagnostics off by default, imports checked against XP.
- Song names too long for one line wrap to two, with an 8 px floor and an ellipsis.

## v35.4.0 — 2026-07-12

The baseline of the history: only `bms_scanner`, `config`, `mod_main` and `trace_hooks` survive from it.

- Charts translated to FNEX events in memory when the song starts; no `.ez`/`.ezi`/`.ssf` written per
  song.
- WAV copied straight into the game's audio buffer; MP3/OGG/FLAC and others decoded in memory (Media
  Foundation, then ffmpeg through a pipe).
- BGA played from the original video through DirectShow, with ffmpeg as fallback; a random clip from
  `Video\_spv` when the song has none.
- Song names loaded per category on first visit; custom Top-5 suppressed.

## v3 – v25 — June and July 2026

The prototypes that found the way in. Only their release notes survive.

| Version | What it established |
|---|---|
| v3 | Trampoline hooks on the game's functions by RVA; the game's own logger switched on |
| v4 | Register and argument tracing of the song-select filter, to map its structures |
| v5–v6 | First custom song injected into song select, played through a native song (`babydance`) |
| v7–v8 | Redirects through the CRT's `fsopen` wrapper and `SetCurrentDirectoryA` |
| v9 | List bridge: `song.bin` and `Stage.ini` untouched, BMSData overlaid in memory |
| v10 – v10.6 | BMS converted to `.ez`, the `.ezi` keysound map encrypted, `#PLAYLEVEL` mapped to difficulties, audio to `.ssf` |
| v11 – v16 | Converter output matched to the native formats; ffmpeg as the audio backend |
| v19 – v20 | Virtual `song.bin` rows and generated art; categories from the BMSData folders |
| v21 | The FNEX file encryption implemented inside the DLL |
| v22 – v24 | Category navigation fixes, keysounds, lane mapping; a crash from redirecting `Stage.ini` removed |
| v25 | Custom songs no longer leak into every native category |
