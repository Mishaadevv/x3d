#pragma once

#include "Core/String/String.h"

namespace zx {

struct CoreInitParams
{
    StringView appName = "ZeqouX3D";
    /// Directory for logs/crash dumps; empty = "<exe dir>/Saved".
    StringView savedDirectory;
    /// Worker thread count for the job system (0 = auto).
    u32 jobWorkers = 0;
    u32 ioThreads = 2;
    bool enableFileLog = true;
    bool enableCrashHandler = true;
    /// Mount "/Engine" and "/Shaders" from the engine installation (off for packaged games: they mount a package).
    bool mountEngineDirectories = true;
    /// Report leaked allocations made after initialization at shutdown (non-Shipping).
    bool reportLeaksOnShutdown = true;
};

/// Initializes Core subsystems in order: main-thread marker, console, crash handler, command line, log,
/// file system (mounts "/Engine" to Engine/Content and "/Saved"), job system, profiler.
bool CoreInit(const CoreInitParams& params = {});
/// Shuts down in reverse order. Returns the number of leaked allocations (0 when tracking is off).
u64 CoreShutdown();

/// Root directory of the engine installation (contains Engine/). Found by walking up from the executable.
StringView GetEngineRootDirectory();
/// Same search without requiring CoreInit (used by tests and tools).
String FindEngineRootDirectory();
/// "<root>/Saved" or the directory passed to CoreInit.
StringView GetSavedDirectory();

} // namespace zx
