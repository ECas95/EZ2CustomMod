#pragma once

void InstallTraceHooks();
void RemoveTraceHooks();

// Bytes the SongSelect object must be for the raised master[] to fit, or 0 if the capacity patch
// did not install. mod_main's VirtualAlloc hook sizes the object from this instead of a fixed
// constant -- the two numbers used to be independent, and the buffer holding less than the patched
// capacity dropped songs off the end of the list with nothing in the log.
unsigned int RequiredObjectBytes();
