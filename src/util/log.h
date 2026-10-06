// Thread-safe debug logger: appends timestamped lines to log.txt.
// Callable from any thread; opens the file on first use.
#pragma once

// printf-style formatting appended to log.txt in the working directory.
void logWrite(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
