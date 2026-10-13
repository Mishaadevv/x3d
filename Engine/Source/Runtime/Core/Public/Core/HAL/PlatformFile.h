#pragma once

// Low-level synchronous file API on physical (OS) paths. UTF-8 paths, '/' or '\\' separators.
// Higher-level code should use FileSystem (virtual mount points, async IO).

#include "Core/Containers/Array.h"
#include "Core/Containers/Function.h"
#include "Core/String/String.h"

namespace zx::platform {

enum class FileMode : u8
{
    Read,      // existing file, read-only
    Write,     // create/truncate, write-only
    Append,    // create/open, write at end
    ReadWrite, // create/open, read+write
};

/// RAII file handle.
class File
{
public:
    File() = default;
    ~File() { Close(); }
    File(File&& o) noexcept : m_handle(o.m_handle) { o.m_handle = nullptr; }
    File& operator=(File&& o) noexcept
    {
        if (this != &o) {
            Close();
            m_handle = o.m_handle;
            o.m_handle = nullptr;
        }
        return *this;
    }
    ZX_NON_COPYABLE(File)

    bool Open(StringView path, FileMode mode);
    void Close();
    bool IsOpen() const { return m_handle != nullptr; }

    /// Returns bytes read (0 at EOF or on error).
    usize Read(void* buffer, usize size);
    /// Positional read (does not move the file pointer).
    usize ReadAt(void* buffer, usize size, u64 offset);
    usize Write(const void* data, usize size);
    bool Seek(u64 position);
    u64 Tell() const;
    u64 Size() const;
    bool Flush();

private:
    void* m_handle = nullptr;
};

struct FileInfo
{
    u64 size = 0;
    i64 modifiedTime = 0; // 100ns ticks since 1601 (FILETIME), monotonic across edits
    bool isDirectory = false;
    bool isReadOnly = false;
};

bool FileExists(StringView path);
bool DirectoryExists(StringView path);
bool GetFileInfo(StringView path, FileInfo& out);
/// Creates directory and all parents. Returns true if it exists afterwards.
bool CreateDirectoryTree(StringView path);
bool DeleteFileAt(StringView path);
/// Recursively deletes a directory.
bool DeleteDirectoryTree(StringView path);
bool MoveFileAt(StringView from, StringView to, bool overwrite = true);
bool CopyFileAt(StringView from, StringView to, bool overwrite = true);

struct DirectoryEntry
{
    String name; // file name only
    bool isDirectory;
    u64 size;
    i64 modifiedTime;
};

/// Lists entries of a directory (not recursive, skips "." and "..").
bool ListDirectory(StringView path, Array<DirectoryEntry>& out);

/// Current working directory (UTF-8, forward slashes).
String GetCurrentDirectoryPath();
bool SetCurrentDirectoryPath(StringView path);
/// Absolute normalized path (forward slashes).
String GetAbsolutePath(StringView path);
/// %LOCALAPPDATA% / temp directory.
String GetUserLocalAppDataDirectory();
String GetUserDocumentsDirectory();
String GetTempDirectory();

/// Watches a directory tree for changes (ReadDirectoryChangesW). Callbacks are queued and delivered on Poll().
class DirectoryWatcher
{
public:
    enum class Action : u8
    {
        Added,
        Removed,
        Modified,
        Renamed
    };
    struct Change
    {
        Action action;
        String path; // relative to the watched root, forward slashes
        String oldPath; // for Renamed
    };

    DirectoryWatcher() = default;
    ~DirectoryWatcher();
    ZX_NON_COPYABLE(DirectoryWatcher)

    bool Start(StringView directory, bool recursive = true);
    void Stop();
    bool IsRunning() const { return m_impl != nullptr; }
    /// Moves pending changes into `out` (thread-safe). Returns number of changes.
    usize Poll(Array<Change>& out);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace zx::platform
