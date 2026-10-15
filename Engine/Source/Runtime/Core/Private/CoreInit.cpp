#include "Core/CoreInit.h"
#include "Core/Console/Console.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/CrashHandler.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Log/Log.h"
#include "Core/Memory/Memory.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Path.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"
#include "Core/Version.h"

namespace zx {

namespace {
struct CoreState
{
    String engineRoot;
    String savedDir;
    u64 allocationBaseline = 0;
    bool reportLeaks = true;
    bool initialized = false;
};

CoreState& Core()
{
    static CoreState s;
    return s;
}

} // namespace

String FindEngineRootDirectory()
{
    // Walk up from the executable directory until a folder containing Engine/Content is found.
    String dir(platform::GetExecutableDirectory());
    for (int i = 0; i < 8 && !dir.Empty(); ++i) {
        if (platform::DirectoryExists(dir + "/Engine/Content"))
            return dir;
        const StringView parent = Path::Parent(dir);
        if (parent.Size() == dir.Size())
            break;
        dir = String(parent);
    }
    return String(platform::GetExecutableDirectory());
}

StringView GetEngineRootDirectory() { return Core().engineRoot; }
StringView GetSavedDirectory() { return Core().savedDir; }

bool CoreInit(const CoreInitParams& params)
{
    CoreState& c = Core();
    if (c.initialized)
        return true;
    Thread::MarkMainThread();
    Thread::SetCurrentName("Main");
    platform::InitConsole();
    CommandLine::Initialize(platform::GetCommandLineArgs());

    c.engineRoot = FindEngineRootDirectory();
    c.savedDir = params.savedDirectory.Empty() ? c.engineRoot + "/Saved" : Path::Normalize(params.savedDirectory);

    if (params.enableCrashHandler) {
        const String crashDir = c.savedDir + "/Crashes";
        InstallCrashHandler(CrashHandlerSettings{crashDir, true});
    }
    if (CommandLine::HasFlag("unattended"))
        SetErrorDialogsEnabled(false);

    String logPath;
    if (params.enableFileLog)
        logPath = String::Format("{}/Logs/{}.log", c.savedDir, params.appName);
    Log::Initialize(logPath);

    const platform::CpuInfo& cpu = platform::GetCpuInfo();
    const platform::MemoryInfo mem = platform::GetMemoryInfo();
    ZX_LOG_INFO(LogCore, "{} {} ({}) - {}", EngineName, EngineVersionString, BuildConfigurationName, params.appName);
    ZX_LOG_INFO(LogCore, "CPU: {} ({} cores, {} threads, AVX2 {}), RAM {:.1f} GB", cpu.brand, cpu.physicalCores,
                cpu.logicalCores, cpu.avx2 ? "yes" : "no", f64(mem.totalPhysical) / f64(1_GB));
    ZX_LOG_INFO(LogCore, "Engine root: {}", c.engineRoot);

    fs::Initialize(params.ioThreads);
    if (params.mountEngineDirectories) {
        fs::Mount("/Engine", c.engineRoot + "/Engine/Content", 0, true);
        fs::Mount("/Shaders", c.engineRoot + "/Engine/Shaders", 0, false);
    }
    platform::CreateDirectoryTree(c.savedDir);
    fs::Mount("/Saved", c.savedDir, 0, true);

    jobs::Initialize(jobs::Settings{params.jobWorkers});
    ZX_LOG_INFO(LogCore, "Job system: {} workers", jobs::GetWorkerCount());

    CommandLine::ApplyCVarOverrides();
    profiler::BeginFrame();

    c.reportLeaks = params.reportLeaksOnShutdown;
    c.allocationBaseline = GetAllocationSequence();
    c.initialized = true;
    return true;
}

u64 CoreShutdown()
{
    CoreState& c = Core();
    if (!c.initialized)
        return 0;
    jobs::Shutdown();
    fs::Shutdown();
    ZX_LOG_INFO(LogCore, "Shutdown complete");
    Log::Shutdown();
    c.initialized = false;
    u64 leaks = 0;
    if (c.reportLeaks)
        leaks = ReportMemoryLeaks(c.allocationBaseline);
    return leaks;
}

} // namespace zx
