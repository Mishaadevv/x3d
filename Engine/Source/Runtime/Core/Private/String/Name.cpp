#include "Core/String/Name.h"
#include "Core/Containers/HashMap.h"
#include "Core/Memory/Allocator.h"
#include "Core/Threading/Mutex.h"

#include <atomic>

namespace zx {

namespace {

// Entries are stored in fixed-size chunks so pointers (and lock-free reads by index) stay valid forever.
constexpr u32 ChunkShift = 12;
constexpr u32 ChunkSize = 1u << ChunkShift;
constexpr u32 MaxChunks = 1024; // 4M names

struct Entry
{
    const char* text;
    u32 length;
};

struct NameTable
{
    RWLock lock;
    HashMap<StringView, u32> lookup;
    std::atomic<Entry*> chunks[MaxChunks] = {};
    std::atomic<u32> count{1}; // index 0 = None
    LinearAllocator stringStorage{64 * 1024};

    NameTable()
    {
        Entry* first = static_cast<Entry*>(Malloc(sizeof(Entry) * ChunkSize, 16, MemTag::Strings));
        first[0] = Entry{"", 0};
        chunks[0].store(first, std::memory_order_release);
    }

    const Entry& Get(u32 index) const
    {
        const Entry* chunk = chunks[index >> ChunkShift].load(std::memory_order_acquire);
        return chunk[index & (ChunkSize - 1)];
    }
};

NameTable& Table()
{
    // Intentionally leaked: Names may be used during static destruction.
    static NameTable* table = [] {
        MemTagScope tag(MemTag::Strings);
        LeakIgnoreScope permanent;
        return new NameTable();
    }();
    return *table;
}

} // namespace

Name::Name(StringView text)
{
    if (text.Empty())
        return;
    NameTable& t = Table();
    {
        ScopedReadLock read(t.lock);
        if (const u32* found = t.lookup.Find(text)) {
            m_index = *found;
            return;
        }
    }
    ScopedWriteLock write(t.lock);
    LeakIgnoreScope permanent; // interned names live for the whole process
    if (const u32* found = t.lookup.Find(text)) {
        m_index = *found;
        return;
    }
    const u32 index = t.count.load(std::memory_order_relaxed);
    ZX_ASSERT(index < MaxChunks * ChunkSize, "Name table overflow");
    const u32 chunkIndex = index >> ChunkShift;
    if (!t.chunks[chunkIndex].load(std::memory_order_relaxed)) {
        MemTagScope tag(MemTag::Strings);
        t.chunks[chunkIndex].store(static_cast<Entry*>(Malloc(sizeof(Entry) * ChunkSize, 16, MemTag::Strings)),
                                   std::memory_order_release);
    }
    char* storage = static_cast<char*>(t.stringStorage.Allocate(text.Size() + 1, 1));
    MemCopy(storage, text.Data(), text.Size());
    storage[text.Size()] = '\0';
    Entry* chunk = t.chunks[chunkIndex].load(std::memory_order_relaxed);
    chunk[index & (ChunkSize - 1)] = Entry{storage, u32(text.Size())};
    {
        MemTagScope tag(MemTag::Strings);
        t.lookup.Insert(StringView(storage, text.Size()), index);
    }
    t.count.store(index + 1, std::memory_order_release);
    m_index = index;
}

Name Name::Find(StringView text)
{
    Name n;
    if (text.Empty())
        return n;
    NameTable& t = Table();
    ScopedReadLock read(t.lock);
    if (const u32* found = t.lookup.Find(text))
        n.m_index = *found;
    return n;
}

StringView Name::View() const
{
    const Entry& e = Table().Get(m_index);
    return StringView(e.text, e.length);
}

const char* Name::CStr() const { return Table().Get(m_index).text; }

usize GetNameTableSize() { return Table().count.load(std::memory_order_acquire); }

} // namespace zx
