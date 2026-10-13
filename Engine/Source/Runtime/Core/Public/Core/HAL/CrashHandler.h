#pragma once

#include "Core/String/StringView.h"

namespace zx {

struct CrashHandlerSettings
{
    /// Directory for minidumps and crash logs (UTF-8). Empty = "<exe dir>/Saved/Crashes".
    StringView crashDirectory;
    bool writeMinidump = true;
};

/// Installs an unhandled-exception filter (access violations, stack overflow, etc.) that prints the exception,
/// a symbolized callstack of the faulting thread, and writes a minidump. Call once at startup.
void InstallCrashHandler(const CrashHandlerSettings& settings = {});

/// Called with the crash report text before the process terminates (e.g. to flush logs).
void SetCrashReportCallback(void (*callback)(StringView report));

} // namespace zx
