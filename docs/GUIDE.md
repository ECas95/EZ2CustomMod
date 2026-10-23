# EZ2CustomMod guide

Installing on the cabinet, organising the songs, controls and settings.

- [1. Requirements](#1-requirements)
- [2. Installation](#2-installation)
- [3. Organising BMSData](#3-organising-bmsdata)
- [4. Controls](#4-controls)
- [5. Settings](#5-settings)
- [6. Updating and rolling back](#6-updating-and-rolling-back)
- [7. Troubleshooting](#7-troubleshooting)

## 1. Requirements

- **EZ2AC FNEX** with **2EZConfig**, which is what injects the DLL (`2EZ.dll`, `2EZConfig.exe`).
- Windows XP SP3 32-bit (the cabinet) or any later Windows.
- On XP, **do not** use dgVoodoo or DDrawCompat (both need Vista or later): the game uses the
  system's DirectDraw.

## 2. Installation

1. Download the ZIP from the latest release and copy its contents into the game folder, next to
   `EZ2B.exe`:

   | File | Purpose |
   |---|---|
   | `EZ2CustomMod.dll` | the mod |
   | `BMSMod.ini` | settings (every option is commented in the file) |
   | `ffmpeg.exe`, `pthreadGC2.dll` | XP-compatible fallback decoder for audio and video |
   | `wqy-microhei.ttc` | font for Japanese, Chinese and Korean titles |
   | `LaneCover\` | folder for lane cover pictures (png, jpg or bmp) |

2. **Windows XP only**: copy `GdiPlus.dll` from
   `C:\WINDOWS\WinSxS\x86_Microsoft.Windows.GdiPlus_6595b64144ccf1df_1.0.2600.*\` into the game folder
   as `gdiplus.dll`. XP keeps GDI+ only in WinSxS, where the DLL cannot find it. `deploy_to_cabinet.bat`
   does this step for you.
3. Open **2EZConfig → Settings → Extra DLLs** and enter `EZ2CustomMod.dll`.
4. Create `BMSData\` with your songs (see the next section).
5. Start the game. The first start scans the whole library and creates `CustomCache\`. With about
   23,000 songs this takes several minutes on the cabinet; later starts only re-read the folders
   that changed.

**Optional:** a `Translation\` folder shows the game's menus in English. It is built from the game's
own textures with `tools\texture_translate.py`.

### Deploying from the development PC

Connect the cabinet's disk to the PC and run:

```bat
deploy_to_cabinet.bat "H:\000 - MAIN" "F:\WINDOWS"
```

The first argument is the game folder on the cabinet's disk, the second that disk's Windows folder
(where `gdiplus.dll` comes from). It is safe to run again: it only copies what changed, and it never
copies dgVoodoo, logs or backups.

## 3. Organising BMSData

```text
BMSData\
  5KeyMix\                    source mode ([AutoChart] SourceMode)
    DJMAX\                    one folder = one category on the wheel
      details.txt             optional: the category's long name
      bg.png, ico.png         optional: the category's background and icon in song select
      Song Title - Artist\    one folder per song, no subfolders
        *.bms / *.bme / *.bml
        the audio, images and video the charts reference
```

- **Difficulties**: a song's charts are sorted by level and fill NM, HD, SHD and EX. If the song has
  a fifth chart, the easiest one goes to the EZ column (the leftmost one). With `LevelFromNotes=1`
  the level is worked out from the notes instead of taken from `#PLAYLEVEL`.
- **Other modes**: AutoChart builds StreetMix, 7Key, ClubMix, SpaceMix, RubyMix, Catch, TurnTable and
  CV2 versions from 5KeyMix. The original BMS files are never modified.
- **Limit**: a category shows at most 872 songs. If a folder holds more, the log says `TRUNCATED`:
  split it, for example into `CHU.1`, `CHU.2`, and so on.
- **Art**: the song name, disc and eyecatch are generated from the song's images. A song without
  images gets its title drawn instead.

### What CustomCache holds

| File | Contents | Safe to delete? |
|---|---|---|
| `scan.cache` | the scan results | yes: forces a full rescan |
| `idmap.txt` | each song's stable id (`bms000123`) | **no**: rankings and favorites would be lost |
| `playdata.txt` | favorites, recent and most played | only if you want to reset them |
| `rank\` | Top-5 tables for custom songs | no |
| `native_model.cache` | what AutoChart learned from the native charts | yes: it is learned again |

## 4. Controls

### Song select

| Input | Action |
|---|---|
| Turntable / dial | move the wheel (as in the original game) |
| E2 / E3 | previous / next category (as in the original game) |
| **PEDAL + E2 / E3** | jump 10 categories |
| **PEDAL + E1** | add the song to FAVORITES, or remove it |
| **PEDAL + E4 (tap)** | change the category's sort order: folder, level ↑, level ↓, title, BPM, shuffle |
| **PEDAL + E4 (hold ~1 s)** | open the lane cover menu |
| **RANDOM** row | the first row of every category: plays a random song from that category |

The **FAVORITES**, **RECENT** and **MOST PLAYED** categories come first in every mode. While empty,
each lists 30 random songs.

**Lane cover menu**: E2 / E3 change the picture (a thumbnail shows under the card), E1 keeps the
choice and E4 leaves without changes. The wheel does not move while the menu is open. The list holds
"BLACK (default)" plus every picture in `LaneCover\`.

### In play

| Input | Action |
|---|---|
| **Hold E1 + turntable** | raise or lower the lane cover (the size is saved to `BMSMod.ini`) |
| Tap E1 | bass boost, as in the original game (it fires on release) |

The lane cover does not appear in the scratch modes or in Ez2Catch.

## 5. Settings

All settings live in `BMSMod.ini`, and every option is commented there. The most useful ones:

| Option | Effect |
|---|---|
| `[Gameplay] LaneCover`, `LaneCoverSize`, `LaneCoverStep`, `LaneCoverImage` | lane cover on/off, size (0-1000), turntable sensitivity and picture |
| `[Gameplay] JudgeOffsetMs` | shift judgement earlier or later (with `[Debug] TraceJudge=1` the log suggests a value) |
| `[Gameplay] ConstantSpeed` | sets the speed so every song scrolls at the same visual speed |
| `[Gameplay] CustomCourses` | how many custom courses to add in Radio Mix (0 = none) |
| `[Gameplay] PreviewFps` | frame rate of the BGA preview in song select (more fps = more memory) |
| `[Gameplay] VirtualCategories`, `CategorySort`, `CustomTop5`, `DetailPanels` | turn each extra on or off |
| `[CustomSongs] LevelFromNotes` | level worked out from the notes (1) or taken from the BMS (0) |
| `[CustomSongs] KeysoundCacheMB`, `AssetCacheMB` | memory for keysounds and art; 0 = sized automatically for the machine |
| `[AutoChart] Enable`, `TargetModes`, `Intensity` | chart generation for the other modes |

`[Debug] EnableFunctionTrace=1` is **not** a diagnostic: turning it off disables the mod.

## 6. Updating and rolling back

1. Rename the cabinet's current DLL, for example to `EZ2CustomMod.dll.bak_YYYYMMDD_vXX`.
2. Copy in the new `EZ2CustomMod.dll`.
3. Start the game and check that the `[version]` line in `logs\EZ2CustomMod.log` shows the new version.

To roll back, delete the new DLL and remove the `.bak_…` suffix from the old one. `CustomCache` does
not need to be touched: it is compatible across versions.

## 7. Troubleshooting

The log is `logs\EZ2CustomMod.log` and starts fresh on every launch. Each line begins with the
milliseconds since the DLL loaded.

| Symptom | What to look for |
|---|---|
| The mod does not load (no log) | `Extra DLLs` in 2EZConfig; on XP, `gdiplus.dll` next to `EZ2B.exe` |
| Stutter in song select | `[slowframe]` and `[frametime]` lines: they name the part of the mod that took the time |
| A song name or disc does not appear | `RENDER FAILED` or `could not publish` in the log |
| Songs missing from a category | `TRUNCATED` (more than 872 songs) or `[filter] ... missing=` |
| The lane cover does not move | `[lanecover] turntable raw=` should appear while turning; if not, report it with the log |
| The lane cover is blank | `[lanecover] texture upload FAILED` |
| An unexplained freeze | set `[Debug] HangWatchdog=1` for one session (it costs performance; set it back to 0) |

### Performance on the cabinet

The CPU (2 cores) and the disk are the limits, more than the GPU. An **SSD** helps start-up, art
generation and previews the most. A 4-core LGA775 CPU helps too, because art, previews, keysounds
and BGA decoding run on background threads. More RAM (4 GB) only grows Windows' disk cache: XP 32-bit
limits the process to 2 GB.
