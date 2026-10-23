# 1. Method: how the game was studied

[← Index](../RESEARCH.md) · [Next: the binary →](02-binary-and-protection.md)

The work behind EZ2CustomMod combined five independent lines of evidence. None of them was
trusted alone: every important claim in this study was confirmed by at least two of them, and
several early claims that rested on one line only turned out to be wrong.

## 1.1 Sources of evidence

| Line of evidence | What it is | What it is good for |
|---|---|---|
| **Static decompilation** | Four decompilers run over the unpacked game image: a Ghidra headless export (2,014 functions, all decompiled), IDA Pro / Hex-Rays, Snowman and Rec Studio 4 | Finding functions, reading control flow, naming fields. Disagreements between decompilers point at the instructions worth reading by hand |
| **Byte-matching decompilation** ([EZ2DECOMP](#13-the-ez2decomp-project)) | Reconstructed C++ that recompiles, with the original compiler, into byte-identical functions | Ground truth for structure layouts, calling conventions and exact semantics: a function that matches 100% *is* the original logic |
| **The mod as a probe** | The DLL itself, injected into the running game, hooking functions and logging their arguments and memory | Facts the static view cannot give: which code path really runs, what a field holds at run time, what the game writes every frame |
| **Bulk sweeps over the shipped data** | Parsers run over every file the game ships, about 290,000 of them | Format rules. A decoder that handles three sample files proves nothing; the tail of the distribution is where formats actually live |
| **Measurement on the real cabinet** | Logs from the arcade machine (Pentium Dual-Core E5800, 2 GB, Windows XP) | Performance, memory and timing claims, which are meaningless when measured on a development PC |

## 1.2 Static analysis

The decompilations are taken from the **unpacked image** of the game executable (see
[chapter 2](02-binary-and-protection.md) for why the retail file cannot be analysed directly).
Because the image has no ASLR and no relocations, every address in every decompiler output is
the address the code runs at, which made it possible to move freely between a decompiler listing,
a hook in the mod and a value in the running process.

Using four decompilers was not redundancy for its own sake. They disagree on exactly the code that
is hard to read: compiler-specific calling conventions, `/GS` prologues, switch tables and
floating-point code that passes arguments in SSE registers. Where they disagreed, the disassembly
was read directly.

The decompilers also analysed the community tools that predate this work (an `.abm` converter,
`.ez` and `.ezi` chart converters and a song-database editor), which is where the first
understanding of the file encryption came from.

## 1.3 The EZ2DECOMP project

EZ2DECOMP is a separate, private project: a **byte-matching decompilation** of EZ2AC in the style
of the LEGO Island (`isle`) decompilation. It reconstructs source that, compiled with the original
compiler, produces functions byte-identical to the original's, verified per function with
`reccmp`. Its findings were shared with this project and are a major source for chapters 3 to 7.

What makes it authoritative is the matching discipline:

- The original toolchain was identified from the executable's Rich header (build 3077, linker
  7.10) and confirmed by running the same compiler: **Microsoft Visual C++ 7.1** (Visual Studio
  .NET 2003, `cl.exe` 13.10.3077), static CRT (`/MT`), `/GS`, `/O2 /G7 /arch:SSE2`, under Wine.
- A function counts as done only when `reccmp` reports a 100% match against the original. At the
  time of writing: **every referenced function is reconstructed** (1,466 entities, 0 unannotated
  functions, 97 pinned vtables with every slot covered), and **1,351 of 1,563 compared entities
  are byte-exact** (95.99% accuracy). The rest are documented compiler-codegen residues: register
  permutations and stack-slot choices that no source spelling reproduces.
- Fifteen patches had to be made to `reccmp` itself, for example for floating-point constant
  pools without relocation tables, and to stop large struct offsets being misread as addresses.
  The image was also based at `0x11000000` so that no immediate in the original can collide with
  an address of the recompiled code.

A byte match is a much stronger statement than a plausible decompilation. "Matched at 100%" means
the reconstructed source *is* the original logic, down to the order of evaluation. Where this
study quotes a matched function, the behaviour described is the game's, not an interpretation of
it.

EZ2DECOMP also keeps a list of compiler-forensics rules (its "cookbook") that explain how
particular source constructs compile under `cl` 13.10: when a register is cached, how `/GS`
pads buffers, when a switch becomes a jump table. Those rules are what let a reader infer source
structure from machine code, and they recur throughout the reverse engineering of this binary.

## 1.4 The mod as a probe

Before the mod wrote a single custom song into the game, its early versions (v3 and v4, June 2026)
did nothing but observe:

- **Trampoline hooks** on the song loader, the chart loader, the keysound loader, the result and
  ranking screens and song select, each logging its registers and arguments, then running the
  original instructions and returning to the game.
- **Structure dumps**: song select's object was dumped field by field, before and after the
  game filled it, which is how the 884-byte song entry and the level floats at `+0x214` were found.
- **Marker builds**: when it was unclear which of several arrays fed a sprite on screen, a build
  wrote a distinct letter into each candidate, and the screen showed which one it was. Three earlier
  guesses about the category label had been reasoned about rather than looked at, and all three
  were wrong.
- **A millisecond clock on every log line**, and a per-frame breakdown of the time spent in each
  of the mod's subsystems (`[frametime]`, `[slowframe]`). Most performance work started from one of
  these lines in a cabinet log.

Three diagnostic tools went further:

- **`ez2tracer.dll`**, a single-step execution tracer. It sets the trap flag and records every
  instruction the game executes from an "arm" address, as 4-byte relative addresses, while a
  hardware breakpoint lets library code run at full speed. Single-stepping is thousands of times
  slower than native, so the trace is armed just before the code of interest and capped.
- **`GsProbe`**, a detour on the `/GS` failure path. The game's cookie check
  (`cmp ecx,[0x4AFCA0]` at `0x47882F`) is reached by a `call` from each of 275 protected functions,
  so on entry the return address points inside the function whose frame was smashed. By the time
  the CRT's abort handler runs, that frame is gone. The probe logs it before it disappears.
- **`HangWatchdog`**, which suspends every thread every two seconds and logs where a frozen thread
  sits.

## 1.5 Mechanical verification

The most important methodological lesson of the project is that **reading fails**. In the scoring
subsystem alone, four readings that looked correct were wrong: two gauge drains were crossed, the
wrong director's gauge was read, a combo latch was misplaced, and hold notes were paid at the head.
Three of those four were caught only by a second reading of the same disassembly, which is luck
with a delay. The fourth was caught the moment a mechanical check pointed at it.

The verification techniques that replaced reading:

- **The differential oracle.** The game's judgement sink (`PlayerSlot::commit` at `0x42e620`) was
  hooked inside the running game. Every call was recorded with its inputs, the call site and the
  state before and after, and the recording was replayed through an independent reimplementation
  of the scoring rules. The first live capture, one stage with 552 events, replayed with **zero
  divergences**; a two-player capture of 226 interleaved events did too. A trace verifies exactly
  what reading cannot: which sequences of events really occur.
- **Library-wide sweeps.** Each format parser was run over everything the game ships and required
  to land exactly on the end of every file. 12,359 of 12,361 charts, 178,470 of 178,474 samples and
  all 10,593 animation clips parse. The few failures are corrupt files in the shipped game.
- **Cross-checks by redundancy.** Many claims were confirmed by a second, independent quantity.
  A chart's 64 tracks each start with `EZTR`, and walking them must land on the file's end. In a
  `.ssf` header, two fields are derivable from the other four. A trace of one chart shows 309
  hold-pump calls, which is exactly `Σ(length/12 − 1)` over its 31 holds.
- **Self-checks in the mod's own tests.** Pure logic (the category filter's index, the lane-cover
  arithmetic, the catalogue order, the songname window) lives in headers with standalone tests that
  run without the game.

## 1.6 Measuring on the cabinet

The target machine is a 2010-era arcade PC: a Pentium Dual-Core E5800 at 3.2 GHz, 2 GB of RAM, a
GeForce GT 520 and Windows XP Professional 32-bit. Work that takes milliseconds on a development
PC can take a second there.

All performance claims in this study come from cabinet logs. Two of them, both found on
2026-10-22 and both invisible on a desktop, are a 0.7–1.7 s freeze on every category change and a
0.4–0.6 s freeze on every song the cursor rests on
([chapter 8](08-performance-on-xp.md)).

## 1.7 Ground rules for handling the original

The study follows the same rules as EZ2DECOMP:

- **No bytes of the original are reproduced.** Addresses, structure layouts and descriptions of
  instruction sequences are analysis. Instruction bytes, data tables copied out of the binary, key
  tables and assets are not, and none appears here. The mod's patch-verification signatures are
  short prologue byte patterns compared at run time; they are code, not reproduced content.
- **Key material is not published.** Where the study describes a cipher, it describes its
  structure, never its key tables.
- **The licence protection is described, not defeated.** [Chapter 2](02-binary-and-protection.md)
  explains how the retail executable is protected, because that explains why the analysis targets
  an unpacked image. It contains no procedure for obtaining such an image.
