#pragma once

#include "Core/Containers/Function.h"
#include "Core/String/String.h"

namespace zx {

enum class ThreadPriority : u8
{
    Lowest,
    BelowNormal,
    Normal,
    AboveNormal,
    Highest,
    TimeCritical
};

/// OS thread. Must be joined (or detached) before destruction.
class Thread
{
public:
    Thread() = default;
    ~Thread();
    Thread(Thread&& o) noexcept;
    Thread& operator=(Thread&& o) noexcept;
    ZX_NON_COPYABLE(Thread)

    /// Starts the thread. stackSize 0 = default (1 MB).
    bool Start(StringView name, Function<void()> entry, ThreadPriority priority = ThreadPriority::Normal,
               usize stackSize = 0);
    void Join();
    void Detach();
    bool IsJoinable() const { return m_handle != nullptr; }
    u32 GetId() const { return m_id; }
    const String& GetName() const { return m_name; }
    void SetPriority(ThreadPriority priority);
    /// Restricts the thread to the given logical cores (bit mask).
    void SetAffinityMask(u64 mask);

    static u32 CurrentId();
    static void SetCurrentName(StringView name);
    static void SleepMs(u32 ms);
    /// Yields the rest of the time slice.
    static void Yield();
    static u32 HardwareConcurrency();

    /// Marks the calling thread as the main thread (call once at startup).
    static void MarkMainThread();
    static bool IsMainThread();

    struct StartData; // internal

private:
    void* m_handle = nullptr;
    u32 m_id = 0;
    String m_name;
};

} // namespace zx
