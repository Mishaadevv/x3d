#pragma once

#include "Core/Containers/Array.h"
#include "Core/Hash/Hash.h"

namespace zx {

namespace detail {

/// Open-addressing hash table with Robin Hood probing and backward-shift deletion.
/// Metadata word per slot: 0 = empty, otherwise (hash32 | 0x80000000). Capacity is a power of two.
/// Pointers to elements are invalidated by insertion (rehash) and by removal (backward shift).
template <typename Entry, typename Key, typename KeyOf>
class HashTable
{
public:
    HashTable() = default;
    explicit HashTable(Allocator* allocator) : m_allocator(allocator) {}
    HashTable(const HashTable& o) : m_allocator(o.m_allocator) { CopyFrom(o); }
    HashTable(HashTable&& o) noexcept { StealFrom(o); }
    ~HashTable() { Destroy(); }

    HashTable& operator=(const HashTable& o)
    {
        if (this != &o) {
            Clear();
            CopyFrom(o);
        }
        return *this;
    }
    HashTable& operator=(HashTable&& o) noexcept
    {
        if (this != &o) {
            Destroy();
            StealFrom(o);
        }
        return *this;
    }

    usize Size() const { return m_size; }
    bool Empty() const { return m_size == 0; }
    usize Capacity() const { return m_capacity; }

    void Clear()
    {
        if constexpr (!std::is_trivially_destructible_v<Entry>) {
            for (usize i = 0; i < m_capacity; ++i)
                if (m_meta[i])
                    m_entries[i].~Entry();
        }
        if (m_meta)
            MemZero(m_meta, m_capacity * sizeof(u32));
        m_size = 0;
    }

    void Reserve(usize count)
    {
        usize needed = 8;
        while (needed * MaxLoadNum < count * MaxLoadDen)
            needed *= 2;
        if (needed > m_capacity)
            Rehash(needed);
    }

    template <typename Q>
    Entry* FindEntry(const Q& key) const
    {
        if (m_size == 0)
            return nullptr;
        const u32 h = MakeHash(key);
        const usize mask = m_capacity - 1;
        usize pos = h & mask;
        for (usize dist = 0;; ++dist) {
            const u32 m = m_meta[pos];
            if (m == 0 || Distance(m, pos) < dist)
                return nullptr;
            if (m == h && KeyOf::Get(m_entries[pos]) == key)
                return &m_entries[pos];
            pos = (pos + 1) & mask;
        }
    }

    /// Inserts entry constructed from args if the key is absent. Returns {entry, inserted}.
    template <typename K, typename... Args>
    Pair<Entry*, bool> TryEmplace(const K& key, Args&&... args)
    {
        if (Entry* e = FindEntry(key))
            return {e, false};
        if ((m_size + 1) * MaxLoadDen > m_capacity * MaxLoadNum)
            Rehash(m_capacity ? m_capacity * 2 : 8);
        Entry tmp(forward<Args>(args)...);
        return {InsertNew(MakeHash(KeyOf::Get(tmp)), move(tmp)), true};
    }

    template <typename Q>
    bool Erase(const Q& key)
    {
        Entry* e = FindEntry(key);
        if (!e)
            return false;
        EraseAt(usize(e - m_entries));
        return true;
    }

    // Iteration over occupied slots.
    template <bool Const>
    class IteratorT
    {
    public:
        using Table = std::conditional_t<Const, const HashTable, HashTable>;
        using Ref = std::conditional_t<Const, const Entry&, Entry&>;
        IteratorT(Table* t, usize i) : m_table(t), m_index(i) { Skip(); }
        Ref operator*() const { return m_table->m_entries[m_index]; }
        auto* operator->() const { return &m_table->m_entries[m_index]; }
        IteratorT& operator++()
        {
            ++m_index;
            Skip();
            return *this;
        }
        bool operator==(const IteratorT& o) const { return m_index == o.m_index; }

    private:
        void Skip()
        {
            while (m_index < m_table->m_capacity && m_table->m_meta[m_index] == 0)
                ++m_index;
        }
        Table* m_table;
        usize m_index;
    };
    using Iterator = IteratorT<false>;
    using ConstIterator = IteratorT<true>;

    Iterator begin() { return Iterator(this, 0); }
    Iterator end() { return Iterator(this, m_capacity); }
    ConstIterator begin() const { return ConstIterator(this, 0); }
    ConstIterator end() const { return ConstIterator(this, m_capacity); }

    /// Removes all entries matching predicate. Returns count removed.
    template <typename Pred>
    usize RemoveIf(Pred pred)
    {
        usize removed = 0;
        for (usize i = 0; i < m_capacity;) {
            if (m_meta[i] && pred(m_entries[i])) {
                EraseAt(i); // backward shift may move a new entry into slot i: re-check it
                ++removed;
            } else {
                ++i;
            }
        }
        return removed;
    }

private:
    static constexpr usize MaxLoadNum = 4; // max load factor 4/5 = 0.8
    static constexpr usize MaxLoadDen = 5;

    template <typename Q>
    static u32 MakeHash(const Q& key)
    {
        const u64 h = Hasher<std::decay_t<Q>>::Hash(key);
        return u32(h ^ (h >> 32)) | 0x80000000u;
    }

    usize Distance(u32 meta, usize pos) const { return (pos - (meta & (m_capacity - 1))) & (m_capacity - 1); }

    Entry* InsertNew(u32 h, Entry&& entry)
    {
        const usize mask = m_capacity - 1;
        usize pos = h & mask;
        usize dist = 0;
        Entry* result = nullptr;
        Entry carry(move(entry));
        u32 carryMeta = h;
        for (;;) {
            const u32 m = m_meta[pos];
            if (m == 0) {
                ::new (&m_entries[pos]) Entry(move(carry));
                m_meta[pos] = carryMeta;
                ++m_size;
                return result ? result : &m_entries[pos];
            }
            const usize existingDist = Distance(m, pos);
            if (existingDist < dist) {
                // Robin Hood: steal the slot from the richer entry and continue inserting it.
                swap(carry, m_entries[pos]);
                swap(carryMeta, m_meta[pos]);
                if (!result)
                    result = &m_entries[pos];
                dist = existingDist;
            }
            pos = (pos + 1) & mask;
            ++dist;
        }
    }

    void EraseAt(usize pos)
    {
        const usize mask = m_capacity - 1;
        m_entries[pos].~Entry();
        m_meta[pos] = 0;
        --m_size;
        // Backward shift following entries that are not in their home slot.
        usize next = (pos + 1) & mask;
        while (m_meta[next] != 0 && Distance(m_meta[next], next) != 0) {
            ::new (&m_entries[pos]) Entry(move(m_entries[next]));
            m_entries[next].~Entry();
            m_meta[pos] = m_meta[next];
            m_meta[next] = 0;
            pos = next;
            next = (next + 1) & mask;
        }
    }

    void Rehash(usize newCapacity)
    {
        Entry* oldEntries = m_entries;
        u32* oldMeta = m_meta;
        const usize oldCapacity = m_capacity;

        m_capacity = newCapacity;
        m_meta = static_cast<u32*>(AllocFrom(m_allocator, newCapacity * sizeof(u32), 16));
        MemZero(m_meta, newCapacity * sizeof(u32));
        m_entries = static_cast<Entry*>(AllocFrom(m_allocator, newCapacity * sizeof(Entry),
                                                  alignof(Entry) > 16 ? alignof(Entry) : 16));
        m_size = 0;
        for (usize i = 0; i < oldCapacity; ++i) {
            if (oldMeta[i]) {
                InsertNew(oldMeta[i], move(oldEntries[i]));
                oldEntries[i].~Entry();
            }
        }
        if (oldMeta) {
            FreeTo(m_allocator, oldMeta, oldCapacity * sizeof(u32));
            FreeTo(m_allocator, oldEntries, oldCapacity * sizeof(Entry));
        }
    }

    void CopyFrom(const HashTable& o)
    {
        if (o.m_size == 0)
            return;
        Reserve(o.m_size);
        for (usize i = 0; i < o.m_capacity; ++i)
            if (o.m_meta[i])
                InsertNew(o.m_meta[i], Entry(o.m_entries[i]));
    }

    void StealFrom(HashTable& o)
    {
        m_allocator = o.m_allocator;
        m_entries = o.m_entries;
        m_meta = o.m_meta;
        m_size = o.m_size;
        m_capacity = o.m_capacity;
        o.m_entries = nullptr;
        o.m_meta = nullptr;
        o.m_size = 0;
        o.m_capacity = 0;
    }

    void Destroy()
    {
        Clear();
        if (m_meta) {
            FreeTo(m_allocator, m_meta, m_capacity * sizeof(u32));
            FreeTo(m_allocator, m_entries, m_capacity * sizeof(Entry));
        }
        m_meta = nullptr;
        m_entries = nullptr;
        m_capacity = 0;
    }

    Entry* m_entries = nullptr;
    u32* m_meta = nullptr;
    usize m_size = 0;
    usize m_capacity = 0;
    Allocator* m_allocator = nullptr;
};

template <typename K, typename V>
struct MapKeyOf
{
    static const K& Get(const Pair<K, V>& e) { return e.first; }
};

template <typename K>
struct SetKeyOf
{
    static const K& Get(const K& e) { return e; }
};

} // namespace detail

/// Unordered key -> value map. Keys need Hasher<K> and operator==. Heterogeneous lookup works when
/// Hasher<Q> produces the same hash as Hasher<K> (e.g. String keys with StringView queries).
template <typename K, typename V>
class HashMap
{
    using Table = detail::HashTable<Pair<K, V>, K, detail::MapKeyOf<K, V>>;

public:
    using Entry = Pair<K, V>;

    HashMap() = default;
    explicit HashMap(Allocator* allocator) : m_table(allocator) {}
    HashMap(std::initializer_list<Entry> il)
    {
        Reserve(il.size());
        for (const Entry& e : il)
            Insert(e.first, e.second);
    }

    usize Size() const { return m_table.Size(); }
    bool Empty() const { return m_table.Empty(); }
    usize Capacity() const { return m_table.Capacity(); }
    void Clear() { m_table.Clear(); }
    void Reserve(usize n) { m_table.Reserve(n); }

    template <typename Q>
    V* Find(const Q& key)
    {
        Entry* e = m_table.FindEntry(key);
        return e ? &e->second : nullptr;
    }
    template <typename Q>
    const V* Find(const Q& key) const
    {
        const Entry* e = m_table.FindEntry(key);
        return e ? &e->second : nullptr;
    }
    template <typename Q>
    bool Contains(const Q& key) const
    {
        return m_table.FindEntry(key) != nullptr;
    }
    /// Returns value or `fallback` if absent.
    template <typename Q>
    V FindOr(const Q& key, const V& fallback) const
    {
        const V* v = Find(key);
        return v ? *v : fallback;
    }
    /// Asserts presence.
    template <typename Q>
    V& At(const Q& key)
    {
        V* v = Find(key);
        ZX_ASSERT(v, "HashMap::At: key not found");
        return *v;
    }
    template <typename Q>
    const V& At(const Q& key) const
    {
        const V* v = Find(key);
        ZX_ASSERT(v, "HashMap::At: key not found");
        return *v;
    }

    /// Inserts if absent (does not overwrite). Returns {value*, inserted}.
    Pair<V*, bool> Insert(const K& key, const V& value)
    {
        auto r = m_table.TryEmplace(key, key, value);
        return {&r.first->second, r.second};
    }
    Pair<V*, bool> Insert(K&& key, V&& value)
    {
        if (Entry* e = m_table.FindEntry(key))
            return {&e->second, false};
        auto r = m_table.TryEmplace(key, move(key), move(value));
        return {&r.first->second, r.second};
    }
    /// Inserts or overwrites. Returns reference to stored value.
    V& InsertOrAssign(const K& key, V value)
    {
        if (Entry* e = m_table.FindEntry(key)) {
            e->second = move(value);
            return e->second;
        }
        return m_table.TryEmplace(key, key, move(value)).first->second;
    }
    /// Returns existing value or inserts a default-constructed one.
    template <typename Q>
    V& operator[](const Q& key)
    {
        if (Entry* e = m_table.FindEntry(key))
            return e->second;
        return m_table.TryEmplace(key, K(key), V()).first->second;
    }
    template <typename Q>
    bool Remove(const Q& key)
    {
        return m_table.Erase(key);
    }
    template <typename Pred>
    usize RemoveIf(Pred pred)
    {
        return m_table.RemoveIf([&](Entry& e) { return pred(e.first, e.second); });
    }

    /// Collects keys / values into arrays (order is unspecified).
    Array<K> Keys() const
    {
        Array<K> out;
        out.Reserve(Size());
        for (const Entry& e : m_table)
            out.PushBack(e.first);
        return out;
    }
    Array<V> Values() const
    {
        Array<V> out;
        out.Reserve(Size());
        for (const Entry& e : m_table)
            out.PushBack(e.second);
        return out;
    }

    /// Iteration yields Pair<K, V>& - do not modify `first`.
    auto begin() { return m_table.begin(); }
    auto end() { return m_table.end(); }
    auto begin() const { return m_table.begin(); }
    auto end() const { return m_table.end(); }

private:
    Table m_table;
};

template <typename K>
class HashSet
{
    using Table = detail::HashTable<K, K, detail::SetKeyOf<K>>;

public:
    HashSet() = default;
    explicit HashSet(Allocator* allocator) : m_table(allocator) {}
    HashSet(std::initializer_list<K> il)
    {
        Reserve(il.size());
        for (const K& k : il)
            Insert(k);
    }

    usize Size() const { return m_table.Size(); }
    bool Empty() const { return m_table.Empty(); }
    void Clear() { m_table.Clear(); }
    void Reserve(usize n) { m_table.Reserve(n); }

    /// Returns true if inserted (false if already present).
    bool Insert(const K& key) { return m_table.TryEmplace(key, key).second; }
    bool Insert(K&& key)
    {
        if (m_table.FindEntry(key))
            return false;
        return m_table.TryEmplace(key, move(key)).second;
    }
    template <typename Q>
    bool Contains(const Q& key) const
    {
        return m_table.FindEntry(key) != nullptr;
    }
    template <typename Q>
    const K* Find(const Q& key) const
    {
        return m_table.FindEntry(key);
    }
    template <typename Q>
    bool Remove(const Q& key)
    {
        return m_table.Erase(key);
    }
    template <typename Pred>
    usize RemoveIf(Pred pred)
    {
        return m_table.RemoveIf(pred);
    }

    auto begin() const { return m_table.begin(); }
    auto end() const { return m_table.end(); }

private:
    Table m_table;
};

} // namespace zx
