#pragma once
#include <cstdarg>
#include <string>

void LogInit();
void LogClose();
void LogLine(const char* fmt, ...);
void LogV(const char* prefix, const char* fmt, va_list ap);
void LogRaw(const std::string& s);

// Optional sink: gets every formatted log line (without trailing newline).
// Used by the startup progress popup. Null = no sink.
void SetLogSink(void (*sink)(const char* line));
