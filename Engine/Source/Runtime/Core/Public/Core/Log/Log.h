#pragma once

// Logging.
//
//   // Header (optional, for categories shared between files):
//   ZX_DECLARE_LOG_CATEGORY(LogRender);
//   // One .cpp:
//   ZX_DEFINE_LOG_CATEGORY(LogRender, Info);
//   // Use:
//   ZX_LOG_INFO(LogRender, "Created swapchain {}x{}", w, h);
//   ZX_LOG(LogRender, Warning, "Fallback to {}", name);
//
// Messages below the category's verbosity are discarded before formatting.
// Sinks receive messages under a lock (in order); they must not log recursively.

#include "Core/Containers/Array.h"
#include "Core/Memory/Pointers.h"
#include "Core/String/String.h"

namespace zx {

enum class LogLevel : u8
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
    Off
};

const char* LogLevelName(LogLevel level);

struct LogCategory
{
    const char* name;
    LogLevel verbosity;
    LogCategory* next; // intrusive registry list

    LogCategory(const char* categoryName, LogLevel defaultVerbosity);
    bool IsEnabled(LogLevel level) const { return level >= verbosity; }
};

struct LogMessage
{
    const LogCategory* category;
    LogLevel level;
    StringView text;
    f64 timeSeconds; // since process start
    u32 threadId;
    const char* file;
    i32 line;
};

class LogSink
{
public:
    virtual ~LogSink() = default;
    virtual void Write(const LogMessage& message) = 0;
    virtual void Flush() {}
    /// Sinks returning true also receive assert/crash reports (as Fatal messages).
    virtual bool WantsFailureReports() const { return false; }
};

namespace Log {

/// Initializes default sinks (debugger output + console). Optional file path enables the file sink.
/// `consoleMinLevel` filters what is printed to the console (other sinks receive everything).
void Initialize(StringView logFilePath = {}, LogLevel consoleMinLevel = LogLevel::Trace);
void Shutdown();

void AddSink(UniquePtr<LogSink> sink);
/// Removes and destroys a sink previously added (pointer identity).
void RemoveSink(LogSink* sink);
void Flush();

/// Iterates registered categories.
LogCategory* GetCategories();
LogCategory* FindCategory(StringView name);
/// Sets verbosity for a category by name ("*" = all). Returns false if not found.
bool SetVerbosity(StringView categoryName, LogLevel level);

void Write(const LogCategory& category, LogLevel level, const char* file, i32 line, StringView text);

template <typename... Args>
ZX_NOINLINE void WriteF(const LogCategory& category, LogLevel level, const char* file, i32 line, StringView fmt,
                        const Args&... args)
{
    char stackBuffer[1024];
    FixedBufferSink sink(stackBuffer, sizeof(stackBuffer));
    FormatTo(sink, fmt, args...);
    if (!sink.Truncated()) {
        Write(category, level, file, line, sink.View());
        return;
    }
    const String big = String::Format(fmt, args...);
    Write(category, level, file, line, big.View());
}

} // namespace Log

/// Keeps the last N messages in memory (for the editor console). Thread-safe snapshotting.
class RingBufferLogSink final : public LogSink
{
public:
    struct Entry
    {
        String text;
        const LogCategory* category;
        LogLevel level;
        f64 timeSeconds;
        u64 sequence;
    };

    explicit RingBufferLogSink(usize capacity = 4096);
    ~RingBufferLogSink() override;
    void Write(const LogMessage& message) override;
    /// Copies entries with sequence > afterSequence. Returns the newest sequence.
    u64 CopyEntries(Array<Entry>& out, u64 afterSequence = 0) const;
    void Clear();

private:
    struct Impl;
    UniquePtr<Impl> m_impl;
};

/// Appends to a UTF-8 text file. Flushes on Warning and above.
class FileLogSink final : public LogSink
{
public:
    explicit FileLogSink(StringView path);
    ~FileLogSink() override;
    bool IsOpen() const;
    void Write(const LogMessage& message) override;
    void Flush() override;
    bool WantsFailureReports() const override { return true; }

private:
    struct Impl;
    UniquePtr<Impl> m_impl;
};

} // namespace zx

#define ZX_DECLARE_LOG_CATEGORY(Name) extern ::zx::LogCategory Name
#define ZX_DEFINE_LOG_CATEGORY(Name, DefaultLevel) ::zx::LogCategory Name(#Name, ::zx::LogLevel::DefaultLevel)
/// File-local category.
#define ZX_DEFINE_LOG_CATEGORY_STATIC(Name, DefaultLevel)                                                              \
    static ::zx::LogCategory Name(#Name, ::zx::LogLevel::DefaultLevel)

#define ZX_LOG(Category, Level, fmt, ...)                                                                              \
    do {                                                                                                               \
        if ((Category).IsEnabled(::zx::LogLevel::Level))                                                               \
            ::zx::Log::WriteF(Category, ::zx::LogLevel::Level, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__);    \
    } while (0)

#if ZX_ENABLE_LOG_VERBOSE
    #define ZX_LOG_TRACE(Category, fmt, ...) ZX_LOG(Category, Trace, fmt __VA_OPT__(, ) __VA_ARGS__)
    #define ZX_LOG_DEBUG(Category, fmt, ...) ZX_LOG(Category, Debug, fmt __VA_OPT__(, ) __VA_ARGS__)
#else
    #define ZX_LOG_TRACE(Category, fmt, ...)                                                                           \
        do {                                                                                                           \
        } while (0)
    #define ZX_LOG_DEBUG(Category, fmt, ...)                                                                           \
        do {                                                                                                           \
        } while (0)
#endif
#define ZX_LOG_INFO(Category, fmt, ...) ZX_LOG(Category, Info, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ZX_LOG_WARN(Category, fmt, ...) ZX_LOG(Category, Warning, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ZX_LOG_ERROR(Category, fmt, ...) ZX_LOG(Category, Error, fmt __VA_OPT__(, ) __VA_ARGS__)

/// Core engine category.
ZX_DECLARE_LOG_CATEGORY(LogCore);
