#pragma once

// ZX_ASSERT(cond [, fmt, args...])   - checked in Debug/Development, compiled out in Shipping (cond NOT evaluated).
// ZX_VERIFY(cond [, fmt, args...])   - like ZX_ASSERT but cond is always evaluated.
// ZX_CHECK_SLOW(cond [, fmt, ...])   - Debug only (expensive checks).
// ZX_FATAL(fmt, args...)             - always terminates (all configurations).
// ZX_UNREACHABLE()                   - asserts + optimizer hint.
// A failed assert is fatal unless a handler returns AssertAction::Continue (used by tests).

#include "Core/String/Format.h"

namespace zx {

enum class AssertAction : u8
{
    Break,    // debugger break (if attached) then terminate
    Continue, // ignore and continue execution
    Abort     // terminate immediately
};

struct AssertInfo
{
    const char* expression; // may be null for ZX_FATAL
    const char* file;
    i32 line;
    StringView message;
};

using AssertHandler = AssertAction (*)(const AssertInfo& info);

/// Installs a handler called on assertion failure; returns the previous handler. nullptr = default.
AssertHandler SetAssertHandler(AssertHandler handler);

/// Enables/disables modal error dialogs on fatal errors (disable for unattended runs: tests, CI, cooking).
void SetErrorDialogsEnabled(bool enabled);

namespace detail {

/// No-op used by compiled-out asserts so that variables referenced only in asserts don't trigger "unused" warnings.
template <typename... Args>
constexpr void IgnoreAssertArgs(const Args&...)
{
}

/// Hook invoked with the full failure report (used by the log system to persist it). Not for general use.
void SetFailureReportHook(void (*hook)(StringView report));
void OnReportFailureHook(StringView report);

bool OnAssertFailed(const char* expr, const char* file, i32 line, StringView message);
ZX_NORETURN void OnFatalError(const char* file, i32 line, StringView message);

ZX_NOINLINE inline bool AssertFailedF(const char* expr, const char* file, i32 line)
{
    return OnAssertFailed(expr, file, line, StringView());
}

template <typename... Args>
ZX_NOINLINE bool AssertFailedF(const char* expr, const char* file, i32 line, StringView fmt, const Args&... args)
{
    char buffer[2048];
    const usize n = FormatToBuffer(buffer, sizeof(buffer), fmt, args...);
    return OnAssertFailed(expr, file, line, StringView(buffer, n));
}

template <typename... Args>
ZX_NORETURN ZX_NOINLINE void FatalF(const char* file, i32 line, StringView fmt, const Args&... args)
{
    char buffer[2048];
    const usize n = FormatToBuffer(buffer, sizeof(buffer), fmt, args...);
    OnFatalError(file, line, StringView(buffer, n));
}

} // namespace detail
} // namespace zx

#define ZX_FATAL(fmt, ...) ::zx::detail::FatalF(__FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

#if ZX_ENABLE_ASSERTS
    #define ZX_ASSERT(cond, ...)                                                                                       \
        do {                                                                                                           \
            if (!(cond)) [[unlikely]] {                                                                                \
                if (::zx::detail::AssertFailedF(#cond, __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__))                 \
                    ZX_DEBUGBREAK();                                                                                   \
            }                                                                                                          \
        } while (0)
    #define ZX_VERIFY(cond, ...) ZX_ASSERT(cond __VA_OPT__(, ) __VA_ARGS__)
    #define ZX_UNREACHABLE()                                                                                           \
        do {                                                                                                           \
            ZX_ASSERT(false, "Unreachable code reached");                                                              \
            ZX_ASSUME(0);                                                                                              \
        } while (0)
#else
    #define ZX_ASSERT(cond, ...)                                                                                       \
        do {                                                                                                           \
            (void)[&]() {                                                                                              \
                (void)(cond);                                                                                          \
                ::zx::detail::IgnoreAssertArgs(__VA_ARGS__);                                                           \
            };                                                                                                         \
        } while (0)
    #define ZX_VERIFY(cond, ...)                                                                                       \
        do {                                                                                                           \
            (void)(cond);                                                                                              \
        } while (0)
    #define ZX_UNREACHABLE() ZX_ASSUME(0)
#endif

#if ZX_ENABLE_SLOW_CHECKS
    #define ZX_CHECK_SLOW(cond, ...) ZX_ASSERT(cond __VA_OPT__(, ) __VA_ARGS__)
#else
    #define ZX_CHECK_SLOW(cond, ...)                                                                                   \
        do {                                                                                                           \
            (void)[&]() {                                                                                              \
                (void)(cond);                                                                                          \
                ::zx::detail::IgnoreAssertArgs(__VA_ARGS__);                                                           \
            };                                                                                                         \
        } while (0)
#endif
