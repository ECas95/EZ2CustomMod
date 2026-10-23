# Developing EZ2CustomMod

- [Toolchain](#toolchain)
- [Building](#building)
- [Tests](#tests)
- [Deploying](#deploying)
- [Making a release](#making-a-release)
- [How it works](#how-it-works)
- [Source map](#source-map)
- [Reading the log](#reading-the-log)
- [Ground rules](#ground-rules)

## Toolchain

The production DLL is built with **MSYS2 MINGW32** (i686 GCC). MSVC is not used: the code relies on
GCC specifics, and the DLL must load on Windows XP.

```bat
winget install MSYS2.MSYS2
C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm mingw-w64-i686-gcc"
```

Python 3 (`py -3`) runs the build script, the tests and the tools. `tests/abm_tool_test.py` and
`tools/abm_tool.py` also need Pillow (`pip install pillow`).

## Building

```bat
build_mingw_direct_msys2_cmd.bat
```

or, from a shell that has `C:\msys64\mingw32\bin` in `PATH`:

```bat
py -3 build_mingw_direct.py            :: add --clean to start fresh, --debug for -O0 -g
```

The output is `EZ2CustomMod.dll` at the repository root. If an `EZ2AC_FNEX\` folder (a local copy of
the game) sits next to it, the script also copies the DLL there. The DLL links the MinGW runtime
statically: the only imports are system DLLs that XP has. Media Foundation is loaded at run time, so
its absence on XP only disables that decoder.

`CMakeLists.txt` lists the same sources, for IDEs. It is not the production build.

## Tests

Each `tests/*_test.cpp` is a standalone program that includes the header (or source) it covers and
asserts on it. No framework is used. To run all of them, plus the Python tests:

```bat
py -3 tests/run_tests.py          :: everything
py -3 tests/run_tests.py lane     :: only names containing "lane"
```

Two known failures depend on the development machine, not on the code:

- `text_codec_test`: its Latin-1 case expects the ANSI code page to be 1252 (as on the cabinet). A PC
  with "Use Unicode UTF-8 for worldwide language support" enabled (ANSI code page 65001) fails it.
- `abm_tool_test.py`: fails with `No module named 'PIL'` until Pillow is installed.

Before shipping a DLL, always check that it still loads on XP:

```bat
py -3 tests/xp_compat_check.py EZ2CustomMod.dll
```

It walks the import table and fails on any API or DLL introduced after XP.

## Deploying

`deploy_to_cabinet.bat "<game folder on the cabinet's disk>" "<that disk's WINDOWS folder>"` copies
the local game copy (`EZ2AC_FNEX\`) to the cabinet's disk. It leaves out what must not run on XP
(dgVoodoo, DDrawCompat, `ffmpeg-modern.exe`) and adds `gdiplus.dll` from the cabinet's WinSxS. It
only copies what changed.

To swap only the DLL, keep the previous one as `EZ2CustomMod.dll.bak_YYYYMMDD_vXX` next to it, so the
cabinet can be rolled back without a PC.

## Making a release

1. Bump the version string logged at start-up: `LogLine("[version] EZ2CustomMod_vXX_Y_Z_...")` in
   `src/mod_main.cpp`.
2. Build, run `tests/run_tests.py` and `tests/xp_compat_check.py`.
3. Add the version to `CHANGELOG.md`.
4. Tag `vXX.Y.Z` and attach `EZ2CustomMod-vXX.Y.Z.zip`. Its top folder holds `EZ2CustomMod.dll`,
   `BMSMod.ini`, `ffmpeg.exe`, `pthreadGC2.dll`, `wqy-microhei.ttc` (and its licence), `GUIDE.md`,
   an empty `LaneCover\` folder and `xp\gdiplus.dll` (taken from an XP machine's WinSxS; only XP needs
   it).

### History and older releases

The history before this repository existed was rebuilt from the dated source backups kept during
development: one commit per backup on the `history` side of the merge, from the partial v35.4.0
baseline (2026-07-12) to the v36.0.0 production build (2026-09-26). The `v36.0.0-dev.YYYYMMDD` tags
mark weekly development builds. The August ones carry the DLLs built at the time; the September ones
were rebuilt from their snapshot with the current toolchain.

## How it works

The DLL is loaded into `EZ2B.exe` by 2EZConfig. Nothing on disk belonging to the game is modified:
every change is an in-memory patch, an import-table hook or a file redirect.

**Start-up** (`mod_main.cpp` `InitThread`)
- Installs the import-table hooks (`CreateFileA`, `FindFirstFileA`, `ReadFile`, …). Through them the
  game opens files that do not exist on disk: generated art, previews and chart `.ini` files are
  served from memory.
- Scans `BMSData` (`bms_scanner.cpp` `ScanBMSData`). `CustomCache\scan.cache` makes later starts
  incremental, and `idmap.txt` keeps every song's `bmsNNNNNN` id stable across scans.
- AutoChart expands the catalogue with virtual copies of every song for each target mode.
- Patches the song-select capacity: the song arrays, category tables and texture arrays are
  relocated into a larger, mod-owned object, so 23k songs and 100+ categories fit.
- The game window is held back until all of this is done.

**Song select** (`trace_hooks.cpp`)
- The first category filter injects every custom song of the active mode into the game's `master[]`
  array and writes the category id tables. Later filters find the injection already in place
  (`MasterHoldsId`) and skip it.
- The game's category filter (`sub_435070`) is replaced by `FilterCategoryImpl`. The original used a
  376 KB stack array, which capped every category at 436 songs; the replacement uses a hashed id
  index. It also fills the RANDOM row.
- Wheel textures load only for a window around the cursor (`LoadSongnameWindow`). The game never
  frees a texture while song select is open, so loading whole categories ran out of GDI objects.
- Art is rendered by GDI+ on a background thread (`artPrefetchProc`) and kept in an LRU RAM store.
  The hover preview is cut by another thread (`SongPreviewWarm`), and the game's preview timer is
  held until the file is ready.
- A per-frame hook on the song-select renderer drives the extras: Top-5, constant speed, the
  shortcuts, the HUD card, the detail panels and the lane cover menu.

**Play**
- `bms_scanner.cpp` translates the BMS chart into FNEX events in memory and decodes keysounds
  (in-process minimp3 / stb_vorbis, then Media Foundation, then `ffmpeg.exe`). A hover prefetch
  thread decodes the next song's compressed keysounds ahead of time.
- The BGA plays from the original video through DirectShow, or through ffmpeg piping BGR24 frames.
- The lane cover is drawn from the keys panel's overlay call, and `getState` is replaced so that
  holding E1 does not trigger bass boost.

## Source map

| File | Contents |
|---|---|
| `mod_main.cpp` | entry point, import-table hooks, file redirects, texture ring, BGA preview, HUD card, detail panels, lane cover and its menu |
| `trace_hooks.cpp` | song-select patches: capacity, category tables, the replacement filter, wheel window, shortcuts, sort, virtual categories, Top-5 |
| `bms_scanner.cpp` | BMS scan and cache, AutoChart, direct chart/audio load, art generation, RAM asset store, previews, play data |
| `audio_decode.*` | in-process MP3/OGG decoding and PCM conversion |
| `fnex_crypto.*` | the game's file encryption, used for the `.ini` files it reads |
| `config.*` | `BMSMod.ini` loading and the automatic memory tiers |
| `chart_layout.h`, `lane_fold.h`, `ln_pairing.h` | lane assignment, 7-key-to-5-key folding, long-note pairing |
| `course_builder.h`, `play_data.h`, `rank_redirect.h` | Radio Mix courses, favorites/recent/most played, custom rank files |
| `lane_cover.h`, `texture_ring.h`, `text_codec.h`, `image_bga.h`, `path_rebase.h`, `supersample.h`, `translation_map.h` | the pure, testable halves of their features |
| `iat.*`, `patch.*`, `window_hook.*`, `logger.*`, `progress_popup.*` | hooking, code patching, window capture, log, start-up progress window |

The `tools/` scripts maintain the song library (audits, offset and audio fixes, importers) and build
the English textures (`texture_translate.py`, driven by `translation/spec.py` and its CSVs).

## Reading the log

`logs\EZ2CustomMod.log` starts fresh on every launch. Each line is `[ms since load] [tag] …`.

| Tag | Meaning |
|---|---|
| `[version]` | which build is running |
| `[config] memory tier` | the cache sizes chosen for this machine |
| `[scancache]`, `[bms] indexed` | the scan: songs reused from the cache, songs parsed |
| `[frametime]` | every 5 s: frame-time average and worst case, plus the time spent in each part of the mod (`trace`, `files`, `video`, `tex`, `filter`) |
| `[slowframe]` | a single frame that took too long, with the same breakdown |
| `[filter] cat=…` | a category change: ids found, rows written, missing or unplayable songs |
| `[wheeltex]` | wheel textures loaded around the cursor, and the process's GDI object count |
| `[assets] lazy …` | art rendered for one song and role |
| `[cache] preview …` | a hover preview cut, with how long it took |
| `[lanecover]` | lane cover hooks, texture, size changes and menu choices |
| `RENDER FAILED`, `TRUNCATED`, `FAILED` | problems worth reading first |

## Ground rules

- **XP first.** Every build must pass `xp_compat_check.py`. No API newer than XP, and no new runtime
  DLL dependency.
- **Measure on the cabinet.** A Pentium E5800 is several times slower than a development PC. Judge a
  change by the cabinet's `[slowframe]`/`[frametime]` lines, not by how it feels on the PC.
- **The game thread must not wait.** Decoding, rendering and file I/O belong on background threads,
  with the game thread only picking up finished results.
- **Never modify the game's files.** Patch in memory, hook, or redirect.
- **One check per piece of logic.** Pure logic goes into a header with a standalone test in `tests/`.
