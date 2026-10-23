# 13. Scan, cache, configuration and diagnostics

[← Generated art, text and graphics resources](12-art-text-and-resources.md) · [Index](../RESEARCH.md)

The previous chapters describe what the mod does. This one describes what supports it:

- the start-up sequence;
- the scan of the library and its cache;
- the permanent song ids;
- the configuration file, including options the production file does not list;
- the window handling;
- the log;
- the diagnostics;
- the maintenance tools and the tests.

## 13.1 Start-up

The DLL starts a thread when the launcher loads it. That thread does everything below **before the
game window is allowed to appear**. The window is held back until the end because song select must
find the custom songs ready the first time it is built.

1. **Configuration.** `BMSMod.ini` is read and the memory tier is chosen (13.4). With
   `[Core] Enable=0`, the mod stops here.
2. **The log** is started, the crash logger installed, and the version line written.
3. **English textures** are indexed (`Translation\`).
4. **File redirects and guards.** Generated art, previews, chart `.ini` files, rank files and the
   courses' `Stage.ini` are served from the mod. Guards are installed on game routines that crash on
   inputs the original game never produced (13.7).
5. **The texture ring, the direct chart loader, the judge offset and the lane cover** are installed.
   Each first checks that the code it expects is present. On a different executable, it logs
   "does not match" and stays off rather than patching blind.
6. **The progress window** opens, if enabled.
7. **The scan** (13.2), AutoChart ([7.6](07-bms-to-fnex.md#76-autochart-every-mode-from-5-key-charts)),
   the rating ([7.7](07-bms-to-fnex.md#77-rating-charts-from-their-notes)) and the category split
   ([9.1](09-song-select-extras.md#91-categories-at-55-times-the-design-size)) run.
8. **The song-select integration** is installed, sized from the library.
9. **The progress window closes and the game window is released.** If the game's window already
   exists, it is captured for the Alt+Enter toggle.
10. **The hang watchdog** starts, if enabled.

On the cabinet, this takes about 27 seconds for 23,587 songs, most of it in steps 7 and 8
([8.3](08-performance-on-xp.md#83-start-up)).

**The progress window** shows a bar and the last songs processed (`[N/M] title`), so a long first scan
does not look like a hang. Its list is bounded at 4,000 lines.

## 13.2 The scan

**The layout** is always `BMSData\<mode>\<category>\<song>\`:

- **Mode folders.** Accepted names: `5KeyMix`, `5KeyOnly`, `StreetMix`, `7KeyMix`, `ClubMix`,
  `SpaceMix`, `RubyMix`, `Ez2Catch`, `TurnTable` and `CV2Mix`, in any letter case and with common
  variants (`5konly`, `5k_only`). Any other first-level folder is skipped, and the log says so.
- **Charts.** A song folder holds its charts (`.bms`, `.bme`, `.bml`) and their files. Sounds may also
  sit in a `sound\` subfolder.
- **Song metadata.** Title, artist, levels and BPM come from the charts.
- **Song files.** The lists of audio, image and video files come from the folder.

**Charts to difficulties.** A song's charts are sorted from easiest to hardest and fill NM, HD, SHD
and EX in order. A fifth chart goes to the EZ column
([9.6](09-song-select-extras.md#96-the-ez-column)). Difficulty tags in titles (`[EASY 5K]`) are
recognised, and the lane-count suffix is ignored.

**Measured at scan time.** Each chart's statistics (notes, NPS, peak, chords, jacks, the density
profile, the lane mask) are measured while its notes are in hand, because they are not available
later ([9.8](09-song-select-extras.md#98-detail-panels)).

## 13.3 Ids and the scan cache

**Permanent ids.** Each song gets an id `bmsNNNNNN`, which the game uses for everything: file names,
rank files, course stages. The number comes from `CustomCache\idmap.txt`, one `number|key` line per
song:

| Song | Key |
|---|---|
| a scanned song | `mode/category/folder` |
| an AutoChart copy | `autogen/target mode/source mode/category/folder` |

A key seen before keeps its number, and a new key takes the next free number. Ids therefore survive
rescans, additions and removals. Deleting `idmap.txt` renumbers everything and orphans every rank file,
which is why the guide marks it "do not delete".

**The scan cache.** Every launch used to re-open and re-parse every chart to recover the same
metadata it recovered last time: 13–26 seconds at the time, thrown away on exit. The parse result is
now stored in `CustomCache\scan.cache`:

- **Invalidation comes free.** The folder walk already returns each folder's last-write time. A song
  whose folder has not changed since it was cached is taken from the cache **without opening a single
  file**, and a changed one is parsed as before.
- **Only scanned songs are stored.** AutoChart's copies are rebuilt each start; they are a hash lookup
  per song.
- **The format is length-prefixed binary.** Titles contain tabs, quotes and newlines often enough that
  any text delimiter needs escaping, and a wrong escape would silently corrupt a song's metadata
  instead of failing loudly.
- **The magic changes when a meaning changes.** The entries hold finished values, not raw input, so any
  change in what a value means invalidates the whole cache and costs one full rescan. Each new magic
  marked one such change: the real BPM instead of the declared one, the note measurements, the density
  profile, the blended rating, and the lane mask. The current magic is `EZ2SCANA`.

**Moving the install.** The cache stores absolute folder paths. Copying the game to another drive (the
cabinet's disk, for example) would turn every entry into a miss and re-parse the whole library, which
takes hours on the cabinet. The install folder an entry was made in is whatever precedes
`\<BMSData>\<mode>\` in its path. Swapping that prefix for the current install keeps the entry
([`src/path_rebase.h`](../../src/path_rebase.h)).

## 13.4 Configuration

`BMSMod.ini` sits next to the game and is read once at start-up. The production file comments every
option it lists. The table below also includes the options it does not list.

| Section | Key | Default | Meaning |
|---|---|---|---|
| Core | `Enable` | 1 | 0 = the mod does nothing |
| Debug | `EnableLog` | 1 | write `logs\EZ2CustomMod.log` |
| Debug | `EnableFunctionTrace` | 1 | **not a diagnostic**: installs the song-select integration; 0 disables the mod |
| Debug | `TraceGameLog` | 0 | route the game's own debug logger into the log |
| Debug | `TraceFileApi` | 0 | log the file requests the mod redirects |
| Debug | `TraceBMSScan` | 0 | log the scan in detail |
| Debug | `TraceJudge` | 0 | per-song judgement statistics and a suggested offset ([10.7](10-lane-cover-and-play-options.md#107-judge-offset)) |
| Debug | `GsProbe` | 0 | name the function whose stack check failed; costly, keep off |
| Debug | `HangWatchdog` | 0 | report where a frozen thread is stuck; costly, keep off on the cabinet |
| Debug | `ShowInjectedMessageBox` | 0 | a message box when the DLL loads |
| Debug | `ShowProgressPopup` | 1 | the start-up progress window |
| Video | `AltEnter` | 1 | Alt+Enter toggles borderless full screen |
| Video | `BorderlessOnly` | 1 | use a borderless window rather than a mode change |
| Video | `CaptureExistingWindow` | 1 | find the game's window if it already exists |
| Visuals | `BgaPreview` | 1 | the video preview on the disc ([11.6](11-bga-previews-and-audio.md#116-the-disc-preview-in-song-select)) |
| Visuals | `CategoryOverlay` | 1 | `bg.png` / `ico.png` per category ([9.11](09-song-select-extras.md#911-category-backgrounds-and-icons)) |
| BMSData | `ScanCache` | 1 | reuse the previous scan (13.3) |
| CustomSongs | `EnableBMSData` | 1 | load `BMSData` at all |
| CustomSongs | `Root` / `CacheRoot` | `BMSData` / `CustomCache` | the two folders |
| CustomSongs | `LevelFromNotes` | 1 | rate charts from their notes |
| CustomSongs | `KeysoundCacheMB` / `AssetCacheMB` | 0 | cache sizes; 0 = from the memory tier |
| CustomSongs | `AssetsInMemory` | 1 | generated art in the RAM store ([12.6](12-art-text-and-resources.md#126-the-ram-asset-store)) |
| CustomSongs | `PageSize`, `BridgeMode`, `ModifyOriginalFiles`, `UseNative…AsBase`, `FlatSongFolder`, `RecursiveSongAssets`, `Infer…FromBMS`, `GenerateCacheManifest`, `RedirectCustomToNative`, `RedirectNativeId`, `PinCustomAtTop`, `ForceSelectCustom`, `EnableSongSelectInject` | as shipped | settings from earlier designs, kept at their shipped values; `ModifyOriginalFiles` must stay 0 |
| AutoChart | `Enable`, `SourceMode`, `TargetModes`, `Intensity` (0–200), `Seed`, `EnrichSourceMode`, `LearnNativeCharts`, `NativeChartRoot`, `NativeModelMaxChartsPerMode` (8–2048) | see the file | [7.6](07-bms-to-fnex.md#76-autochart-every-mode-from-5-key-charts) |
| Gameplay | `HudPanel`, `DetailPanels`, `CategorySort`, `VirtualCategories`, `CustomTop5`, `ConstantSpeed` | 1 | the song-select extras ([9](09-song-select-extras.md)) |
| Gameplay | `HudPanelX` / `HudPanelY` | 296 / 86 | where the notice card sits |
| Gameplay | `TextureRing`, `ArtPrefetch`, `EnglishTextures` | 1 | [12](12-art-text-and-resources.md) |
| Gameplay | `LaneCover`, `LaneCoverSize` (0–1000), `LaneCoverStep`, `LaneCoverImage` | 1, 0, 8, empty | [10](10-lane-cover-and-play-options.md) |
| Gameplay | `CustomCourses` | 12 | courses per Radio Mix family; 0 = none |
| Gameplay | `PreviewFps` | 20 | 5–30, capped by the memory tier |
| Gameplay | `FpsLimit` | 0 | [10.8](10-lane-cover-and-play-options.md#108-frame-limiter) |
| Gameplay | `JudgeOffsetMs` | 0 | [10.7](10-lane-cover-and-play-options.md#107-judge-offset) |

**Memory tiers.** What limits the mod is the process's address space before physical memory: 2 GB on
32-bit XP (3 GB with `/3GB`), and 4 GB for this large-address-aware executable on a 64-bit Windows. The
tier is chosen from the process's total virtual size and the machine's RAM:

| Tier | When | Keysounds | Art | PreviewFps cap |
|---|---|---|---|---|
| `2GB` | ≤ 2.2 GB of address space, or < 1.5 GB of RAM | 128 MB | 96 MB | 10 |
| `3GB` | ≤ 3.2 GB, or < 3 GB of RAM | 256 MB | 160 MB | 20 |
| `4GB` | otherwise | 384 MB | 256 MB | 30 |

A session that browsed a lot reached 1.5 GB committed with the largest sizes, which leaves nothing on
a 2 GB process, so the smaller tiers cut both caches and the preview clip. The log's
`[config] memory tier` line says which tier was picked. Explicit sizes override the tier; anything
under 16 MB is raised to 16.

**Two values are written back by the mod:** `LaneCoverSize` when the player lets go of E1, and
`LaneCoverImage` when the picker confirms.

## 13.5 The window

- **Alt+Enter** toggles between the game's window and a borderless window that covers the monitor. The
  previous style and position are restored on the way back.
- **Holding the window back.** The window is held back at creation until start-up has finished (13.1).
- **Capturing an existing window.** If the game window already exists when the mod finishes, it is
  found and captured instead.

## 13.6 The log

`logs\EZ2CustomMod.log` is recreated on every start. Each line is
`[milliseconds since the DLL loaded] [tag] text`.

**Frame time.** Every five seconds, a `[frametime]` line gives the average and worst frame and the time
spent in each of the mod's subsystems:

- `trace`: the per-frame integration;
- `files`: redirected file reads;
- `video`: the BGA;
- `tex`: texture loads;
- `filter`: the category filter.

Any single slow frame gets a `[slowframe]` line with the same breakdown for that frame alone. Times
are measured with the performance counter around each subsystem's work.

**Memory.** When something fails for lack of memory (a render, GDI+ start-up), the mod logs the
process's footprint: committed and reserved bytes, the number of regions, the largest free block and
the largest regions. "The art stopped loading" can then be told apart: either a fragmented 2 GB
address space or the GDI object ceiling.

**Tags worth knowing:**

| Tag | What it reports |
|---|---|
| `[version]`, `[config]` | the build, the memory tier, AutoChart's settings |
| `[scancache]`, `[bms]` | the scan: songs reused, songs parsed, folders skipped |
| `[category]` | categories built, split, or truncated |
| `[filter]` | each category change: ids found, rows written, songs missing |
| `[wheeltex]`, `[assets]`, `[artprefetch]` | wheel textures and the GDI count, art rendered, the art thread |
| `[cache] preview`, `[preview]` | audio preview cuts, disc previews |
| `[bga-direct]`, `[bgpool]` | the background chosen, its start time, its backend |
| `[favorites]`, `[sort]`, `[speed]`, `[judge]`, `[lanecover]`, `[ez]` | the extras of chapters 9 and 10 |
| `[frametime]`, `[slowframe]` | performance (8.2) |
| `RENDER FAILED`, `TRUNCATED`, `FAILED` | problems worth reading first |

## 13.7 Diagnostics

All of these are off by default except the crash logger, and the costly ones say so in `BMSMod.ini`.

**The crash logger** is always on. On a fault, it writes the exception, the registers, the module and
offset of the faulting address, and a walk of the stack to the log, so a crash on the cabinet leaves
something to read.

**The hang watchdog** (`HangWatchdog=1`) exists because a hang leaves no trace: no exception and no log
line.

- Every two seconds, it samples the instruction pointer of every thread.
- A thread that has not left a 64-byte window across four samples, inside the game's own code, is
  reported with a stack walk.
- Eight seconds on the same handful of instructions is a loop or a wait, not work.
- Threads blocked in system code (audio, video, the message pump) are ignored.
- Sampling a thread requires suspending it for microseconds. On a two-core cabinet, doing that to every
  thread every two seconds is felt, so it is off for play.

**`GsProbe=1`** names the function whose stack-buffer check failed, when the C runtime reports one
([2](02-binary-and-protection.md)). It sits on the return path of 275 functions, so it is off for play.

**The execution tracer** (`tracer\ez2tracer.dll`) is a separate, optional DLL for hangs the watchdog
cannot explain. Placed next to the game, it records the sequence of game instructions executed after
a chosen point, up to a cap, into a compact binary file. `tracer\decode_trace.py` decodes it. It slows
the game enormously while recording, so it is a laboratory tool only.

**Guards on game routines.** Four routines in the game crash on inputs it never produced with its own
430 songs:

- a null disc texture in the wheel draw past about 2,000 songs;
- a full texture pool;
- a null text texture;
- a null disc slot on a category change.

Each has a guard that turns the crash into a skipped draw or a flush and retry. The game's bitmap
loader also reads a whole file into a fixed 8 MB buffer. A BMS title picture of that size or more (a
1920×1080 32-bit BMP is 8.3 MB) is therefore never handed to it, and generated art stands in.

## 13.8 Maintenance tools

The `tools\` scripts maintain a BMS library. None of them is needed to play. They are listed here
because several encode findings about specific packs.

| Tool | Purpose |
|---|---|
| `bms_audit.py` | audit the library for timing and structure problems, read-only |
| `audio_sync.py`, `bms_render_check.py`, `raw_compare.py` | measure how well charts sit on their music; render a keysounded chart to audio; compare converted charts with their raw sources |
| `fix_offset.py` | shift charts whose notes sit early or late against their own music |
| `audio_mismatch.py`, `fix_audio_swap.py` | find and replace music files that differ from the song's original |
| `bga_sync.py`, `fix_sbx_bga.py` | measure and fix background videos that start at the wrong time |
| `fix_bangdream.py`, `fix_chunithm.py`, `fix_cytus_audio.py`, `fix_musynx_keysounds.py`, `fix_sm_tempo.py` | pack-specific repairs of converted charts |
| `import_3s.py`, `import_ezmuse2.py` | import songs from other games' installations the user owns into `BMSData` |
| `abm_tool.py`, `ez2ac_ini.py` | read and write the game's `.abm` textures and its encrypted settings file |
| `texture_inventory.py`, `texture_translate.py` | find and re-letter the Korean menu textures ([12.9](12-art-text-and-resources.md#129-english-menus)) |

## 13.9 Tests

- **One file, one test.** Every `.cpp` in `tests\` is a standalone executable testing one piece of
  pure logic. Each feature keeps its testable half in a header
  ([`src/lane_cover.h`](../../src/lane_cover.h), [`src/texture_ring.h`](../../src/texture_ring.h),
  [`src/play_data.h`](../../src/play_data.h), [`src/course_builder.h`](../../src/course_builder.h), …)
  so it can be tested without the game.
- **Python tools.** The Python tools have their own tests.
- **The runner.** `tests\run_tests.py` builds and runs them all.
- **The XP check.** `tests\xp_compat_check.py` checks every build's import table against a list of
  functions introduced after Windows XP. A build that would not load on the cabinet fails before it
  gets there.

The test suite is described in [DEVELOPMENT.md](../DEVELOPMENT.md#tests).
