#include "Core/Log/Log.h"
#include "Core/Assert.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"

ZX_DEFINE_LOG_CATEGORY(LogCore, Info);

namespace zx {

namespace {

// Constant-initialized so categories can register from static constructors in any order.
constinit LogCategory* g_categoryHead = nullptr;

struct LogState
{
    Mutex mutex;
    Array<UniquePtr<LogSink>> sinks;
    bool initialized = false;
};

LogState& State()
{
    static LogState s;
    return s;
}

thread_local bool t_inLog = false;

constexpr const char* LevelNames[] = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal", "Off"};

void FormatLine(FixedBufferSink& sink, const LogMessage& m, bool includeTime)
{
    if (includeTime)
        FormatTo(sink, "[{:9.3f}] ", m.timeSeconds);
    if (m.level == LogLevel::Info)
        FormatTo(sink, "{}: {}\n", m.category->name, m.text);
    else
        FormatTo(sink, "{}: {}: {}\n", m.category->name, LogLevelName(m.level), m.text);
}

class DebugOutputSink final : public LogSink
{
public:
    void Write(const LogMessage& m) override
    {
        char buffer[4096];
        FixedBufferSink sink(buffer, sizeof(buffer));
        FormatLine(sink, m, true);
        platform::DebugOutput(sink.View());
    }
};

class ConsoleSink final : public LogSink
{
public:
    explicit ConsoleSink(LogLevel minLevel) : m_minLevel(minLevel) {}
    void Write(const LogMessage& m) override
    {
        if (m.level < m_minLevel)
            return;
        char buffer[4096];
        FixedBufferSink sink(buffer, sizeof(buffer));
        const char* color = "";
        switch (m.level) {
        case LogLevel::Trace:
        case LogLevel::Debug:
            color = "\x1b[90m";
            break;
        case LogLevel::Warning:
            color = "\x1b[33m";
            break;
        case LogLevel::Error:
        case LogLevel::Fatal:
            color = "\x1b[31m";
            break;
        default:
            break;
        }
        sink.Append(color);
        FormatLine(sink, m, false);
        if (*color) {
            // Put the reset code before the trailing newline.
            sink.Append("\x1b[0m");
        }
        platform::ConsoleWrite(sink.View(), m.level >= LogLevel::Error);
    }

private:
    LogLevel m_minLevel;
};

void OnFailureReport(StringView report)
{
    // Persist assert/crash reports in the log file, then flush everything.
    LogState& s = State();
    if (!s.mutex.TryLock())
        return; // a sink is mid-write on this or another thread; don't deadlock in a crash path
    for (auto& sink : s.sinks) {
        LogMessage m{&LogCore, LogLevel::Fatal, report, Clock::NowSeconds(), Thread::CurrentId(), "", 0};
        if (sink->WantsFailureReports())
            sink->Write(m);
        sink->Flush();
    }
    s.mutex.Unlock();
}

} // namespace

const char* LogLevelName(LogLevel level) { return LevelNames[u32(level)]; }

LogCategory::LogCategory(const char* categoryName, LogLevel defaultVerbosity)
    : name(categoryName), verbosity(defaultVerbosity), next(g_categoryHead)
{
    g_categoryHead = this;
}

namespace Log {

void Initialize(StringView logFilePath, LogLevel consoleMinLevel)
{
    LogState& s = State();
    ScopedLock<Mutex> guard(s.mutex);
    if (s.initialized)
        return;
    s.sinks.PushBack(MakeUnique<DebugOutputSink>());
    s.sinks.PushBack(MakeUnique<ConsoleSink>(consoleMinLevel));
    if (!logFilePath.Empty()) {
        auto file = MakeUnique<FileLogSink>(logFilePath);
        if (file->IsOpen())
            s.sinks.PushBack(move(file));
    }
    s.initialized = true;
    detail::SetFailureReportHook(&OnFailureReport);
}

void Shutdown()
{
    LogState& s = State();
    detail::SetFailureReportHook(nullptr);
    ScopedLock<Mutex> guard(s.mutex);
    for (auto& sink : s.sinks)
        sink->Flush();
    s.sinks.Reset();
    s.initialized = false;
}

void AddSink(UniquePtr<LogSink> sink)
{
    LogState& s = State();
    ScopedLock<Mutex> guard(s.mutex);
    LeakIgnoreScope permanent; // the sink list buffer lives as long as the process
    s.sinks.PushBack(move(sink));
}

void RemoveSink(LogSink* sink)
{
    LogState& s = State();
    ScopedLock<Mutex> guard(s.mutex);
    s.sinks.RemoveIf([sink](const UniquePtr<LogSink>& p) { return p.Get() == sink; });
}

void Flush()
{
    LogState& s = State();
    ScopedLock<Mutex> guard(s.mutex);
    for (auto& sink : s.sinks)
        sink->Flush();
}

LogCategory* GetCategories() { return g_categoryHead; }

LogCategory* FindCategory(StringView name)
{
    for (LogCategory* c = g_categoryHead; c; c = c->next)
        if (name.EqualsIgnoreCase(c->name))
            return c;
    return nullptr;
}

bool SetVerbosity(StringView categoryName, LogLevel level)
{
    if (categoryName == "*") {
        for (LogCategory* c = g_categoryHead; c; c = c->next)
            c->verbosity = level;
        return true;
    }
    LogCategory* c = FindCategory(categoryName);
    if (!c)
        return false;
    c->verbosity = level;
    return true;
}

void Write(const LogCategory& category, LogLevel level, const char* file, i32 line, StringView text)
{
    if (t_inLog)
        return; // sink logged recursively: drop
    LogState& s = State();
    const LogMessage m{&category, level, text, Clock::NowSeconds(), Thread::CurrentId(), file, line};
    t_inLog = true;
    {
        ScopedLock<Mutex> guard(s.mutex);
        if (!s.initialized) {
            // Before Initialize: write straight to the debugger so nothing is lost.
            char buffer[2048];
            FixedBufferSink sink(buffer, sizeof(buffer));
            FormatLine(sink, m, false);
            platform::DebugOutput(sink.View());
        }
        for (auto& sink : s.sinks)
            sink->Write(m);
    }
    t_inLog = false;
}

} // namespace Log

// --- RingBufferLogSink ----------------------------------------------------------------------------------------------

struct RingBufferLogSink::Impl
{
    mutable Mutex mutex;
    Array<Entry> entries;
    usize capacity;
    usize head = 0; // index of oldest when full
    u64 sequence = 0;
};

RingBufferLogSink::RingBufferLogSink(usize capacity) : m_impl(MakeUnique<Impl>())
{
    m_impl->capacity = capacity > 0 ? capacity : 1;
    m_impl->entries.Reserve(m_impl->capacity);
}

RingBufferLogSink::~RingBufferLogSink() = default;

void RingBufferLogSink::Write(const LogMessage& m)
{
    ScopedLock<Mutex> guard(m_impl->mutex);
    Entry e{String(m.text), m.category, m.level, m.timeSeconds, ++m_impl->sequence};
    if (m_impl->entries.Size() < m_impl->capacity) {
        m_impl->entries.PushBack(move(e));
    } else {
        m_impl->entries[m_impl->head] = move(e);
        m_impl->head = (m_impl->head + 1) % m_impl->capacity;
    }
}

u64 RingBufferLogSink::CopyEntries(Array<Entry>& out, u64 afterSequence) const
{
    ScopedLock<Mutex> guard(m_impl->mutex);
    const usize n = m_impl->entries.Size();
    for (usize i = 0; i < n; ++i) {
        const Entry& e = m_impl->entries[(m_impl->head + i) % n];
        if (e.sequence > afterSequence)
            out.PushBack(e);
    }
    return m_impl->sequence;
}

void RingBufferLogSink::Clear()
{
    ScopedLock<Mutex> guard(m_impl->mutex);
    m_impl->entries.Clear();
    m_impl->head = 0;
}

// --- FileLogSink ----------------------------------------------------------------------------------------------------

struct FileLogSink::Impl
{
    platform::File file;
    Array<char> buffer;
};

FileLogSink::FileLogSink(StringView path) : m_impl(MakeUnique<Impl>())
{
    const usize slash = path.FindLastOf("/\\");
    if (slash != StringView::NPos)
        platform::CreateDirectoryTree(path.Left(slash));
    if (m_impl->file.Open(path, platform::FileMode::Write)) {
        static const u8 bom[] = {0xEF, 0xBB, 0xBF};
        m_impl->file.Write(bom, sizeof(bom));
        const String header = String::Format("Log started {}\n", DateTime::Now().ToString());
        m_impl->file.Write(header.Data(), header.Size());
    }
}

FileLogSink::~FileLogSink() { Flush(); }

bool FileLogSink::IsOpen() const { return m_impl->file.IsOpen(); }

void FileLogSink::Write(const LogMessage& m)
{
    if (!m_impl->file.IsOpen())
        return;
    char line[4096];
    FixedBufferSink sink(line, sizeof(line));
    FormatTo(sink, "[{:9.3f}][{:5}] ", m.timeSeconds, m.threadId);
    FormatLine(sink, m, false);
    m_impl->buffer.Append(Span<const char>(line, sink.Length()));
    if (m.level >= LogLevel::Warning || m_impl->buffer.Size() > 16 * 1024)
        Flush();
}

void FileLogSink::Flush()
{
    if (!m_impl->buffer.Empty() && m_impl->file.IsOpen()) {
        m_impl->file.Write(m_impl->buffer.Data(), m_impl->buffer.Size());
        m_impl->buffer.Clear();
    }
}

} // namespace zx
