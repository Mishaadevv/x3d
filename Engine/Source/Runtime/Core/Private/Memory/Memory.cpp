#include "Core/Memory/Memory.h"
#include "Core/Assert.h"
#include "Core/HAL/PlatformMemory.h"
#include "Core/HAL/PlatformMisc.h"

#include "Windows/WindowsInclude.h"

#include <atomic>

// Block layout (user pointer aligned to the requested alignment):
//
//   base -> [ ...padding... ][ DebugNode (tracking builds) ][ AllocHeader ][ user data ... ]
//
// AllocHeader sits immediately before the user pointer and stores the size, tag and distance to base.

namespace zx {

namespace {

constexpr u16 HeaderMagic = 0x5A58; // "ZX"

struct AllocHeader
{
    u64 size;
    u32 offsetToBase;
    u8 tag;
    u8 reserved;
    u16 magic;
};
static_assert(sizeof(AllocHeader) == 16);

#if ZX_ENABLE_MEMORY_TRACKING
constexpr u32 CallstackDepth = ZX_ENABLE_LEAK_CALLSTACKS ? 12 : 0;

struct DebugNode
{
    DebugNode* prev;
    DebugNode* next;
    u64 sequence;
    void* frames[CallstackDepth > 0 ? CallstackDepth : 1];
};
constexpr usize DebugNodeSize = AlignUp(sizeof(DebugNode), usize(16));

struct TrackingState
{
    SRWLOCK lock = SRWLOCK_INIT;
    DebugNode* head = nullptr;
    std::atomic<u64> sequence{0};
    std::atomic<i64> current[usize(MemTag::Count)] = {};
    std::atomic<i64> peak[usize(MemTag::Count)] = {};
    std::atomic<i64> count[usize(MemTag::Count)] = {};
    std::atomic<i64> total[usize(MemTag::Count)] = {};
    std::atomic<i64> totalCurrent{0};
    std::atomic<i64> totalPeak{0};
    std::atomic<i64> totalCount{0};
};

// Constant-initialized (no dynamic init), so it is usable from the very first allocation.
constinit TrackingState g_tracking;

void AtomicMax(std::atomic<i64>& target, i64 value)
{
    i64 prev = target.load(std::memory_order_relaxed);
    while (prev < value && !target.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
}
#else
constexpr usize DebugNodeSize = 0;
#endif

thread_local MemTag t_currentTag = MemTag::General;
thread_local u32 t_leakIgnoreDepth = 0;
constexpr u8 FlagIgnoreLeak = 1;

constexpr const char* TagNames[] = {"General", "Containers", "Strings", "Core",      "Jobs",  "Reflection",
                                    "Assets",  "Render",     "RHI",     "Physics",   "Animation", "Audio",
                                    "UI",      "Scene",      "Script",  "Editor",    "Game",  "Temp"};
static_assert(ZX_ARRAY_COUNT(TagNames) == usize(MemTag::Count));

ZX_FORCEINLINE AllocHeader* HeaderOf(const void* user)
{
    return reinterpret_cast<AllocHeader*>(const_cast<u8*>(static_cast<const u8*>(user)) - sizeof(AllocHeader));
}

ZX_NORETURN ZX_NOINLINE void OutOfMemory(usize size)
{
    ZX_FATAL("Out of memory: failed to allocate {} bytes", size);
}

} // namespace

const char* MemTagName(MemTag tag) { return usize(tag) < usize(MemTag::Count) ? TagNames[usize(tag)] : "Invalid"; }

void* Malloc(usize size, usize alignment, MemTag tag)
{
    if (alignment < DefaultAlignment)
        alignment = DefaultAlignment;
    ZX_ASSERT(IsPowerOfTwo(alignment), "Alignment must be a power of two ({})", alignment);
    if (tag == MemTag::Count)
        tag = t_currentTag;

    const usize headerSpace = DebugNodeSize + sizeof(AllocHeader);
    const usize total = size + headerSpace + (alignment - DefaultAlignment);
    u8* base = static_cast<u8*>(platform::HeapAllocate(total));
    if (!base) [[unlikely]]
        OutOfMemory(size);

    u8* user = static_cast<u8*>(AlignPtr(base + headerSpace, alignment));
    AllocHeader* header = HeaderOf(user);
    header->size = size;
    header->offsetToBase = u32(user - base);
    header->tag = u8(tag);
    header->reserved = t_leakIgnoreDepth > 0 ? FlagIgnoreLeak : 0;
    header->magic = HeaderMagic;

#if ZX_ENABLE_MEMORY_TRACKING
    DebugNode* node = reinterpret_cast<DebugNode*>(reinterpret_cast<u8*>(header) - DebugNodeSize);
    node->sequence = g_tracking.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    #if ZX_ENABLE_LEAK_CALLSTACKS
    const u32 frames = platform::CaptureStackTrace(node->frames, CallstackDepth, 1);
    for (u32 i = frames; i < CallstackDepth; ++i)
        node->frames[i] = nullptr;
    #endif
    ::AcquireSRWLockExclusive(&g_tracking.lock);
    node->prev = nullptr;
    node->next = g_tracking.head;
    if (g_tracking.head)
        g_tracking.head->prev = node;
    g_tracking.head = node;
    ::ReleaseSRWLockExclusive(&g_tracking.lock);

    const usize t = usize(tag);
    const i64 cur = g_tracking.current[t].fetch_add(i64(size), std::memory_order_relaxed) + i64(size);
    AtomicMax(g_tracking.peak[t], cur);
    g_tracking.count[t].fetch_add(1, std::memory_order_relaxed);
    g_tracking.total[t].fetch_add(1, std::memory_order_relaxed);
    const i64 totalCur = g_tracking.totalCurrent.fetch_add(i64(size), std::memory_order_relaxed) + i64(size);
    AtomicMax(g_tracking.totalPeak, totalCur);
    g_tracking.totalCount.fetch_add(1, std::memory_order_relaxed);
#endif
    return user;
}

void Free(void* ptr)
{
    if (!ptr)
        return;
    AllocHeader* header = HeaderOf(ptr);
    ZX_ASSERT(header->magic == HeaderMagic, "Free: pointer {} was not allocated by zx::Malloc (or double free)", ptr);
    u8* base = static_cast<u8*>(ptr) - header->offsetToBase;

#if ZX_ENABLE_MEMORY_TRACKING
    DebugNode* node = reinterpret_cast<DebugNode*>(reinterpret_cast<u8*>(header) - DebugNodeSize);
    ::AcquireSRWLockExclusive(&g_tracking.lock);
    if (node->prev)
        node->prev->next = node->next;
    else
        g_tracking.head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    ::ReleaseSRWLockExclusive(&g_tracking.lock);

    const usize t = header->tag;
    const i64 size = i64(header->size);
    g_tracking.current[t].fetch_sub(size, std::memory_order_relaxed);
    g_tracking.count[t].fetch_sub(1, std::memory_order_relaxed);
    g_tracking.totalCurrent.fetch_sub(size, std::memory_order_relaxed);
    g_tracking.totalCount.fetch_sub(1, std::memory_order_relaxed);
#endif
    header->magic = 0xDEAD;
    platform::HeapFree(base);
}

void* Realloc(void* ptr, usize newSize, usize alignment)
{
    if (!ptr)
        return Malloc(newSize, alignment);
    if (newSize == 0) {
        Free(ptr);
        return nullptr;
    }
    const AllocHeader* header = HeaderOf(ptr);
    ZX_ASSERT(header->magic == HeaderMagic, "Realloc: invalid pointer {}", ptr);
    void* result = Malloc(newSize, alignment, MemTag(header->tag));
    MemCopy(result, ptr, header->size < newSize ? header->size : newSize);
    Free(ptr);
    return result;
}

usize AllocationSize(const void* ptr) { return ptr ? usize(HeaderOf(ptr)->size) : 0; }

MemTagScope::MemTagScope(MemTag tag) : m_previous(t_currentTag) { t_currentTag = tag; }
MemTagScope::~MemTagScope() { t_currentTag = m_previous; }
MemTag GetThreadMemTag() { return t_currentTag; }

LeakIgnoreScope::LeakIgnoreScope() { ++t_leakIgnoreDepth; }
LeakIgnoreScope::~LeakIgnoreScope() { --t_leakIgnoreDepth; }

MemoryStats GetMemoryStats()
{
    MemoryStats s;
#if ZX_ENABLE_MEMORY_TRACKING
    for (usize i = 0; i < usize(MemTag::Count); ++i) {
        s.tags[i].currentBytes = g_tracking.current[i].load(std::memory_order_relaxed);
        s.tags[i].peakBytes = g_tracking.peak[i].load(std::memory_order_relaxed);
        s.tags[i].currentCount = g_tracking.count[i].load(std::memory_order_relaxed);
        s.tags[i].totalAllocations = g_tracking.total[i].load(std::memory_order_relaxed);
    }
    s.totalCurrentBytes = g_tracking.totalCurrent.load(std::memory_order_relaxed);
    s.totalPeakBytes = g_tracking.totalPeak.load(std::memory_order_relaxed);
    s.totalCurrentCount = g_tracking.totalCount.load(std::memory_order_relaxed);
#endif
    return s;
}

u64 GetAllocationSequence()
{
#if ZX_ENABLE_MEMORY_TRACKING
    return g_tracking.sequence.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

u64 ReportMemoryLeaks(u64 sinceSequence, usize maxReported)
{
#if ZX_ENABLE_MEMORY_TRACKING
    u64 leakCount = 0;
    u64 leakBytes = 0;
    char line[1024];

    ::AcquireSRWLockShared(&g_tracking.lock);
    usize reported = 0;
    for (DebugNode* node = g_tracking.head; node; node = node->next) {
        if (node->sequence <= sinceSequence)
            continue;
        const AllocHeader* header = reinterpret_cast<const AllocHeader*>(reinterpret_cast<u8*>(node) + DebugNodeSize);
        if (header->reserved & FlagIgnoreLeak)
            continue;
        ++leakCount;
        leakBytes += header->size;
        if (reported >= maxReported)
            continue;
        ++reported;
        usize n = FormatToBuffer(line, sizeof(line), "[Memory] Leak #{}: {} bytes, tag {}, at {}\n", node->sequence,
                                 header->size, MemTagName(MemTag(header->tag)),
                                 static_cast<const void*>(header + 1));
        platform::DebugOutput(StringView(line, n));
        platform::ConsoleWrite(StringView(line, n), true);
    #if ZX_ENABLE_LEAK_CALLSTACKS
        for (u32 i = 0; i < CallstackDepth && node->frames[i]; ++i) {
            char sym[512];
            const usize sn = platform::SymbolizeAddress(node->frames[i], sym, sizeof(sym));
            n = FormatToBuffer(line, sizeof(line), "      {}\n", StringView(sym, sn));
            platform::DebugOutput(StringView(line, n));
            platform::ConsoleWrite(StringView(line, n), true);
        }
    #endif
    }
    ::ReleaseSRWLockShared(&g_tracking.lock);

    if (leakCount > 0) {
        const usize n = FormatToBuffer(line, sizeof(line), "[Memory] {} leaked allocations, {} bytes total\n",
                                       leakCount, leakBytes);
        platform::DebugOutput(StringView(line, n));
        platform::ConsoleWrite(StringView(line, n), true);
    }
    return leakCount;
#else
    ZX_UNUSED(sinceSequence);
    ZX_UNUSED(maxReported);
    return 0;
#endif
}

} // namespace zx

// ---------------------------------------------------------------------------------------------------------------------
// Global operator new/delete -> zx::Malloc/Free (so every C++ allocation is tagged and tracked).
// ---------------------------------------------------------------------------------------------------------------------

void* operator new(std::size_t size) { return zx::Malloc(size); }
void* operator new[](std::size_t size) { return zx::Malloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return zx::Malloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return zx::Malloc(size); }
void* operator new(std::size_t size, std::align_val_t align) { return zx::Malloc(size, std::size_t(align)); }
void* operator new[](std::size_t size, std::align_val_t align) { return zx::Malloc(size, std::size_t(align)); }
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
    return zx::Malloc(size, std::size_t(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
    return zx::Malloc(size, std::size_t(align));
}

void operator delete(void* p) noexcept { zx::Free(p); }
void operator delete[](void* p) noexcept { zx::Free(p); }
void operator delete(void* p, std::size_t) noexcept { zx::Free(p); }
void operator delete[](void* p, std::size_t) noexcept { zx::Free(p); }
void operator delete(void* p, std::align_val_t) noexcept { zx::Free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { zx::Free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { zx::Free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { zx::Free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { zx::Free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { zx::Free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { zx::Free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { zx::Free(p); }
