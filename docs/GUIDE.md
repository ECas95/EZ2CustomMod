# EZ2CustomMod user guide

How to install the mod, add your own songs, play with the extra controls and change its settings,
with examples you can copy. The examples use made-up names (`My Pack`, `Song Title - Artist`).
Replace them with your own.

- [1. What you need](#1-what-you-need)
- [2. Installing](#2-installing)
- [3. Adding songs](#3-adding-songs)
- [4. Making a category look nice](#4-making-a-category-look-nice)
- [5. Controls](#5-controls)
- [6. Lane cover](#6-lane-cover)
- [7. What song select shows](#7-what-song-select-shows)
- [8. Backgrounds and previews](#8-backgrounds-and-previews)
- [9. Custom courses](#9-custom-courses)
- [10. Changing settings](#10-changing-settings)
- [11. Updating and going back](#11-updating-and-going-back)
- [12. When something goes wrong](#12-when-something-goes-wrong)
- [13. Glossary](#13-glossary)

## 1. What you need

- **EZ2AC FNEX**, already working, with **2EZConfig**: the game folder has `EZ2B.exe`, `2EZ.dll` and
  `2EZConfig.exe`.
- **Windows XP SP3** (an arcade cabinet) or any later Windows.
- **BMS songs**: folders containing `.bms`, `.bme` or `.bml` charts with their sounds. Most BMS
  packs work as they are.

## 2. Installing

In these examples the game is in `C:\Games\EZ2AC\`.

**Step 1. Copy the files.** Download `EZ2CustomMod-vX.Y.Z.zip` from the
[latest release](../../../releases/latest), open it, and copy everything inside its top folder into
the game folder. Afterwards the game folder looks like this:

```text
C:\Games\EZ2AC\
├── EZ2B.exe                 (the game, already there)
├── 2EZ.dll, 2EZConfig.exe   (already there)
├── EZ2CustomMod.dll         ← the mod
├── BMSMod.ini               ← its settings
├── ffmpeg.exe               ← decodes unusual audio and video formats
├── pthreadGC2.dll           ← needed by ffmpeg.exe
├── wqy-microhei.ttc         ← font for Japanese, Chinese and Korean titles
└── LaneCover\               ← pictures for the lane cover (empty to start with)
```

**Step 2. Windows XP only.** Also copy `xp\gdiplus.dll` from the ZIP into the game folder. On
Windows Vista or later, skip this step: Windows already provides that file.

**Step 3. Turn the mod on.** Run `2EZConfig.exe`, open **Settings → Extra DLLs**, type
`EZ2CustomMod.dll` and save.

**Step 4. Add at least one song** (next section), then start the game.

The first start scans every song and can take a while: a few seconds for a small library, several
minutes for tens of thousands of songs on an old PC. A small window shows the progress. Later starts
only look at what changed and are much faster.

To confirm the mod is running, open `C:\Games\EZ2AC\logs\EZ2CustomMod.log`. Its first lines include
`[version] EZ2CustomMod_v…`.

## 3. Adding songs

### The folder layout

Songs go in a folder called `BMSData`, inside the game folder, always three levels deep:
**mode → category → song**.

```text
C:\Games\EZ2AC\BMSData\
└── 5KeyMix\                         ← the mode
    ├── My Pack\                     ← a category: shows as "My Pack" on the wheel
    │   ├── Song Title - Artist\     ← one song: its charts, sounds and pictures
    │   │   ├── normal.bms
    │   │   ├── hard.bms
    │   │   ├── expert.bme
    │   │   ├── bgm.ogg
    │   │   ├── kick.wav
    │   │   ├── snare.wav
    │   │   ├── jacket.png
    │   │   └── movie.mp4
    │   └── Another Song - Another Artist\
    │       └── ...
    └── Another Pack\
        └── ...
```

Rules:
- **One folder per song**, with everything that song needs directly inside it. Sounds may also sit in a
  `sound\` subfolder, as many packs ship them.
- **Category = folder name.** Use short names (`My Pack`, `ARCADE.1`); the wheel tab has little room.
- **`5KeyMix` is the main mode.** Put your songs there. The mod builds versions of every song for the
  other modes on its own (see AutoChart below).
- Other mode folders are accepted too, and their songs then appear only in that mode:
  `5KeyOnly`, `StreetMix`, `7KeyMix`, `ClubMix`, `SpaceMix`, `RubyMix`, `Ez2Catch`, `TurnTable`,
  `CV2Mix`. A folder with any other name is skipped, and the log says so.

### Example: adding a new pack

You downloaded a pack called "Summer Hits" with 40 song folders.

1. Create `C:\Games\EZ2AC\BMSData\5KeyMix\Summer Hits\`.
2. Copy the 40 song folders into it, so you get
   `BMSData\5KeyMix\Summer Hits\<song folder>\*.bms`.
3. Start the game. In 5KeyMix, a category **Summer Hits** appears on the wheel with the 40 songs in
   alphabetical order.

If the download unpacks to an extra level, such as `Summer Hits\Summer Hits\<songs>`, move the songs up
one level. A song folder must sit directly inside the category folder.

### How charts become difficulties

Each song shows up to four difficulties: **NM**, **HD**, **SHD** and **EX**. The mod sorts a song's
charts from easiest to hardest and fills them in that order:

| Charts in the folder | Result |
|---|---|
| 1 chart | NM only |
| 3 charts (easy, normal, hard) | NM, HD, SHD |
| 4 charts | NM, HD, SHD, EX |
| 5 charts | the easiest goes to the extra **EZ** column (far left), the other four to NM–EX |

The level shown is worked out from the notes themselves, not copied from the chart's `#PLAYLEVEL`, so
levels are comparable across packs. To use the chart's own number, set `LevelFromNotes=0` (section 10).

### AutoChart: your songs in every mode

You only need 5-key charts. For every song in `5KeyMix`, the mod generates playable charts for
StreetMix, 7Key, ClubMix, SpaceMix, RubyMix, Catch, TurnTable and CV2. The same "Summer Hits"
category appears in all of them. Nothing is written to your BMS files.

If a song also ships a chart made for 7 keys, or a double chart, that one is played as written in
7KeyMix, ClubMix or SpaceMix instead of a generated one.

### Example: a category that is too big

A category holds up to **800 songs**. A bigger folder is split on its own into several categories,
in title order, with the same name and a part number:

```text
5KeyMix\Big Pack\  (1,500 songs)   →   on the wheel:  Big Pack 1/2  (750 songs)
                                                      Big Pack 2/2  (750 songs)
```

Nothing has to be moved, and `details.txt`, `bg.png` and `ico.png` apply to every part. If you prefer
your own split (by artist, by year), split the folder yourself; parts are only made for folders over
800 songs.

### Example: removing or renaming songs

- **Remove** a song or category: delete its folder and restart the game.
- **Rename** a category: rename its folder. A song is identified by its full path (mode, category and
  song folder), so after a rename its favorites, play counts and Top-5 scores start over. To change
  only how a category's name looks, use `details.txt` instead (section 4).

## 4. Making a category look nice

Three optional files can go **in the category folder** (next to the song folders, not inside them):

```text
5KeyMix\Summer Hits\
├── details.txt      ← long name and subtitle
├── bg.png           ← background shown while this category is selected
├── ico.png          ← icon shown while this category is selected
└── <song folders>
```

**`details.txt`** is plain text with two lines:

```ini
CATEGORY=Summer Hits 2026
SUBTEXT=40 songs - collected by Your Name
```

`CATEGORY` is the big title in the category animation and `SUBTEXT` the smaller line under it. The
wheel tab always shows the short folder name.

**`bg.png`** is stretched to the 640×480 screen. **`ico.png`** should be square; 480×480 is ideal.

**Song pictures** need no setup. The mod uses the largest picture in the song folder for the disc and
the song name banner, and draws the title instead when there is none.

## 5. Controls

The cabinet buttons: **E1–E4** are the four effector buttons, **PEDAL** is the foot pedal, and
**START** confirms.

### In song select

| Press | What happens |
|---|---|
| Turntable / dial | move through the songs (as normal) |
| E2 / E3 | previous / next category (as normal) |
| **PEDAL + E2** / **PEDAL + E3** | jump 10 categories back / forward |
| **PEDAL + E1** | add the highlighted song to FAVORITES, or remove it (custom songs) |
| **PEDAL + E4** (tap) | change the order of the category's songs (custom categories; see below) |
| **PEDAL + E4** (hold about 1 second) | open the lane cover menu (section 6), in any category |

Hold PEDAL first, then press the button. The game's own songs and categories are left as they are.

**Song order.** Each tap of PEDAL + E4 moves to the next order: folder order → level low to high →
level high to low → title A to Z → BPM slow to fast → shuffle → back to folder order. A card on screen
names the order you are on.

**Special rows and categories:**
- **RANDOM** is the first row of every category. Choosing it plays a random song from that category.
- **FAVORITES**, **RECENT** and **MOST PLAYED** come first in every mode. They fill up as you mark
  favorites and play songs. Until then, each shows 30 random songs.

**Example: building a favorites list.** Highlight a song, hold PEDAL and press E1; a card says
"ADDED". Do the same for a few more songs, then go to the FAVORITES category: they are all there, in
every mode. PEDAL + E1 on the same song again says "REMOVED".

**When you choose a difficulty**, a card shows the speed the mod will set so that every song scrolls
at the same visual speed (constant speed). It can be turned off (section 10).

### During a song

| Press | What happens |
|---|---|
| **Hold E1 and turn the turntable** | lower or raise the lane cover |
| Tap E1 | bass boost, as in the original game |

## 6. Lane cover

The lane cover (called SUDDEN+ in other games) is a curtain over the top of the lanes, so notes appear
later and closer to the judge line. Many players read fast songs more easily with it.

**Set its size during a song:** hold E1 and turn the turntable. The size is saved and used from then
on. Turn it all the way up to remove the cover.

**Choose its picture in song select:**
1. Copy pictures (`.png`, `.jpg` or `.bmp`) into `C:\Games\EZ2AC\LaneCover\`. Tall pictures look best;
   they are stretched to about 1:2 (for example 256×512). Dark pictures make the notes easier to see.
2. In song select, hold PEDAL and hold E4 for about a second. The **LANE COVER** card opens with a
   thumbnail of the current picture.
3. E2 / E3 go through the pictures. The first entry, "BLACK (default)", is a plain black cover.
4. Press E1 to keep the picture shown, or E4 to leave without changing anything.

The wheel does not move while the menu is open. The lane cover is not shown in the scratch modes or
in Ez2Catch.

## 7. What song select shows

Besides the wheel, custom songs get a few extra read-outs. Native songs look exactly as before.

**Above the disc**, the chart under the cursor:

| Figure | Meaning |
|---|---|
| NOTES | how many notes to play (a long note counts once) |
| NPS | notes per second, on average |
| PEAK | notes per second in the busiest two seconds |
| CHORD | how many notes come together with another one |
| JACK | how many notes repeat the same lane quickly (within 0.15 s) |

**Along the bottom**, a **density graph**: 64 bars from the start of the chart to its end. Tall,
orange bars are the hard bursts; a long low stretch is a breather. The chart's length is in the corner.

**The disc** of the song under the cursor plays a few seconds of its background video after you
stop on it for a moment (section 8), and its music preview starts shortly after.

**The ranking** (E4 toggles it, as in the original) shows the Top-5 of custom songs too. Scores are kept
in `CustomCache\rank\`.

**A fifth difficulty.** Songs with five charts show the easiest one in an extra **EZ** column, to the
left of NM. Move the difficulty cursor all the way left to choose it.

**Notice cards.** A small card near the top confirms what the extra keys did: the new sort order, the
category you jumped to, a song added to or removed from FAVORITES, and the speed constant speed will
set. Each card fades after a couple of seconds.

**Example: comparing two hard songs.** Both say level 15. The first has PEAK 18 and a flat graph: it
is steady all the way through. The second has PEAK 31 and one tall orange spike near the end: it is
easy until a short, very dense burst. Pick the one that suits you.

## 8. Backgrounds and previews

**During a song**, the background (BGA) is chosen in this order:

1. the video the chart itself starts (its `#BMP` and channel 04 lines), at the moment the chart starts
   it;
2. any video in the song folder (`.mp4`, `.avi`, `.wmv`, `.mpg`, `.mpeg`, `.mov`), from the start;
3. the chart's **pictures**, if it shows pictures instead of a video (a still image, or a sequence),
   each at its own time;
4. otherwise a random clip from `Video\_spv\` in the game folder, repeated until the song ends.

Wide (16:9) videos and pictures fill the screen's height and lose a little at the sides. Other shapes
are fitted whole, with black bars, so nothing is squashed.

**Most videos play through Windows.** Formats Windows XP cannot play (for example old `.mpg` files)
are played through the bundled `ffmpeg.exe` instead, kept in time with the music.

**In song select**, two previews start when you stop on a song:

| Preview | What it is |
|---|---|
| **Video on the disc** | 12 seconds of the song's own video, starting 10 seconds in, looped inside the disc. Songs without their own video show their disc picture instead (never a random clip). |
| **Music** | 20 seconds taken from the first 40 seconds of the song's music, so it usually starts past the intro. The song folder needs a full music file (the biggest audio file is used). Songs made only of keysounds have no music preview. |

On a slow PC, lower the video preview's frame rate with `PreviewFps` (section 10), or turn it off:

```ini
[Visuals]
BgaPreview=0
```

**Example: a song with no background.** Copy any video file into the song's folder and restart the
game; it will play from the start of the song. To make it start at a given moment, add a `#BMP` line
and a channel 04 event to the chart, as BMS editors do.

## 9. Custom courses

The Radio Mix modes play **courses**: four songs in a row on one life gauge. The mod adds courses made
from your songs after the game's own channels, as **CUSTOM COURSE 1, 2, …**:

- each course takes four songs from one level band (1–4, 5–8, 9–12, 13–16 or 17–20), easiest first;
- 5RadioMix uses your 5-key songs; the other Radio Mix modes use 7-key, ClubMix and SpaceMix songs;
- the courses stay the same from one start to the next as long as you do not add or remove songs, so
  their rankings stay meaningful.

`CustomCourses` sets how many per mode (12 by default, 0 for none). 5RadioMix has room for 14 at most.

## 10. Changing settings

All settings are in `BMSMod.ini`, in the game folder. Open it with Notepad, change a value, save, and
restart the game. Every option has a comment above it explaining what it does. Values are `1` for on
and `0` for off unless the comment says otherwise.

**Example: notes feel late.** If your presses always register late (for example on a slow TV), move the
judgement:

```ini
[Gameplay]
JudgeOffsetMs=15
```

Positive values mean "my presses arrive late, count them earlier". To find a good value, set
`TraceJudge=1` under `[Debug]`, play two or three songs, and look for the `[judge] song stats` line in
the log: it suggests a number. Then set `TraceJudge=0` again.

**Example: turn off constant speed** to keep each song's own speed:

```ini
[Gameplay]
ConstantSpeed=0
```

**Example: no lane cover at all:**

```ini
[Gameplay]
LaneCover=0
```

**Example: the lane cover moves too fast (or too slow) with the turntable:**

```ini
[Gameplay]
LaneCoverStep=4      ; smaller = slower. A negative number reverses the direction.
```

**Other useful settings:**

| Setting | What it does |
|---|---|
| `[Gameplay] VirtualCategories=0` | hide FAVORITES, RECENT and MOST PLAYED |
| `[Gameplay] CustomCourses=0` | no custom courses in Radio Mix |
| `[Gameplay] PreviewFps=10` | a smoother-running song select on slow PCs (fewer frames in the video preview) |
| `[Gameplay] EnglishTextures=0` | the game's menus in their original language |
| `[Gameplay] DetailPanels=0` | hide the figures and the density graph |
| `[Gameplay] CategorySort=0` | no sort order or category jumps with the pedal |
| `[Gameplay] HudPanel=0` | no notice cards |
| `[Visuals] BgaPreview=0` | no video on the song-select disc |
| `[Visuals] CategoryOverlay=0` | ignore every category's `bg.png` and `ico.png` |
| `[CustomSongs] LevelFromNotes=0` | show the level written in the chart instead of the computed one |
| `[AutoChart] Enable=0` | only use the charts you provide; no generated charts for other modes |

Leave `[Debug] EnableFunctionTrace=1` as it is: despite the name, it switches the mod on.

### Files the mod keeps

The mod writes everything it needs into `CustomCache\` in the game folder.

| File | What it is | Safe to delete? |
|---|---|---|
| `scan.cache` | the list of scanned songs | yes; the next start scans everything again |
| `idmap.txt` | a permanent number for each song | **no**: scores and favorites depend on it |
| `playdata.txt` | favorites, recently played and play counts | only if you want to reset them |
| `rank\` | the Top-5 scores of your custom songs | no |
| `native_model.cache` | what AutoChart learned from the game's own charts | yes; it is learned again |

## 11. Updating and going back

**To update:**
1. Rename the current `EZ2CustomMod.dll` to keep it, for example to
   `EZ2CustomMod.dll.bak_2026-10-22`.
2. Copy in the new `EZ2CustomMod.dll`. Compare your `BMSMod.ini` with the new one, and copy over any
   new options you want.
3. Start the game. The first `[version]` line of the log shows the new version.

**To go back:** delete the new `EZ2CustomMod.dll` and rename the kept file back to
`EZ2CustomMod.dll`. You never need to touch `CustomCache\` or `BMSData\`.

**To remove the mod:** delete `EZ2CustomMod.dll` from the Extra DLLs setting in 2EZConfig. The game
is exactly as it was, because the mod never changes the game's own files.

## 12. When something goes wrong

The log, `logs\EZ2CustomMod.log`, is recreated on every start. Each line begins with the number of
milliseconds since the mod loaded. Searching it for the words below usually finds the cause.

| What you see | What to check |
|---|---|
| No custom songs and no log file | 2EZConfig's Extra DLLs setting; on XP, that `gdiplus.dll` is in the game folder |
| A category is missing | the folder layout (section 3); the log line `skip first-level folder` means the mode folder name is wrong |
| A song is missing from its category | `missing=` in a `[filter]` line, or `TRUNCATED` in a `[category]` line |
| A song shows no difficulties or will not start | its folder must contain the `.bms` files directly, not inside another folder |
| A title shows strange characters | the font file `wqy-microhei.ttc` must be in the game folder |
| A song name or disc picture is blank | `RENDER FAILED` in the log |
| Song select stutters | `[slowframe]` lines name what took the time; on slow PCs try `PreviewFps=10` |
| The lane cover does not move | while turning, the log should show `[lanecover] turntable raw=` |
| The lane cover is blank | `[lanecover] texture upload FAILED` |

When reporting a problem, include the log file: it says what the mod was doing at the time.

### A note on old hardware

On an old cabinet PC, the disk and the processor limit the mod much more than the graphics card. An
**SSD** speeds up start-up, song pictures and previews the most. On Windows XP, more than 4 GB of RAM
makes no difference.

## 13. Glossary

| Term | Meaning |
|---|---|
| BMS | A common format for rhythm-game charts: text files (`.bms`, `.bme`, `.bml`) plus their sounds |
| Chart | One difficulty of a song: the notes to play |
| Keysound | A short sound a note plays when hit |
| BGA | The background video or pictures behind the notes |
| Category | A group of songs on the song-select wheel; here, one folder |
| AutoChart | The mod's generator of charts for the other game modes |
| Lane cover / SUDDEN+ | A curtain over the top of the lanes |
| E1–E4 | The cabinet's four effector buttons |
