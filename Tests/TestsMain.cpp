// ZXTests entry point.
// Usage: ZXTests [--filter=Pattern] [--verbose|-v] [--list]

#include "Core/Assert.h"
#include "Core/HAL/CrashHandler.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Log/Log.h"
#include "Core/Memory/Memory.h"
#include "Core/Testing/Test.h"

int main(int argc, char** argv)
{
    using namespace zx;
    platform::InitConsole();
    SetErrorDialogsEnabled(false);
    InstallCrashHandler(CrashHandlerSettings{.writeMinidump = false});
    Log::Initialize({}, LogLevel::Warning); // engine warnings/errors (e.g. D3D12 debug layer) show up in test output

    test::RunOptions options;
    for (int i = 1; i < argc; ++i) {
        const StringView arg(argv[i]);
        if (arg.StartsWith("--filter="))
            options.filter = arg.DropLeft(9);
        else if (arg == "--verbose" || arg == "-v")
            options.verbose = true;
        else if (arg == "--list")
            options.listOnly = true;
        else if (!arg.StartsWith("-"))
            options.filter = arg;
    }
    const u64 allocationBaseline = GetAllocationSequence();
    const i32 failed = test::RunTests(options);
    // Every allocation made by the tests must be released (process-lifetime data uses LeakIgnoreScope).
    const u64 leaks = options.listOnly ? 0 : ReportMemoryLeaks(allocationBaseline);
    if (leaks > 0)
        platform::ConsoleWrite("\x1b[31mMemory leaks detected\x1b[0m\n", true);
    Log::Shutdown();
    return failed == 0 && leaks == 0 ? 0 : 1;
}
