# Reverse-engineering notes: how EZ2AC FNEX was opened up

What had to be learned about `EZ2B.exe` to load thousands of custom songs into it without touching
its files: how the work was done, the engine's structures and formats, and the weaknesses that
were exploited or neutralised along the way. The [CHANGELOG](../CHANGELOG.md) has the order in which
it happened; the source comments next to each patch have the full reasoning.

All of this concerns a game installation owned and run locally. Nothing here touches a network or
another machine.

- [1. Method](#1-method)
- [2. The target](#2-the-target)
- [3. Getting code into the process](#3-getting-code-into-the-process)
- [4. Files: how the game opens them, and how to serve files that do not exist](#4-files-how-the-game-opens-them-and-how-to-serve-files-that-do-not-exist)
- [5. File formats](#5-file-formats)
- [6. Song select internals](#6-song-select-internals)
- [7. Weaknesses found, and what was done with each](#7-weaknesses-found-and-what-was-done-with-each)
- [8. Input](#8-input)
- [9. Drawing our own things](#9-drawing-our-own-things)
- [10. Lessons from the cabinet](#10-lessons-from-the-cabinet)
- [11. Address reference](#11-address-reference)

## 1. Method

- **Static analysis.** Full decompilations of `EZ2B.exe` (Ghidra's `FUN_xxxxxxxx` naming and IDA/Hex-Rays'
  `sub_xxxxxx` are both quoted in the source). They are not in this repository.
- **The mod as a probe.** The early builds (v3–v4) did nothing but hook functions and log their
  registers, arguments and memory. Before writing any custom song into a structure, they mapped it by
  dumping what the game itself put there.
- **Marker builds.** When it was unclear which of several arrays fed a sprite on screen, a build
  wrote a distinct letter into each candidate and the screen showed which one it was. Three earlier
  guesses about the category label had been reasoned about rather than looked at, and all three were
  wrong.
- **Decrypting the shipped data.** Native charts, judgement files and keysound maps were decrypted
  and measured (note lengths, tick grids, judge windows, disc geometry), so that generated files
  match them byte for byte where it matters.
- **Runtime tracing.** `tracer/ez2tracer.dll` single-steps the game around a crash; the mod's
  `GsProbe` names the function whose stack cookie was smashed; `HangWatchdog` reports where a frozen
  thread sits.
- **Measuring on the real machine.** The log stamps every line with milliseconds since load and
  breaks each slow frame down by subsystem. Most performance work began from a cabinet log, not a
  guess.

## 2. The target

| Fact | Consequence |
|---|---|
| `EZ2B.exe`, 32-bit, image base `0x00400000`, no ASLR and no relocations | Every address in the decompilation is the address at run time: functions, globals and immediates can be patched by absolute address. |
| Large-address-aware | 4 GB of address space on a 64-bit Windows, 2 GB on the XP cabinet. |
| Built with `/GS`: 275 functions check a stack cookie (`cmp ecx,[0x4AFCA0]` at `0x47882F`) | A stack overrun ends the process with no catchable exception. That explains several "silent" crashes, and it is what `GsProbe` hooks. |
| DirectDraw 7 / Direct3D 7 renderer, 640×480 | Textures are DirectDraw surfaces; on XP the system's DirectDraw is used, on newer Windows dgVoodoo. |
| Launched through 2EZConfig's `2EZ.dll` | Its **Extra DLLs** setting loads our DLL into the process. No loader of our own is needed. |

## 3. Getting code into the process

Every patch checks the bytes it expects before writing, and stays off if they differ. The techniques,
from least to most invasive:

1. **Import-table hooks** (`iat.cpp`): `CreateFileA`, `FindFirstFileA`/`FindNextFileA`,
   `SetCurrentDirectoryA`, `GetFileAttributesA`, `ReadFile`, `GetFileSize`, `SetFilePointer`,
   `CloseHandle`, `VirtualAlloc`, `CreateWindowExA`, `ExitProcess`/`TerminateProcess`. The same
   technique is applied to `quartz.dll`, to redirect DirectShow's own file opens.
2. **Inline jumps with trampolines** (`patch.cpp`): a 5- to 11-byte `jmp` over a function's prologue
   to a gateway that saves registers, calls the mod and replays the overwritten bytes. The v3
   trampolines, on the song loader and the result and ranking screens, started everything.
3. **Function replacement**: the jump goes to our implementation and never comes back. Used for the
   category filter `sub_435070` (section 7), the input getter `getState` `0x40C1C0` and the stem copy
   `sub_412F00`.
4. **Call-site retargeting**: one `call rel32` pointed elsewhere. The lane cover is drawn by
   retargeting the keys panel's call to its overlay draw (`0x42D672` → `0x42BBE0`).
5. **Immediate patches**: capacity constants and the `cmp r32, imm8` ceilings on the category index
   (`83 /7 ib`), widened so the wheel accepts 100+ categories.
6. **Allocation inflation**: the song-select object is a single `VirtualAlloc` of about `0x210000`
   bytes. The `VirtualAlloc` hook recognises that request by size and returns a much larger region,
   so the song arrays can be relocated inside it and hold 24,000 songs instead of 436.

## 4. Files: how the game opens them, and how to serve files that do not exist

- Most files go through `CreateFileA`, but charts and keysound maps are opened through the CRT's
  internal `fsopen` wrapper (`FUN_0047960E`). This was found in v7, when a redirect on `CreateFileA`
  alone left the chart loader untouched.
- Song folders are entered with `SetCurrentDirectoryA("SOUND\<id>")`, and files inside them are then
  opened by bare name, so the current directory is part of the path.
- A custom song is a set of ids (`bms000123`) that exist nowhere on disk. Every open that names one is
  answered by the mod: from the song's BMS folder, from `CustomCache`, or **from memory**. For the
  last case, `CreateFileA` returns a marker handle and the hooked `ReadFile`/`GetFileSize`/
  `SetFilePointer`/`CloseHandle` serve a byte buffer. Generated art therefore never touches the disk.
- The preview `.wav` is the one exception: it is opened by the CRT sample loader, which needs a real
  file, so it is written to disk for that one open.

## 5. File formats

| Format | What was found |
|---|---|
| **FNEX encryption** (`.ini`, `.ez`, `.ezi`) | A 512-byte key table per file kind. The data is walked in blocks alternating 8 and 16 bytes; each byte has the next key byte added (mod 256) and is written into its block in reverse order. `KParser` decrypts an `.ini` into a sibling `.int` before reading it. Implemented in `fnex_crypto.cpp` from the community `ez2djDecoder`/`Encode-FNEX` tools and checked against the game's own unpacker. |
| `.ezi` → `.ezz` | The keysound map is shipped encrypted, and the game unpacks it (`FUN_00410C90`) into a plaintext `.ezz` of `sample_id sample_type sample.ssf` lines. Feeding it plaintext produced garbage (v10 → v10.1). |
| `.ez` charts | Event lists on a tick grid of 192 per measure in the shipped charts; the mod uses 384 for BMS precision. A tap note always carries length 6, the format's floor for "instantaneous". Judge windows are in ticks and widened by the engine by a fixed 3 ticks, so they must be scaled to the grid or a custom chart judges twice as tightly. |
| Per-chart `.ini` (gds) | Judgement windows, gauge rates and level. Without one the engine falls back to Kool 6 / Cool 24 / Good 36 / Miss 72 and level 99. |
| `.abm` | A BMP with the magic `AW` and four header dwords XOR-obfuscated (offsets 10, 18, 22, 28 with `0x109A`, `0xCFA1`, `0x51AE`, `0xB18F`). All song art is `.abm`. |
| `.ssf` | An 18-byte header (channels, rate, byte rate, alignment, bits, data size) followed by PCM. |
| `song.bin`, `Stage.ini` | The native song list and categories per mode. They are never modified: v23 redirected `Stage.ini` to a synthetic file and crashed on entering the mode, so categories are written into memory instead. |
| `.scr` / `.str` | A background timeline and the scenes it plays, undocumented past their headers. Times are in 60 Hz scene frames, not milliseconds: a `.str` declares `fps 60`, and a cymbal flash keyed at 0 and 30 lasts half a second. Reversed far enough to composite native backgrounds for previews. |
| Disc art | A circle of radius 125 on a 256×256 canvas, black outside, identical across every shipped disc. |

## 6. Song select internals

The song-select object (`ecx` of its methods) holds everything. Offsets are from its base.

| Offset | Field |
|---|---|
| `+0x12C` | `visible[]`: the filtered list the wheel draws, 884-byte (`0x374`) entries |
| `+0x5E2BC` | `master[]`: every song of the mode (relocated by the mod into the inflated region) |
| `+0x7C` / `+0x80` / `+0x84` | visible count / master count / selected row |
| `+0x1C` | active category |
| `+0xF8` | 1 = reload the big disc on the next frame |
| `+0xBC57C` | the category strip's index |
| `+0xBFDB4` | 4 = browsing, 5 = choosing a difficulty |
| `+0x20C6BC` | > 0 while the option panel is open |
| `+0x20C6D4` | difficulty column (0 NM, 1 HD, 3 SHD, 4 EX) |
| `+0x20E894` | frames since the cursor moved; past 30 the preview `.wav` is opened |

A song entry (884 bytes):

| Offset | Field |
|---|---|
| `+0x000` / `+0x004` | small disc texture / song name texture (drawn straight from the record) |
| `+0x10C` | id, up to 15 characters |
| `+0x210` | float read by the difficulty stepper: when non-zero, the dormant leftmost column accepts the cursor (the mod's EZ column) |
| `+0x214`, `+0x220`, `+0x22C`, `+0x238` | NM/HD/SHD/EX blocks of 12 bytes: level float, then two BPM floats |
| `+0x370` | version index, which picks the label under the title |

Other findings:
- **Category tables** are globals: counts at `0x03789BD4` and ids at `0x03789C90` (16 bytes each, 436
  per category, 47 categories). Only 16 of the 47 hold songs of their own (the game's versions);
  the rest are views (HOT, NEW, ALL, the LV levels, alphabetical). The mod relocates both tables, keeps
  19 native categories (HOT, NEW, ALL and the 16 versions), drops the LV and alphabetical views to make
  room for its custom categories from slot 19 on, and repoints the filter's two absolute references.
- **Three different sprite arrays** draw category art: the tab strip, the label under the title and
  the two-line overlay of the category-change animation. The game's own loader refills the first
  after we fill it, so it has to be held every frame.
- **Textures** are loaded through `0x408D00` and cached in a binary tree keyed by file name
  (`sub_414460`). The pool is a bump allocator: nothing is freed until the screen is torn down. Each
  texture also costs a GDI object.
- **Rendering**: `DrawSprite` `0x403F80`, `SetBlend` `0x40A490` (render states 19/20), frame end
  `0x40A470`. A `KTexture` has a UV window (`+0x88` flag, `+0x8C..+0x98` u0 v0 u1 v1), which the lane
  cover uses to show only the lower slice of its picture.

## 7. Weaknesses found, and what was done with each

The engine was built for about 430 songs on fixed hardware. Scaling it to 23,000 songs exposed the
following. Each was turned to the mod's advantage, or neutralised so it could no longer take the
process down.

| Weakness | Effect | What was done |
|---|---|---|
| The category filter `sub_435070` builds the list in a **376 KB array on the stack** (`mov eax,0x5E1C8` / `__chkstk`): 436 entries, **no bounds check** | A category with more than 436 songs writes past the array and `/GS` kills the process | The function is replaced whole: the list is built on the heap from a hashed id index, and the ceiling becomes 872 (the exact room before the next object field) |
| The same filter finds each id by a **linear `_stricmp` over `master[]`** | O(ids × songs): about 26 million compares per category change at 60,000 songs | Hash index, rebuilt only when `master[]` changes |
| The filter **publishes the matched count but writes only playable songs**, and never reconciles them | The wheel scrolls into stale slots; the disc loader `sprintf`s a garbage id into `system\disc\%s.bmp`, the open fails, and the failure path overruns a 260-byte stack buffer (the "CYT-2.2 crash") | The tail is filled with the last valid entry; the count is left alone because other code depends on it |
| A **missing `.abm`** runs `sub_407E50 → sub_412F00 → sub_407B60` off a 260-byte stack buffer; `sub_412F00` also copies a **negative length** for some stems | Any art file that fails to generate is a crash | `sub_412F00` replaced; every role always gets a file, a flat tile if rendering fails |
| `KGraphics::loadBmpTexture` reads the **whole file into a static 8 MB buffer** (`0x530C48`) with no size check, right before the texture profile | A 1920×1080 32-bit stagefile overwrites `.data` | Oversized art is never served; generated art stands in |
| A category with **zero songs** reaches a `% count` | `STATUS_INTEGER_DIVIDE_BY_ZERO` at `0x440439` / `0x441911` | An empty category is never registered; modes with no songs get AutoChart copies |
| The texture pool **never frees**, and each texture costs a **GDI object** (10,000 per process) | Browsing 25 categories exhausted GDI; then every allocation failed at once, including GDI+ start-up | Textures load only for a window around the cursor; a fixed ring of textures is reused for custom art |
| The renderer passes a **row index straight off its stack**, not always valid | A negative modulo put a read 584 bytes below the object: the LV-category crash | Every index from the renderer is validated before use |
| A **texture pool rebuild** after each song releases every surface, ours included, without recreating song select | Dangling surfaces drawn into dgVoodoo: crash after a song; previews and labels vanished | Each overlay texture is checked to still point at our surface every frame, and rebuilt when not |
| The `.str` clip arena (`KProfile` at `0x530C2C`) is `memset` whole at every screen init | Once raised to 512 MB, it made 512 MB resident for nothing | Its `push imm32` put back to 64 MB |
| **Pure black is the colour key** on opaque sprites | A black lane cover or overlay drew only its edges | Black is lifted to the darkest shade that survives a 16-bit surface |
| The **turntable total resets** past a small per-screen limit | A delta computed from it alternates sign | The raw 8-bit counter is read instead |
| The effector reads **E1 through `getState`** | Holding E1 to adjust the cover toggled bass boost | `getState` replaced; only the effector's call range sees E1 as held, and a tap is replayed on release |
| **No ASLR, no relocations** | — | What made everything above possible: every address is fixed and every patch is deterministic |

## 8. Input

- The input object (`Obj40C180`) lives at `*(0x1B2E710)`. Each button is a 20-byte slot from `+0x10`:
  state 2 on the frame it is pressed, 3 while held, below 2 when up. Slots: 4/5 START,
  6–9 EFX1–EFX4, `0x0F`/`0x10` the dial keys, `0x11`/`0x19` the pedals.
- The poll (`FUN_0040C4B0`) reads the IO board through `in` from ports `0x101`–`0x106`. The 1P
  turntable is port `0x103`: its raw byte is kept at `+0x420220`, the per-poll delta (wrapped to
  ±128) at `+0x420230`, and a running total at `+0x420238`, which is zeroed whenever it passes
  `+0x420268` (5, 10 or 20, set by whichever screen ran last).
- Song select's pager and EFX4 do nothing while a pedal is held, which is why every mod shortcut is a
  **PEDAL +** chord: those combinations are free.

## 9. Drawing our own things

- An overlay is a texture the game already knows how to draw, re-pointed at a surface the mod owns:
  load a base file through the game's loader under a **unique alias** (the alias is the cache key; two
  overlays sharing one were handed the same texture), create a surface, and swap it in.
- Pixels go in with `Lock`/`Unlock`. A `GetDC` on a texture surface under dgVoodoo is a GPU readback
  and costs about 7 ms.
- GDI will not antialias small Arial, so the panels are drawn at several times their size and
  averaged down, to sit next to the game's pre-smoothed art.
- Song titles have no declared encoding. UTF-8, Shift-JIS and EUC-KR are told apart by the bytes they
  spell, and drawn with the wide API, so a title never passes through the system code page.

## 10. Lessons from the cabinet

The cabinet is a Pentium E5800 with 2 GB of RAM on Windows XP 32-bit, and most performance problems
only showed up there:

- Work that takes milliseconds on a desktop takes a second here: copying the 23k-song catalogue on
  every category change, or cutting a preview on the game thread.
- The process has 2 GB of address space. Caches are sized per machine (`[config] memory tier`), and
  the smallest tier also lowers the BGA preview's frame rate.
- Anything slow belongs on a background thread, with the game's own timers held until its result is
  ready. Freezing the game thread also eats turntable input.

## 11. Address reference

| Address | What |
|---|---|
| `0x402880` | the game's logger (patched to write into our log) |
| `0x403F80` | draw sprite |
| `0x404950` | generic textured-quad draw (null-texture guard) |
| `0x407450` | create surface |
| `0x407A50` | renders text into a texture (null-destination guard) |
| `0x408D00` | load texture (texture ring hooks here) |
| `0x40A470` / `0x40A490` | frame end / set blend states |
| `0x40C1C0` | `Obj40C180::getState` (replaced) |
| `0x40C4B0` | IO poll |
| `0x410C90` | `.ezi` → `.ezz` unpack |
| `0x411090` / `0x411460` | load `.ezi` keysound map / load `.ez` chart |
| `0x412F00` | stem copy (replaced) |
| `0x414554` | texture pool insert (pool-full guard) |
| `0x41E620`–`0x41F85F` | effector update (sees E1 through the lane cover filter) |
| `0x423A90` | song / chart load |
| `0x42BBE0`, `0x42D5C0`, `0x42D672` | panel overlay draw, keys panel update, the call retargeted for the lane cover |
| `0x432200` | wheel draw (null-slot guard) |
| `0x435070` | category filter (replaced) |
| `0x4433B9` | song select, after the background is drawn (category overlay) |
| `0x451F10`, `0x456B20`, `0x4570A0`, `0x457A90`, `0x433D50` | result, song Top-5, global Top-99, ranking UI, song-select Top-5 draw |
| `0x47960E` | CRT `fsopen` wrapper |
| `0x47882F` / `0x4AFCA0` | `/GS` cookie check / the cookie |
| `0x4ABD14` | difficulty column cursor (0 = the EZ column) |
| `0x530C2C` / `0x530C48` | `KProfile` clip arena / static 8 MB texture read buffer |
| `0x1B2E320`, `0x1B2F054` | render context, graphics context pointer |
| `0x1B2E680` | current mode name |
| `0x1B2E710` | `Obj40C180*`, the input object |
| `0x1B2EB04` | selected song id, kept across category changes |
| `0x1B2EF8C` | vertical lift of the play field |
| `0x03789BD4` / `0x03789C90` | category counts / category id tables |
