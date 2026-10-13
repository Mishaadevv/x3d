#include "Core/Assert.h"
#include "Core/HAL/PlatformMisc.h"

#include <atomic>

namespace zx {

namespace {

std::atomic<AssertHandler> g_assertHandler{nullptr};
std::atomic<bool> g_inFatal{false};
std::atomic<bool> g_dialogsEnabled{true};
std::atomic<void (*)(StringView)> g_failureHook{nullptr};

void ReportFailure(const AssertInfo& info, const char* kind)
{
    char text[8192];
    FixedBufferSink sink(text, sizeof(text));
    FormatTo(sink, "{} at {}({})\n", kind, info.file, info.line);
    if (info.expression)
        FormatTo(sink, "  Expression: {}\n", info.expression);
    if (!info.message.Empty())
        FormatTo(sink, "  Message: {}\n", info.message);
    sink.Append("  Callstack:\n");

    char trace[6144];
    const usize n = platform::FormatStackTrace(trace, sizeof(trace), 2);
    sink.Append(trace, n);

    platform::DebugOutput(sink.View());
    platform::ConsoleWrite(sink.View(), true);
    detail::OnReportFailureHook(sink.View());

    if (g_dialogsEnabled.load() && !platform::IsDebuggerAttached())
        platform::ShowErrorMessageBox("ZeqouX3D - Fatal Error", sink.View());
}

} // namespace

namespace detail {

void SetFailureReportHook(void (*hook)(StringView)) { g_failureHook.store(hook); }

void OnReportFailureHook(StringView text)
{
    if (auto hook = g_failureHook.load())
        hook(text);
}

bool OnAssertFailed(const char* expr, const char* file, i32 line, StringView message)
{
    const AssertInfo info{expr, file, line, message};
    if (AssertHandler handler = g_assertHandler.load()) {
        switch (handler(info)) {
        case AssertAction::Continue:
            return false;
        case AssertAction::Abort:
            platform::TerminateProcessNow(3);
        case AssertAction::Break:
            break;
        }
    }

    // Recursive failure while reporting: bail out immediately.
    if (g_inFatal.exchange(true)) {
        if (platform::IsDebuggerAttached())
            return true;
        platform::TerminateProcessNow(3);
    }

    ReportFailure(info, "Assertion failed");
    if (platform::IsDebuggerAttached())
        return true; // caller breaks; continuing from the debugger is allowed
    platform::TerminateProcessNow(3);
}

void OnFatalError(const char* file, i32 line, StringView message)
{
    if (!g_inFatal.exchange(true)) {
        const AssertInfo info{nullptr, file, line, message};
        ReportFailure(info, "Fatal error");
    }
    if (platform::IsDebuggerAttached())
        ZX_DEBUGBREAK();
    platform::TerminateProcessNow(3);
}

} // namespace detail

AssertHandler SetAssertHandler(AssertHandler handler) { return g_assertHandler.exchange(handler); }

void SetErrorDialogsEnabled(bool enabled) { g_dialogsEnabled.store(enabled); }

} // namespace zx
