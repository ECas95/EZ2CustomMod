# 2. The binary and its protection

[← Method](01-method.md) · [Index](../RESEARCH.md) · [Next: engine architecture →](03-engine-architecture.md)

## 2.1 Two executables

An EZ2AC FNEX installation carries two executables that matter here:

- **`EZ2AC.exe`** (864 KB) is the retail executable, protected by a licence-bound packer.
- **`EZ2B.exe`** is an **unpacked image** of the same game: the code and data the protector would
  have produced in memory, written out as an ordinary executable. On the cabinet the game is
  launched from it through 2EZConfig ([section 2.6](#26-how-the-game-is-launched)).

Every address in this study refers to the unpacked image. Its code is identical to what the
protector produces at run time. The image used by EZ2DECOMP differs from an earlier community dump
by twelve bytes, all inside the packer stub, so the addresses transfer one-to-one between them.

## 2.2 The retail protector

The retail `EZ2AC.exe` is not a plain PE file. The protector, as established by EZ2DECOMP:

1. **A stage-1 dropper at the PE entry point**, in a tail section at `0x3800000`. It resolves a
   handful of `kernel32` functions by walking the module's export table, finds an embedded `MZ`
   image, writes a 15,872-byte stage-2 loader to the temporary directory and runs it.
2. **A stage-2 loader**, itself ASPack-packed, whose imports are entirely about licensing: a URL
   download function, process-token and special-folder functions, and `setupapi.dll` / `hid.dll`
   for probing hardware. This stage performs the online / hardware licence check.
3. **The payload cipher is AES-128 in ECB mode.** A protector section at `0x33e3000` carries a full
   AES implementation with both the forward and inverse S-boxes. The game's `.text` section is
   encrypted as independent 16-byte blocks. `.data` ships in the clear, and `.rdata` differs from
   the in-memory image only in its import table, which the loader rebuilds.
4. **The key is not in the file.** No AES key of any length is stored in the executable. Together
   with the stage-2 imports, this means the key is derived at run time from the machine's licence.
   The code cannot be decrypted offline.

### Validating an unpacked image without the key

Because the key is unavailable, an unpacked image cannot be checked by re-encrypting it. It can be
checked without the key, because AES-ECB is a fixed permutation of 16-byte blocks: a given
plaintext block always becomes the same ciphertext block. Pairing every 16-byte block of the
unpacked `.text` with the block at the same offset in the retail file must therefore give a
consistent one-to-one codebook.

For the image used here, the codebook is a perfect bijection: 31,983 distinct blocks on each side,
with zero forward and zero reverse conflicts. `.data` is byte-identical, and `.rdata` differs only
inside the import directory. That is strong evidence that the image is a faithful decryption of
this exact build, obtained without ever knowing the key.

This study goes no further into the protector. It describes the protector because it explains why
all analysis works on the unpacked image, and it gives no procedure for producing one.

## 2.3 The unpacked image

| Section | Address range | Size | Contents |
|---|---|---|---|
| headers | `0x00400000` | 4 KB | PE headers |
| `.text` | `0x00401000`–`0x00486FFF` | 536 KB | game code (encrypted in the retail file) |
| `.rdata` | `0x00487000`–`0x004A4FFF` | 120 KB | constants, strings, vtables, import tables |
| `.data` | `0x004A5000`–`0x037E2713` | **53.7 MB** | initialised data plus a very large BSS |
| `.l33t` | `0x037E3000` | 116 KB | residue of the packer (RWX) |

Two properties shape everything else:

- **No ASLR and no relocations.** The image base is fixed at `0x400000` and `DllCharacteristics` is
  zero: no dynamic base, and no NX-compatibility flag either. Every function, global and immediate
  can be patched by absolute address, and every patch is deterministic from one run to the next.
  This one property makes the whole mod possible.
- **A 53.7 MB static data section.** Most of the game's state lives in fixed global storage rather
  than on the heap: the input object, the song table, the category tables (at `0x03789BD4`), the
  per-track chart queues and hundreds of globals in the `0x1B2E000` region. In the cabinet's memory
  map the image accounts for 52 MB on its own. For a modder this is convenient, because state has a
  fixed address. It also explains why so many of the game's limits are compile-time sizes: they are
  the sizes of static arrays.

The image imports from nine DLLs: `WINMM`, `DSOUND`, `DINPUT`, `DDRAW`, `KERNEL32`, `USER32`,
`GDI32`, `ADVAPI32` and `OLE32`. The C runtime is linked statically, so no CRT DLL appears.

## 2.4 The compiler, and what it implies

EZ2DECOMP identified the compiler from the Rich header (build 3077, linker 7.10) and confirmed it
by recompiling: **Visual C++ 7.1** with the static multithreaded CRT, `/O2 /G7 /arch:SSE2`, C++
exception handling (165 functions carry an EH prologue) and **`/GS`**.

Several observations in later chapters follow from the compiler:

- **`/GS` protects only some functions.** VC 7.1 places a stack cookie only in functions that hold
  character buffers. 275 functions call the cookie check at `0x47882F`, which compares against the
  global cookie at `0x4AFCA0` and aborts with exit code 3 on a mismatch. A function whose large
  local array is an array of structures, such as the song-select category filter
  ([chapter 6](06-song-select.md#64-the-category-seed-the-filter)), carries no cookie, and an overrun there is caught by nothing.
  A function that does carry one fails loudly instead: an uncatchable abort with no stack left to
  inspect, which is why the mod's `GsProbe` exists.
- **Custom calling conventions.** `cl` 13.10 gives `static` functions that are not address-taken
  their own register conventions, including floating-point arguments passed in SSE registers.
  Hooks have to be written against the convention actually used, read from the disassembly, not
  the one a C declaration would suggest.
- **Identical-code folding.** The original linker folded identical functions together. One address
  can therefore serve several unrelated callers (the empty `Log` function at `0x402880` is one), and
  patching it changes all of them.
- **The C runtime is inside the image.** `rand` is a linear congruential generator at `0x47928F`,
  seeded with `srand(time(0))` where the game needs randomness (the RANDOM options, course picks).

## 2.5 Size of the program

The Ghidra export finds **2,014 functions**. EZ2DECOMP's reachability walker, starting from the
application's entry points and following direct calls plus every slot of every vtable that
reachable code stores, places about **438 KB of the 477 KB of game code on the reachable "spine"**:
around 92% of it, so very little of the program is dead. The game is a C++ program with its own
small engine: an application class, screen "directors", and engine services prefixed `K`
([chapter 3](03-engine-architecture.md)).

## 2.6 How the game is launched

On the cabinet, as on most home installations of the EZ2 series, the game is started by
**2EZConfig** (GPL-3.0, by kasaski, a rewrite of ben-rnd's original). It consists of a launcher,
`2EZConfig.exe`, and a companion DLL, `2EZ.dll`:

- The launcher creates the game process **suspended**, injects `2EZ.dll` with the classic
  `VirtualAllocEx` + `WriteProcessMemory` + `CreateRemoteThread(LoadLibraryA)` sequence, and
  injects any further DLLs listed under **Extra DLLs** the same way. This is how EZ2CustomMod
  enters the process: it needs no loader of its own.
- `2EZ.dll` **emulates the cabinet's I/O board**. The game reads its buttons with `IN` and `OUT`
  instructions on ports `0x100`–`0x106`. In user mode those instructions raise a
  privileged-instruction exception. `2EZ.dll` installs a vectored exception handler that catches
  it, decodes the instruction, answers from a port cache filled from the configured controllers,
  and resumes the game after the instruction. The port map is `0x101` system buttons, `0x102`
  player-1 keys, `0x103` player-1 turntable, `0x104` player-2 turntable and `0x106` player-2 keys.
- It also carries a JSON patch library (by relative address or by pattern scan), 60 Hz refresh
  patches, DirectDraw fixes and an optional low-latency audio path.

Inside the game, all 24 port instructions sit behind one flag at `0x4200F4`. With the flag clear
the game takes a no-board path, which makes the board optional by the game's own design.

One quirk documented by 2EZConfig is worth repeating for anyone installing the game. **The game
folder's path must not contain a `.`**: the game finds its `.ini` by scanning its own path and
stopping at the first dot.
