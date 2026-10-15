#include "Core/FileSystem/FileSystem.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Containers/HashMap.h"
#include "Core/Containers/RingBuffer.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogFileSystem, Info);

namespace zx::fs {

// --- Directory source -----------------------------------------------------------------------------------------------

namespace {

class DirectorySource final : public MountSource
{
public:
    DirectorySource(StringView root, bool writable) : m_root(Path::Normalize(root)), m_writable(writable)
    {
        m_name = String::Format("Dir({})", m_root);
    }
    String Full(StringView rel) const { return rel.Empty() ? m_root : m_root + "/" + rel; }
    bool Exists(StringView rel) const override
    {
        platform::FileInfo info;
        return platform::GetFileInfo(Full(rel), info);
    }
    bool IsDirectory(StringView rel) const override { return platform::DirectoryExists(Full(rel)); }
    bool GetInfo(StringView rel, platform::FileInfo& out) const override { return platform::GetFileInfo(Full(rel), out); }
    bool Read(StringView rel, Array<u8>& out) const override
    {
        platform::File f;
        if (!f.Open(Full(rel), platform::FileMode::Read))
            return false;
        const u64 size = f.Size();
        out.ResizeUninitialized(usize(size));
        return f.Read(out.Data(), usize(size)) == size;
    }
    bool List(StringView rel, Array<platform::DirectoryEntry>& out) const override
    {
        return platform::ListDirectory(Full(rel), out);
    }
    bool IsWritable() const override { return m_writable; }
    String PhysicalPath(StringView rel) const override { return Full(rel); }
    StringView DebugName() const override { return m_name; }

private:
    String m_root;
    String m_name;
    bool m_writable;
};

struct MountEntry
{
    String root; // "/Game" (no trailing slash)
    UniquePtr<MountSource> source;
    i32 priority;
};

} // namespace

struct IOSystem
{
    RWLock mountLock;
    Array<MountEntry> mounts; // sorted by priority (desc), stable

    Mutex queueMutex;
    ConditionVariable queueCv;
    RingBuffer<IORequestRef> queues[u32(IOPriority::Count)];
    std::atomic<u32> pending{0};
    Array<Thread> threads;
    bool quit = false;

    static IORequestRef Create(StringView path, Function<void(IORequest&)> callback, IOPriority priority)
    {
        IORequestRef req = MakeRef<IORequest>();
        req->m_path = path;
        req->m_callback = move(callback);
        req->m_priority = priority;
        return req;
    }

    static void Cancel(IORequest& r)
    {
        r.m_state.store(u32(IORequest::State::Cancelled), std::memory_order_release);
        AtomicWakeAll(r.m_state);
    }

    static void Complete(IORequest& r, bool ok)
    {
        r.m_state.store(u32(ok ? IORequest::State::Completed : IORequest::State::Failed), std::memory_order_release);
        AtomicWakeAll(r.m_state);
    }

    void Worker()
    {
        for (;;) {
            IORequestRef req;
            {
                ScopedLock<Mutex> guard(queueMutex);
                for (;;) {
                    for (auto& q : queues) {
                        if (!q.Empty()) {
                            req = q.PopFront();
                            break;
                        }
                    }
                    if (req || quit)
                        break;
                    queueCv.Wait(queueMutex);
                }
            }
            if (!req)
                return;
            if (req->GetState() != IORequest::State::Cancelled) {
                const bool ok = ReadFile(req->m_path, req->m_data);
                if (req->m_callback && ok)
                    req->m_callback(*req);
                if (req->GetState() != IORequest::State::Cancelled)
                    Complete(*req, ok);
                if (!ok)
                    ZX_LOG_WARN(LogFileSystem, "Async read failed: {}", req->m_path);
            }
            pending.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
};

namespace {
IOSystem* g_io = nullptr;
IOSystem& IO()
{
    ZX_ASSERT(g_io, "FileSystem not initialized (call fs::Initialize)");
    return *g_io;
}

String NormalizeRoot(StringView root)
{
    String r = Path::Normalize(root);
    if (!r.StartsWith("/"))
        r = String("/") + r;
    return r;
}

/// Finds mounts matching `path`. Fills relative path. Returns indices into mounts (priority order).
SmallArray<usize, 4> Resolve(const IOSystem& io, StringView path, String& relative)
{
    SmallArray<usize, 4> result;
    if (!path.StartsWith('/'))
        return result;
    const String norm = Path::Normalize(path);
    for (usize i = 0; i < io.mounts.Size(); ++i) {
        const String& root = io.mounts[i].root;
        if (norm.Size() >= root.Size() && norm.View().Left(root.Size()).EqualsIgnoreCase(root) &&
            (norm.Size() == root.Size() || norm[root.Size()] == '/')) {
            relative = norm.Size() == root.Size() ? String() : norm.SubStr(root.Size() + 1);
            result.PushBack(i);
        }
    }
    return result;
}

/// Physical path for writing (first writable mount or the path itself).
String WritablePath(StringView path)
{
    if (g_io && path.StartsWith('/')) {
        ScopedReadLock lock(g_io->mountLock);
        String rel;
        for (usize idx : Resolve(*g_io, path, rel))
            if (g_io->mounts[idx].source->IsWritable())
                return g_io->mounts[idx].source->PhysicalPath(rel);
        if (!Resolve(*g_io, path, rel).Empty())
            return String(); // virtual but read-only
    }
    return String(path);
}

bool IsVirtual(StringView path)
{
    if (!g_io || !path.StartsWith('/'))
        return false;
    ScopedReadLock lock(g_io->mountLock);
    String rel;
    return !Resolve(*g_io, path, rel).Empty();
}

} // namespace

UniquePtr<MountSource> MakeDirectorySource(StringView physicalRoot, bool writable)
{
    return MakeUnique<DirectorySource>(physicalRoot, writable);
}

void Initialize(u32 ioThreadCount)
{
    ZX_ASSERT(!g_io, "FileSystem already initialized");
    g_io = New<IOSystem>();
    g_io->threads.Resize(ioThreadCount ? ioThreadCount : 1);
    for (usize i = 0; i < g_io->threads.Size(); ++i)
        g_io->threads[i].Start(String::Format("ZX IO {}", i), [] { g_io->Worker(); }, ThreadPriority::AboveNormal);
}

void Shutdown()
{
    if (!g_io)
        return;
    {
        ScopedLock<Mutex> guard(g_io->queueMutex);
        g_io->quit = true;
        for (auto& q : g_io->queues) {
            while (!q.Empty()) {
                IORequestRef r = q.PopFront();
                IOSystem::Cancel(*r);
                g_io->pending.fetch_sub(1);
            }
        }
    }
    g_io->queueCv.NotifyAll();
    for (Thread& t : g_io->threads)
        t.Join();
    zx::Delete(g_io);
    g_io = nullptr;
}

void MountSourceAt(StringView virtualRoot, UniquePtr<MountSource> source, i32 priority)
{
    IOSystem& io = IO();
    ScopedWriteLock lock(io.mountLock);
    const String name(source->DebugName());
    io.mounts.PushBack(MountEntry{NormalizeRoot(virtualRoot), move(source), priority});
    StableSort(io.mounts, [](const MountEntry& a, const MountEntry& b) { return a.priority > b.priority; });
    ZX_LOG_INFO(LogFileSystem, "Mounted {} -> {}", NormalizeRoot(virtualRoot), name);
}

bool Mount(StringView virtualRoot, StringView physicalDirectory, i32 priority, bool writable)
{
    if (!platform::DirectoryExists(physicalDirectory)) {
        ZX_LOG_WARN(LogFileSystem, "Mount {}: directory '{}' does not exist", virtualRoot, physicalDirectory);
        return false;
    }
    MountSourceAt(virtualRoot, MakeDirectorySource(platform::GetAbsolutePath(physicalDirectory), writable), priority);
    return true;
}

void Unmount(StringView virtualRoot)
{
    IOSystem& io = IO();
    const String root = NormalizeRoot(virtualRoot);
    ScopedWriteLock lock(io.mountLock);
    io.mounts.RemoveIf([&](const MountEntry& m) { return m.root.EqualsIgnoreCase(root); });
}

bool IsMounted(StringView virtualRoot)
{
    if (!g_io)
        return false;
    const String root = NormalizeRoot(virtualRoot);
    ScopedReadLock lock(g_io->mountLock);
    for (const auto& m : g_io->mounts)
        if (m.root.EqualsIgnoreCase(root))
            return true;
    return false;
}

String ToPhysicalPath(StringView path) { return WritablePath(path); }

String ToVirtualPath(StringView physicalPath)
{
    if (!g_io)
        return String();
    const String abs = Path::Normalize(platform::GetAbsolutePath(physicalPath));
    ScopedReadLock lock(g_io->mountLock);
    for (const auto& m : g_io->mounts) {
        const String root = m.source->PhysicalPath(StringView());
        if (!root.Empty() && Path::IsUnder(abs, root)) {
            const String rel = Path::MakeRelative(abs, root);
            return rel == "." ? m.root : m.root + "/" + rel.View();
        }
    }
    return String();
}

// --- Synchronous operations -----------------------------------------------------------------------------------------

#define ZX_FS_DISPATCH(path, virtualExpr, physicalExpr)                                                                \
    if (IsVirtual(path)) {                                                                                             \
        ScopedReadLock lock(g_io->mountLock);                                                                          \
        String rel;                                                                                                    \
        for (usize idx : Resolve(*g_io, path, rel)) {                                                                  \
            const MountSource& src = *g_io->mounts[idx].source;                                                        \
            if (virtualExpr)                                                                                           \
                return true;                                                                                           \
        }                                                                                                              \
        return false;                                                                                                  \
    }                                                                                                                  \
    return physicalExpr;

bool Exists(StringView path)
{
    platform::FileInfo info;
    ZX_FS_DISPATCH(path, src.Exists(rel), platform::GetFileInfo(path, info))
}

bool IsDirectory(StringView path) { ZX_FS_DISPATCH(path, src.IsDirectory(rel), platform::DirectoryExists(path)) }

bool GetInfo(StringView path, platform::FileInfo& out) { ZX_FS_DISPATCH(path, src.GetInfo(rel, out), platform::GetFileInfo(path, out)) }

bool ReadFile(StringView path, Array<u8>& out)
{
    auto readPhysical = [&]() {
        platform::File f;
        if (!f.Open(path, platform::FileMode::Read))
            return false;
        const u64 size = f.Size();
        out.ResizeUninitialized(usize(size));
        return f.Read(out.Data(), usize(size)) == size;
    };
    ZX_FS_DISPATCH(path, src.Read(rel, out), readPhysical())
}

bool ReadTextFile(StringView path, String& out)
{
    Array<u8> bytes;
    if (!ReadFile(path, bytes))
        return false;
    usize start = 0;
    if (bytes.Size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        start = 3;
    out = StringView(reinterpret_cast<const char*>(bytes.Data()) + start, bytes.Size() - start);
    return true;
}

bool WriteFile(StringView path, ByteSpan data)
{
    const String physical = WritablePath(path);
    if (physical.Empty()) {
        ZX_LOG_ERROR(LogFileSystem, "WriteFile: '{}' is not writable", path);
        return false;
    }
    const StringView parent = Path::Parent(physical);
    if (!parent.Empty())
        platform::CreateDirectoryTree(parent);
    const String temp = physical + ".tmp";
    {
        platform::File f;
        if (!f.Open(temp, platform::FileMode::Write))
            return false;
        if (f.Write(data.Data(), data.Size()) != data.Size()) {
            f.Close();
            platform::DeleteFileAt(temp);
            return false;
        }
    }
    return platform::MoveFileAt(temp, physical, true);
}

bool WriteTextFile(StringView path, StringView text)
{
    return WriteFile(path, ByteSpan(reinterpret_cast<const u8*>(text.Data()), text.Size()));
}

bool CreateDirectories(StringView path)
{
    const String physical = WritablePath(path);
    return !physical.Empty() && platform::CreateDirectoryTree(physical);
}

bool Delete(StringView path)
{
    const String physical = WritablePath(path);
    if (physical.Empty())
        return false;
    if (platform::DirectoryExists(physical))
        return platform::DeleteDirectoryTree(physical);
    return platform::DeleteFileAt(physical);
}

bool Move(StringView from, StringView to)
{
    const String a = WritablePath(from), b = WritablePath(to);
    if (a.Empty() || b.Empty())
        return false;
    platform::CreateDirectoryTree(Path::Parent(b));
    return platform::MoveFileAt(a, b, true);
}

bool Copy(StringView from, StringView to)
{
    Array<u8> bytes;
    return ReadFile(from, bytes) && WriteFile(to, bytes.View());
}

namespace {
void ListRecursive(StringView dir, StringView prefix, Array<platform::DirectoryEntry>& out, const ListOptions& opt,
                   bool& ok)
{
    Array<platform::DirectoryEntry> entries;
    bool listed = false;
    if (IsVirtual(dir)) {
        ScopedReadLock lock(g_io->mountLock);
        String rel;
        for (usize idx : Resolve(*g_io, dir, rel))
            listed |= g_io->mounts[idx].source->List(rel, entries); // merge all sources
    } else {
        listed = platform::ListDirectory(dir, entries);
    }
    if (!listed) {
        ok = false;
        return;
    }
    // Remove duplicates from overlapping sources (first = highest priority wins).
    HashSet<String> seen;
    for (platform::DirectoryEntry& e : entries) {
        if (!seen.Insert(e.name))
            continue;
        const String relName = prefix.Empty() ? e.name : String(prefix) + "/" + e.name;
        if (e.isDirectory) {
            if (opt.includeDirectories && opt.extension.Empty())
                out.PushBack(platform::DirectoryEntry{relName, true, 0, e.modifiedTime});
            if (opt.recursive)
                ListRecursive(String(dir) + "/" + e.name.View(), relName, out, opt, ok);
        } else if (opt.includeFiles && (opt.extension.Empty() || Path::HasExtension(e.name, opt.extension))) {
            out.PushBack(platform::DirectoryEntry{relName, false, e.size, e.modifiedTime});
        }
    }
}
} // namespace

bool List(StringView directory, Array<platform::DirectoryEntry>& out, const ListOptions& options)
{
    bool ok = true;
    ListRecursive(directory, StringView(), out, options, ok);
    return ok;
}

// --- Async ----------------------------------------------------------------------------------------------------------

bool IORequest::Wait(u32 timeoutMs) const
{
    const u64 start = timeoutMs == U32Max ? 0 : Clock::NowTicks();
    while (!IsDone()) {
        u32 remaining = U32Max;
        if (timeoutMs != U32Max) {
            const f64 elapsed = Clock::TicksToMilliseconds(Clock::NowTicks() - start);
            if (elapsed >= timeoutMs)
                return false;
            remaining = u32(timeoutMs - elapsed) + 1;
        }
        AtomicWait(m_state, u32(State::Pending), remaining);
    }
    return true;
}

void IORequest::Cancel()
{
    u32 expected = u32(State::Pending);
    if (m_state.compare_exchange_strong(expected, u32(State::Cancelled)))
        AtomicWakeAll(m_state);
}

IORequestRef ReadFileAsync(StringView path, Function<void(IORequest&)> onComplete, IOPriority priority)
{
    IOSystem& io = IO();
    IORequestRef req = IOSystem::Create(path, move(onComplete), priority);
    io.pending.fetch_add(1, std::memory_order_acq_rel);
    {
        ScopedLock<Mutex> guard(io.queueMutex);
        io.queues[u32(priority)].PushBack(req);
    }
    io.queueCv.NotifyOne();
    return req;
}

u32 PendingIORequests() { return g_io ? g_io->pending.load(std::memory_order_acquire) : 0; }

} // namespace zx::fs
