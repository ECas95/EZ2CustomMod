#pragma once
#include <cstdint>

void InstallTraceHooks();
void RemoveTraceHooks();

// Bytes the SongSelect object must be for the raised master[] to fit, or 0 if the capacity patch
// did not install. mod_main's VirtualAlloc hook sizes the object from this instead of a fixed
// constant -- the two numbers used to be independent, and the buffer holding less than the patched
// capacity dropped songs off the end of the list with nothing in the log.
unsigned int RequiredObjectBytes();

// Song-select BGA preview, driven from the per-frame renderer hook. Defined in mod_main.
void BgaPreviewTick(uint32_t obj, const char* selectedId);
void BgaPreviewStop(uint32_t obj);

// Per-category bg/ico overlays, drawn from the song-select frame hook. Defined in mod_main.
void CategoryOverlaySetCategory(const char* name);
void CategoryOverlayDraw();
void CategoryOverlayReset();

// The mod's notice card in song select (sort mode, category jumps, speed). Defined in mod_main.
// `hint` is the short amber key label in the corner ("EFX4", "PEDAL+EFX3"); holdMs is how long
// the card stays fully up before fading.
void HudPanelShow(const char* title, const char* line, const char* sub, const char* hint,
                  unsigned long holdMs);
void HudPanelHide();
void HudPanelDraw();
void HudPanelReset();

// The EZ difficulty column: its level digit (and, with the cursor on it, the four the game stops
// drawing) plus its label, drawn at the end of the song-select frame. Defined in mod_main.
void EzColumnDraw(uint32_t obj);

// Constant scroll speed: the row the cursor rests on tells its BPM, the chart-load hook applies
// the speed. Defined in mod_main.
void ConstantSpeedNoteSelection(float bpm, const char* id);

// Frame-time meter. Every hook of ours adds the QPC ticks it spent to one of these buckets; the
// frame-end hook reports frame pacing and the buckets every few seconds, so a stutter can be
// attributed (or not) to the mod. Defined in trace_hooks.
enum ModTimeBucket { kModTimeTrace = 0, kModTimeFiles = 1, kModTimeVideo = 2, kModTimeBuckets = 3 };
void ModTimeAdd(int bucket, long long qpcTicks);
long long ModTimeNow();
