#pragma once

// Virtual file system.
//
//   fs::Mount("/Engine", "C:/ZeqouX3D/Engine/Content");
//   fs::Mount("/Game", projectDir + "/Content");
//   Array<u8> bytes;  fs::ReadFile("/Game/Textures/Rock.png", bytes);
//   auto req = fs::ReadFileAsync("/Game/Maps/Level.zscene");  ...  req->Wait();
//
// Paths starting with a mounted root are virtual; any other path is treated as a physical OS path.
// Several sources can be mounted at the same root (e.g. a .zpak over a directory); higher priority wins
// for reads, writes always go to the first writable source.

#include "Core/Containers/Function.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Memory/Pointers.h"

#include <atomic>

namespace zx::fs {

/// A provider of files under a mount root (physical directory, package file, ...). Paths are relative
/// to the mount root, '/'-separated, without a leading slash.
class MountSource
{
public:
    virtual ~MountSource() = default;
    virtual bool Exists(StringView relativePath) const = 0;
    virtual bool IsDirectory(StringView relativePath) const = 0;
    virtual bool GetInfo(StringView relativePath, platform::FileInfo& out) const = 0;
    virtual bool Read(StringView relativePath, Array<u8>& out) const = 0;
    virtual bool List(StringView relativeDir, Array<platform::DirectoryEntry>& out) const = 0;
    virtual bool IsWritable() const { return false; }
    /// Physical path for writable sources (empty otherwise).
    virtual String PhysicalPath(StringView relativePath) const
    {
        ZX_UNUSED(relativePath);
        return String();
    }
    virtual StringView DebugName() const = 0;
};

/// Physical directory source.
UniquePtr<MountSource> MakeDirectorySource(StringView physicalRoot, bool writable = true);

void Initialize(u32 ioThreadCount = 2);
void Shutdown();

/// Mounts a physical directory at a virtual root like "/Game". Returns false if the directory does not exist.
bool Mount(StringView virtualRoot, StringView physicalDirectory, i32 priority = 0, bool writable = true);
void MountSourceAt(StringView virtualRoot, UniquePtr<MountSource> source, i32 priority = 0);
void Unmount(StringView virtualRoot);
bool IsMounted(StringView virtualRoot);

/// Physical path for a virtual path (via the highest-priority writable source), or the input if not virtual.
String ToPhysicalPath(StringView path);
/// Virtual path for a physical path inside a mounted directory, or empty.
String ToVirtualPath(StringView physicalPath);

bool Exists(StringView path);
bool IsDirectory(StringView path);
bool GetInfo(StringView path, platform::FileInfo& out);
bool ReadFile(StringView path, Array<u8>& out);
bool ReadTextFile(StringView path, String& out); // strips UTF-8 BOM
/// Writes atomically (temp file + rename). Creates parent directories.
bool WriteFile(StringView path, ByteSpan data);
bool WriteTextFile(StringView path, StringView text);
bool CreateDirectories(StringView path);
bool Delete(StringView path); // file or directory tree
bool Move(StringView from, StringView to);
bool Copy(StringView from, StringView to);

struct ListOptions
{
    bool recursive = false;
    bool includeDirectories = true;
    bool includeFiles = true;
    StringView extension; // filter like ".png" (empty = all)
};
/// Lists entries; names in `out` are paths relative to `directory`.
bool List(StringView directory, Array<platform::DirectoryEntry>& out, const ListOptions& options = {});

// --- Async IO -------------------------------------------------------------------------------------------------------

enum class IOPriority : u8
{
    High,
    Normal,
    Low,
    Count
};

class IORequest : public RefCounted
{
public:
    enum class State : u32
    {
        Pending,
        Completed,
        Failed,
        Cancelled
    };

    State GetState() const { return State(m_state.load(std::memory_order_acquire)); }
    bool IsDone() const { return GetState() != State::Pending; }
    bool Succeeded() const { return GetState() == State::Completed; }
    /// Blocks until done. Returns false on timeout.
    bool Wait(u32 timeoutMs = U32Max) const;
    /// Requests cancellation (no effect if already being read).
    void Cancel();

    const String& Path() const { return m_path; }
    /// Valid after successful completion. The caller may move the data out.
    Array<u8>& Data() { return m_data; }

private:
    friend struct IOSystem;
    String m_path;
    Array<u8> m_data;
    Function<void(IORequest&)> m_callback;
    IOPriority m_priority = IOPriority::Normal;
    mutable std::atomic<u32> m_state{u32(State::Pending)};
};

using IORequestRef = RefPtr<IORequest>;

/// Reads a whole file on an IO thread. `onComplete` (optional) runs on the IO thread after reading.
IORequestRef ReadFileAsync(StringView path, Function<void(IORequest&)> onComplete = nullptr,
                           IOPriority priority = IOPriority::Normal);

/// Number of queued + in-flight async requests.
u32 PendingIORequests();

} // namespace zx::fs
