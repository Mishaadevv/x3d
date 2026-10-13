#include "Core/HAL/PlatformMisc.h"
#include "Core/String/Format.h"

#include "Windows/WindowsInclude.h"

#include <DbgHelp.h>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>

namespace zx::win {

usize Utf8ToWide(StringView utf8, wchar_t* out, usize capacity)
{
    if (capacity == 0)
        return 0;
    if (utf8.Empty()) {
        out[0] = L'\0';
        return 0;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.Data(), int(utf8.Size()), out, int(capacity - 1));
    if (n <= 0) {
        // Buffer too small: convert as much as fits by halving input until success.
        usize len = utf8.Size();
        while (n <= 0 && len > 0) {
            len /= 2;
            n = MultiByteToWideChar(CP_UTF8, 0, utf8.Data(), int(len), out, int(capacity - 1));
        }
        if (n < 0)
            n = 0;
    }
    out[n] = L'\0';
    return usize(n);
}

usize WideToUtf8(const wchar_t* wide, usize wideLen, char* out, usize capacity)
{
    if (capacity == 0)
        return 0;
    if (wideLen == 0) {
        out[0] = '\0';
        return 0;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, int(wideLen), out, int(capacity - 1), nullptr, nullptr);
    if (n <= 0) {
        usize len = wideLen;
        while (n <= 0 && len > 0) {
            len /= 2;
            n = WideCharToMultiByte(CP_UTF8, 0, wide, int(len), out, int(capacity - 1), nullptr, nullptr);
        }
        if (n < 0)
            n = 0;
    }
    out[n] = '\0';
    return usize(n);
}

} // namespace zx::win

namespace zx::platform {

bool IsDebuggerAttached() { return ::IsDebuggerPresent() != FALSE; }

void DebugOutput(StringView text)
{
    wchar_t buffer[4096];
    while (!text.Empty()) {
        // Convert in chunks; cut at a UTF-8 boundary.
        usize chunk = text.Size() < 2000 ? text.Size() : 2000;
        while (chunk < text.Size() && chunk > 0 && (u8(text[chunk]) & 0xC0) == 0x80)
            --chunk;
        win::Utf8ToWide(text.Left(chunk), buffer, ZX_ARRAY_COUNT(buffer));
        ::OutputDebugStringW(buffer);
        text = text.DropLeft(chunk);
    }
}

void ConsoleWrite(StringView text, bool isError)
{
    HANDLE h = ::GetStdHandle(isError ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    ::WriteFile(h, text.Data(), DWORD(text.Size()), &written, nullptr);
}

void InitConsole()
{
    ::SetConsoleOutputCP(CP_UTF8);
    for (DWORD id : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE h = ::GetStdHandle(id);
        DWORD mode = 0;
        if (h && h != INVALID_HANDLE_VALUE && ::GetConsoleMode(h, &mode))
            ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

void ShowErrorMessageBox(StringView title, StringView text)
{
    wchar_t wtitle[256];
    wchar_t wtext[8192];
    win::Utf8ToWide(title, wtitle, ZX_ARRAY_COUNT(wtitle));
    win::Utf8ToWide(text, wtext, ZX_ARRAY_COUNT(wtext));
    ::MessageBoxW(nullptr, wtext, wtitle, MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);
}

void TerminateProcessNow(i32 exitCode)
{
    ::TerminateProcess(::GetCurrentProcess(), UINT(exitCode));
    std::_Exit(exitCode); // unreachable in practice
}

u32 CaptureStackTrace(void** frames, u32 maxFrames, u32 framesToSkip)
{
    return ::RtlCaptureStackBackTrace(framesToSkip + 1, maxFrames, frames, nullptr);
}

namespace {

SRWLOCK g_dbgHelpLock = SRWLOCK_INIT;
bool g_symInitialized = false;

void EnsureSymbolsInitialized()
{
    if (g_symInitialized)
        return;
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
    g_symInitialized = true;
}

} // namespace

usize SymbolizeAddress(const void* address, char* buffer, usize capacity)
{
    ::AcquireSRWLockExclusive(&g_dbgHelpLock);
    EnsureSymbolsInitialized();

    HANDLE process = ::GetCurrentProcess();
    const DWORD64 addr = DWORD64(reinterpret_cast<uptr>(address));

    alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;

    const char* name = "???";
    DWORD64 displacement = 0;
    if (::SymFromAddr(process, addr, &displacement, symbol))
        name = symbol->Name;

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp = 0;
    const bool hasLine = ::SymGetLineFromAddr64(process, addr, &lineDisp, &line) != FALSE;

    IMAGEHLP_MODULE64 module = {};
    module.SizeOfStruct = sizeof(module);
    const char* moduleName = ::SymGetModuleInfo64(process, addr, &module) ? module.ModuleName : "?";

    usize n;
    if (hasLine)
        n = FormatToBuffer(buffer, capacity, "{}!{} ({}:{})", moduleName, name, line.FileName, u32(line.LineNumber));
    else
        n = FormatToBuffer(buffer, capacity, "{}!{} + {:#x}", moduleName, name, u64(displacement));

    ::ReleaseSRWLockExclusive(&g_dbgHelpLock);
    return n;
}

usize FormatStackTrace(char* buffer, usize capacity, u32 framesToSkip)
{
    void* frames[64];
    const u32 count = CaptureStackTrace(frames, 64, framesToSkip + 1);
    FixedBufferSink sink(buffer, capacity);
    char line[1024];
    for (u32 i = 0; i < count; ++i) {
        const usize n = SymbolizeAddress(frames[i], line, sizeof(line));
        FormatTo(sink, "  [{:2}] {}\n", i, StringView(line, n));
    }
    return sink.Length();
}

const CpuInfo& GetCpuInfo()
{
    static const CpuInfo info = [] {
        CpuInfo ci;
        int regs[4] = {};
        __cpuid(regs, 0);
        const int maxLeaf = regs[0];
        if (maxLeaf >= 1) {
            __cpuid(regs, 1);
            ci.sse41 = (regs[2] & (1 << 19)) != 0;
            ci.sse42 = (regs[2] & (1 << 20)) != 0;
            ci.fma = (regs[2] & (1 << 12)) != 0;
            ci.f16c = (regs[2] & (1 << 29)) != 0;
            const bool osxsave = (regs[2] & (1 << 27)) != 0;
            const bool avxHw = (regs[2] & (1 << 28)) != 0;
            const bool osAvx = osxsave && ((_xgetbv(0) & 0x6) == 0x6);
            ci.avx = avxHw && osAvx;
            if (maxLeaf >= 7) {
                __cpuidex(regs, 7, 0);
                ci.avx2 = ci.avx && (regs[1] & (1 << 5)) != 0;
            }
        }
        __cpuid(regs, int(0x80000000));
        if (unsigned(regs[0]) >= 0x80000004u) {
            int brand[12];
            __cpuid(brand + 0, int(0x80000002));
            __cpuid(brand + 4, int(0x80000003));
            __cpuid(brand + 8, int(0x80000004));
            std::memcpy(ci.brand, brand, sizeof(brand));
            ci.brand[sizeof(ci.brand) - 1] = '\0';
            // Trim leading spaces some vendors put in the brand string.
            usize start = 0;
            while (ci.brand[start] == ' ')
                ++start;
            if (start > 0)
                std::memmove(ci.brand, ci.brand + start, std::strlen(ci.brand + start) + 1);
        }

        SYSTEM_INFO si = {};
        ::GetSystemInfo(&si);
        ci.logicalCores = si.dwNumberOfProcessors;

        DWORD len = 0;
        ::GetLogicalProcessorInformation(nullptr, &len);
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION procInfo[256];
        if (len > 0 && len <= sizeof(procInfo) && ::GetLogicalProcessorInformation(procInfo, &len)) {
            u32 physical = 0;
            for (DWORD i = 0; i < len / sizeof(procInfo[0]); ++i) {
                if (procInfo[i].Relationship == RelationProcessorCore)
                    ++physical;
                else if (procInfo[i].Relationship == RelationCache && procInfo[i].Cache.Level == 1)
                    ci.cacheLineSize = procInfo[i].Cache.LineSize;
            }
            if (physical > 0)
                ci.physicalCores = physical;
        } else {
            ci.physicalCores = ci.logicalCores;
        }
        return ci;
    }();
    return info;
}

MemoryInfo GetMemoryInfo()
{
    MemoryInfo mi;
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status)) {
        mi.totalPhysical = status.ullTotalPhys;
        mi.availablePhysical = status.ullAvailPhys;
    }
    SYSTEM_INFO si = {};
    ::GetSystemInfo(&si);
    mi.pageSize = si.dwPageSize;
    mi.allocationGranularity = si.dwAllocationGranularity;

    // K32GetProcessMemoryInfo lives in kernel32 on Win7+, no psapi.lib needed.
    struct ProcessMemoryCounters
    {
        DWORD cb;
        DWORD PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
    } pmc = {};
    pmc.cb = sizeof(pmc);
    using Fn = BOOL(WINAPI*)(HANDLE, void*, DWORD);
    static Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
    if (fn && fn(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
        mi.processWorkingSet = pmc.WorkingSetSize;
        mi.processPeakWorkingSet = pmc.PeakWorkingSetSize;
    }
    return mi;
}

namespace {

struct ExePaths
{
    char path[1024] = {};
    usize pathLen = 0;
    usize dirLen = 0;
    char args[8192] = {};
    usize argsLen = 0;
};

const ExePaths& GetExePaths()
{
    static const ExePaths paths = [] {
        ExePaths p;
        wchar_t wpath[1024];
        const DWORD n = ::GetModuleFileNameW(nullptr, wpath, ZX_ARRAY_COUNT(wpath));
        p.pathLen = win::WideToUtf8(wpath, n, p.path, sizeof(p.path));
        for (usize i = 0; i < p.pathLen; ++i)
            if (p.path[i] == '\\')
                p.path[i] = '/';
        p.dirLen = p.pathLen;
        while (p.dirLen > 0 && p.path[p.dirLen - 1] != '/')
            --p.dirLen;
        if (p.dirLen > 0)
            --p.dirLen;

        // Skip the executable token of the command line (quoted or not).
        const wchar_t* cmd = ::GetCommandLineW();
        if (*cmd == L'"') {
            ++cmd;
            while (*cmd && *cmd != L'"')
                ++cmd;
            if (*cmd)
                ++cmd;
        } else {
            while (*cmd && *cmd != L' ' && *cmd != L'\t')
                ++cmd;
        }
        while (*cmd == L' ' || *cmd == L'\t')
            ++cmd;
        p.argsLen = win::WideToUtf8(cmd, wcslen(cmd), p.args, sizeof(p.args));
        return p;
    }();
    return paths;
}

} // namespace

StringView GetExecutablePath() { return StringView(GetExePaths().path, GetExePaths().pathLen); }
StringView GetExecutableDirectory() { return StringView(GetExePaths().path, GetExePaths().dirLen); }
StringView GetCommandLineArgs() { return StringView(GetExePaths().args, GetExePaths().argsLen); }

} // namespace zx::platform
