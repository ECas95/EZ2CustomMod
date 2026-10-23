# 6. Song select

[← Timing, judgement and scoring](05-timing-judgement-scoring.md) · [Index](../RESEARCH.md)

Song select is where a custom library has to appear, and it was built for about 430 songs. This
chapter describes how it works in the original game.

## 6.1 The director

`SongSelectDirector` is named by its own runtime-class descriptor at `0x4AAED8`.

| Method | Address | Notes |
|---|---|---|
| constructor | `0x435390` | one of the largest functions in the program: it loads every texture, clip and sound the screen uses |
| draw (`update`) | `0x443380` | 4,672 bytes, about 1,330 instructions |
| input tick (`update2`) | `0x446540` | dispatches through a thirteen-way switch (`0x446440`) to one input handler per mode |
| category seed | `0x435070` | builds the list for one category ([section 6.4](#64-the-category-seed-the-filter)) |
| difficulty stepper | `0x444AE0` | |
| difficulty readout | `0x432930` | |
| command-code matcher | `0x4462E0` | |

The object is a single allocation from the engine's pool. The `operator new` at `0x4016A0` ignores
the requested size and hands out a fixed block of about 2 MB (`0x210000`). The decompilation shows
live fields up to `+0x210988`, with a dense cluster between `+0x20C000` and `+0x20E900`.

### Object layout (selected fields)

| Offset | Field |
|---|---|
| `+0x1C` | active category |
| `+0x7C` / `+0x80` | visible count / master count |
| `+0x84` | selected row |
| `+0xC4`, `+0xCC` | wheel scroll position (row × 100.0) |
| `+0xF8` | 1 = reload the big disc art on the next frame |
| `+0x104` | big disc texture slots |
| `+0x12C` | **`visible[436]`**: the rows of the current category, 884 bytes each |
| `+0x5E2BC` | **`master[436]`**: every song of the mode, 884 bytes each |
| `+0xBC4B8` | small category textures, exactly 47 entries; `+0xBC574`, right after, is live render state |
| `+0xBC57C` | the category strip's index |
| `+0xBEAF4` | the version-label animation object (one-line label) |
| `+0xBED10` | the "sortstyle" animation object (two-line overlay) |
| `+0xBFD10`, `+0xBFD20` | the cursor-move sound and the preview sound |
| `+0xBFDB4` | screen state: 4 browsing, 5 choosing a difficulty, 7 a Radio Mix channel |
| `+0xBFF74`, `+0xBFFC0`, `+0xBFFC8` | version-label arrays A and B, and the sortstyle array |
| `+0xC000C` | HOT / NEW textures |
| `+0xC0084` | big category textures (47) |
| `+0xC040C` | level-digit textures |
| `+0x20C6BC` | > 0 while the option panel is open |
| `+0x20C6D4` | difficulty column (0 NM, 1 HD, 3 SHD, 4 EX) |
| `+0x20E894` | frames since the cursor last moved ([section 6.6](#66-the-hover-preview)) |

`visible[]` and `master[]` are adjacent arrays of equal size: `0x12C + 436 × 884` is exactly
`0x5E2BC`.

### The song entry

| Offset | Field |
|---|---|
| `+0x000` | small disc texture, drawn during the cursor-move animation |
| `+0x004` | song-name texture |
| `+0x10C` | song id, up to 15 characters |
| `+0x210` | a float the difficulty stepper tests before letting the cursor rest on the dormant leftmost column |
| `+0x214`, `+0x220`, `+0x22C`, `+0x238` | the NM / HD / SHD / EX blocks: a level float followed by two BPM floats each |
| `+0x244` | Top-5 names: five difficulty courses × five 8-byte names |
| `+0x30C` | Top-5 scores |
| `+0x340` | index of the level-digit texture |
| `+0x370` | version index, which selects the label shown under the title |

A level of zero at `+0x214` means "this song has no chart in the current mode", and the filter
skips such entries.

## 6.2 Loading the list

The constructor fills `master[]` from the mode's `song.bin` records. Its preload loop (`0x4361A0`)
does five things for every entry, all keyed by the same short song id:

| Step | Path / function |
|---|---|
| song folder | `sound\%s\` |
| song info | `loadSongInfo` (`0x431660`) |
| rankings | `readRanking` × 5, one per difficulty (`0x430CF0`) |
| small disc art | `system\discsmall\%s.bmp` |
| title | `system\songname\%s.bmp` |

**Song titles are pictures.** Every title is a pre-rendered 256×32 bitmap named after the song id,
which is why `song.bin` leaves the name field empty in twelve of thirteen tables: nothing reads it.
For a custom song the title has to be drawn as a picture, in the right font and size, before the
game can show it.

**Everything is loaded up front.** The preload runs over the whole list, and the texture cache
frees nothing while the screen lives. With 430 songs that is harmless. With a large library it is
fatal.

The **disc art** of the focused song is loaded separately, on every cursor stop, by `sprintf`ing
`system\disc\%s.bmp` with the row's id into a 260-byte stack buffer. The small disc is drawn while
the wheel animates (`sub_431AE0`, below 176 px). Once it settles, the shared full-size "HotSpotDisc"
texture takes over.

## 6.3 Categories

The wheel shows 47 categories, numbered 0–46. Decoding their tile images
(`System\SongSelect\Sortimage\category_NN`) shows what they are:

| Rows | Categories | Nature |
|---|---|---|
| 0–2 | HOT, NEW, ALL | views |
| 3–18 | 1st, S/E, 2nd … NT, TT | **the 16 game versions, the only categories with songs of their own** |
| 19–36 | LV1 … LV18+ | the library re-sorted by level |
| 37–46 | ABC, DEF … YZ+, OTH | the library re-sorted alphabetically |

Membership is held in two global tables:

- song counts at `0x03789BD4`, one dword per category;
- song-id lists at `0x03789C90`, 436 × 16-byte names per category.

Both are filled from `Stage.ini` ([chapter 4](04-file-formats.md#47-songbin-and-stageini--the-song-database)).

The category index is bounded by small immediates spread over the code. Each is a `cmp r32, imm8`
followed by a conditional jump:

| Address | Bound | Purpose |
|---|---|---|
| `0x4372EE` | 47 | the category loader's loop |
| `0x4322D9` | 47 | the wheel renderer's loop |
| `0x4446F4` | 46 | the pager's forward wrap |
| `0x433320`, `0x444799`, `0x444935` | 2..18 | the version-label update |
| `0x4435DC` (dispatch), `0x443604` | 0..1 and 19..46 | the category-change overlay |
| `0x4447F3`, `0x444818` and twins | 19..46 | the sortstyle sprite |

Two details complicate any extension of the wheel:

- **Three different sprite arrays draw category art**: the small tab strip, the label under the
  title (arrays A and B, indexed by the song's version) and the two-line overlay of the
  category-change animation. The sortstyle array's base is **biased**: it is only ever indexed with
  categories 19 and above, so its effective entries start two slots in.
- **The game's own icon loader refills the tab strip** from `category_01..47` after any external
  code has written it, so a modified strip must be re-asserted every frame.

## 6.4 The category seed (the filter)

`SongSelectDirector::m435070` (`0x435070`) builds `visible[]` for a category. EZ2DECOMP's
reconstruction, abridged:

```c
void SongSelectDirector::m435070(int cat, int keep, int unused)
{
    char buf[32];
    char localRows[436][0x374];            // 385 KB on the stack
    int  found = 0, count = g_catCounts[cat];

    if (keep) { i = 0; do { c = g_1b2eb04[i]; buf[i] = c; i++; } while (c); }   // no bound

    for (i = 0; i < count; i++)                                  // every id in the category ...
        for (j = 0; j < f80; j++)                                // ... searched linearly in master[]
            if (_stricmp(g_catNames[cat][i], master[j].name) == 0) {
                memcpy(localRows[found++], &master[j], 0x374);   // no bound on `found`
                break;
            }

    f7c = found;                                                  // the GATHERED count
    for (i = 0, j = 0; i < found; i++)
        if (*(float *)(localRows[i] + 0x214) != 0.0f)             // playable in this mode?
            memcpy(&visible[j++], localRows[i], 0x374);           // only those are written

    for (sel = 0; sel < f7c; sel++) if (_stricmp(buf, rows[sel].name) == 0) break;
    if (sel >= f7c) sel = 0;
    f84 = f8c = sel;  f1c = cat;  f_cc = f_c4 = sel * 100.0f;
}
```

The mod's own reading of the disassembly, made independently, describes the same function the same
way. Four properties matter:

1. the stack table is the 436-songs-per-category limit, and nothing bounds it;
2. the lookup is O(ids × songs) string comparisons;
3. the **gathered** count is published while only the playable rows are written;
4. when `keep` is 0, `buf` is never initialised and is still compared.

The constructor calls the seed with `keep = 1`, to restore the cursor; the category pager calls it
with 0, to reset it.

## 6.5 Input

Song select's input tick dispatches to **one handler per mode**. They are not interchangeable:

| Mode | Handler | Mode | Handler |
|---|---|---|---|
| 5KeyMix | `0x43FF00` | ClubMix | `0x440AF0` |
| RubyMix | `0x43B8D0` | SpaceMix | `0x441540` |
| StreetMix | `0x43C2E0` | ScratchMix | `0x43AD10` |
| 7StreetMix | `0x442770` | EZ2CATCH | `0x43A150` |
| 14RadioMix | `0x43F390` | 5RadioMix | `0x43DC00` |

Differences between them that a player can see:

- **The option menu.** Each mode steps the first option through its own list of allowed values;
  ScratchMix uses `≥ 11` where the others use `== 11`. A fourth option exists only in the 7-key,
  Street, Club and Space modes.
- **ClubMix has no per-player split.** It reads both players' turntables and buttons together.
- **The two Radio Mix modes** step the cursor directly without easing it and publish a channel
  level (`"Select Channel Level : %02d"`). Their state goes to 7 instead of 5, and they pick course
  songs through `pickDistinct` from named channel lists (`random`, `random2`, … `7znpfg`, `11ptgc`).
- **Preview directories differ per mode**: `System\Preview\`, `System\Preview_14radio\` and
  `System\Preview_5radio\`.

Shared rules matter to anyone adding controls:

- **E2 / E3 (slots 7 and 8) page the categories** and **E4 (slot 9) toggles the ranking display**
  (`0x1B2EB74`), and **both do nothing while a pedal is held**.
- **E1 (slot 6) opens the option panel**, which the game also refuses while a pedal is held.

That makes every **pedal + effector chord free**, which is why every control the mod adds is one.

**The dormant difficulty column.** The difficulty stepper lets the cursor rest on the leftmost
column only while the row's float at `+0x210` is non-zero. No native song sets it. The column draw
(`0x4343B0`) places the four native level digits at x = 169, 249, 329 and 409, and draws nothing
for the leftmost column. The mod uses that column for a song's fifth chart.

## 6.6 The hover preview

The audio preview runs off the frame counter at `+0x20E894`:

```c
if (cursorMoved) { f_20e894 = 0; stopPreview(); ... }
f_20e894++;
if (f_20e894 > 30 && !previewPlaying) {
    sprintf(buf, "System\\Preview\\%s.wav", rows[sel].name);
    open(preview, buf); open(preview, buf);           // opened twice
    if (failed) preview = createSample("System\\Preview\\0Blank.wav", 1, 0);
    play(preview, loop);
}
```

The preview starts 30 frames (half a second at 60 Hz) after the cursor stops, and the file is
opened twice. Because the counter keeps running while a preview plays, a preview that ends is
reloaded and looped.

**The counter is a lever.** Holding it at or below 30 delays the preview without touching anything
else; the mod does exactly that while it prepares a custom song's preview off the game thread
([chapter 8](08-performance-on-xp.md#84-two-freezes-in-song-select)).

## 6.7 Rankings

- **A rank file per song and difficulty.** Each song's Top-5 for each difficulty is a file,
  `sound\rank_<mode>_<id>[-hd|-shd|-ex].bin` (with an `e_rank_` variant for courses): five 8-byte
  names, then five 32-bit scores.
- **Only native songs are loaded.** The constructor's loader (`m430CF0`) runs over the native songs
  only.
- **Files are created on first touch.** The result and ranking screens create an empty file the
  first time they touch a song (`_access` fails, so it is opened with `"wb"`).
- **Channel rankings.** Channel rankings keep 99 places in `system\ranking` (`0x4570A0`).
