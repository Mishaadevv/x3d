#pragma once

#include "Core/Types.h"

#include <cstring>

namespace zx {

/// Category used for memory statistics. The current thread tag (MemTagScope) applies to all
/// allocations including global operator new.
enum class MemTag : u8
{
    General,
    Containers,
    Strings,
    Core,
    Jobs,
    Reflection,
    Assets,
    Render,
    RHI,
    Physics,
    Animation,
    Audio,
    UI,
    Scene,
    Script,
    Editor,
    Game,
    Temp,
    Count
};

const char* MemTagName(MemTag tag);

inline constexpr usize DefaultAlignment = 16;

/// Allocates `size` bytes aligned to `alignment` (power of two). Never returns null (fatal on OOM).
void* Malloc(usize size, usize alignment = DefaultAlignment, MemTag tag = MemTag::Count /* = current thread tag */);
/// Reallocates; contents up to min(old, new) size are preserved. ptr may be null.
void* Realloc(void* ptr, usize newSize, usize alignment = DefaultAlignment);
void Free(void* ptr);
/// Usable size of a block returned by Malloc.
usize AllocationSize(const void* ptr);

/// Sets the thread's current memory tag for its lifetime.
class MemTagScope
{
public:
    explicit MemTagScope(MemTag tag);
    ~MemTagScope();
    MemTagScope(const MemTagScope&) = delete;
    MemTagScope& operator=(const MemTagScope&) = delete;

private:
    MemTag m_previous;
};

MemTag GetThreadMemTag();

/// Allocations made on this thread inside the scope are never reported as leaks. Use for intentional
/// process-lifetime data (interned names, type registry, profiler buffers).
class LeakIgnoreScope
{
public:
    LeakIgnoreScope();
    ~LeakIgnoreScope();
    LeakIgnoreScope(const LeakIgnoreScope&) = delete;
    LeakIgnoreScope& operator=(const LeakIgnoreScope&) = delete;
};

struct MemTagStats
{
    i64 currentBytes = 0;
    i64 peakBytes = 0;
    i64 currentCount = 0;
    i64 totalAllocations = 0;
};

struct MemoryStats
{
    MemTagStats tags[usize(MemTag::Count)];
    i64 totalCurrentBytes = 0;
    i64 totalPeakBytes = 0;
    i64 totalCurrentCount = 0;
};

/// Snapshot of tracked memory statistics (zeros when tracking is compiled out).
MemoryStats GetMemoryStats();

/// Allocation sequence id; leaks reported by ReportMemoryLeaks only include allocations made after this id.
u64 GetAllocationSequence();

/// Reports allocations still alive that were made after `sinceSequence`. Returns number of leaked blocks.
/// Writes a report (with callstacks in Debug) to the debugger output and stderr.
u64 ReportMemoryLeaks(u64 sinceSequence = 0, usize maxReported = 32);

inline void MemCopy(void* dst, const void* src, usize n) { std::memcpy(dst, src, n); }
inline void MemMove(void* dst, const void* src, usize n) { std::memmove(dst, src, n); }
inline void MemSet(void* dst, u8 value, usize n) { std::memset(dst, value, n); }
inline void MemZero(void* dst, usize n) { std::memset(dst, 0, n); }
inline int MemCompare(const void* a, const void* b, usize n) { return std::memcmp(a, b, n); }

/// Typed helpers. These are plain new/delete (which route to zx::Malloc/Free), so polymorphic deletion through
/// a base pointer is correct even with multiple inheritance (the deleting destructor adjusts the pointer).
template <typename T, typename... Args>
T* New(Args&&... args)
{
    return new T(forward<Args>(args)...);
}

template <typename T>
void Delete(T* p)
{
    delete p;
}

} // namespace zx
