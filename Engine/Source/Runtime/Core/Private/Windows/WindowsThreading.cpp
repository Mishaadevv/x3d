#include "Core/Assert.h"
#include "Core/Memory/Memory.h"
#include "Core/String/Utf.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"

#include "Windows/WindowsInclude.h"

#include <immintrin.h>
#include <process.h>

#pragma comment(lib, "synchronization.lib") // WaitOnAddress

namespace zx {

static_assert(sizeof(SRWLOCK) == sizeof(void*));
static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void*));

#define ZX_SRW(p) reinterpret_cast<PSRWLOCK>(&(p))

// --- Mutex / RWLock -------------------------------------------------------------------------------------------------

void Mutex::Lock() { ::AcquireSRWLockExclusive(ZX_SRW(m_srw)); }
bool Mutex::TryLock() { return ::TryAcquireSRWLockExclusive(ZX_SRW(m_srw)) != 0; }
void Mutex::Unlock() { ::ReleaseSRWLockExclusive(ZX_SRW(m_srw)); }

void RWLock::LockShared() { ::AcquireSRWLockShared(ZX_SRW(m_srw)); }
void RWLock::UnlockShared() { ::ReleaseSRWLockShared(ZX_SRW(m_srw)); }
void RWLock::LockExclusive() { ::AcquireSRWLockExclusive(ZX_SRW(m_srw)); }
void RWLock::UnlockExclusive() { ::ReleaseSRWLockExclusive(ZX_SRW(m_srw)); }
bool RWLock::TryLockExclusive() { return ::TryAcquireSRWLockExclusive(ZX_SRW(m_srw)) != 0; }

void CpuPause() { _mm_pause(); }

void SpinLock::Lock()
{
    u32 spins = 0;
    for (;;) {
        if (!m_flag.exchange(true, std::memory_order_acquire))
            return;
        while (m_flag.load(std::memory_order_relaxed)) {
            if (++spins < 64)
                _mm_pause();
            else
                ::SwitchToThread();
        }
    }
}

// --- ConditionVariable ----------------------------------------------------------------------------------------------

bool ConditionVariable::Wait(Mutex& mutex, u32 timeoutMs)
{
    return ::SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(&m_cv), ZX_SRW(mutex.m_srw),
                                       timeoutMs == U32Max ? INFINITE : timeoutMs, 0) != 0;
}
void ConditionVariable::NotifyOne() { ::WakeConditionVariable(reinterpret_cast<PCONDITION_VARIABLE>(&m_cv)); }
void ConditionVariable::NotifyAll() { ::WakeAllConditionVariable(reinterpret_cast<PCONDITION_VARIABLE>(&m_cv)); }

// --- Semaphore / Event ----------------------------------------------------------------------------------------------

Semaphore::Semaphore(u32 initialCount, u32 maxCount)
{
    m_handle = ::CreateSemaphoreW(nullptr, LONG(initialCount), LONG(maxCount), nullptr);
    ZX_ASSERT(m_handle, "CreateSemaphore failed");
}
Semaphore::~Semaphore() { ::CloseHandle(m_handle); }
void Semaphore::Release(u32 count) { ::ReleaseSemaphore(m_handle, LONG(count), nullptr); }
bool Semaphore::Acquire(u32 timeoutMs)
{
    return ::WaitForSingleObject(m_handle, timeoutMs == U32Max ? INFINITE : timeoutMs) == WAIT_OBJECT_0;
}

Event::Event(bool manualReset, bool initialState)
{
    m_handle = ::CreateEventW(nullptr, manualReset, initialState, nullptr);
    ZX_ASSERT(m_handle, "CreateEvent failed");
}
Event::~Event() { ::CloseHandle(m_handle); }
void Event::Signal() { ::SetEvent(m_handle); }
void Event::Reset() { ::ResetEvent(m_handle); }
bool Event::Wait(u32 timeoutMs)
{
    return ::WaitForSingleObject(m_handle, timeoutMs == U32Max ? INFINITE : timeoutMs) == WAIT_OBJECT_0;
}

void AtomicWait(const std::atomic<u32>& address, u32 undesiredValue, u32 timeoutMs)
{
    ::WaitOnAddress(const_cast<std::atomic<u32>*>(&address), &undesiredValue, sizeof(u32),
                    timeoutMs == U32Max ? INFINITE : timeoutMs);
}
void AtomicWakeOne(std::atomic<u32>& address) { ::WakeByAddressSingle(&address); }
void AtomicWakeAll(std::atomic<u32>& address) { ::WakeByAddressAll(&address); }

// --- Thread ---------------------------------------------------------------------------------------------------------

namespace {
thread_local bool t_isMainThread = false;

int ToWinPriority(ThreadPriority p)
{
    switch (p) {
    case ThreadPriority::Lowest:
        return THREAD_PRIORITY_LOWEST;
    case ThreadPriority::BelowNormal:
        return THREAD_PRIORITY_BELOW_NORMAL;
    case ThreadPriority::Normal:
        return THREAD_PRIORITY_NORMAL;
    case ThreadPriority::AboveNormal:
        return THREAD_PRIORITY_ABOVE_NORMAL;
    case ThreadPriority::Highest:
        return THREAD_PRIORITY_HIGHEST;
    case ThreadPriority::TimeCritical:
        return THREAD_PRIORITY_TIME_CRITICAL;
    }
    return THREAD_PRIORITY_NORMAL;
}
} // namespace

struct Thread::StartData
{
    Function<void()> entry;
    String name;
};

static unsigned __stdcall ThreadTrampoline(void* param)
{
    auto* data = static_cast<Thread::StartData*>(param);
    Thread::SetCurrentName(data->name);
    data->entry();
    Delete(data);
    return 0;
}

Thread::~Thread() { ZX_ASSERT(m_handle == nullptr, "Thread '{}' destroyed without Join/Detach", m_name); }

Thread::Thread(Thread&& o) noexcept : m_handle(o.m_handle), m_id(o.m_id), m_name(move(o.m_name))
{
    o.m_handle = nullptr;
    o.m_id = 0;
}

Thread& Thread::operator=(Thread&& o) noexcept
{
    if (this != &o) {
        ZX_ASSERT(m_handle == nullptr, "Thread move-assigned while running");
        m_handle = o.m_handle;
        m_id = o.m_id;
        m_name = move(o.m_name);
        o.m_handle = nullptr;
        o.m_id = 0;
    }
    return *this;
}

bool Thread::Start(StringView name, Function<void()> entry, ThreadPriority priority, usize stackSize)
{
    ZX_ASSERT(m_handle == nullptr, "Thread already started");
    m_name = name;
    StartData* data = New<StartData>(StartData{move(entry), String(name)});
    unsigned id = 0;
    const uptr h = _beginthreadex(nullptr, unsigned(stackSize), &ThreadTrampoline, data, CREATE_SUSPENDED, &id);
    if (h == 0) {
        Delete(data);
        return false;
    }
    m_handle = reinterpret_cast<void*>(h);
    m_id = id;
    if (priority != ThreadPriority::Normal)
        ::SetThreadPriority(m_handle, ToWinPriority(priority));
    ::ResumeThread(m_handle);
    return true;
}

void Thread::Join()
{
    if (!m_handle)
        return;
    ::WaitForSingleObject(m_handle, INFINITE);
    ::CloseHandle(m_handle);
    m_handle = nullptr;
}

void Thread::Detach()
{
    if (!m_handle)
        return;
    ::CloseHandle(m_handle);
    m_handle = nullptr;
}

void Thread::SetPriority(ThreadPriority priority)
{
    if (m_handle)
        ::SetThreadPriority(m_handle, ToWinPriority(priority));
}

void Thread::SetAffinityMask(u64 mask)
{
    if (m_handle)
        ::SetThreadAffinityMask(m_handle, DWORD_PTR(mask));
}

u32 Thread::CurrentId() { return ::GetCurrentThreadId(); }

void Thread::SetCurrentName(StringView name)
{
    const Array<wchar_t> wide = Utf8ToUtf16(name);
    ::SetThreadDescription(::GetCurrentThread(), wide.Data());
}

void Thread::SleepMs(u32 ms) { ::Sleep(ms); }
void Thread::Yield() { ::SwitchToThread(); }

u32 Thread::HardwareConcurrency()
{
    SYSTEM_INFO si = {};
    ::GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

void Thread::MarkMainThread() { t_isMainThread = true; }
bool Thread::IsMainThread() { return t_isMainThread; }

} // namespace zx
