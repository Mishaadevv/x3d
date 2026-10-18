#pragma once

#include "Core/String/String.h"

namespace zx::platform {

bool IsDebuggerAttached();

/// Writes UTF-8 text to the debugger output window (OutputDebugString).
void DebugOutput(StringView text);

/// Writes UTF-8 text to stdout/stderr (if a console is attached).
void ConsoleWrite(StringView text, bool isError = false);

/// Enables ANSI colors / UTF-8 code page on the attached console.
void InitConsole();

/// Shows a modal error box (UTF-8). Blocks. Safe to call from any thread.
void ShowErrorMessageBox(StringView title, StringView text);

/// Terminates the process immediately without running destructors.
ZX_NORETURN void TerminateProcessNow(i32 exitCode);

/// Captures return addresses of the current call stack. Returns number of frames written.
u32 CaptureStackTrace(void** frames, u32 maxFrames, u32 framesToSkip = 0);

/// Writes "module!function (file:line)" for an address into buffer. Returns chars written.
usize SymbolizeAddress(const void* address, char* buffer, usize capacity);

/// Writes a human-readable stack trace of the current thread into buffer.
usize FormatStackTrace(char* buffer, usize capacity, u32 framesToSkip = 0);

struct CpuInfo
{
    char brand[64] = {};
    u32 logicalCores = 1;
    u32 physicalCores = 1;
    bool sse41 = false;
    bool sse42 = false;
    bool avx = false;
    bool avx2 = false;
    bool fma = false;
    bool f16c = false;
    u64 cacheLineSize = 64;
};

const CpuInfo& GetCpuInfo();

struct MemoryInfo
{
    u64 totalPhysical = 0;
    u64 availablePhysical = 0;
    u64 processWorkingSet = 0;
    u64 processPeakWorkingSet = 0;
    u64 pageSize = 4096;
    u64 allocationGranularity = 65536;
};

MemoryInfo GetMemoryInfo();

/// Full path of the running executable (UTF-8, forward slashes).
StringView GetExecutablePath();

/// Directory of the running executable (UTF-8, forward slashes, no trailing slash).
StringView GetExecutableDirectory();

/// Raw command line of the process (UTF-8, without the executable name).
StringView GetCommandLineArgs();

/// The user's interface language as a two-letter code ("en", "ru", "de"; "en" if unknown).
String GetUserLanguage();

} // namespace zx::platform
