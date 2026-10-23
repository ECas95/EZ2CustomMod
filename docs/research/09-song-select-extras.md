# 9. Song select: what the mod adds

[← Performance on the cabinet](08-performance-on-xp.md) · [Index](../RESEARCH.md) · [Next: lane cover and play options →](10-lane-cover-and-play-options.md)

[Chapter 6](06-song-select.md) describes song select as the game ships it. This chapter covers
everything the mod adds to that screen and how each addition was fitted into the original:
categories for 23,587 songs, the RANDOM row, the three virtual categories, sorting and category
jumps, Top-5 rankings for custom songs, a fifth difficulty column, the notice card, the detail
panels, constant speed, custom courses and per-category backgrounds.

Three rules apply to every addition:

- **Native behaviour stays native.** Songs, categories and controls that the game already has
  behave as before. The mod adds; it does not reinterpret.
- **Every new control is a pedal chord.** The game ignores E2, E3 and E4 while a pedal is held,
  and refuses to open the option panel with E1 in the same situation
  ([6.5](06-song-select.md#65-input)). PEDAL + E1…E4 is therefore free, and nothing native is
  shadowed.
- **Every check the game makes before acting, the mod makes too.** A shortcut does nothing while the
  option panel is open, while the player is choosing a difficulty, or while another menu owns the
  buttons.

## 9.1 Categories at 55 times the design size

**What the wheel shows.** The native wheel has 47 category slots, of which only 19 present songs of
their own: HOT, NEW, ALL and the 16 game versions. The other 28 are level and alphabetical views of
the same songs ([6.3](06-song-select.md#63-categories)). The mod keeps the 19, drops the 28 views,
and places custom categories from slot 19 onwards, one per folder in `BMSData\<mode>\`, in
alphabetical order. The production library has 106 such folders.

**Labels and icons.** Each category on the wheel needs four pictures: a tab, a large centre label, a
version label and a sort-style overlay. For the custom categories, the mod generates all four in
memory and serves them under file names the game never ships (`category_48.bmp` onwards, and
`categorybig_NN.bmp`, a name that appears nowhere in the executable). Two lessons came from this:

- The game's own loader fills the tab slots `0..46` from `category_01..47` in its constructor. After
  the native views were dropped, slot N held the tile of a category that no longer existed, so the
  kept native slots are re-pointed at `category_(N+4)`.
- The texture pool is a bump allocator keyed by file name that never frees
  ([12.7](12-art-text-and-resources.md#127-the-texture-ring)). Custom labels are therefore loaded
  **once per song-select object**, while the 19 native labels are re-copied on **every** filter
  pass. Copying them once failed: on the first pass, the game's loader had not run yet, 19 zeros were
  copied, and every native song lost its label. A round trip through the menu can also reuse the
  object's address while the pool has been reset underneath it, so "our first custom slot is empty"
  also triggers a reload.

**Long categories are split, not truncated.** The visible rows of a category live inside the
song-select object, and at 872 rows of 884 bytes they reach the object's live fields. Rather than drop
songs off the end of a big folder, a folder with more than 800 songs is published as several
categories, `FOLDER 1/3`, `FOLDER 2/3`…, filled in title order. The split runs after AutoChart has
created the other modes, so each mode is split on its own count, and after the scan cache has been
saved, so the cache keeps the real folder names. Everything downstream keys on the category name,
and `details.txt` and `bg.png` are looked up from the song's folder, so each part behaves as a
folder of its own.

**`details.txt`.** An optional file in a category folder sets the large label's two lines:

```ini
CATEGORY=Summer Hits 2026
SUBTEXT=40 songs - collected by Your Name
```

The tab always shows the folder name, because the tab is too small for anything longer.

## 9.2 The RANDOM row

Every category starts with a **RANDOM** row that plays a random song of that category.

**It is not a fake song.** On every category entry, row 0 receives a byte-for-byte copy of a real
row from the same category (id, levels, version, everything). When the player confirms it, the game
loads that song through its normal path, and nothing has to intercept the confirmation. A synthetic
record would have to satisfy every pointer the renderer follows. An early half-filled record had
already shown what happens otherwise: the BABY DANCE disc appeared on a custom song.

Three fields differ from the copied row:

- **The song-name picture** is replaced with a tile reading `RANDOM`, so the row does not look like a
  duplicate of the song it rolled.
- **The small disc** is set to nothing, so the roll stays hidden until the song starts.
- **The disc-reload flag** is raised. The renderer reloads the big disc only when the focused row
  changes. Entering a category leaves the cursor on row 0, so without the flag the previous
  category's disc would stay on screen.

The `RANDOM` tile is looked up again on every roll rather than cached. Playing a song rebuilds the
texture pool while reusing the song-select object, so a cached handle came back dangling and the row
drew nothing after every game. A lookup of a name the pool still holds costs nothing.

## 9.3 FAVORITES, RECENT and MOST PLAYED

The first three custom categories in every mode are built from what the player has done. They hold
up to 800, 100 and 100 songs.

**The key is the folder, not the id.** Ids are per mode: AutoChart gives every generated mode its
own copy of a song, with its own id. The song's folder, relative to `BMSData` and lower-cased, is the
same in every mode. Keying on it means that a favorite marked in 5-key is a favorite everywhere, and
that a play counts once per song, whichever mode it was in.

**Order comes from a counter, not a clock.** Each change increments a counter, and favorites and
plays record the counter's value. Only the order matters, and the cabinet's clock is not reliable.

**The file**, `CustomCache\playdata.txt`, has one tab-separated record per line:

| Record | Meaning |
|---|---|
| `C <n>` | the counter's current value |
| `F <added> <key>` | a favorite, and when it was marked |
| `P <count> <last> <key>` | a song played `count` times, the last time at `last` |

A malformed line is skipped, never fatal: the file is the player's history, and one bad line must
not cost the rest. The file is written whole to a temporary file and renamed, so a power cut during
a write keeps the previous version.

**Order inside each category:**

- **FAVORITES**: the most recently marked song first.
- **RECENT**: the most recently played song first.
- **MOST PLAYED**: the most plays first, ties going to the most recently played song.

**The categories update without leaving song select.** A generation number increases with every
change, and a virtual category whose list is older than the current generation is rewritten on its
next visit. A favorite marked now is in FAVORITES the next time the category is entered.

**A category is never empty.** The game divides by a category's size. Until the player has marked or
played anything, each of the three shows 30 random songs of the mode, the same 30 for the whole
session.

## 9.4 Sorting and category jumps

**Six sort orders** for a custom category, cycled by tapping PEDAL + E4:

| Order | Key |
|---|---|
| FOLDER ORDER | the order the category was injected in (alphabetical by folder) |
| LEVEL LOW > HIGH | the level of the difficulty column the cursor is on; NM if that column is empty |
| LEVEL HIGH > LOW | the same, descending |
| TITLE A > Z | title, case-folded |
| BPM SLOW > FAST | the NM chart's BPM |
| SHUFFLE | a linear congruential shuffle seeded from the clock |

Each order is computed from the folder order with a stable sort, so returning to FOLDER ORDER is
exact and the order of equal keys never drifts. After a re-sort, the category is re-seeded with the
cursor kept on the same song.

**Tap or hold.** The tap is detected on **release**. That frees the hold: PEDAL + E4 held for 800 ms
opens the lane-cover picker instead ([10.6](10-lane-cover-and-play-options.md#106-the-picker)).

**Jumps of ten categories** with PEDAL + E2 / E3 wrap around the ends as the native pager does. A
jump also repeats what the pager does after seeding a category:

- it reloads the disc;
- it restarts the preview countdown and stops the current preview;
- it replays the category label's animation;
- it plays the pager's own sound.

The jump writes the new category index and calls the same seed function as the pager, so the result
is indistinguishable from paging ten times.

## 9.5 Top-5 for custom songs

**Where the files go.** The game keeps one rank file per song and difficulty
([6.7](06-song-select.md#67-rankings)) and creates an empty file the first time the result screen
touches a song. For custom songs, that filled the game's `SOUND\` folder with files named after the
mod's ids. They now live in `CustomCache\rank\`. The redirect must cover the existence check as well
as the open. Redirecting only the open would make the check fail, and the game would overwrite the
table with an empty one.

**Loading them.** The constructor's ranking loader runs only over native songs, so the ranking
tables of custom rows held garbage, and early versions hid the ranking panel for them. The mod now
runs the same loader for the row under the cursor. It reads the five names and five scores for each
difficulty from the redirected files, and fills in the game's placeholder name and zero scores where
a file is missing or short.

## 9.6 The EZ column

Some songs ship five charts. The game's difficulty stepper accepts a dormant leftmost column on any
row whose float at `+0x210` is non-zero, and no native song sets it
([6.5](06-song-select.md#65-input)). The mod sets it for songs with a fifth chart, and puts the
easiest chart there.

The game draws no digit for that column, so the mod draws them:

- the EZ level at x = 89, always, lit while the column is chosen;
- the four native digits, dimmed, while the cursor is on EZ (the game hides them then);
- an `EZ` label above the column.

All of these use the game's own digit textures and follow the banner's slide animation, so they move
in and out with the rest of the row.

**Loading the right chart.** The EZ column publishes the same chart name as NM, so at chart load the
column cursor is what tells them apart. The request gets `-ez` before its extension, and the game's
text slot, from which it builds the names of the chart's `.ini` and rank files, is rewritten to
match. Radio Mix course stages name their charts themselves and are left alone.

## 9.7 The notice card

The mod's messages appear on one card, built the way the game builds its own panels
(`TT_course_notice1`, `TT_UseKey2_panel` and the ranking panel were decoded to match them).

**Two textures, no alpha channel:**

1. The **mask** is drawn with the blend pair (destination colour, inverse source alpha). Inside the
   card's shape, it multiplies the background down to near black. Outside, it leaves the background
   alone.
2. The **art** is drawn additively on top, so its black areas add nothing.

The result has rounded corners and a fade, even though neither texture has an alpha channel. The
fade is the modulation colour applied to both passes.

**The card.** It is 320×72 pixels:

- a near-black fill with the key panel's grey rim;
- a cyan title;
- a bold white line behind the yellow triangle that the effector rows use;
- a grey sub-line;
- key badges in the game's style (red for effectors, grey for the pedal), laid out right to left.

It fades in over 120 ms, settles 10 pixels down as it appears, holds, and fades out over 220 ms.
Showing it again while it is up replaces the text and restarts the hold, so a held button reads as
one live card rather than a stack of them. It is drawn at the end of the song-select frame, above
the HUD.

**What it shows:**

| Card | When |
|---|---|
| `EZ2CustomMod / SORT CATEGORY` | once per song-select entry, listing the extra keys |
| `SORT` | after PEDAL + E4, naming the new order |
| `CATEGORY` | after a jump, with "category N of M" |
| `FAVORITES` | `ADDED` / `REMOVED`, with the song's title; on the RANDOM row, a hint that it cannot be a favorite |
| `CONSTANT SPEED` | on entering the difficulty choice, the speed the next song will get (9.9) |
| `LANE COVER` | the picker ([10.6](10-lane-cover-and-play-options.md#106-the-picker)) |

## 9.8 Detail panels

Two read-outs describe the chart under the cursor. They sit in the two places the layout leaves
free.

**The figures**, above the disc at (300, 80):

| Field | Meaning |
|---|---|
| NOTES | playable notes; a long note counts once |
| NPS | notes per second, from the first note to the last |
| PEAK | the busiest two seconds, in notes per second |
| CHORD | the share of notes that sound together with another (within 1 ms) |
| JACK | the share of notes that repeat their lane within 150 ms |

**The density graph**, along the bottom between FREE PLAY and the logo at (385, 410):

- it has 64 bars, one per equal slice of the chart;
- each bar is scaled against the busiest slice, so every chart fills the graph and the shape of the
  chart is what reads;
- each bar's colour runs from the panel's cyan to a hot orange as the bar grows;
- the chart's length is shown in the corner.

**Where the numbers come from.** All of these are measured once, at scan time, while the notes are
in hand, and stored in the scan cache. They are the same measurements the level rating uses
([7.7](07-bms-to-fnex.md#77-rating-charts-from-their-notes)). Nothing is drawn for native songs or
for charts that were never measured.

**Layout was the constraint.** The band between the category tabs (y ≈ 60–75) and the top of the disc
(y ≈ 160) is all there is. Growing the panel upwards put it through the tabs, so the content was
fitted to the band and not the other way round. An early layout drew the note count over its own
label and let the last row fall off the panel.

**The panels use the card's mask and art convention, which is not symmetric.** The art is black
outside the shape and coloured inside; the mask is white outside and dark inside. Getting the two the
wrong way round let the background show through the panels while blacking out a rectangle around
them.

The text is drawn at four times its size and averaged down
([12.8](12-art-text-and-resources.md#128-panels-and-overlays)), because GDI does not antialias
Arial at 9–15 px.

## 9.9 Constant speed

**The problem.** The game's scroll speed is a percentage from 50 to 999 in steps of 25. The player
adjusts it with the effectors during play, and it carries over from song to song. A 90 BPM ballad and
a 200 BPM chart therefore scroll at the same multiplier, and so at very different visual speeds.

**The rule.** What the player actually tuned is BPM × speed. At each chart load, the mod reads the
speed the previous song was left at, multiplies it by that song's BPM, and divides by the new song's
BPM:

```text
new speed = round_to_25( old speed × previous BPM / new BPM ),  clamped to 50..999
```

- The first song of a session changes nothing; it is the reference.
- For a custom chart, the BPM used is the **dominant** BPM, the tempo the song spends most of its time
  at ([7.4](07-bms-to-fnex.md#74-notes-long-notes-and-timing-events)), because a chart's opening tempo
  is sometimes an intro.
- For a native chart, it is the chart's own BPM field.

**The prediction.** When the player enters the difficulty choice, a card shows the speed that will be
set and the arithmetic behind it, for example: `SPEED 225%`, `225% x 160 BPM = 360 BPM-equivalent,
as tuned`.

## 9.10 Custom courses

The Radio Mix modes play **channels**: four songs back to back on one gauge, listed in
`system\<family>radiomix\Stage.ini` as `"<channel>"="<stage>;<stage>;<stage>;<stage>"`, where a stage
is a song id plus its chart suffix (`bms000123-hd`).

**Four families.** Each Radio Mix family plays the lanes of one normal mode (their panel descriptors
pair up), so its courses are cut from that mode's custom songs:

| Family | Lanes of | Course ids |
|---|---|---|
| 5radiomix | 5keyMix | `bmsc5_01`… |
| radiomix | 7keyMix | `bmscr_01`… |
| 10radiomix | clubMix | `bmsc10_01`… |
| 14radiomix | spaceMix | `bmsc14_01`… |

For the modes that have charts written for their own lanes
([7.5](07-bms-to-fnex.md#75-five-keys-from-anything)), those charts alone are used when there are at
least 16 songs with them.

**The channel table holds 100 channels**, so a family only gets the room its native list leaves free
(5radiomix has room for 14 courses at most). The mod builds `CustomCourses` per family, 12 by
default:

- **Level bands.** Charts are grouped into five bands: 1–4, 5–8, 9–12, 13–16 and 17–20.
- **Four songs from one band, easiest first.** A course is then a fair climb rather than a lottery. A
  song appears once per band, through its first chart in that band.
- **Bands take turns** until the requested number of courses is reached, so the courses span the
  difficulty range.
- **Deterministic.** The shuffle inside each band is seeded from AutoChart's `Seed` and the family's
  name, so a course keeps its songs, and therefore its ranking, from one start to the next as long as
  the library does not change.

The courses are appended to the native channel list in memory. The game's `Stage.ini` is read and
decrypted, the course names are added to its `channel_normal` line, and each course gets a stage line
and a level line. The result is encrypted again and served in place of the file. The file on disk is
never written. On the wheel, the courses appear after the game's own channels as CUSTOM COURSE 1,
2, and so on. Course art is generated like the rest
([12.1](12-art-text-and-resources.md#121-what-is-generated)).

## 9.11 Category backgrounds and icons

A category folder may contain `bg.png` and `ico.png`:

- **`bg.png`** is drawn over the game's animated background at 50% opacity, stretched to 640×480.
- **`ico.png`** (ideally 480×480) is drawn opaque, 64 pixels wide, in a corner.

**Over, not instead of.** The song-select background is a 75-frame loop of 512×512 tiles. Replacing
its frames per category would cost about 59 MB of texture per category, because the pool never frees.
Two surfaces of the mod's own, refilled on each category change, cost 1.6 MB for the whole session.

**Opacity needed a blend change, not just a colour.** The game's sprite call takes an ARGB modulation
colour, but the blend pair around it is the game's default (one, zero): an opaque copy in which
source alpha has no effect. The four blend pairs the executable ever sets are opaque, additive and
the mask pair. The overlays switch to (source alpha, inverse source alpha) for their draw and put the
default back afterwards, so nothing drawn next inherits it.

## 9.12 Summary

| Addition | Where it lives | How it stays native |
|---|---|---|
| 106 custom categories | slots 19 onwards, after the 19 native ones | the 28 dropped slots were views of native songs |
| Split categories | `NAME 1/N` | a part behaves exactly like a folder |
| RANDOM row | row 0 | a real copy of a real row |
| FAVORITES / RECENT / MOST PLAYED | first three custom categories | ordinary categories whose id lists come from `playdata.txt` |
| Sort and jumps | PEDAL + E4 / E2 / E3 | the pager's own seed and side effects |
| Custom Top-5 | `CustomCache\rank` | the game's own loader logic, run for the cursor's row |
| EZ column | the dormant leftmost column | the stepper's own condition, the game's own digits |
| Notice card, detail panels | free areas of the screen | the game's own two-texture panel recipe |
| Constant speed | the speed variable at chart load | the player's tuned value, kept |
| Courses | Radio Mix channel lists | appended in memory, file untouched |
| Backgrounds | over the animated background | two surfaces, not a replaced animation |
