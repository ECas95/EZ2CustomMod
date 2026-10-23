# EZ2CustomMod

A DLL for **EZ2AC FNEX** that loads BMS/BME/BML songs from a `BMSData` folder and lists them in song
select as if they were native songs, without modifying any of the game's own files.

It runs on the original cabinet (Windows XP 32-bit, Pentium Dual-Core, 2 GB RAM) as well as on
modern PCs.

## Features

- **Custom songs**: scans `BMSData\<Mode>\<Category>\<Song>`. Each category folder becomes its own
  category on the wheel, sorted alphabetically.
- **Direct in-memory loading**: the BMS chart is translated into FNEX events when the song starts.
  WAV, MP3, OGG, FLAC and other audio is decoded in memory, and the BGA plays from the original video.
- **AutoChart**: generates virtual charts for every mode (StreetMix, 7Key, ClubMix, SpaceMix, RubyMix,
  Catch, TurnTable, CV2) from the 5-key charts, without writing any BMS file.
- **Better song select**: audio and BGA previews, generated art (song name, disc, eyecatch), NPS and
  density panels, Top-5 rankings for custom songs, and a RANDOM row in every category.
- **Virtual categories**: FAVORITES, RECENT and MOST PLAYED.
- **Shortcuts**: jump 10 categories, change the sort order (folder, level, title, BPM, shuffle) and
  mark favorites.
- **Lane cover (SUDDEN+)**: adjusted in play with E1 + turntable. Its picture is chosen in song select
  from a menu (hold PEDAL + E4).
- **Custom courses** for the Radio Mix modes, **constant speed**, **judge offset**, and English menu
  textures.

## Documentation

| Document | What it covers |
|---|---|
| [docs/GUIDE.md](docs/GUIDE.md) | User guide: installing, adding songs, controls, lane cover and settings, with examples |
| [CHANGELOG.md](CHANGELOG.md) | What changed in every version, from the v3 prototypes to today |
| [docs/RESEARCH.md](docs/RESEARCH.md) | How the game was reverse-engineered: structures, formats, weaknesses and addresses |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Building, tests, architecture, the log and troubleshooting |
| [BMSMod.ini](BMSMod.ini) | The production configuration, with every option commented |

## Quick start

1. Download the ZIP from the latest [release](../../releases).
2. Copy its contents into the game folder (next to `EZ2B.exe`).
3. Put your BMS songs in `BMSData\5KeyMix\<Category>\<Song>\`.
4. In **2EZConfig → Settings → Extra DLLs**, enter `EZ2CustomMod.dll`.

The first start scans the whole library and saves `CustomCache\scan.cache`. Later starts only re-read
what changed. See the [guide](docs/GUIDE.md) for the full steps.

## Building

```bat
build_mingw_direct_msys2_cmd.bat
```

This needs MSYS2 with `mingw-w64-i686-gcc` in `C:\msys64`. See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Repository layout

```text
src/          the DLL's source (C++17, x86)
tests/        standalone tests (one .cpp = one executable), the test runner and the XP check
tools/        Python maintenance scripts for BMSData and textures
tracer/       optional execution tracer (ez2tracer.dll) for diagnosing hangs
translation/  translation table for the menu textures
third_party/  minimp3, stb_vorbis and the WenQuanYi Micro Hei font, with their licences
docs/         user guide and developer documentation
```

The repository does not include the game's files (`EZ2AC.exe`, `EZ2B.exe`, `2EZ.dll`, the `system\`
and `SOUND\` data), any songs, or the decompiled code used during reverse engineering.

## Third-party components

- [minimp3](https://github.com/lieff/minimp3) (CC0) and [stb_vorbis](https://github.com/nothings/stb)
  (public domain / MIT): keysound decoding.
- WenQuanYi Micro Hei (Apache 2.0): font for Japanese, Chinese and Korean titles.
- The release ships a 2008 `ffmpeg.exe` (FFmpeg SVN-r11870, XP-compatible, LGPL/GPL) with its
  `pthreadGC2.dll`, used as a fallback for uncommon audio and video formats.
