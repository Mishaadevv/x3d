#include "Core/HAL/CrashHandler.h"
#include "Core/Assert.h"
#include "Core/HAL/PlatformMisc.h"

#include "Windows/WindowsInclude.h"

#include <DbgHelp.h>
#include <atomic>
#include <cwchar>
#include <cstdio>

namespace zx {

namespace {

wchar_t g_crashDir[1024] = {};
bool g_writeMinidump = true;
std::atomic<void (*)(StringView)> g_crashCallback{nullptr};
std::atomic<bool> g_handling{false};

const char* ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000409:
        return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000374:
        return "STATUS_HEAP_CORRUPTION";
    default:
        return "UNKNOWN_EXCEPTION";
    }
}

void WriteMinidump(EXCEPTION_POINTERS* info, char* pathOut, usize pathCap)
{
    pathOut[0] = '\0';
    ::CreateDirectoryW(g_crashDir, nullptr);
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    wchar_t path[1200];
    swprintf_s(path, L"%s\\Crash_%04u%02u%02u_%02u%02u%02u_%u.dmp", g_crashDir, t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, ::GetCurrentProcessId());
    HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = ::GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    const auto type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules);
    ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, type, &mei, nullptr, nullptr);
    ::CloseHandle(file);
    win::WideToUtf8(path, wcslen(path), pathOut, pathCap);
}

usize WalkStack(CONTEXT* ctx, FixedBufferSink& sink)
{
    HANDLE process = ::GetCurrentProcess();
    HANDLE thread = ::GetCurrentThread();
    CONTEXT context = *ctx;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    // Make sure symbols are initialized (SymbolizeAddress does it lazily).
    char dummy[8];
    platform::SymbolizeAddress(reinterpret_cast<void*>(uptr(context.Rip)), dummy, sizeof(dummy));

    usize count = 0;
    char line[1024];
    for (u32 i = 0; i < 64; ++i) {
        if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                           ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0)
            break;
        const usize n = platform::SymbolizeAddress(reinterpret_cast<void*>(uptr(frame.AddrPC.Offset)), line,
                                                   sizeof(line));
        FormatTo(sink, "  [{:2}] {}\n", i, StringView(line, n));
        ++count;
    }
    return count;
}

LONG WINAPI UnhandledExceptionFilterImpl(EXCEPTION_POINTERS* info)
{
    if (g_handling.exchange(true))
        return EXCEPTION_EXECUTE_HANDLER;

    static char report[16384];
    FixedBufferSink sink(report, sizeof(report));
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    FormatTo(sink, "\n==== ZeqouX3D crashed ====\n{} ({:#010x}) at {}\n", ExceptionName(rec->ExceptionCode),
             u32(rec->ExceptionCode), rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const char* op = rec->ExceptionInformation[0] == 0 ? "reading" : (rec->ExceptionInformation[0] == 1 ? "writing" : "executing");
        FormatTo(sink, "Access violation {} address {:#x}\n", op, u64(rec->ExceptionInformation[1]));
    }
    sink.Append("Callstack:\n");
    WalkStack(info->ContextRecord, sink);

    if (g_writeMinidump && rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        char dumpPath[1024];
        WriteMinidump(info, dumpPath, sizeof(dumpPath));
        if (dumpPath[0])
            FormatTo(sink, "Minidump: {}\n", dumpPath);
    }

    platform::DebugOutput(sink.View());
    platform::ConsoleWrite(sink.View(), true);
    if (auto cb = g_crashCallback.load())
        cb(sink.View());
    detail::OnReportFailureHook(sink.View());
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void InstallCrashHandler(const CrashHandlerSettings& settings)
{
    if (!settings.crashDirectory.Empty()) {
        win::Utf8ToWide(settings.crashDirectory, g_crashDir, ZX_ARRAY_COUNT(g_crashDir));
    } else {
        const StringView dir = platform::GetExecutableDirectory();
        const usize n = win::Utf8ToWide(dir, g_crashDir, ZX_ARRAY_COUNT(g_crashDir));
        wcscat_s(g_crashDir + n, ZX_ARRAY_COUNT(g_crashDir) - n, L"\\Crashes");
    }
    for (wchar_t* c = g_crashDir; *c; ++c)
        if (*c == L'/')
            *c = L'\\';
    g_writeMinidump = settings.writeMinidump;

    // Reserve stack so the handler can run after a stack overflow.
    ULONG guarantee = 64 * 1024;
    ::SetThreadStackGuarantee(&guarantee);
    ::SetUnhandledExceptionFilter(&UnhandledExceptionFilterImpl);
    // Route CRT abort/invalid-parameter paths to the same report.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

void SetCrashReportCallback(void (*callback)(StringView report)) { g_crashCallback.store(callback); }

} // namespace zx
