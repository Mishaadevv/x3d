#pragma once

#include "Core/Types.h"

#include <atomic>

namespace zx {

/// Non-recursive mutex (SRW lock). Cheap, no kernel object until contended.
class Mutex
{
public:
    Mutex() = default;
    ZX_NON_COPYABLE(Mutex)
    void Lock();
    bool TryLock();
    void Unlock();

private:
    friend class ConditionVariable;
    void* m_srw = nullptr; // SRWLOCK
};

/// Reader/writer lock (SRW lock).
class RWLock
{
public:
    RWLock() = default;
    ZX_NON_COPYABLE(RWLock)
    void LockShared();
    void UnlockShared();
    void LockExclusive();
    void UnlockExclusive();
    bool TryLockExclusive();

private:
    void* m_srw = nullptr;
};

/// Busy-wait lock for very short critical sections (uses pause + yield backoff).
class SpinLock
{
public:
    SpinLock() = default;
    ZX_NON_COPYABLE(SpinLock)
    void Lock();
    bool TryLock() { return !m_flag.exchange(true, std::memory_order_acquire); }
    void Unlock() { m_flag.store(false, std::memory_order_release); }

private:
    std::atomic<bool> m_flag{false};
};

template <typename L>
class ScopedLock
{
public:
    explicit ScopedLock(L& lock) : m_lock(lock) { m_lock.Lock(); }
    ~ScopedLock() { m_lock.Unlock(); }
    ZX_NON_COPYABLE(ScopedLock)

private:
    L& m_lock;
};

class ScopedReadLock
{
public:
    explicit ScopedReadLock(RWLock& l) : m_lock(l) { m_lock.LockShared(); }
    ~ScopedReadLock() { m_lock.UnlockShared(); }
    ZX_NON_COPYABLE(ScopedReadLock)

private:
    RWLock& m_lock;
};

class ScopedWriteLock
{
public:
    explicit ScopedWriteLock(RWLock& l) : m_lock(l) { m_lock.LockExclusive(); }
    ~ScopedWriteLock() { m_lock.UnlockExclusive(); }
    ZX_NON_COPYABLE(ScopedWriteLock)

private:
    RWLock& m_lock;
};

#define ZX_SCOPED_LOCK(lock) ::zx::ScopedLock ZX_UNIQUE_NAME(zxScopedLock_)(lock)

class ConditionVariable
{
public:
    ConditionVariable() = default;
    ZX_NON_COPYABLE(ConditionVariable)
    /// Atomically unlocks `mutex` and waits. Returns false on timeout. Spurious wakeups are possible.
    bool Wait(Mutex& mutex, u32 timeoutMs = U32Max);
    void NotifyOne();
    void NotifyAll();

private:
    void* m_cv = nullptr; // CONDITION_VARIABLE
};

/// Counting semaphore.
class Semaphore
{
public:
    explicit Semaphore(u32 initialCount = 0, u32 maxCount = 0x7FFFFFFF);
    ~Semaphore();
    ZX_NON_COPYABLE(Semaphore)
    void Release(u32 count = 1);
    /// Returns false on timeout.
    bool Acquire(u32 timeoutMs = U32Max);

private:
    void* m_handle;
};

/// Manual- or auto-reset event.
class Event
{
public:
    explicit Event(bool manualReset = false, bool initialState = false);
    ~Event();
    ZX_NON_COPYABLE(Event)
    void Signal();
    void Reset();
    /// Returns false on timeout.
    bool Wait(u32 timeoutMs = U32Max);

private:
    void* m_handle;
};

/// Wait/wake on an address (WaitOnAddress). Efficient futex-like primitive.
void AtomicWait(const std::atomic<u32>& address, u32 undesiredValue, u32 timeoutMs = U32Max);
void AtomicWakeOne(std::atomic<u32>& address);
void AtomicWakeAll(std::atomic<u32>& address);

/// CPU pause hint for spin loops.
void CpuPause();

} // namespace zx
