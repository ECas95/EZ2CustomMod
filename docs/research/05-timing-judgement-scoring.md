# 5. Timing, judgement and scoring

[← File formats](04-file-formats.md) · [Index](../RESEARCH.md) · [Next: song select →](06-song-select.md)

This is the part of the game where reproducing behaviour exactly matters most: if a player could
notice a difference in a replay of the same chart with the same inputs, it is a difference in
gameplay. Almost everything here comes from EZ2DECOMP's matched reconstructions and was confirmed
against a live trace of the running game.

## 5.1 The song clock

- **The judge's clock is a tick counter** in the chart player, `KEZPlayer.f7258` (`+0x7258`). It
  is set to 1 when play starts and advanced by one for every player step, in the callback at
  `0x40FD70`.
- **Steps come from the sound pump thread** (`Obj414750::run`, `0x414AB0`), which calls
  `KSongPlayerBase::advance` (`0x4146A0`) with `timeGetTime()` about every millisecond. The clock is
  therefore fresh to about 1 ms and does not freeze between frames.
- **A step lasts `60000 / ((rate/4) × BPM)` ms.** `setSpeed` (`0x40F5C0`) recomputes it. Its only
  caller in the whole binary is the dispatcher arm for BPM-change records, so tempo changes take
  effect exactly when the pump walks past them.
- **The unit is 1/192 of a beat**, not a millisecond. A `.ez` file is written at 48 ticks per beat,
  but the chart loader multiplies the resolution by 4 and rescales every record to it
  (`m_nTPB = 192` in its log). One tick therefore lasts:

  ```text
  1 tick = 312.5 / BPM ms        (2.60 ms at 120 BPM, 1.56 ms at 200 BPM)
  ```

  The mod's own charts are written at 384 ticks per measure
  ([chapter 7](07-bms-to-fnex.md#73-the-tick-grid)), and
  the engine honours that: it derives ticks per beat from the chart's declared resolution.

## 5.2 From a button press to a judgement

1. **Detection is per frame.** `Obj40C180::tick` (`0x40CC90`) compares the input state once a
   frame. The main-game update then calls the per-lane handler (`0x430970`) with an event code:
   2 for a press, 1 for a release, 3 for the hold-sustain pump.
2. **The press time is synthesised when the frame processes the press**, not when the button went
   down. The handler asks the chart player's vtable slot 22 (`0x40F450`):

   ```c
   return max(clock - 5, 1);
   ```

   The `− 5` is a **fixed latency compensation** of five ticks (about −10 ms at 150 BPM), roughly
   the mean delay that frame sampling adds. All sampling jitter, up to one frame (16.7 ms at 60 Hz),
   therefore lands in the judged time as a late bias.
3. **Judging runs only while the chart is playing**, gated by vtable slot 27 (`0x40F490`).
4. **The candidate note.** `Judge::judgePress` (`0x426700`) finds the candidate note on the lane;
   when two candidates qualify, the nearer one wins.
5. **The grading core**, `Judge::grade` (`0x426500`), computes `dt = pressTime − noteTime`, notes
   whether it was early or late, and compares `|dt|` against the lane's window table, tightest
   first:

   | Grade | Name | Meaning |
   |---|---|---|
   | 5 | KOOL | inside the tightest window |
   | 4 | COOL | also counts FAST (`0x1B2EAA8`) or SLOW (`0x1B5F1A0`) |
   | 3 | GOOD | |
   | 2 | FAIL | inside the miss window but outside GOOD (the "mash" band) |
   | 1 | MISS | a note nobody pressed (never produced by a press) |
   | 0 | stray | outside every window: the note is not consumed |

   The grade number is used directly as an index into the per-grade counters and gauge rates.
   FAIL does not break the combo; only MISS does.

### Windows

- **Each lane keeps two six-entry window tables**, one for early presses and one for late. Both
  default to {−1, −1, 24, 12, 4, 2}, from widest to tightest; the two −1 slots can never match, so
  grades 1 and 0 never come out of the table walk. Asymmetric windows are therefore possible, but
  shipped charts set both tables the same.
- **The windows come from the chart's `.ini`** and are **widened by 3 ticks** before use, or by 1
  in the alternate model (flag `0x1B2EB6C`), at the call to `Judge::setWindows` (`0x426EA0`). The
  default 6 / 24 / 36 / 72 therefore judges as 9 / 27 / 39 / 75.
- **The windows are beat-relative.** The same file is three times tighter in milliseconds on a
  200 BPM chart than on a 66 BPM one:

| BPM | 1 tick | KOOL (±9) | COOL (±27) | GOOD (±39) | MISS (±75) |
|---|---|---|---|---|---|
| 120 | 2.60 ms | ±23 ms | ±70 ms | ±102 ms | ±195 ms |
| 150 | 2.08 ms | ±19 ms | ±56 ms | ±81 ms | ±156 ms |
| 180 | 1.74 ms | ±16 ms | ±47 ms | ±68 ms | ±130 ms |
| 200 | 1.56 ms | ±14 ms | ±42 ms | ±61 ms | ±117 ms |

Earlier community documentation read these values as milliseconds. They are ticks.

### Consequences

- **The press time has one choke point.** Because a press time is a value returned by one vtable
  slot, replacing that slot can shift or de-quantise every judgement. The mod's `JudgeOffsetMs`
  works this way: it replaces slot 22 and converts the offset to ticks at every chart load. A launcher that timestamps button edges could replace the
  synthesised time with the true edge time.
- **Keysounds bypass the judge.** A press always sounds the lane's nearest keysound, hit or miss
  (`playLaneKeysound`, vtable slot 32, `0x40FFA0`). The keysound is picked as whichever neighbour on
  that lane is nearer to the current time, using the record's sample id, volume and pan. It takes
  no lock, while the pump thread plays the background notes through the same path under a lock.
- **Audio latency affects feel but not judgement.** The judge reads the tick clock, not the sound.
  On the cabinet, with DirectSound mixed in hardware, the path from press to sound is short. On
  Vista and later DirectSound is emulated over shared WASAPI, which adds 30–60 ms. That is why the
  2EZConfig fork offers an exclusive-mode audio shim.

## 5.3 Hold notes

- **A hold is judged twice and counted once.**
  - The **release** goes through `Judge::judgeRelease` (`0x4268C0`), which grades the hold's tail
    at `tick + length − 1` with the same core and the same windows. A tail outside every window is
    forced to FAIL.
  - Both ends go through the same commit (`0x42FD90`), told apart by a flag. A per-lane hold cursor
    (`+0x3E4 + lane × 8`, −1 when idle) remembers an open hold.
  - **Neither end emits the ordinary judgement.** The commit's "do effects" flag is cleared on
    both paths.
  - The commit averages head and tail grades, `(g + tail + 1) / 2`, and then **discards the
    average**: the flag that would publish it is cleared on the same branch. The disassembly and
    778 live trace events (none with the release flag) agree that the average is dead code.
- **A hold pays out in instalments while it is held.** A sustain tick (`0x42F4E0`) and a pump
  (`0x42F880`) pay one instalment at a time into the judgement sink, each instalment counting as a
  note.
- **The step depends on the note's "kind"**: byte `+0x32` of the note record, latched into the
  player slot.

  | Kind | Step |
  |---|---|
  | 1 | half a beat |
  | 2 | an eighth |
  | 3 | a sixteenth |
  | 0, 6, ≥ 13 | a quarter |
  | 4, 5 | one instalment for the whole note |
  | 7–12 | nothing is paid |

- **The number of instalments** is `length / step − 1`, clamped up to 1. The `− 1` is skipped in
  mix style 7 and for kind 6.
- **Verified on one chart.** A traced chart with 31 holds of 48, 96, 144 and 192 ticks produced
  exactly 309 pump calls, which is `Σ(length/12 − 1)`. Its 551 judged notes are 242 heads plus
  those 309 instalments.
- **A failing hold is restored.** When a lane's grade drops below GOOD during a hold, the pump does
  not pay it; it sets the lane back to GOOD. This is the recovery path after an early release.
- **Kind 6** pays only its last instalment, and only while still KOOL. Its last two instalments are
  graded against a window of 1/64 of a measure.
- **The note counter and the hold starter disagree.** The function that counts notes to set the
  maximum score (`0x42F2D0`) divides kind-4 holds by 32. The starter that pays them gives kind 4 a
  single instalment. A kind-4 hold therefore counts `width/32 − 1` notes towards the maximum and can
  only ever pay one, so a perfect score is unreachable on such a chart. The shipped data happens to
  contain no kind-4 or kind-5 notes: the only non-zero kinds are 6, 20 and 3, on 155 notes in 19
  charts.

## 5.4 The gauge

The groove gauge is a float that starts at 100.0 and is clamped to [0, 100] (ceiling at
`0x48DBFC`).

- **Each judgement adds `director + 0x308 + grade × 4`**: the per-chart rate, read positionally
  from the `.ini` ([chapter 4](04-file-formats.md#45-per-chart-ini--judgement-and-gauge)).
- **The gauge freezes at zero.** The sink tests `gauge > 0` before adding anything, so nothing
  lifts a gauge that has emptied.
- **The rates are adjusted twice after reading.** All three director families agree on this, which
  makes it a rule of the game rather than a quirk of one director:
  - Modes 5 and 9 add 0.05 to COOL and 0.02 to GOOD.
  - With the course "hard gauge" option (flag `0x1B2E89C`, offered only in the four course modes),
    the two **negative** rates are **doubled**, from copies saved before the mode bonus.
  - **Ez2Catch zeroes the GOOD rate outright.**
- **Two players.** Each player's gauge is clamped separately. Game over requires both gauges to be
  down, except that a single player in a two-player cabinet ends immediately.
- **The difficulty scale is neutral.** A per-difficulty multiplier table exists (`0x49E3C8`), but
  in this game it is all 1.0.

## 5.5 Score and grade

The key modes and Ez2Catch use **different scoring models**:

- **Key modes** (`PlayerSlot::commit`, `0x42E620`):
  - In the **normal model**, every note is worth a flat amount by tier: GOOD 41, COOL 150,
    KOOL 300, and nothing for FAIL or MISS. The maximum is `notes × 300` (`0x430940`), so a full
    combo of KOOLs is exactly 100.00% on any chart.
  - In the **alternate model** (`0x1B2EB6C` set), the tier values are 40 / 100 / 170, multiplied
    by a 1,500-entry combo table (`0x4A9160`, rising from 1.0 to 2.0586). The table equals
    `1 + log10(c)/3` to within 6 × 10⁻⁷.
- **Ez2Catch** (`ScoreKeeper::rank`, `0x472B40`) scores by combo, not by tier, with a logarithmic
  term:

  ```c
  total += 250 - (int)((log10(i + 1.0f) * 0.2f) * -250.0f);
  ```

  The cast truncates rather than rounds, and `0.2f` is a float literal promoted to double. Both
  details change the result: from the third note on, rounding would diverge.
- **Grade letters** come from two shared ladders: a hit-rate ladder (95 / 90 / 80 / 70 / 60 / 50%)
  and a score ladder (100 / 98 / 95 / 93 / 90 / 85 / 80 / 70 / 60 / 50%).
- **The counters.** The counters per grade sit at `+0x1C4 + grade × 4`, worst first: MISS, FAIL,
  GOOD, COOL, KOOL. Next to them are the combo (`+0x1DC`), max combo (`+0x1E0`), score (`+0x1E4`)
  and gauge (`+0x1F4`).
- **Batch commits.** A batch commit (used by `applyTracks`) advances the combo by its count while
  the tally and score move by one. A zero `flag` argument gates the combo and nothing else.
- **Write-only state.** The commit maintains a 128-entry "chord" table and a counter at
  `0x37DCE98` that nothing in the program ever reads.
- **Autoplay.** The `0x1B5F2E0` option marks every lane so that the judge grades each note KOOL at
  its own time, with no input.

## 5.6 Note-order options

The note-order menu value (`0x37DCECC`, 0–10) is not what gameplay sees. It indexes an icon table,
each icon selects an effect id, and a dispatcher (`0x422F20`) applies the transform when the stage
starts:

| Option | Effect |
|---|---|
| RANDOM | sixteen lane swaps over the whole chart |
| MRANDOM | the same, including scratch and pedal |
| FR | RANDOM within one half (ClubMix, 10RadioMix) or the inner lanes (7-key modes) |
| SRANDOM / HRANDOM | re-shuffled every beat (HRANDOM includes scratch and pedal) |
| PS | re-shuffled every eight measures |
| MIRROR / MIRROR_A / KEY | reverse the row, all of it, or the keys only |
| SP | swap scratch and pedal |

- **Per mode.** Each mode offers its own subset, through its own cycle in the input handler: two
  options in CV2Mix, all ten only in the 7-key modes.
- **Swaps are wholesale.** A note moves even when its counterpart lane is empty at that tick.
- **Hold vetoes differ per mode.** The time-sliced shuffles refuse to swap a lane while a hold
  covers the slice start, and the three modes test that differently:
  - The key modes keep a lane locked 8 ticks past the end of its hold.
  - The scratch modes have no such slack.
  - Ez2Catch drops the lower bound and uses `> 6` where the others use `≥ 6`.
- **Randomness** comes from the CRT's `rand`, seeded with `srand(time(0))`.
