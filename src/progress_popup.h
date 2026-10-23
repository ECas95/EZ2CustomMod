#pragma once

// Startup progress popup: a small Win32 window with a marquee progress bar and
// a scrolling read-only log box. Fed by the logger sink (SetLogSink), so every
// LogLine during ScanBMSData/asset conversion shows up live. Lives on its own
// message-pump thread so it paints while DllMain's InitThread does the work.
void ShowStartupPopup();   // create + start pump
void CloseStartupPopup();  // close window, join thread

// Called once per fully-processed song from the scan thread. Drives the determinate bar +
// "% (done/total)" title, and is the ONLY thing shown in the popup log (one line per song).
void StartupProgress(int done, int total, const char* songLabel);
