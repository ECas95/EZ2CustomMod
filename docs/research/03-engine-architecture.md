# 3. Engine architecture

[← The binary](02-binary-and-protection.md) · [Index](../RESEARCH.md) · [Next: file formats →](04-file-formats.md)

EZ2AC is a C++ program written against a small in-house engine. Class names come from the game's
own runtime-class descriptors (an MFC-style `KRuntimeClass` with a name, a base and a factory) and
from its log strings, so most names below are the original developers' own.

## 3.1 The layers

```text
EZ2ACApp  (application; ctor 0x414EA0, derived from KEngineApp, vtable 0x487418)
 └─ screen driver  Obj401780::run  @0x401930         one loop that runs whichever screen is current
     └─ Directors  (one per screen)
          TitleDirector · ModeSelectDirector · SongSelectDirector · EZ2DJMainGameDirector
          GFMainGameDirector (scratch modes) · CatchMainGameDirector (Ez2Catch)
          ResultDirector · GFResultDirector · RankingDirector · TotalResultDirector
          EndingDirector · SystemMenu / TestInput (operator test menu)
 └─ engine services
      KGraphics   DirectDraw7 + Direct3D7 renderer, texture loader, text
      EzSubsystem / KSoundDS   DirectSound sample manager, WinMM mixer
      Obj40C180   input: DirectInput (polled) + cabinet I/O ports
      KParser / KProfile   tokenizer, profile files, two bump arenas
      KEZPlayer   the chart player: the song clock and keysound playback
      EzChart     ("EZModule") chart loading, decryption, track merge
      Obj40B2E0   DirectShow video player
```

The **screen driver** runs the current screen's director each frame through its vtable: an update
step (`update2`, input and state), then a draw step (`update`). A director's constructor loads
everything the screen needs (textures, `.str` clips, sounds) and its destructor releases it.
Moving between screens therefore tears down and rebuilds most graphical state, which matters to
anyone who keeps pointers to the game's textures.

The game supports thirteen play **modes**, chosen on the mode-select screen. A name table at
`0x487278` (32-byte entries) is scanned by `0x414E50`, and the matching index is stored at
`0x1B2E548`. Every per-mode switch in the binary reads that index. The roster has exactly 13
entries, ending with CV2Mix; AndromedaMix, present in older games, is not in it. The current mode's
name is kept at `0x1B2E680`. Charts are found **by name**, with the format string
`"%s%dp-%s.ez"` (mode, player count, song id), so the mode is part of every chart's file name.

Three directors play notes: `EZ2DJMainGameDirector` for the key modes, `GFMainGameDirector` for
the scratch modes and `CatchMainGameDirector` for Ez2Catch. They share a skeleton and differ in
constants, which turns out to matter for timing and scoring ([chapter 5](05-timing-judgement-scoring.md)).

Players live on **three global singly-linked lists** (one per player-object kind), each with the
same trio of helpers (count, find, iterate). Two-player play is a property of those lists, not of
the directors.

## 3.2 Graphics: KGraphics

`KGraphics` is a fixed-function **Direct3D 7** renderer on top of **DirectDraw 7**. EZ2DECOMP
settled the API version from vtable indices: the singleton holds `IDirectDraw7` at `+0x28` and
`IDirect3DDevice7` at `+0x30`, and six independent call sites land on the right DDraw7/D3D7 slot
names (`EnumSurfaces`, `RestoreAllSurfaces`, `BeginScene`/`EndScene`, `SetRenderState`,
`SetTexture`). The front and back surfaces sit at `+0x34` and `+0x38`.

What the engine draws with:

- **Pre-transformed quads.** The 2D path draws 4-vertex triangle strips with the vertex format
  `XYZRHW | DIFFUSE | SPECULAR | TEX1` (FVF `0x1C4`, stride 32 bytes). Screen-space coordinates
  ignore the camera, which matters only for the 3D background meshes. The workhorse is
  `drawTex` at `0x403F80`.
- **Blending through two render states.** `setBlend` at `0x40A490` sets `SRCBLEND` and `DESTBLEND`
  (render states 19 and 20) to any pair of D3D blend factors. The common pairs are (ONE, ZERO) for
  opaque sprites, (ONE, ONE) for additive glows and (DESTCOLOR, INVSRCALPHA) for masks.
- **Sixteen texture-combiner recipes.** `KGraphics::m408130` (190 `SetTextureStageState` calls) is
  a table of sixteen multitexture setups. Modes 5 to 15 program a second texture stage with its own
  UV set, using `MODULATE2X`, `ADDSIGNED`, `BLENDTEXTUREALPHA` and the `COMPLEMENT` /
  `ALPHAREPLICATE` argument modifiers. These are the sixteen "mtpreset" values that the authoring
  tool Ez2Visual exposes. D3D stage state persists between draws and some recipes rely on what the
  previous one left set, so a recipe is a sequence of state changes, not a self-contained state.
- **Two different fill primitives.** `drawRect` (`0x40A4C0`) draws a 3D quad from edge
  coordinates. `m40A6A0` is a DirectDraw `COLORFILL` blit straight into the back buffer, taking
  an origin and a size, where size 0 means the whole screen.
- **A global vertical flip** (flag at `0xD36CB0`) that both fill primitives honour, inconsistently:
  one mirrors about y = 479 and the other about y = 480.
- **Presentation** (`m405D80`) either flips the chain or blits the back buffer into a destination
  rectangle at `+0x18`. That rectangle is the seam where the 640×480 picture is scaled to a modern
  display.

**Textures** are loaded by `KGraphics::loadTexture` at `0x408D00`. Three behaviours shape every
texture the game uses:

- The game asks for `.bmp` names, and **not a single `.bmp` ships with it**. When a flag at
  `KGraphics+0xD4` is set, the loader quietly substitutes the `.abm` of the same name
  ([chapter 4](04-file-formats.md)).
- **Transparency is a colour key, not alpha.** 16- and 24-bit art has no alpha channel. The loader
  sets a source colour key of black, so pure black pixels are transparent. 43,819 of the game's
  62,201 textures rely on it.
- **Texture objects are cached by file name and never freed while a screen is alive.** The cache is
  a binary tree keyed by name (search at `sub_414460`). Its nodes are allocated from a bump arena,
  and each texture also holds a DirectDraw surface, which costs a GDI object. The only release is
  the wholesale teardown when the screen changes. That pass (`KGraphics::m408C80`) releases every
  pooled surface until its reference count reaches zero, foreign ones included.

**Text** comes from two bitmap fonts that ship with the game:

- `fontEn.dat` holds 128 ASCII glyphs of 8×16 with per-glyph advances.
- `fontkr.dat` holds the **2,350 precomposed Hangul syllables of KS X 1001**, 16×16 at a fixed
  15-pixel advance. The glyph index is `(lead − 0xB0) × 94 + (trail − 0xA1)`, which the compiler
  folded into a single `lea` with displacement −0x4141, and bit columns are stored least
  significant bit first.
- `drawText` at `0x406F50` locks the back buffer and writes the glyphs directly.
- Song titles are **not text at all**: each song's name is a pre-rendered 256×32 picture,
  `system\songname\<id>.bmp` ([chapter 6](06-song-select.md)).

**Video** is played by `Obj40B2E0`, a DirectShow wrapper embedded in the backgrounds and the
directors. Song backgrounds are `.spv` files, which are ASF/WMV ([chapter 4](04-file-formats.md)).
DirectShow (`quartz.dll`) opens them with its own `CreateFileW`, a path that a hook on the
executable's import table never sees.

## 3.3 Sound: EzSubsystem and KSoundDS

- **One DirectSound secondary buffer per voice.** The engine creates a buffer per voice, with
  `DSBUFFERDESC` flags for position, global focus, pan and volume control, and lets DirectSound
  mix. It does not mix into a ring of its own.
- **Two voices per sample, used in turn**, so a sample retriggered while still playing overlaps
  its previous instance instead of cutting it off.
- **Every sample enters through `EzSubsystem::createSample`** (`0x40E3C0`) after
  `openSample` (`0x40E2A0`) has resolved its name. The engine's loaders only ever open `.ssf` (and
  the older `.ezw`): a requested `.wav` name is swapped to `.ssf` on disk. The backend is a
  sub-object at `+0x54`.
- **Master volume** goes through the WinMM mixer API (`KSNDMixer`), not through the buffers.
- **A dedicated pump thread** (`Obj414750::run` at `0x414AB0`) wakes about every millisecond
  (`Sleep(1)`). It reads `timeGetTime()` and advances the chart player. The song clock is
  therefore independent of the frame rate ([chapter 5](05-timing-judgement-scoring.md)).

## 3.4 Input: Obj40C180

The input object is huge (about 4 MB, most of it still unnamed) and lives at `*(0x1B2E710)`. It
merges DirectInput devices, polled once per frame, with the cabinet's I/O ports:

- **Buttons** are an array of 20-byte slots from `+0x10`. A slot's state is 2 on the frame the
  button goes down, 3 while it is held, and below 2 when it is up. The game asks for a slot's state
  through `Obj40C180::getState` at `0x40C1C0`, a 14-byte leaf function. Known slots: 4 and 5 are
  START, 6–9 are EFX1–EFX4, `0x0F` and `0x10` are the dial keys, `0x11` and `0x19` are the pedals,
  and 10–25 map onto the keys and turntables as the mode descriptors number them
  ([chapter 4](04-file-formats.md#46-gds--mode-descriptors)).
- **The turntable** is read in `FUN_0040C4B0`, once per poll: the raw 8-bit position of port `0x103`
  is kept at `+0x420220`, the per-poll delta (wrapped to ±128) at `+0x420230`, and a running total
  at `+0x420238`. **The running total is reset to zero whenever its magnitude exceeds a limit** at
  `+0x420268`, which each screen sets to 5, 10 or 20. Code that wants "how far did the turntable
  move" must therefore use the raw position, not the total.
- **Edge detection is per frame** (`Obj40C180::tick` at `0x40CC90`). A press is only noticed on the
  frame after it happens, a fact that reappears in the judgement timing.

## 3.5 Configuration: KParser and KProfile

- **KParser** is the engine's tokenizer (`parseToken` at `0x413940`). Every configuration format,
  including the brace-structured `.gds` mode descriptors and `.pvi` panel files, is read through
  it. `slurpFile` (`0x4135F0`) reads a whole file and, for encrypted files, decrypts it first.
- **KProfile** is the key/value configuration store (`Load` at `0x413FE0`, which logs
  `"KProfile::Load - STR/FLOAT/INT %s:%s"`). It writes itself back out as an in-order tree dump.
- **Two KProfile arenas are bump allocators**, reset at every screen teardown and sized by
  `push imm32` instructions in the executable's static constructors. One, at `0xD49C98`, holds
  texture objects of `0xF0` bytes each. The other, at `0x530C2C`, holds `.str` animation clips
  (`0x2B210` bytes plus the file size each; the largest native background needs 18 MB). The
  cabinet's executable carries two modified immediates for these arenas: 16 MB for textures, and
  64 MB for clips after an earlier 512 MB proved wasteful, because the arena's initialisation
  `memset`s it whole.
- **The engine's own `operator new`** at `0x4016A0` (`__stdcall`) hands out fixed-size pool blocks
  regardless of the requested size. The song-select object is one of those blocks, which caps its
  arrays until the mod replaces the allocation.

## 3.6 Settings and globals

Operator settings (coin options, difficulty, sound levels) and per-session options live in a run of
globals that a reset function (`0x430B50`) clears at the start of a game. Globals that recur in
this study:

| Address | Meaning |
|---|---|
| `0x1B2E548` | current mode index (0..12) |
| `0x1B2E680` | current mode name |
| `0x1B2E710` | the input object |
| `0x1B2E718`, `0x1B2EC18` | text-slot index and the 32 × 128-byte text slots (song names published for loading) |
| `0x1B2E890` | scroll speed, percent (50–999 in steps of 25) |
| `0x1B2EB04` | name of the song last selected, kept across category changes |
| `0x1B2EB6C` | alternate scoring/judgement model flag |
| `0x1B2EBC8` | mix style (7 selects a second variant of the hold and commit logic) |
| `0x1B2E89C` | course-mode "hard gauge" option (negative gauge rates doubled) |
| `0x1B5F2E0` | autoplay (every note judged KOOL at its own time) |
| `0x37DCECC` | the note-order option (0–10) |
| `0x4ABD14` | the difficulty-column cursor (0 = the leftmost, normally dormant column) |
| `0x03789BD4`, `0x03789C90` | category song counts and category song-id tables |
