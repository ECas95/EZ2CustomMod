# Next steps

A backlog of ideas for EZ2CustomMod: 750 items in five categories, each split into three tiers of 50.
This is a menu to choose from, not a commitment. An item's place here says nothing about its priority.

| Category | What it covers |
|---|---|
| [1. New features](#1-new-features) | Things the mod does not do yet |
| [2. Optimizations](#2-optimizations) | Start-up time, frame time, memory and graphics resources |
| [3. Fixes](#3-fixes) | Confirmed issues, edge cases to verify and guard, and defects that are not ours to fix |
| [4. Enhancements](#4-enhancements) | Improvements to features that already exist |
| [5. Adjustments](#5-adjustments) | Defaults, tuning constants, configuration, documentation and process |

**Tiers**

- **Feasible**: fits the current architecture; days of work; low risk.
- **Medium**: needs research, measurements on the cabinet, or deeper integration with the game; weeks of
  work or real risk.
- **Out of reach**: blocked by the hardware, the engine, missing data or legal limits; blocked by the
  project's own rule of keeping the game's native behaviour; or costing far more than it returns. Each
  item records why, so the question does not have to be reopened unless something changes.

Numbers quoted below come from the [research](RESEARCH.md) and the cabinet's logs.

---

## 1. New features

### 1.1 Feasible

1. **#RANDOM / #IF blocks.** Evaluate random branches with a seed derived from the chart's path, so a chart always plays the same way and its rating matches what is played.
2. **HIDDEN+ cover.** A cover over the lower part of the lanes, drawn and adjusted like SUDDEN+.
3. **Green number.** Show how long a note is visible, in milliseconds, while the cover or the speed is being adjusted.
4. **Lane cover per mode.** Remember the cover's size and picture separately for each mode.
5. **Per-song judge offset.** An offset stored with each song and added to `JudgeOffsetMs`, for packs with badly aligned audio.
6. **UNPLAYED category.** A virtual category of the custom songs never played.
7. **NOT CLEARED category.** Songs played but never cleared.
8. **Level categories for custom songs.** Virtual categories per computed level across the whole library, like the game's own level views.
9. **RANDOM within a level range.** A RANDOM row that respects the current level filter.
10. **Favorites import and export.** Save and load favorites as a plain-text list, to move them between machines.
11. **Play log.** Append every result to a CSV file: date, song, mode, chart, score, grade counts and maximum combo.
12. **Personal-best export.** Write the best score of each chart to a CSV file.
13. **Session summary.** At exit, write the songs played, notes hit and play time to the log and to a text file.
14. **Sort by artist.** Order a category by artist, then by title.
15. **Sort by last played.**
16. **Sort by play count.**
17. **Sort by date added.** Use the time each song folder first appeared in the scan cache.
18. **NEW badge.** Mark songs added since a recent scan, for a configurable number of days.
19. **Scan report.** After each scan, write the new, removed and rejected songs (with the reason for each rejection) to a text file.
20. **`category.ini`.** An optional file in a category folder that sets its display name.
21. **Custom category order.** A list file that fixes the wheel order instead of the alphabetical one.
22. **Hidden categories.** An ini list of categories to leave out without moving their folders.
23. **`song.ini` overrides.** An optional per-song file that forces the title, artist, level, offset or preview start.
24. **#PREVIEW.** Use the chart's declared preview file when it exists.
25. **Preview start point.** Start previews at the chart's busiest section, or at a fixed percentage of the song.
26. **Duration in the panel.** Show the chart's length in song select.
27. **BPM range in the panel.** Show the minimum, maximum and dominant BPM.
28. **Note and hold counts in the panel.**
29. **Declared and computed level.** Show the chart's `#PLAYLEVEL` next to the level computed from its notes.
30. **Pack name in the panel.** Show the folder a song comes from.
31. **BGM-only playback.** An option that mutes keysounds, for listening practice.
32. **Keysound-only playback.** The opposite: mute the background tracks to hear only what the player triggers.
33. **Remappable cover button.** Choose which button, held with the turntable, adjusts the lane cover.
34. **Independent cover for player 2.**
35. **Cover opacity.** A setting for how transparent the cover picture is.
36. **Result screenshot.** A button that saves the result screen as a PNG in a `Screenshots` folder.
37. **Rescan request.** A button combination at the title screen that forces a full rescan on the next start.
38. **Rejected-songs notice.** A short on-screen message when the scan rejected songs, pointing at the report.
39. **Frame-time overlay.** A toggle that shows the frame rate and the worst recent frame on screen.
40. **Courses from text.** Define the Radio Mix custom courses in a plain-text file instead of generating them.
41. **Level filter.** A button combination in song select that sets a minimum and maximum level.
42. **Library check tool.** A Python tool that validates a `BMSData` tree (encodings, missing sounds, empty charts) before it is copied to the cabinet.
43. **Duplicate finder.** A tool that finds the same chart in several packs by hashing its notes.
44. **Missing-keysound report.** List the charts whose `#WAV` files are absent.
45. **Delta deploy tool.** Copy only the songs and caches that changed to the cabinet's disk.
46. **Configuration check.** At start-up, log unknown keys and out-of-range values in `BMSMod.ini`.
47. **Version banner.** Show the mod's version and the number of songs on the title screen.
48. **Chart details on demand.** A button in song select that writes the selected chart's path, encoding, layout and rating inputs to the log.
49. **Favorite from the result screen.** Mark a song as a favorite without going back to song select.
50. **Quick retry.** Restart the same chart from the result screen with one button.

### 1.2 Medium

1. **Practice mode.** Start at a chosen measure and loop a section.
2. **Replays.** Record the inputs of a play and play them back through the same judge.
3. **Pacemaker.** A target-score bar during play, against the personal best or a grade.
4. **FAST/SLOW.** Show whether each press that was not a KOOL came early or late.
5. **Timing histogram.** An early/late distribution on the result screen.
6. **Offset calibration from plays.** Suggest a `JudgeOffsetMs` from the mean timing error of recent plays.
7. **Latency test screen.** A metronome with taps, to measure the cabinet's audio and input delay.
8. **LIFT.** Raise the judgement line itself. Unlike a cover, this changes where notes are drawn.
9. **bmson.** Read the JSON-based chart format.
10. **PMS layout.** A dedicated mapping of 9-button charts onto the closest mode, instead of folding them.
11. **#RANK to judgement windows.** Map a chart's `#RANK` onto one of the game's own window sets.
12. **#TOTAL to the gauge.** Map `#TOTAL` onto gauge rates.
13. **#DIFFICULTY slots.** Use the declared difficulty to decide which slot a chart takes when levels collide.
14. **Clear lamps.** Show each song's best clear (failed, cleared, full combo, perfect) on the wheel.
15. **Medals per chart.** Track full combos and all-KOOL plays.
16. **Player profiles.** Separate scores and settings for each named player, chosen at start.
17. **Offline difficulty tables.** Import a downloaded difficulty-table file as virtual categories.
18. **Nested categories.** Two-level browsing for packs with sub-folders.
19. **Live rescan.** Rescan in the background and swap in the new catalogue at the next song select.
20. **Second library drive.** Load additional songs from another drive or a USB stick.
21. **Playlists.** User-defined song lists that appear as virtual categories.
22. **Song queue.** A list of songs played in order, for events.
23. **Event mode.** A fixed set of songs, a timer and no menus, for tournaments.
24. **Dan courses.** Grade courses defined in text, with the gauge carried between songs.
25. **Gauge carry-over in custom courses.**
26. **AutoChart variants.** Easy, normal and hard versions of each generated chart.
27. **Pattern-aware AutoChart.** Learn short note patterns from native charts, not only distributions.
28. **AutoChart doubles from singles.** Generate charts that use both sides musically.
29. **Ez2Catch from BMS.** Generate catch charts that follow the melody's pitch.
30. **Result graphs.** Gauge and score over time on the result screen.
31. **Density bar during play.**
32. **Scrolling titles.** Titles wider than the 256×32 picture scroll while the cursor rests on them.
33. **Mod options menu.** A song-select menu for the most used `BMSMod.ini` settings.
34. **Score transfer between cabinets.** Export and import custom-song scores.
35. **Statistics report.** An offline HTML report built from the play log.
36. **Animated covers.** Image sequences as lane covers.
37. **BGA layer channel.** Draw the BMS layer images (channel 07) over image BGAs.
38. **Poor BGA.** Show the miss image (channel 06) for image BGAs.
39. **Slow practice playback.** Play a chart at 75% speed in practice mode, with pitch lowered.
40. **Different charts in 2P.** Each player picks a different chart of the same song.
41. **Theme folder.** Replace song-select backgrounds and frames from a folder.
42. **More menu languages.** Extend the translation map to Spanish and Korean.
43. **Custom songs in attract mode.**
44. **Song-select music.** A playlist plays while the player browses.
45. **Song notes.** Show `#COMMENT` or a readme from the song's folder.
46. **Search.** An on-screen keyboard, driven by the turntable, that searches titles.
47. **Similar songs.** A virtual category of songs close to the current one in BPM and level.
48. **Daily selection.** A selection of songs seeded by the date.
49. **Lane cover per song.** Remember the cover size per song for players who adjust it per chart.
50. **Stop indicator.** A visual cue during long `#STOP`s.

### 1.3 Out of reach

1. **Online rankings.** The cabinet has no service to talk to, and the project is private.
2. **Online multiplayer.** No server, and the engine has no network play.
3. **HD or widescreen.** Every screen is laid out for 640×480; changing that means redrawing every screen and asset.
4. **Gameplay above 60 Hz.** Animated scenes are timed in 60 Hz frames.
5. **Mines.** The note record has no mine kind and the judge has no path for one.
6. **#SCROLL and #SPEED.** The engine draws notes at positions proportional to ticks; per-section scroll needs a new note renderer.
7. **Negative BPM and backward scroll.** Same reason.
8. **Running the retail executable directly.** Its code is encrypted under a licence-derived key, and the project does not work around that protection.
9. **Native Linux or macOS port.** Would require reimplementing the engine.
10. **A new renderer.** Replacing Direct3D 7 replaces the whole graphics layer of the original program.
11. **More than 2,048 keysounds per chart.** The engine's keysound table has 0x800 slots.
12. **More than 2,048 events per track.** The table sizes are fixed in the game's static data section.
13. **Faithful Ez2Catch conversion.** BMS has nothing equivalent to catch notes.
14. **Hand-quality 14-key charts from any 5-key song.** That takes a human charter.
15. **A neural chart generator on the cabinet.** No training set of that size, and no compute on XP.
16. **Cabinet lamps driven by charts.** Lamp output goes through the launcher's I/O layer, which the mod does not own.
17. **Card readers and accounts.** No hardware and no service.
18. **Cloud saves.** No network on the cabinet, and XP's TLS is obsolete.
19. **Downloading songs on the cabinet.** Same reason, plus copyright.
20. **All library art in memory at once.** 10,000 GDI objects and 2 GB of address space.
21. **Every BMS gimmick.** Several have no counterpart in the engine.
22. **Perfect encoding detection.** BMS declares no encoding, and short titles are ambiguous between Shift-JIS, EUC-KR and GBK.
23. **Unicode text in native screens.** Native fonts cover ASCII and KS X 1001 only.
24. **Replacing the corrupt shipped files.** Replacements cannot be distributed.
25. **New judgement rules for native songs.** That would change the game.
26. **A ready-to-play song library.** The songs belong to their artists.
27. **Distributing the game or its assets.** Copyright.
28. **Publishing the decompilation.** It stays private.
29. **Rebuilding the whole game from source.** EZ2DECOMP matches 1,351 of 1,563 entities; a complete, buildable game is far off and would still be private.
30. **WASAPI or ASIO on XP.** XP has no WASAPI, and the engine is built on DirectSound.
31. **HD video BGAs on the cabinet.** Decoding is CPU-bound on a 2010 dual-core.
32. **Hardware video decoding on the cabinet.** No usable path through the XP driver and the 2008 decoders.
33. **A 64-bit game.** The game is 32-bit.
34. **Touch-screen song select.** No hardware.
35. **Streaming from the cabinet.** CPU and operating system.
36. **Pitch-preserving time-stretch of every keysound in real time.** Too costly for the cabinet's CPU.
37. **An in-game chart editor.** Needs new screens and an input model beyond the engine's.
38. **Difficulty that adapts during a song.** Notes are loaded before play starts.
39. **Several modes in one chart.** Each gameplay director handles a single mode.
40. **More than two players.** The engine supports two.
41. **Server-side score verification.** No service.
42. **Per-device press compensation.** The five-tick compensation is an engine-wide constant.
43. **Sub-frame input timing.** The engine samples presses once per frame.
44. **Windows 98 or 2000.** The mod and the launcher need XP's APIs.
45. **Shader effects.** The renderer is fixed-function Direct3D 7.
46. **Title pictures of other sizes.** Song select expects 256×32 pictures.
47. **Automatic library updates from the internet.** No network, and copyright.
48. **Official support.** The rights holder has not endorsed the project.
49. **Future EZ2 releases.** Other executables and formats.
50. **Native ranking screens for custom songs.** They read the game's own ranking files, which stay untouched; custom songs keep their own Top-5.

---

## 2. Optimizations

### 2.1 Feasible

1. **Cache chart ratings** in the scan cache, keyed by each chart's size and date. Saves most of the 3.5 s rating step.
2. **Cache AutoChart output**, keyed by the source chart and the model's version. Saves most of the 2.3 s step.
3. **Persist the sorted per-mode orders** instead of verifying them at each start (7.5 s).
4. **Persist the song-select sizing counts** (7.2 s).
5. **Split the 4.2 s "remaining set-up" step** in the log, to find out what it is.
6. **Rate charts on both cores.**
7. **Generate AutoChart charts on both cores.**
8. **Read the scan cache in one read** and parse it in place.
9. **Intern repeated strings** (category, artist, mode) in the catalogue.
10. **Reserve catalogue vectors** at their known size (212,283 entries).
11. **Pre-size hash maps.**
12. **Replace the mod's remaining case-insensitive linear name searches** with hash lookups.
13. **Index the mod's own song-id lookups.**
14. **Skip unchanged folders by their timestamps** before opening any file.
15. **Larger buffered reads** for BMS parsing.
16. **Sequential-scan hints** when opening large files.
17. **Buffer log writes** and flush them every second and on a crash.
18. **Check that disabled tracing costs nothing** on its fast path.
19. **Lower background-thread priority during play.**
20. **Pause the art and preview workers during play.**
21. **Prefetch keysounds** while the cursor rests on a song.
22. **Keep the last songs' decoded keysounds**, since retries are common.
23. **Keep mono keysounds mono** in the cache.
24. **Keep cut previews on disk**, so a second visit is instant.
25. **Evict previews by size and age.**
26. **Render song-name art while idle** at the title screen.
27. **Prefetch the neighbouring categories' names** once a category settles.
28. **Size the texture ring from measurements**: visible slots plus a measured margin.
29. **16-bit surfaces for song-name textures**, halving their memory.
30. **Reuse one DIB section** for text rendering.
31. **Cache glyphs of the CJK font.**
32. **Create GDI+ objects once per thread.**
33. **Count the ffmpeg fallback's launches** and convert the common cases offline.
34. **Start ffmpeg below normal priority.**
35. **Keep density-graph and NPS data in the cache.**
36. **Avoid copies in lane folding.**
37. **Link-time optimisation** in the release build.
38. **Strip symbols** from the release DLL.
39. **A build tuned for the cabinet's CPU**, kept beside the generic build.
40. **Load GDI+ on first use.**
41. **Release the preview video decoder** when song select ends.
42. **Log lock wait times** of the art and preview threads.
43. **Format `[slowframe]` lines only when a frame is slow.**
44. **Lower the preview frame rate** while the cursor is moving fast.
45. **Skip previews entirely while scrolling fast.**
46. **Coalesce category changes** when the player skips several in a row.
47. **One filter rebuild per jump of 10 categories**, not one per category.
48. **Rebuild FAVORITES only when it changes.**
49. **Rebuild RECENT and MOST PLAYED only after a play.**
50. **Run the test suite in parallel.**

### 2.2 Medium

1. **Lazy AutoChart.** Generate a virtual chart the first time it is selected, not at start-up.
2. **Lazy rating**, backed by the cache.
3. **Cache prebuilt on a PC.** Build the scan cache on a development PC and ship it to the cabinet.
4. **Art prebuilt on a PC.** Ship pre-rendered song-name art to the cabinet.
5. **Start-up under 10 seconds** on the cabinet.
6. **Incremental catalogue updates** after a rescan, instead of rebuilding.
7. **One keysound pool per song**, shared by all its charts.
8. **Compressed keysound cache** (IMA ADPCM), to fit more into 128 MB.
9. **Load the chart during the stage intro.**
10. **Release custom textures on category change**, safely, within the game's cache rules.
11. **A decoding helper process**, keeping decode memory outside the game's 2 GB.
12. **Profile-guided optimisation** with MinGW.
13. **In-process FLAC and unusual WAV decoding**, so those files no longer need the ffmpeg fallback.
14. **A DirectSound buffer pool** for the mod's own voices.
15. **Stream song-length samples from disk** instead of decoding them whole.
16. **Arenas for chart conversion**, to reduce fragmentation.
17. **Log the largest free block** every minute, to track fragmentation.
18. **Address-space budgets per subsystem**, enforced at run time.
19. **Soak tests.** Eight hours of automated browsing on the cabinet.
20. **Frame-time regression checks.** Compare frame-time summaries from the logs of consecutive versions.
21. **Frame budget alarms per subsystem.**
22. **Prefetch tuned to the disk.** Measure seek cost and prefetch accordingly.
23. **Hash the game's per-song id search** while keeping its exact order semantics.
24. **Incremental song-select sizing.**
25. **Scan changed folders on both cores.**
26. **Compact catalogue entries** inside the mod: indexes instead of full 884-byte entries.
27. **Materialise virtual entries on demand.**
28. **A texture atlas for song names**, reducing GDI objects.
29. **Half-resolution video BGAs** on XP.
30. **Lower video work under the cover.** Decode fewer frames when the lane cover hides most of the BGA.
31. **Preview files in a compact format** to cut disk reads.
32. **A more compact AutoChart model file.**
33. **Defer non-essential initialisation** until after the game window appears.
34. **An earlier window**, finishing catalogue work behind a loading screen.
35. **A faster BMS tokenizer** that does not allocate per line.
36. **Encoding detection only on non-ASCII lines.**
37. **Measure and reduce page faults** at song start.
38. **Warm the file cache** for the song under the cursor.
39. **Fewer textures per custom song entry.**
40. **Quieter logging when the disk is slow.**
41. **Decode keysounds on both cores** at song start.
42. **Check cache integrity in the background.**
43. **Split the scan cache into hot and cold data** to shorten its 1,063 ms load.
44. **Compress the scan cache.**
45. **A benchmark harness** that replays a cabinet log's workload on a throttled PC.
46. **An XP virtual machine image** for repeatable performance tests.
47. **Measure the cost of creating DirectSound voices.**
48. **Cache resolved pointers** in per-frame code paths.
49. **One allocation for the whole catalogue.**
50. **A shared buffer for the per-category gather**, instead of the 385 KB stack table.

### 2.3 Out of reach

1. **Multi-threaded rendering.** The engine renders on one thread.
2. **A modern renderer.** It would replace the original program's graphics layer.
3. **All art in video memory.** GDI and address-space limits come first.
4. **Sub-second start-up without a cache.** 23,587 songs have to be read at least once.
5. **More than 4 GB of address space.** The process is 32-bit.
6. **A 64-bit mod.** It must live in a 32-bit process.
7. **GPU video decoding on XP.** No usable path.
8. **WASAPI or ASIO.** Not on XP, and not in the engine.
9. **A finer song clock than the 1 ms pump.** That thread belongs to the engine.
10. **A different texture-cache policy for native screens.** Engine behaviour.
11. **Re-encoding shipped data for speed.** Game files stay untouched.
12. **Generated charts stored on disk.** 188,696 files cost more to write, scan and keep than to generate.
13. **1080p video BGAs on the cabinet.** CPU.
14. **Playing compressed keysounds without decoding.** DirectSound needs PCM.
15. **Hardware audio mixing.** The cabinet's onboard audio has no hardware buffers under XP.
16. **AVX code paths.** The cabinet's CPU does not have AVX.
17. **More than four cores.** The LGA775 platform tops out at four.
18. **More than 4 GB of usable RAM.** 32-bit XP.
19. **Booting from NVMe.** The board does not support it.
20. **Smaller native song-select structures.** Native layout.
21. **Faster native chart loading.** Game code, untouched.
22. **A faster native sort.** Same reason.
23. **A faster launcher.** 2EZConfig is a separate project.
24. **A faster XP boot.** Outside the project.
25. **Event-driven input.** The engine polls DirectInput once per frame.
26. **Frame pacing independent of the display.** Engine timing.
27. **A smaller static data section.** It is the game's own 53.7 MB.
28. **Sharing caches between cabinets in real time.** No network.
29. **Converting the library to native files.** It would mean writing into the game's data, and it would hit the engine's limits.
30. **Decoding every keysound at scan time.** Tens of gigabytes.
31. **Every song's audio in memory.** 2 GB.
32. **Pre-cut previews for the whole library as WAV.** Gigabytes of disk for songs most sessions never visit.
33. **Rendering art for all 212,283 entries at start-up.**
34. **GPU text rendering.** Not with fixed-function Direct3D 7.
35. **Re-rating charts during play.** No benefit for the cost.
36. **Updating the mod without restarting the game.** The cost far exceeds a restart.
37. **GPU compute for background work.** Direct3D 7 on XP has none.
38. **No slow frames at all.** Hard-disk stalls cannot always be hidden.
39. **Deterministic frame times on XP.** No such scheduling guarantees.
40. **Every format without ffmpeg.** The long tail of codecs is too long.
41. **Scanning 23,587 songs in under a second** on a hard disk.
42. **An unbounded texture ring.** Its bound is the point.
43. **Textures without GDI objects.** The engine allocates them.
44. **A second game instance for preloading.**
45. **A software mixer replacing the engine's.**
46. **A faster scene clock.** 60 Hz is native.
47. **More than two voices per sample in the engine.** Engine design.
48. **Compressed logs at no CPU cost.**
49. **Zero-copy disk reads.** XP's file APIs copy.
50. **A cabinet as fast as a development PC.** Measure on the cabinet instead.

---

## 3. Fixes

The feasible tier lists concrete issues and edge cases. Some of them may already behave correctly; in
that case, the fix is a regression test that keeps it that way.

### 3.1 Feasible

1. **`text_codec_test` fails when Windows uses UTF-8 as its ANSI code page (65001).** Make the test set its code page explicitly.
2. **`abm_tool_test` fails without Pillow.** Skip it with a clear message.
3. **The paper's references 6 and 7 are never cited.** Cite them or drop them.
4. **The art-publish race fix has been confirmed on a development PC only.** Confirm it on the cabinet.
5. **A fallback tile could replace art that already arrived.** Add a regression test for the guard.
6. **Stale `.bak_v25` files in `src/`.** Delete them.
7. **Non-numeric `#PLAYLEVEL` values** ("?", "★12", "12+").
8. **Characters outside the font in generated art** (emoji, rare CJK).
9. **Paths longer than `MAX_PATH`** in deeply nested packs.
10. **Folder names outside the ANSI code page** on XP.
11. **`#BPM` of zero or negative.**
12. **A `#STOP` in measure 0.**
13. **Very long `#STOP` values** overflowing tick arithmetic.
14. **Charts with no playable notes** shown in the list.
15. **Empty audio files.**
16. **Unusual WAV headers**: extensible format, 8-bit, 24-bit.
17. **MP3 files with large ID3v2 tags**, which shift the preview start.
18. **Chained Ogg streams.**
19. **Keysound ids defined twice.**
20. **Missing `#WAV` files**: log once per chart, not once per note.
21. **Extension substitution order** when a chart names `.wav` and the folder has `.ogg`, `.mp3` or `.flac`.
22. **Very small measure-length changes.**
23. **Many simultaneous background samples on channel 01.**
24. **A hold with no tail** at the end of a chart.
25. **`#LNOBJ` and `#LNTYPE` both set.**
26. **Holds whose head and tail fold onto different keys** in 5-key folding.
27. **AutoChart scratch on a lane that is still holding.**
28. **Cover position after a mode change.**
29. **Cover on the 2P side.**
30. **Cover picker with an empty picture folder.**
31. **Cover pictures with unusual sizes** on older graphics cards.
32. **Favorites file after a power cut**: confirm the safe-write path flushes to disk.
33. **RECENT order** when the same song is played twice in a row.
34. **Stable MOST PLAYED order** for ties.
35. **Shuffle order kept** after returning from a song.
36. **Jump-10 at the ends** of the category list.
37. **RANDOM picking a song that fails to load.**
38. **A preview whose cut never finishes.** The held countdown needs a timeout.
39. **Previews of songs with no audio.**
40. **Video BGAs with odd dimensions.**
41. **The sign of `JudgeOffsetMs`** described the same way in the guide, the ini and the code.
42. **Constant speed with extreme BPM changes.**
43. **Top-5 order for equal scores.**
44. **Custom courses that contain a removed song.**
45. **Scan-cache invalidation** when the mod's version changes.
46. **A cache folder on read-only media.**
47. **An ever-growing log.** Rotate it by size.
48. **The progress popup staying on top** after the game starts.
49. **Alt+Enter on XP** losing the display device.
50. **Background threads at exit.** Join them before the DLL unloads.

### 3.2 Medium

1. **Repeated samples cut short.** The engine keeps two voices per sample, so a sample triggered three times in quick succession loses one. Duplicate such samples at conversion.
2. **Charts with more than 2,048 events on one track.** Spread them over spare tracks.
3. **Charts with more than 2,048 keysounds.** Merge identical samples by hash.
4. **Titles misread between Shift-JIS, EUC-KR and GBK.**
5. **Catalogue-key collisions** between packs.
6. **Categories over the 872-song limit.** Split them into pages automatically.
7. **Memory growth in long sessions.** Leak audit with a soak test.
8. **GDI objects creeping up** over many plays.
9. **Display device loss** after the screen saver or a display change on XP.
10. **Video BGAs that need the ffmpeg fallback and fail on XP.**
11. **BGM and keysounds drifting apart** after long `#STOP`s.
12. **Audio and video drifting apart** in long songs.
13. **Overlapping notes after 7-key folding** when a folded hold covers a note.
14. **AutoChart scratch inside holds** in generated modes.
15. **Low ratings for charts with long silent intros**, which pull the average down.
16. **Low ratings for hold-heavy charts**, which density undercounts.
17. **The top of the level scale** for extreme charts.
18. **Previews for songs whose BGM is split into many short samples**, with no single file to cut.
19. **Silent previews** for keysound-only charts.
20. **Mixed-script titles** falling back to the wrong font.
21. **Combining characters** in titles.
22. **Very long artist names** pushing out other information.
23. **2P with different modes.**
24. **Judge offset applied consistently** to BGM and keysounds.
25. **Visual jumps around `#STOP`** with constant speed.
26. **The cover drawn over the BGA** in some modes.
27. **Cover width on the 14-key layout.**
28. **Courses that mix modes.**
29. **Top-5 lost when a song folder is renamed.**
30. **Favorites lost when a folder is renamed.** Key them by chart hash.
31. **Play counts lost when a folder is renamed.**
32. **Cache invalidation** when only a sound file changes.
33. **Charts that reference sounds in sibling folders.**
34. **Windows-1252 accented titles.**
35. **Paths with characters outside the code page.** Move file access to wide-character APIs.
36. **Crash dumps.** Write a minidump on a crash, for post-mortem analysis.
37. **Hang-watchdog false alarms** during long loads.
38. **The progress popup taking focus.**
39. **`FpsLimit` together with vertical sync.**
40. **Timer-resolution changes** affecting the engine's 1 ms pump.
41. **Audio crackle** while background workers use the CPU.
42. **Input lag** when background threads saturate both cores.
43. **Turntable stalls** during disk-heavy moments.
44. **RANDOM choosing a virtual chart** that does not exist in the current mode.
45. **Shuffle after a play.**
46. **Returning to the right category** after a song.
47. **Cursor position after a re-sort.**
48. **Song select state** when player 2 joins mid-selection.
49. **Maximum score shown for converted charts with instalment holds.**
50. **Text decoding** on machines whose ANSI code page is UTF-8, in the mod itself and not only in the test.

### 3.3 Out of reach

1. **The native hold-kind count mismatch.** Native code; no shipped chart triggers it, and the converter never emits that kind.
2. **The crossed gauge names.** Native semantics.
3. **The discarded hold average.** Native semantics.
4. **Corrupt shipped data files.** Replacements cannot be distributed.
5. **XP's 2 GB address space.**
6. **XP's kernel mixer latency.**
7. **Bugs in the GT 520's last XP driver.** No further XP drivers exist.
8. **Wrong metadata in community packs at the source.**
9. **Charts authored with wrong offsets.** A per-song offset can compensate; fixing every pack cannot be done here.
10. **Packs missing sounds.**
11. **Titles that are genuinely ambiguous** between encodings.
12. **The five-tick compensation being an average.** Engine constant.
13. **Frame-quantised input** (16.7 ms at 60 Hz).
14. **Missing characters in native fonts.**
15. **Blur from scaling 640×480** on modern displays.
16. **DirectDraw behaviour on Windows 10 and 11.** The operating system's compatibility layer.
17. **Bugs in 2EZConfig.** Not this project's code; report them upstream.
18. **Native song-select limits** for the native songs.
19. **Turntable encoder wear.** Hardware.
20. **Monitor input lag.** Hardware.
21. **Sound-card resampling quality.** Hardware.
22. **Security software flagging the launcher** on modern PCs.
23. **The retail protector's behaviour.** Not touched.
24. **Audio and system clocks drifting apart** in very long sessions. Hardware.
25. **Ambiguities in the BMS specification** that have no single right answer.
26. **Charts that rely on another player's quirks.**
27. **Timing that differs from other BMS players.** This mod keeps the original game's model by design.
28. **Bad rips in packs**, such as clipped audio.
29. **Video codecs with no XP decoder at all.**
30. **File names mangled before they reach the cabinet**, for example by archive tools.
31. **Native ranking name limits.**
32. **Power loss mid-write on the hardware level.** Atomic renames mitigate it, but cannot remove it.
33. **Disk failure.** Hardware.
34. **The arcade's exact timing on PCs** running at other refresh rates.
35. **Third-party controllers' polling latency.**
36. **XP's lack of security updates.**
37. **XP's lack of modern codecs.**
38. **Colour banding in native 16-bit textures.**
39. **Colour-key fringes in native art.**
40. **Low-resolution native title pictures.**
41. **Native charts that trigger one sample more than twice at once.**
42. **Truncation in Ez2Catch scoring.** Native behaviour.
43. **The single-precision constant in Ez2Catch scoring.** Native behaviour.
44. **Mistakes in shipped charts.** Game data.
45. **CV2Mix's lack of a single lane layout.** Engine design.
46. **Native Hangul text limited to 2,350 syllables.**
47. **The engine freeing textures only when a screen ends.**
48. **The game's linear per-song search** on native-only paths.
49. **Issues that only show on other cabinet hardware revisions** that are not available for testing.
50. **Bugs inside the 2008 ffmpeg build.** It cannot practically be rebuilt for XP.

---

## 4. Enhancements

### 4.1 Feasible

1. **Smoother cover movement**, accelerating with turntable speed.
2. **Cover size readout** while adjusting.
3. **Thumbnails** in the cover picker.
4. **The picker remembers its cursor.**
5. **Cover pictures with alpha** in PNG.
6. **Clearer typography** in the song-select panel.
7. **Peak section marked** on the NPS panel.
8. **Density graph coloured by lane type** (keys, scratch, pedal).
9. **Top-5 with dates and grade counts.**
10. **RANDOM that avoids the last few picks.**
11. **Sort options inside FAVORITES.**
12. **Configurable RECENT length.**
13. **Play counts shown in MOST PLAYED.**
14. **Configurable category jump**, instead of a fixed 10.
15. **Current sort order shown on screen.**
16. **A reshuffle button** for the shuffle order.
17. **Outlined titles** in generated art, for contrast.
18. **Condensed type** for long titles.
19. **Disc art cropped from `#BANNER` or `#STAGEFILE`.**
20. **Eyecatch from `#STAGEFILE`** whenever one exists.
21. **Fade in and out** on previews.
22. **Loudness-matched previews.**
23. **First video frame shown at once** in BGA previews.
24. **Step names and percentages** in the progress popup.
25. **Version and configuration summary** at the top of the log.
26. **Log rotation by day.**
27. **Severity levels** in the log.
28. **Skip hidden and system folders** in the scan.
29. **Chart extensions accepted in any letter case.**
30. **Rating inputs in the log** on request.
31. **Jack bonus logged** with its threshold.
32. **AutoChart avoids identical consecutive scratch patterns.**
33. **AutoChart pedal rhythm follows the dominant BPM.**
34. **Chord-size limits per mode** in AutoChart.
35. **Custom courses named after their songs.**
36. **Constant-speed target shown on screen.**
37. **Judge offset adjustable from song select.**
38. **The remaining untranslated textures** in English.
39. **Screenshots** in the guide.
40. **A longer troubleshooting table** in the guide.
41. **An architecture diagram** in DEVELOPMENT.md.
42. **Tests for the cover picker.**
43. **`text_codec_test` independent of the code page.**
44. **The XP check run on every commit.**
45. **One-command release packaging.**
46. **Release notes generated from the CHANGELOG.**
47. **A `--dry-run` flag** on every `fix_*.py` tool.
48. **One entry point** for the library tools.
49. **A report of untranslated textures.**
50. **Defaults and ranges** in every `BMSMod.ini` comment.

### 4.2 Medium

1. **A redesigned song-select panel** that fits more information into 640×480.
2. **Hold shading** in the density graph.
3. **AutoChart statistics per level**, so easy and hard charts are learned separately.
4. **Phrase-aware AutoChart** using keysound names.
5. **Ratings calibrated against clear rates.**
6. **Separate ratings for doubles.**
7. **Better weighting of long holds** in ratings.
8. **Sound-aware long-note pairing costs.**
9. **Folding that preserves hand patterns** and avoids unnatural jacks.
10. **Musical folding of 9-button charts.**
11. **Encoding detection with title dictionaries.**
12. **Better CJK typography** in generated art.
13. **Preview at the chorus**, found by audio energy.
14. **Smoother video on XP** through frame skipping.
15. **Picture sets per mode** for the cover.
16. **Curated difficulty in custom courses.**
17. **A RECENTLY ADDED category.**
18. **Top-5 per player.**
19. **Continuous integration** with a MinGW cross-build.
20. **A cabinet-log analyser** that summarises slow frames.
21. **A resumable, parallel scan.**
22. **Scan-cache versions with migrations.**
23. **Configuration reload** without restarting.
24. **A hang watchdog that captures the game thread's stack.**
25. **Tracer documentation.**
26. **Clearer on-screen errors.**
27. **Consistent fonts** across translated textures.
28. **A GUI** for library maintenance.
29. **A video walkthrough** of the guide.
30. **Community review** of the paper.
31. **A differential trace of Ez2Catch.**
32. **Every gameplay option traced.**
33. **Tests that replay real cabinet sessions.**
34. **An installer.**
35. **Updates from a USB package.**
36. **Checksums** published with releases.
37. **Declared and computed levels** in the level categories.
38. **A preview cache shared across sessions.**
39. **Art prefetch driven by scroll speed.**
40. **A texture ring for eyecatches.**
41. **Packs with several charts per difficulty.**
42. **One entry per song** when several packs carry it, with all its charts.
43. **Difficulty names** (BEGINNER to INSANE) mapped onto the game's slots.
44. **Keysound loudness normalisation** per chart.
45. **Better resampling filters.**
46. **Pack icons** on category sprites.
47. **A cover interface for player 2.**
48. **RANDOM weighted towards unplayed songs.**
49. **Several favorites lists.**
50. **Structured log lines** for tools.

### 4.3 Out of reach

1. **A high-resolution interface.** 640×480 layouts.
2. **New native fonts.**
3. **More judgement grades.** The engine has three, plus FAIL and MISS.
4. **New gauge types.** The engine's gauge logic is fixed.
5. **Per-note effects.** Fixed-function rendering.
6. **3D backgrounds.**
7. **Redesigned native screens.**
8. **AutoChart equal to human charting.**
9. **A rating that matches every player's experience.**
10. **Perfect encoding detection.**
11. **Official previews for custom songs.** None exist.
12. **Video BGAs at source resolution** on the cabinet.
13. **Animated 3D covers.**
14. **Real-time audio effects** on XP.
15. **Spatial audio.**
16. **Variable lane widths.** Engine layouts are fixed.
17. **Note skins changing mid-song.**
18. **Lane counts changing mid-song.**
19. **Turntable sensitivity beyond the launcher's settings.** The launcher owns the I/O layer.
20. **New levels for native charts.** Native data stays untouched.
21. **Retiming native charts.**
22. **New previews for native songs.**
23. **Upscaled native videos.** Files untouched, and the CPU cannot afford it.
24. **Other players' skins.**
25. **The LR2 skin format.**
26. **Internet ranking protocols.**
27. **Full gimmick support for StepMania or osu!mania charts.**
28. **Charts from other commercial games' protected formats.** Legal.
29. **Lossless folding of 7-key charts.** Folding loses information by nature.
30. **Lossless folding of 14-key charts onto five keys.**
31. **Keysounded AutoChart for songs that only have a BGM file.** There is nothing to key.
32. **Turning BGM-only songs into keysounded charts.** Source separation is beyond XP.
33. **Real-time stem separation.**
34. **Human-quality charts from audio alone.**
35. **Judgement finer than a tick.** The engine judges in ticks.
36. **Windows in milliseconds.** The engine's windows are beat-relative by design.
37. **BMS scoring rules for converted charts.** The project keeps the game's scoring.
38. **LR2's exact timing.**
39. **60 fps video for every song** on the cabinet.
40. **Better audio from onboard sound.**
41. **Seamless song transitions.** The engine's screens load in sequence.
42. **Mod options in the operator menu.** Native code.
43. **Custom scores in the game's ranking files.** Game files stay untouched.
44. **More difficulty slots** than the engine has per mode.
45. **More charts shown at once** than the engine's slots.
46. **New play modes.** The gameplay directors are fixed.
47. **Per-chart judgement sounds.**
48. **Reverse scroll.**
49. **Note-size changes.**
50. **Covers driven by chart events.**

---

## 5. Adjustments

### 5.1 Feasible

1. **`EnableFunctionTrace=0`** in the production configuration.
2. **Confirm `ShowProgressPopup=1`** as the default.
3. **`PreviewFps=15`** automatically on the XP memory tier.
4. **A finer `LaneCoverStep`** while the button is held.
5. **`LaneCoverSize` defaults per mode.**
6. **Review `PageSize=400`** against the 872-song category limit.
7. **Try `NativeModelMaxChartsPerMode=256`.**
8. **AutoChart `Intensity` per mode.**
9. **Document `Seed`** as user-changeable.
10. **Re-measure the memory tiers** (128 MB keysounds, 96 MB art on XP).
11. **Art cache size.**
12. **Texture ring size.**
13. **Art prefetch radius.**
14. **Preview delay**, currently eight frames after the cursor stops.
15. **The `[slowframe]` threshold.**
16. **`[frametime]` every 10 s** in production instead of 5 s.
17. **RECENT length.**
18. **Minimum plays for MOST PLAYED.**
19. **Category jump size.**
20. **Default sort order per category.**
21. **`CustomCourses=12`.**
22. **`HudPanelX`/`HudPanelY` defaults for 2P.**
23. **A documented `JudgeOffsetMs`** for the reference cabinet.
24. **A sensible `FpsLimit`** default for modern PCs.
25. **Long-note pairing penalties** (1,000 and 1,500).
26. **Rating weights** (0.8 and 0.2).
27. **Rating factor 2.6**, refitted on the whole library.
28. **Peak window length** (2 s).
29. **Jack-bonus threshold.**
30. **Level curve**, refitted with new packs.
31. **Level ceiling** at 20.
32. **A 768-tick grid** for rare subdivisions, if measurements justify it.
33. **Tie-break direction** in lane folding.
34. **Where scratch notes go** in 5-key folding.
35. **Memory-tier thresholds** (1.5 GB of RAM).
36. **`BorderlessOnly`** on modern PCs.
37. **`AltEnter`** default.
38. **`HangWatchdog=1`** in development builds.
39. **`GsProbe=0`** in every release.
40. **A guide section on `TraceJudge`** for bug reports.
41. **A configurable log location.**
42. **`CacheRoot` on the SSD.**
43. **Recommended settings** for the XP cabinet and for a modern PC, side by side in the guide.
44. **Release size** shown in the README.
45. **CHANGELOG sections** (Added, Fixed, Changed).
46. **A written version-numbering policy.**
47. **A release checklist** in DEVELOPMENT.md.
48. **Environment-dependent tests reported as skipped**, not failed.
49. **Ignore `*.bak_*`** in `.gitignore`.
50. **A cabinet quick card** in the release ZIP.

### 5.2 Medium

1. **Fit the rating to player clear data.**
2. **Offset presets per pack.**
3. **Cache sizes from measured free memory** at run time.
4. **Prefetch radius from scroll speed.**
5. **`PreviewFps` from measured decode time.**
6. **A calibrated `#RANK` mapping table.**
7. **Calibrated `#TOTAL` gauge rates.**
8. **AutoChart parameters fitted per mode**, tested on native charts held out of training.
9. **The `/3GB` boot switch on the cabinet.** The executable is large-address aware, but the drivers must cope.
10. **A higher GDI handle quota.** Possible through the registry; it needs testing on the cabinet.
11. **Page-file size and placement.**
12. **XP services** that disturb frame times.
13. **A four-core LGA775 CPU**, measured before and after.
14. **An SSD**, measured before and after.
15. **Launcher audio buffer settings.**
16. **Timer resolution effects.**
17. **Thread affinity**: the game on one core, workers on the other.
18. **Thread priorities**, chosen by measurement.
19. **Disk read-ahead** for hard disks.
20. **Judge offset per input device.**
21. **Display lag per monitor.**
22. **The level curve**, rebalanced with player feedback.
23. **Difficulty slots** for songs with many charts.
24. **AutoChart scratch density** at high levels.
25. **Pedal frequency** in the modes that use it.
26. **The 4.17 ms displacement**, re-measured on the whole library.
27. **Long-note penalties**, refitted on the full audit set.
28. **The 872-song category limit.**
29. **Organisation of the 106 categories**: merge the smallest packs.
30. **Duplicate cleanup** guided by the duplicate finder.
31. **Loudness normalisation** across packs, done offline.
32. **Preview length.**
33. **Preview volume** relative to the game's music.
34. **English texture wording**, reviewed by native speakers.
35. **A non-technical operator's guide.**
36. **Release ZIP layout** mirroring the game folder exactly.
37. **Logs without personal paths**, before they are shared.
38. **Performance budgets as release gates** (start-up time, worst frame).
39. **A cabinet soak test as a release gate.**
40. **A compatibility table** of 2EZConfig versions.
41. **A second cabinet** for testing.
42. **Windows 7, 10 and 11** test PCs.
43. **Other sound cards.**
44. **Generated-art colours** tuned for the cabinet's monitor.
45. **Font sizes** tuned for the cabinet's monitor.
46. **Pack-specific junk files** ignored by the scan.
47. **Category names** without pack prefixes.
48. **Title cleaning**: move suffixes such as "[ANOTHER]" into the difficulty.
49. **Artist cleaning**: separate "obj." credits.
50. **Where the virtual categories sit** on the wheel.

### 5.3 Out of reach

1. **The +3-tick widening** for native songs. A game rule.
2. **The five-tick press compensation.** A game rule.
3. **Native gauge rates.** Game data.
4. **Native key-mode scoring** (300 / 150 / 41). A game rule.
5. **The Ez2Catch scoring constant.** A game rule.
6. **The 60 Hz scene clock.**
7. **The 640×480 resolution.**
8. **The 256×32 title picture size.**
9. **A finer tick for native charts.**
10. **Native window sets.** Game data.
11. **More than 3 GB of address space** on 32-bit XP.
12. **More than 65,536 GDI objects.** The operating system's hard cap.
13. **Real-time priority** for the game, safely.
14. **The GT 520's XP driver.**
15. **XP's mixer latency floor.**
16. **Overclocking the cabinet.** OEM board and risk.
17. **RAM beyond 4 GB.**
18. **The I/O board's polling rate.** Launcher and hardware.
19. **Turntable resolution.** Hardware.
20. **The launcher's defaults.** A separate project.
21. **Native song levels.**
22. **Native song titles.**
23. **Native category order.**
24. **Native preview timing.**
25. **Authored levels in the packs.** They belong to their authors.
26. **Authored offsets in the packs.**
27. **One rating that satisfies every community table.**
28. **One judge offset for every player.**
29. **One cover size for every player.**
30. **One AutoChart seed that suits everyone.**
31. **Hard disks tuned to SSD speed.**
32. **A cabinet that needs no maintenance.** XP is past its end of life.
33. **LR2's level scale exactly.**
34. **Another game's gauge.**
35. **Another game's timing windows.**
36. **Folded 7-key charts identical to 5-key originals.**
37. **One encoding for every pack, at the source.**
38. **Performance tuning from a development PC alone.**
39. **A 2 GB budget that flexes per song.**
40. **The game's texture retention.**
41. **The native score storage format.**
42. **Native input mapping** beyond what the launcher offers.
43. **Anything involving the retail protection.**
44. **The two-player limit.**
45. **The number of play modes.**
46. **The order of native screens.**
47. **Native attract-mode timing.**
48. **Native volume curves.**
49. **The game's random generator** for note-order options.
50. **The game's own song list.** It stays as shipped; custom songs are added beside it.
