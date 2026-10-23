# 12. Generated art, text and graphics resources

[← Backgrounds, previews and audio](11-bga-previews-and-audio.md) · [Index](../RESEARCH.md) · [Next: scan, cache, configuration and diagnostics →](13-scan-cache-config-diagnostics.md)

Song select draws pictures, never text: every title on the wheel is a 256×32 picture named after the
song ([6.2](06-song-select.md#62-loading-the-list)). For 23,587 custom songs, the mod must create those
pictures, together with discs, eyecatches and category labels:

- quickly enough that browsing never waits for them;
- in any script a title arrives in;
- without exhausting the 10,000 GDI objects and 2 GB of address space the cabinet allows.

This chapter covers the art, the text, and the rules for the graphics resources they live in.

## 12.1 What is generated

| Role | Size | Shape | Where the game shows it |
|---|---|---|---|
| `songname` | 256×32, 24-bit | title text, right-aligned | the wheel rows |
| `disc` | 256×256, 24-bit | circle | the big disc of the focused song |
| `discsmall` | 128×128, 24-bit | circle | the wheel while it moves |
| `eyecatch` | 1024×512, 24-bit | picture in the top-left 640×480 | the screen before a song |
| `resultdisc` | 128×128, 16-bit | circle | the result screen |
| `bmsrandom` | 256×32 | the word RANDOM | the RANDOM row ([9.2](09-song-select-extras.md#92-the-random-row)) |
| category tab, big label, version label, sort-style overlay | the native sizes | folder name; `details.txt` lines | the category wheel ([9.1](09-song-select-extras.md#91-categories-at-55-times-the-design-size)) |
| course labels | the native sizes | course number and level band | Radio Mix ([9.10](09-song-select-extras.md#910-custom-courses)) |

**The eyecatch is an atlas.** Its canvas is 1024×512, and the game samples only the top-left 640×480.
The picture is fitted into that region and the rest stays black. Cropping it to fill the whole
canvas, as the discs are, would put part of it outside the region the game shows.

**Native formats.** Every generated picture is written as the game's own `.abm`: a BMP whose header
fields are XOR-masked ([4](04-file-formats.md)). The game's loader therefore needs nothing new.

## 12.2 Where the pictures come from

**The source picture** is the largest image in the song folder, by pixel count. For discs it is
cropped from the centre to the target's aspect and drawn with high-quality bicubic scaling, then
masked to a circle.

**Songs with no picture.** 1,095 songs in the library ship no image at all. Early versions left the
game asking for discs that would never exist. Such songs now get their **title** drawn on the disc,
because readable beats black and both beat a missing file.

**If rendering fails** (for example when GDI is short of resources), the mod writes a flat black tile
of the right size and format, because the game cannot survive being told to open an `.abm` that is not
there. The tile is marked with a `.fallback` file. The next request for that song tries a real render
again instead of keeping the placeholder as the song's permanent art. Without the marker, one transient
out-of-memory moment looked like "textures disappear and never come back".

## 12.3 Titles

**One line or two.** A title is drawn on one line if it fits. Whether it fits is measured with GDI in
the actual font, not guessed from a character count. If it does not fit, it is split at the space
that leaves the two halves most even, so the second line is never a lone trailing word. Both lines
then get the same size: they are one title, and a smaller second line would read as a subtitle.

**Sizes:**

- the wheel's plain style uses about 13 px on 32-pixel rows and 11 px on 16-pixel tabs, close to the
  game's own ~10 px type;
- the styled art (bold, with a 1-pixel drop shadow) uses up to 21 px;
- a line that is still too wide shrinks to an **8 px floor** and is then cut with an ellipsis.

Before the floor existed, a title too wide even at 8 px drew nothing at all, which is why some long
titles came up blank instead of merely small.

**Versioned output.** The name art carries a version number (2 since titles started being drawn as
wide text, 12.4). Changing how a title renders changes the version, so that old pictures are not
reused.

## 12.4 Text in any script

**Decoding.** A BMS file declares no encoding. Newer packs use UTF-8, classic Japanese ones Shift-JIS
(code page 932), and Korean ones EUC-KR (949). The old fallback for "not UTF-8" was the system ANSI
code page, which is 1252 on both the development PC and the cabinet. Every Shift-JIS title therefore
came out as Latin mojibake: `空想都市トーキョー` drew as `‹ó'z"s"Žsƒg[ƒLƒ‡[`.

The two legacy code pages accept most of each other's byte pairs, so the choice is made by what the
pairs spell ([`src/text_codec.h`](../../src/text_codec.h)):

1. Strict UTF-8 is tried first. If it decodes, that is the answer.
2. Each byte pair is scored for both candidates:

   | Candidate | Pattern | Score |
   |---|---|---|
   | Shift-JIS | hiragana or katakana (lead byte `0x82`/`0x83`) | +3 |
   | Shift-JIS | kanji (lead `0x88–0x9F` or `0xE0–0xEA`) | +1 |
   | EUC-KR | one of the 2,350 common Hangul syllables (lead `0xB0–0xC8`, trail `0xA1–0xFE`) | +3 |
   | EUC-KR | Hanja (lead `0xCA–0xFD`, trail `0xA1–0xFE`) | +1 |

3. The higher score wins, provided it decodes strictly.
4. If neither scores, the ANSI code page is used, which is right for accented Latin titles.

**Glyphs.** Arial has no kana, kanji or Hangul, and the cabinet's XP has no East Asian font, so
decoded titles still drew as boxes there. The mod ships **WenQuanYi Micro Hei** (Apache 2.0), a single
TrueType collection covering Latin, CJK, kana and Hangul, and loads it privately to the process.
Nothing is installed in Windows.

- **It must carry Latin too.** XP does no font fallback for a private font, and titles mix scripts
  (`stem / HALLPBE.S VS チョビ`). A CJK-only font drew the Latin half as boxes.
- **When it is used.** It is used for any line containing a character above U+024F, other than
  general punctuation. That covers CJK, kana, Hangul and full-width forms, and also symbols (stars,
  notes, arrows), because the cabinet's Arial 2.x has far fewer of them than a modern Windows.
- **Same pictures everywhere.** The rule is the same on every machine, so the PC and the cabinet render
  identical pictures.

## 12.5 Rendering off the game thread

**The art thread.** Wheel art is rendered by a background thread at below-normal priority. Song select
hands it the ids around the cursor, nearest first. A new request reorders the queue instead of
starting a second thread. For each song, the thread renders the name, the small disc and the big disc
if they do not exist yet, so each role is checked before any work.

**GDI+ is started once per process.** It used to be started and shut down for every song. A
168-song category preload spun it up and down 168 times, which cost 719 ms in one log and left
rendering failing for a run of 91 songs once the churn caught up with it.

**Publishing.** A rendered picture is written to a temporary file and renamed into place. The game
thread reads it into memory and deletes it (12.6). Those two file operations, and only those, are
serialised: the rename used to fail with `ERROR_ACCESS_DENIED` when the game thread was reading the
previous copy ([8.4](08-performance-on-xp.md#84-two-freezes-in-song-select)). A fallback tile never
overwrites a good copy that has already arrived.

## 12.6 The RAM asset store

Generated art and cut previews live in memory, not in `CustomCache`:

- **Keys** are `id|role` for art and the file path for previews.
- **Budget.** A least-recently-used store with a byte budget: 96 MB on XP-sized machines, 160 or 256
  MB on larger ones ([8.1](08-performance-on-xp.md#81-the-machine)). It evicts the oldest entries when
  it is full, and an evicted asset is simply rendered again.
- **Files are a pipeline step only.** The renderer is a GDI file pipeline, so a picture still passes
  through a file. That file is read into the store and deleted at once, so `CustomCache` holds almost
  nothing between sessions.
- **The exception is the audio preview**, which the game opens with `fopen` and which must therefore
  exist on disk while it plays ([11.7](11-bga-previews-and-audio.md#117-the-audio-preview)).

`[CustomSongs] AssetsInMemory=0` keeps everything as files across sessions instead. The production
`BMSMod.ini` does not list it, so the default (in memory) applies.

## 12.7 The texture ring

**The problem.** The game's texture loader keeps every texture it loads, keyed by file name, until song
select is torn down. Each one is a DirectDraw surface with a GDI object behind it. Native art is a
fixed set. Custom art was one new name per song the wheel or the big disc ever showed, so browsing
without playing walked the GDI count up about 40 per category change (2,182 → 2,753 in one minute of a
log) towards the 10,000 ceiling. Past that ceiling, no texture loads at all, GDI+ included.

**The ring.** Custom art now lives in a fixed set of slots per role:

| Role | Slots | On screen at once |
|---|---|---|
| songname | 64 | 16 rows |
| discsmall | 40 | 7 discs |
| disc | 8 | 1 |
| eyecatch | 4 | 1 |
| resultdisc | 4 | 1 |

- **First use of a slot** creates its texture the normal way, from the song's own file, under the
  slot's own cache key.
- **Every later use** finds that texture by its key and refills its surface with the new song's pixels
  (through `Lock`/`Unlock`, with no GDI involved).
- **Slot choice.** A request takes the slot that already shows that song, else an empty slot, else the
  least recently used one ([`src/texture_ring.h`](../../src/texture_ring.h)).
- **The wheel checks before trusting.** It asks whether a slot still holds a given song before trusting
  a row's texture pointer, because a recycled slot may have moved on.

**The pool-rebuild bug.** Playing a song tears down the texture pool and rebuilds it without
recreating song select, and the pool recycles its texture objects, often at the very addresses they
had before. A slot still holding a pre-song pointer then "owned" whatever texture now lived there. The
ring found that stale slot first, denied the row its valid new texture, the wheel reloaded it, and the
next frame denied it again: the names around the cursor never filled and the wheel churned.

A recycled texture has a new surface. So a slot whose texture no longer points at the surface it took
belongs to the old pool, and it is emptied. Every use also goes through the key lookup rather than a
remembered pointer, so after a rebuild the lookup misses, creates the texture afresh, and the ring
adopts the new pointer.

**Result.** Browsing the whole library costs a constant number of textures. Sessions sit around
2,100–2,200 GDI objects ([8.5](08-performance-on-xp.md#85-graphics-resources)).

## 12.8 Panels and overlays

The notice card, the detail panels, the category backgrounds, the lane cover, the disc preview and the
picker thumbnail are all textures the mod owns inside the game's pool. They follow one set of rules,
each learned from a failure.

**Uploading pixels without GDI.** Pixels go into a surface through `Lock`/`Unlock`, converted to the
surface's own pixel format from its channel masks. Going through GDI cost about 7 ms per upload under a
DirectDraw wrapper.

**Ownership rules:**

- **Every texture has a unique pool name.** The pool returns the existing texture for a name it already
  holds. Two overlays that shared a name received the same texture object, and the icon's pixels would
  have been drawn as the background.
- **Never release a surface you did not create.** After a pool rebuild, a remembered pointer is recycled
  memory. Releasing it crashed with a null virtual-table read.
- **A texture is yours only while it points at your surface** (11.6, 12.7). This one check catches
  every pool rebuild.

**Supersampling.** GDI does not antialias Arial at 9–15 px: the font's own `gasp` table turns smoothing
off at those sizes. Text and rounded corners came out as hard 1-bit edges next to the game's
pre-smoothed art. The panels are drawn at four times their size and averaged back down with a box
filter ([`src/supersample.h`](../../src/supersample.h)), which gives smooth type at no cost a player can
notice.

## 12.9 English menus

The game's menus are Korean bitmaps. The mod can show English versions without touching them:

- **The override.** A file at `Translation\system\<path>` stands in for `system\<path>`. Paths are
  normalised (lower case, backslashes, the install folder and `.\` removed) to one key, so both
  relative and absolute requests match ([`src/translation_map.h`](../../src/translation_map.h)).
- **The originals stay untouched.** Removing the `Translation` folder, or setting
  `EnglishTextures=0`, brings the Korean back.
- **Building the textures.** `tools/texture_inventory.py` produces contact sheets of every UI texture
  that could carry text, for review. `tools/texture_translate.py` re-letters the chosen ones in
  English, from `translation/spec.py` and its CSV tables.

## 12.10 What the resources taught

- **The texture pool is a bump allocator keyed by name.** Anything per song must come from a fixed
  budget, and every name must be unique.
- **Rebuilds are silent.** The pool is rebuilt behind the mod's back after every song. Every cached
  pointer needs a cheap proof that it is still what it was.
- **A placeholder must say it is one.** Otherwise a transient failure becomes permanent.
- **Measure text in the font you draw it with.** Character counts mean nothing across scripts.
