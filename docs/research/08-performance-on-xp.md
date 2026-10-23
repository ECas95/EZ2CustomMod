# 8. Performance on the cabinet

[← From BMS to FNEX](07-bms-to-fnex.md) · [Index](../RESEARCH.md) · [Next: song select, what the mod adds →](09-song-select-extras.md)

The game was built for its own hardware and about 430 songs. The mod runs on the same class of
hardware with 23,587 songs. This chapter collects what the cabinet's logs showed about time, memory
and graphics resources, and the rules that came out of it.

## 8.1 The machine

| Component | Cabinet |
|---|---|
| CPU | Pentium Dual-Core E5800, 3.2 GHz, 2 cores |
| RAM | 2 GB |
| GPU | GeForce GT 520 |
| OS | Windows XP Professional 32-bit, DirectX 9.0c |
| Display | 640×480 game picture, 60 Hz |

On a 32-bit XP the game process has **2 GB of address space**. The executable is large-address
aware, which would give it 4 GB on a 64-bit Windows, but not on the cabinet. The mod sizes its
caches from the machine at start-up (`[config] memory tier`):

| Host | Keysound cache | Art cache | BGA preview |
|---|---|---|---|
| 64-bit Windows | 384 MB | 256 MB | as configured |
| 3 GB process | 256 MB | 160 MB | as configured |
| XP 32-bit, or under 1.5 GB of RAM | 128 MB | 96 MB | capped at 10 fps |

## 8.2 How the logs measure

Every log line carries the milliseconds since the DLL loaded. Every five seconds a `[frametime]`
line gives the average and worst frame and the time spent in each of the mod's subsystems (trace
hooks, file I/O, video, texture loads, the category filter). Any frame over a threshold gets its
own `[slowframe]` line with the same breakdown. Most of the work in this chapter started from one of
those lines.

## 8.3 Start-up

The cabinet's log of 2026-10-19 (v36.0.0) shows the start-up timeline:

| Time (ms) | Step |
|---|---|
| 0 | DLL loaded, configuration read |
| 1,063 | scan cache loaded: 23,587 songs |
| 1,078 | AutoChart model loaded from its cache (the native `SOUND` walk skipped) |
| 4,563 | 84,210 charts rated from their notes |
| 6,860 | AutoChart: 188,696 virtual charts generated, 212,283 catalogue entries |
| 14,344 | catalogue order verified |
| 15,438 | catalogue indexed |
| 22,641 | song-select capacity set |
| 26,875 | game window released |

Two of the steps, catalogue verification (7.5 s) and capacity sizing (7.2 s), spent most of their
time building sorted per-mode copies of the catalogue. The sort compared category ranks by a linear
`_stricmp` scan over about 100 category names, twice per comparison, and moved whole song records on
every swap. Sorting (rank, index) pairs with the ranks in a hash map, and copying each record once at
the end, produces the identical order (pinned by a test over 300 random catalogues) far faster.

## 8.4 Two freezes in song select

Both were found on 2026-10-22, both were invisible on a development PC, and both made song select
feel broken on the cabinet.

### Changing category: 0.7–1.7 s

In the cabinet log, every category change produced a slow frame of 0.7 to 1.7 seconds, attributed to
the filter, even for a category of 47 songs. The time did not depend on the category's size, which
ruled the filter's own work out as the cause.

The cause was the step that runs before the filter: on **every** category change it built the
mode's full sorted catalogue (23,587 records) only to find that the custom songs had already been
injected, and then returned. Checking for the injection first, from a remembered first id, removed
the copy from the category change entirely.

### Resting on a song: 0.4–0.6 s

The game starts a song's audio preview half a second after the cursor stops
([chapter 6](06-song-select.md#66-the-hover-preview)). For a custom song, the preview had to be cut
from an MP3 first, on the game's thread. **Every song the cursor stopped on froze the game for 0.4 to
0.6 seconds, 86 times in one session.** While frozen, the game also stopped reading the turntable,
so scrolling felt sticky.

The cut now runs on a background thread, starting eight frames after the cursor stops. The game's
own preview countdown is held back until the file is ready, so the preview starts a little later and
the game never waits.

### A race over generated art

Some song names never appeared. The log showed `could not publish … (error 5)`: the background art
thread was renaming a freshly rendered file into place at the moment the game thread was reading and
deleting the previous one. Serialising those two file operations (only them, not the rendering)
removed the failures, and a check stops a fallback tile from replacing art that already arrived.

A later session with the fixed build shows zero publish failures and two slow frames in the whole
session. That session ran on a development PC from the cabinet's disk, not on the cabinet itself.

## 8.5 Graphics resources

The game's texture cache frees nothing while a screen is alive, and each texture costs a GDI
object. Windows gives a process **10,000 GDI objects** by default. Measurements from the cabinet:

- Loading a whole category's names and discs costs one GDI object per song. Twenty-five categories
  exhaust the budget, after which every allocation fails at once, including GDI+ start-up. This
  surfaced as "textures stop showing".
- Loading only a window around the cursor (16 names and 7 discs) and walking 110 categories took the
  process from 840 MB to 2,551 MB and to 6,973 GDI objects, against the 7,040 the window predicts.
- At about 251 KB of surface per texture, walking all 126 categories would cost 1,979 MB, enough on
  its own to exhaust XP's 2 GB.

The answer was a **fixed ring of reusable textures** for custom songs' names and discs, refilled as
the cursor moves, so browsing the whole library costs a constant number of textures. A typical
session now sits around 2,100–2,200 GDI objects.

## 8.6 Rules that came out of it

- **Measure on the cabinet.** A Pentium E5800 is several times slower than a development PC, and
  both freezes above were invisible there.
- **The game thread never waits.** Decoding, rendering and file I/O run on background threads, and
  the game thread only picks up finished results. Freezing the game thread also eats input.
- **Bound everything per screen.** The game frees nothing until a screen ends, so anything loaded
  per song must come from a fixed budget.
- **XP compatibility is checked, not assumed.** Every build's import table is checked against a list
  of APIs introduced after XP. Media Foundation is loaded at run time and is optional. GDI+ comes
  from a copy placed beside the game, because XP keeps it only in the side-by-side store.

## 8.7 Hardware

On this class of machine the CPU and the disk limit the game more than the GPU does:

- **An SSD** helps start-up, art rendering and previews the most, since all three read many small
  files.
- **A four-core LGA775 CPU** (if the board accepts one) helps because art, previews, keysound
  decoding and video decoding all run on background threads.
- **More RAM** helps only as disk cache: XP 32-bit still limits the process to 2 GB.
