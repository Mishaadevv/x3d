#include "Core/Testing/Test.h"
#include "Core/Assert.h"
#include "Core/HAL/PlatformMisc.h"

#include <chrono>

namespace zx::test {

namespace {

TestCase* g_head = nullptr;
TestCase* g_tail = nullptr;
i32 g_currentFailures = 0;
i32 g_expectAssertDepth = 0;
i32 g_assertsCaught = 0;
AssertHandler g_prevHandler = nullptr;

void Print(StringView s) { platform::ConsoleWrite(s); }

template <typename... Args>
void Printf(StringView fmt, const Args&... args)
{
    char buffer[2048];
    const usize n = FormatToBuffer(buffer, sizeof(buffer), fmt, args...);
    Print(StringView(buffer, n));
}

// Simple glob: '*' matches any sequence. Pattern applies to "Suite.Name".
bool GlobMatch(StringView pattern, StringView text)
{
    usize p = 0, t = 0, starP = StringView::NPos, starT = 0;
    while (t < text.Size()) {
        if (p < pattern.Size() && (pattern[p] == text[t] || pattern[p] == '?')) {
            ++p;
            ++t;
        } else if (p < pattern.Size() && pattern[p] == '*') {
            starP = p++;
            starT = t;
        } else if (starP != StringView::NPos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.Size() && pattern[p] == '*')
        ++p;
    return p == pattern.Size();
}

AssertAction TestAssertHandler(const AssertInfo& info)
{
    if (g_expectAssertDepth > 0) {
        ++g_assertsCaught;
        return AssertAction::Continue;
    }
    // Unexpected assert inside a test: record as failure and keep going so the run completes.
    char buffer[1024];
    const usize n = FormatToBuffer(buffer, sizeof(buffer), "Unexpected assertion: {} {}",
                                   info.expression ? info.expression : "", info.message);
    ReportFailure(info.file, info.line, StringView(buffer, n));
    return AssertAction::Continue;
}

} // namespace

int RegisterTest(TestCase* test)
{
    test->next = nullptr;
    if (g_tail)
        g_tail->next = test;
    else
        g_head = test;
    g_tail = test;
    return 0;
}

TestCase* GetRegisteredTests() { return g_head; }

void ReportFailure(const char* file, i32 line, StringView message)
{
    ++g_currentFailures;
    Printf("    \x1b[31mFAIL\x1b[0m {}({}): {}\n", file, line, message);
}

bool CurrentTestFailed() { return g_currentFailures > 0; }

void BeginExpectAssert()
{
    ++g_expectAssertDepth;
    g_assertsCaught = 0;
}

bool EndExpectAssert()
{
    --g_expectAssertDepth;
    return g_assertsCaught > 0;
}

i32 RunTests(const RunOptions& options)
{
    g_prevHandler = SetAssertHandler(&TestAssertHandler);

    i32 total = 0, failed = 0;
    const auto start = std::chrono::steady_clock::now();
    char fullName[256];

    for (TestCase* t = g_head; t; t = t->next) {
        const usize n = FormatToBuffer(fullName, sizeof(fullName), "{}.{}", t->suite, t->name);
        const StringView name(fullName, n);
        if (!options.filter.Empty() && !GlobMatch(options.filter, name))
            continue;
        // Suites named "Manual*" only run when explicitly selected by a filter.
        if (StringView(t->suite).StartsWith("Manual") && !options.filter.StartsWith("Manual"))
            continue;
        if (options.listOnly) {
            Printf("{}\n", name);
            continue;
        }
        ++total;
        g_currentFailures = 0;
        if (options.verbose)
            Printf("[ RUN  ] {}\n", name);
        const auto t0 = std::chrono::steady_clock::now();
        t->fn();
        const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (g_currentFailures > 0) {
            ++failed;
            Printf("[\x1b[31mFAILED\x1b[0m] {} ({:.2f} ms)\n", name, ms);
        } else if (options.verbose) {
            Printf("[  \x1b[32mOK\x1b[0m  ] {} ({:.2f} ms)\n", name, ms);
        }
    }

    SetAssertHandler(g_prevHandler);
    if (options.listOnly)
        return 0;

    const f64 totalMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (failed == 0)
        Printf("\x1b[32m{} tests passed\x1b[0m ({:.1f} ms)\n", total, totalMs);
    else
        Printf("\x1b[31m{} of {} tests FAILED\x1b[0m ({:.1f} ms)\n", failed, total, totalMs);
    return failed;
}

} // namespace zx::test
