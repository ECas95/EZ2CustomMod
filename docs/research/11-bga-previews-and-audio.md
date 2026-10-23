# 11. Backgrounds, previews and audio

[← Lane cover and play options](10-lane-cover-and-play-options.md) · [Index](../RESEARCH.md) · [Next: generated art, text and graphics resources →](12-art-text-and-resources.md)

A BMS song brings its own background (BGA) and its own sounds in formats the game has never seen. This
chapter covers four things:

- how the mod picks a background for a custom song;
- how it plays that background through the game's video player, through ffmpeg or as a picture
  sequence;
- the two previews in song select, the spinning disc and the audio;
- how keysounds are decoded, and decoded ahead of time.

## 11.1 How the game plays a background

A native song's background is `Video\<id>\<id>.spv`, opened by the game's video object through
DirectShow:

```c
sprintf(this + 184, "%s\\Video\\%s\\%s.spv", base, id, id);
```

`.spv` is not a format of its own. It is ASF/WMV under another extension (magic `30 26 B2 75`), 256×256
and video-only, and 153 native songs have one. Songs without a video animate their background with a
`.scr` scene instead ([4](04-file-formats.md)).

The video object decodes into a sample grabber, and the game uploads each frame into a texture that it
draws full screen.

## 11.2 Choosing a background for a custom song

BMS declares backgrounds with `#BMPxx <file>` and triggers them on channel 04 (base layer), 07 (layer)
and 06 (the picture shown on a miss). A declared file may be a video or a picture. The mod takes the
first rule that yields something:

| # | Rule | Start time |
|---|---|---|
| 1 | The **earliest** BGA event (channels 04, 07 or 06) whose `#BMP` resolves to a video; at equal times, 04 before 07 before 06 | that event |
| 2 | A video named explicitly by the chart | 0 |
| 3 | A video found in the song's file list at scan time | 0 |
| 4 | Any video in the song folder, up to three levels deep | 0 |
| 5 | **Image BGA**: channel 04 events whose `#BMP` resolves to pictures (11.5) | the first picture's event |
| 6 | A random clip from the shared pool `Video\_spv\`, looped (11.4) | 0 |

**Converting the start time.** A BGA event's position is converted to milliseconds with the chart's
real timing, including tempo changes and stops, so a video that starts in measure 2 starts in
measure 2. An early version used 48 ticks per beat after the charts had moved to a finer grid, and the
video started twice as late as it should have.

**Resolving file names.** BMS packs habitually declare one extension and ship another (`a.bmp`
declared, `a.png` on disk). A declared file is accepted as written, then under each extension of its
kind, then by name anywhere in the song folder.

## 11.3 Playing a video

**The DirectShow path (the default).** The mod points the game's own video object at the song's file.
DirectShow decodes whatever codecs the system has (on the cabinet, WMV and MPEG-4 variants), and the
game's player clocks itself.

Three things differ from a native `.spv`:

- **The target is 640×480, not 256×256.** The frame is uploaded into a 640×480 surface the mod owns,
  attached to the game's texture. A 16:9 source fills the height and is cropped at the sides, centred.
  Anything else is stretched to 4:3. 16:9 is detected within 3%.
- **The start is delayed.** If the chart starts the video after 0 ms, the player is stopped when the
  video opens and started when the delay has passed.
- **Frames are validated.** Only uncompressed 24- or 32-bit frames whose size matches their header are
  uploaded, with the row order taken from the sign of the height. A bad frame is logged at most three
  times and skipped.

**The ffmpeg path.** When DirectShow cannot open a file (for example the MPEG-1 `.mpg` clips of PIU
packs on the cabinet), the mod runs the bundled ffmpeg and reads raw frames from a pipe:

```text
ffmpeg -i <video> -an -r 30 -s 640x480 -pix_fmt bgr24 -f rawvideo -
```

The bundled ffmpeg is a 2008 build (SVN-r11870), the newest known to run on XP. It predates
`libavfilter`, so options such as `-vf` are not available; `-s` and `-r` work in every era.

## 11.4 The video needs its own clock

The first ffmpeg version published every frame the moment it was decoded, and the game drew whatever
was latest. The background therefore ran at **decode speed**, several times real time, and finished
on screen long before the song finished in the speakers. DirectShow clocks itself, which is why WMV
and native clips looked right and PIU's `.mpg` did not.

**Frame pacing.** A reader thread now holds each frame until its presentation time:

```text
due(n) = start + n × 1000 / 30  ms
```

- The `-r 30` above makes the stream exactly 30 frames per second of content, whatever the source
  runs at, so this schedule is exact.
- A decode slower than real time finds the due time already past and publishes at once, so it catches
  up instead of drifting.
- While the reader waits, the pipe fills and ffmpeg blocks on its write, which costs nothing.

**Hiding ffmpeg's start-up.** The stream is opened when the chart loads, before the background is
due. Frame 0 then waits behind a gate that opens at the chart's BGA start time, so ffmpeg's start-up
does not eat into it.

**Looping pool clips.** The 21 clips in `Video\_spv\` are wallpaper for songs without a BGA. They are
usually shorter than the song. The game's player does not rewind: when a clip ends, it stops handing
out frames (`VFW_E_WRONG_STATE`), and the last frame stayed frozen for the rest of the song. For pool
clips only, the mod stops and restarts the player with the game's own calls, at most every 250 ms.

## 11.5 Image BGA

**Who uses it.** About 980 songs (in packs such as RHY, POPS, PIU.1 and EZM2) show pictures on
channel 04 instead of a movie. Almost all of them show a single still. A few EZM2 charts animate
several hundred frames.

**How they play.** They go through the same 640×480 frame slot and gate as the ffmpeg path, with a
picture per channel-04 event instead of a frame per 1/30 s. They never reach DirectShow, which would
play the first picture alone as a still-image source.

**Placement** follows [`src/image_bga.h`](../../src/image_bga.h):

- **16:9 pictures** fill the height and are cropped at the sides, like videos.
- **Anything else** is fitted whole, centred, with black bars. A 512×512 square stretched to 4:3 would
  be visibly squashed.

**Timing.** The timeline is the events' times in milliseconds after the first picture. The picture on
screen is the last event at or before the elapsed time, found by binary search. The reader thread
decodes the next picture while the current one is up, so an animation falls behind only when one
decode outlasts a frame's time on screen, and then it jumps to whatever is due instead of drifting.
The last picture stays up.

**Not drawn yet:** the layer channel (07) and the miss picture (06). The base layer covers every pack
measured so far.

## 11.6 The disc preview in song select

When the cursor rests on a song, its disc in song select plays a few seconds of its background.

**Where it goes.** The focused song's big disc is a single named texture slot on the song-select
object, reused per song ([6.2](06-song-select.md#62-loading-the-list)). The preview draws into a
256×256 surface the mod owns and puts that texture into the slot while the cursor rests.

**What it decodes:**

- 12 seconds of the song's own video, starting 10 seconds in (backgrounds often open on a title
  card), or from the start if the video is shorter;
- at `PreviewFps` frames per second (20 by default, 5–30; capped at 10 on XP-sized machines);
- into one buffer, all at once, by ffmpeg on a background thread.

The buffer is then looped from memory. At 256×256×3 bytes, a frame is 192 KB:

| PreviewFps | Frames | Buffer |
|---|---|---|
| 10 | 120 | 23 MB |
| 20 | 240 | 46 MB |

The buffer is allocated once and reused for every song, so its cost does not grow with the library.
The decode takes about 420 ms whatever the length; the cost is starting the process and seeking. Each
frame is masked to a circle of radius 125 on the 256 canvas, measured from the shipped disc art, once
per decode rather than per frame.

**Which songs have a preview:**

- **Native songs**: the 153 with a `.spv` use it. Songs that animate with a `.scr` scene are previewed
  from the scene's frames, which already exist on disk as numbered `.abm` runs in 179 of 281 folders,
  so the scene format need not be interpreted.
- **Custom songs**: only their **own** video. A pool clip would tell the player something false about
  the song. Only 1,839 of 20,050 custom songs ship a video. The rest have at most a title image (in a
  sample of 400 charts: 300 with `#STAGEFILE`, 275 with `#BANNER`, and a median of one image in the
  whole folder). An image-sequence preview was built for them, measured at zero songs that benefit,
  and removed.

**Timing.** The preview starts after the cursor has rested 400 ms. After 1.2 s of rest on a custom
song, the keysound prefetch also starts (11.8).

**Five lessons, each from a crash or a vanished preview:**

1. **Use a name nothing else uses.** The pool is keyed by file name. The first version built its
   texture on `initdisc.bmp`, which the game also loads, so it received a *shared* texture and then
   released the surface out from under the game.
2. **Restore only into the object you swapped.** Leaving song select frees the object. Writing the
   saved disc back into it was the menu round-trip crash.
3. **Detect pool rebuilds by the link, not the surface.** Playing a song, or loading new disc art on a
   difficulty change, rebuilds the texture pool without destroying song select. The mod's own surface
   survived, so every check said "fine", while the texture object pointing at it had been freed and
   recycled. A texture is the mod's only while it still points at the mod's surface.
4. **Hold every difficulty slot.** The renderer draws the disc of the current difficulty column, so all
   four slots must carry the preview, every frame, or a difficulty change brings the game's disc back.
5. **Never leave the decode gate shut.** If the decode thread ever exited without reopening its gate
   (an exception from a large allocation can do that), no preview appeared again for the rest of the
   session. The gate is now cleared unconditionally, a gate stuck for 30 s is reopened, and a song with
   nothing to preview is recorded as "tried" so that it is not retried every frame.

## 11.7 The audio preview

**The native preview** is a 20-second clip chosen by hand at the chorus: measured over the 462
shipped previews, 20.0 s median, 19.6 s mean, and 239 of them exactly 20 s. The game opens it 30
frames after the cursor stops ([6.6](06-song-select.md#66-the-hover-preview)).

**A custom song's preview** is cut from its own music:

1. **The source.** The largest audio file in the song folder is taken as the full mix. A `sound\`
   subfolder is also searched, where some packs keep a full-mix file beside the keysounds.
   Keysound-only songs with no full mix have no preview.
2. **The window.** The first 40 seconds are decoded, and a frame-aligned window of 20 seconds is taken
   at a random offset within them, so the preview usually starts past the intro. A source shorter
   than the window is used whole.
3. **The format.** The clip is written as `.ssf`: stereo, 44.1 kHz, 16-bit. The engine mixes at a
   fixed 44.1 kHz stereo and ignores the rate in the header. An early mono 22.05 kHz cut played at
   twice its speed, and left the mixer clocked so that native previews then played slowly.

**Off the game thread.** The cut takes 0.4–0.6 s on the cabinet. It used to run on the game thread
when the game asked for the file, freezing song select 86 times in one session
([8.4](08-performance-on-xp.md#84-two-freezes-in-song-select)). It now starts on a background thread
from the 8th frame of rest, and the game's 30-frame countdown is held until the file exists.

**Bounded on disk.** The game opens the preview with the C runtime's `fopen`, which cannot read from
memory, so the clip must be a file. At most 64 previews sit on disk (about 2 MB each). Older ones move
into the RAM store ([12.6](12-art-text-and-resources.md#126-the-ram-asset-store)), so a revisited
song is written back out instead of being decoded again.

## 11.8 Keysounds

**Decoding** tries each step in order until one succeeds:

| Step | Formats | Cost |
|---|---|---|
| Copy | WAV (PCM) | the bytes go straight into the game's audio buffer |
| In-process decoders | OGG ([stb_vorbis](https://github.com/nothings/stb)), MP3 ([minimp3](https://github.com/lieff/minimp3)) | no process, no start-up; runs on XP |
| Media Foundation | anything it supports (Vista and later only) | one start-up per file |
| ffmpeg pipe | everything else (FLAC, unusual WAV, …) | one process per file; output stereo 44.1 kHz 16-bit, capped at 512 MB |

The in-process decoders replaced one ffmpeg process per keysound. On XP there is no Media Foundation,
so before them every OGG and MP3 keysound cost a process start. Decoding stops between chunks of
about a second of audio when it is cancelled.

**Prefetch.** A song the cursor has rested on for 1.2 s is likely to be played, so a background
thread starts decoding its compressed keysounds into a cache:

- the cache holds 128 MB on XP-sized machines, 256 or 384 MB on larger ones
  ([8.1](08-performance-on-xp.md#81-the-machine));
- confirming the song then costs a copy per keysound instead of a decode;
- anything past the budget is decoded at load time;
- moving to another song cancels the running decode within a chunk and discards the cache.

## 11.9 Summary of the pipeline

```text
cursor rests ──400 ms──▶ disc preview: decode 12 s of video once, loop it in a 256×256 circle
            ──8 frames─▶ audio preview: cut 20 s from 40 s of the full mix, off the game thread
            ──1.2 s────▶ keysound prefetch: decode OGG/MP3 into the budget, cancellable
confirm ───────────────▶ chart converted in memory; keysounds from the prefetch or decoded now
                         background chosen (11.2): DirectShow │ ffmpeg, paced │ image BGA │ pool clip
BGA start time ────────▶ gate opens; the video or the first picture goes up
```
