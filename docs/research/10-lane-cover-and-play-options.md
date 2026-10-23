# 10. Lane cover and play options

[← Song select: what the mod adds](09-song-select-extras.md) · [Index](../RESEARCH.md) · [Next: backgrounds, previews and audio →](11-bga-previews-and-audio.md)

The mod adds four things to gameplay:

- **the lane cover**, a curtain over the top of the lanes;
- **its picker**, which chooses the cover's picture in song select;
- **a judge offset** for setups with input or display latency;
- **a frame limiter** for displays the game cannot synchronise with.

This chapter describes how each one fits into the game, and the two bugs found in the cover on the
cabinet.

## 10.1 What the cover is

Other rhythm games call it **SUDDEN+**: a cover over the top part of the lanes that the player lowers
until notes appear only a comfortable distance above the judgement line. Hiding the top of the lanes
cuts how long each note is visible, which makes dense or fast charts easier to read for many players.

**Its size** is measured the way those games measure it: in thousandths of the distance from the top
of the lanes to the judgement line.

| Value | Meaning |
|---|---|
| 0 | no cover |
| 500 | the cover reaches halfway down to the judgement line |
| 1000 | the whole field is covered |

The value is `[Gameplay] LaneCoverSize` in `BMSMod.ini`. It is saved whenever the player lets go of
the button that adjusts it.

## 10.2 Where it is drawn

**Which screens.** The keys panel, which draws every key mode except the scratch modes and Ez2Catch,
has an overlay step each frame. The mod draws the cover in that step, so it lands after the notes
and under the panel's own overlay (the judgement line's decoration, the frame). The scratch modes and
Ez2Catch use other panels, which is why they have no cover.

**Which part of the screen.** The cover must span the lanes and stop at the judgement line. The panel
holds two candidate rectangles:

| Source | Where it comes from | Problem |
|---|---|---|
| **The judgement bar** | the bar's x, y and width, as the panel draws it | not the lanes' width everywhere: in 7-key the bar's texture is 300 px wide from x = 20, over lanes that span about 210 |
| **The lanes' bounding box** | grown by the panel's layout loader over every lane item of the mode's descriptor | none |

The mod uses the lanes' bounding box and falls back to the bar only when the box looks wrong (width
under 8 px or off screen). The judgement line's y is the bar's y less the game's global lift, the
same value the panel subtracts when it draws the bar.

## 10.3 The picture

**A tall texture that slides.** The cover is a fixed 256×512 texture stretched over the lanes. Only
the covered part is drawn, from the top of the lanes down to the cover's edge. Through the texture's
UV window, it shows the **lower** slice of the picture. As the cover is lowered, the picture slides
down with it, so its bottom edge is always the one in view. Nothing is drawn above the lanes.

**Black is not black.** The game treats pure black as transparent (its textures use a colour key, not
alpha; [3](03-engine-architecture.md)). A black cover therefore drew only its edge and let the lanes
show through. Every pixel darker than 8 in all three channels is lifted to 8. That is the darkest
value that survives a 16-bit surface, where a channel under 8 rounds back to 0 in 5 bits. A
picture's black areas get the same treatment.

**The default cover** is black with a light band along its lower edge (6 of 512 rows). Without the
band, a black cover over black lanes would show only as notes appearing late.

**Drawn opaque, with white vertex colour.** The panel draws with alpha testing enabled, so a vertex
alpha of 0, which the BGA path gets away with, would discard every pixel. The cover is drawn with
opaque white, as the panel's own overlay is.

**Pictures** come from `LaneCover\` next to the game: `.png`, `.jpg`/`.jpeg` or `.bmp`, decoded by
GDI+ and scaled to 256×512 once per picture. Tall, dark pictures work best.

## 10.4 Input: one button, two jobs

The cover is adjusted by **holding E1 and turning the turntable**. E1 already has a job in play: a tap
toggles bass boost. Both must keep working.

**Sharing E1.** The game's effector routine is the only code that acts on E1 in play, so the mod
changes only what that routine sees:

- **While E1 is down**, the routine is shown the button as merely "held", never "pressed", so it does
  not toggle anything yet.
- **When E1 is released and the turntable did not move**, the press it missed is replayed: the routine
  sees "pressed" for one frame and toggles bass boost, one frame late.
- **When E1 is released and the turntable moved**, nothing is replayed. The hold was used for the
  cover, and bass boost stays as it was.

Every other caller and every other button sees the real state. The logic is a small state machine in
[`src/lane_cover.h`](../../src/lane_cover.h) with its own test.

**Reading the turntable: the first bug.** The engine keeps a running total of turntable movement, and
the first version read it. The cover shook in place instead of following the turntable. The cause was
in the engine:

- it zeroes the running total whenever the total passes a small limit (5, 10 or 20 counts) that each
  screen sets;
- a steady turn therefore read back as +3, +3, −3, +3, −3…

The fix reads the board's **raw 8-bit position** instead, and computes the movement since the last
frame exactly as the engine wraps it:

```c
int d = (raw - prevRaw) & 0xff;
return d > 0x80 ? d - 0x100 : d;     // −127 … +128 counts
```

Each count moves the cover by `LaneCoverStep` units (8 by default; a negative value reverses the
direction). The result is clamped to 0…1000.

Two details complete the input:

- **Stale positions.** If no frame has read the turntable for more than 500 ms (between songs, for
  example), the previous position is stale. It is re-seeded instead of treating everything turned in
  song select as one jump of the cover.
- **Keyboard play.** With the launcher's I/O emulation on a keyboard, the two dial keys move the cover
  by 10 counts per press.

## 10.5 Making it load every time: the second bug

The cover "sometimes did not load": it was blank or showed garbage for a whole song. The upload of
its pixels into video memory could fail, or the surface could be lost later (a mode switch, a device
reset, a wrapper recreating surfaces), and the result was never checked.

Now, on every frame the cover is drawn:

1. The texture must still point at the cover's own surface; if the pool was rebuilt, it is created
   again.
2. The surface is asked whether it was lost; a lost surface is restored and refilled.
3. The upload's result is checked. A failed upload is retried on the next frame, and the cover is not
   drawn until it succeeds, because drawing it would show whatever the surface held. The failure is
   logged (`[lanecover] texture upload FAILED`) at most five times.

The picture's pixels are kept, so a retry uploads them again without decoding the file again.

## 10.6 The picker

**Opening it.** In song select, holding **PEDAL + E4** for 800 ms opens the **LANE COVER** card (a tap
still cycles the sort order; [9.4](09-song-select-extras.md#94-sorting-and-category-jumps)).

**Using it.**

- **E2 / E3** step through `BLACK (default)` and every picture in `LaneCover\`, sorted by name
  regardless of case.
- A 48×96 thumbnail of the picture under consideration sits below the card.
- The card shows "N / M" and "(in use)" for the current picture.
- **E1** keeps the choice. It is written to `BMSMod.ini` as `LaneCoverImage`, and the cover texture is
  redrawn. The confirmation reminds the player how to size the cover, or that its size is still 0.
- **E4** leaves without changing anything.

**Owning the buttons.** While the picker is open, the game must see none of the cabinet's buttons,
or the wheel, the pager and START would act on the same presses. So:

- every input the picker uses (START, E1–E4, the dial and the pedals) reads as "up" for the game;
- the turntable's running total is zeroed every frame, because the wheel reads it directly rather than
  through the button state;
- after the picker closes, the buttons stay hidden until **all** of them are released. Otherwise the
  E1 that confirmed the choice would reach the game as a fresh press and open the option panel;
- if song select stops running the picker (its timer ran out and a song started), the picker lets go
  of the buttons after 250 ms;
- the detail panels hide while the picker is open.

## 10.7 Judge offset

**The press time.** The judge compares a note's tick with a press time that the game computes as
`max(clock − 5, 1)`. The 5 is the developers' fixed latency compensation, in chart ticks
([5](05-timing-judgement-scoring.md)).

**The offset.** `[Gameplay] JudgeOffsetMs` shifts that time. A positive value means "my presses
arrive late, count them earlier":

```text
press time = max(clock − 5 − offsetTicks, 1)
```

**Milliseconds to ticks, per chart.** The offset is set in milliseconds but applied in ticks, so it is
converted at every chart load from that chart's BPM and resolution:

```text
tick length (ms) = 60000 / (ticks per beat × BPM)
offsetTicks      = round(JudgeOffsetMs / tick length)
```

A native chart runs at 192 ticks per beat and a converted chart at 384
([7.3](07-bms-to-fnex.md#73-the-tick-grid)), so the same offset is twice as many ticks on a converted
chart. The log records the conversion (`[judge] offset 15 ms = 3 ticks at 150.0 BPM, 192 ticks/beat`).
The offset is installed only when it is non-zero, and only if no other patch already owns the press
time.

**Finding the right value.** With `[Debug] TraceJudge=1`, every grading call is folded into one summary
per song:

- presses: count, mean signed error in ticks and milliseconds, early and late counts;
- the grade bands (KOOL, COOL, GOOD, FAIL, stray), computed from the chart's own windows;
- hold tails: count, mean error, and how many were released too early;
- the offset in use, and a **suggested `JudgeOffsetMs`**, the offset that would centre the presses:

```text
suggest = round((mean press error in ticks + current offset in ticks) × tick length)
```

A running line every 200 presses shows the trend during a long song.

## 10.8 Frame limiter

`[Gameplay] FpsLimit=N` holds every frame to at least 1/N s. It is off by default, because on a 60 Hz
display the game's own page flip waits for the display and paces the game. The limiter is for a
display the game cannot synchronise with.

It sleeps until the last two milliseconds of the interval and then spins, so the interval is exact
without keeping a core busy. The game has already set a 1 ms timer resolution at start-up. If the
game falls far behind, the schedule restarts from the current frame instead of trying to catch up.

## 10.9 What the cabinet taught

- **Read the raw input.** The engine's derived values (the running total) are shaped for its own
  screens and limits, not for a new consumer.
- **Check every upload.** On XP with an old driver, a failed or lost surface is a normal event, not a
  crash. Treating it as one and retrying is what made the cover reliable.
- **Owning input means owning the release too.** The picker was correct while open and wrong for one
  frame after closing, until it held the buttons until they were let go.
