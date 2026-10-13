#include "Core/HAL/PlatformFile.h"
#include "Core/String/Utf.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"

#include "Windows/WindowsInclude.h"

#include <ShlObj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace zx::platform {

namespace {

/// UTF-8 path -> wide path with backslashes, null-terminated. Uses \\?\ prefix for long absolute paths.
Array<wchar_t> WidePath(StringView path)
{
    Array<wchar_t> w = Utf8ToUtf16(path);
    for (wchar_t& c : w)
        if (c == L'/')
            c = L'\\';
    // Strip trailing separators (except drive root "C:\").
    while (w.Size() > 3 && w.Back() == L'\\')
        w.PopBack();
    w.PushBack(L'\0');
    w.PopBack();
    return w;
}

String FromWide(const wchar_t* s, usize len)
{
    String r = Utf16ToUtf8(s, len);
    r.ReplaceChar('\\', '/');
    return r;
}

i64 FileTimeToI64(const FILETIME& ft) { return i64((u64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime); }

} // namespace

// --- File -----------------------------------------------------------------------------------------------------------

bool File::Open(StringView path, FileMode mode)
{
    Close();
    DWORD access = 0, creation = 0;
    DWORD share = FILE_SHARE_READ;
    switch (mode) {
    case FileMode::Read:
        access = GENERIC_READ;
        creation = OPEN_EXISTING;
        share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        break;
    case FileMode::Write:
        access = GENERIC_WRITE;
        creation = CREATE_ALWAYS;
        break;
    case FileMode::Append:
        access = FILE_APPEND_DATA | GENERIC_READ;
        creation = OPEN_ALWAYS;
        break;
    case FileMode::ReadWrite:
        access = GENERIC_READ | GENERIC_WRITE;
        creation = OPEN_ALWAYS;
        break;
    }
    const Array<wchar_t> wpath = WidePath(path);
    HANDLE h = ::CreateFileW(wpath.Data(), access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    m_handle = h;
    if (mode == FileMode::Append) {
        LARGE_INTEGER zero = {};
        ::SetFilePointerEx(h, zero, nullptr, FILE_END);
    }
    return true;
}

void File::Close()
{
    if (m_handle) {
        ::CloseHandle(m_handle);
        m_handle = nullptr;
    }
}

usize File::Read(void* buffer, usize size)
{
    usize total = 0;
    u8* dst = static_cast<u8*>(buffer);
    while (total < size) {
        const DWORD chunk = DWORD(Min<usize>(size - total, 0x40000000));
        DWORD read = 0;
        if (!::ReadFile(m_handle, dst + total, chunk, &read, nullptr) || read == 0)
            break;
        total += read;
    }
    return total;
}

usize File::ReadAt(void* buffer, usize size, u64 offset)
{
    usize total = 0;
    u8* dst = static_cast<u8*>(buffer);
    while (total < size) {
        OVERLAPPED ov = {};
        const u64 pos = offset + total;
        ov.Offset = DWORD(pos);
        ov.OffsetHigh = DWORD(pos >> 32);
        const DWORD chunk = DWORD(Min<usize>(size - total, 0x40000000));
        DWORD read = 0;
        if (!::ReadFile(m_handle, dst + total, chunk, &read, &ov) || read == 0)
            break;
        total += read;
    }
    return total;
}

usize File::Write(const void* data, usize size)
{
    usize total = 0;
    const u8* src = static_cast<const u8*>(data);
    while (total < size) {
        const DWORD chunk = DWORD(Min<usize>(size - total, 0x40000000));
        DWORD written = 0;
        if (!::WriteFile(m_handle, src + total, chunk, &written, nullptr) || written == 0)
            break;
        total += written;
    }
    return total;
}

bool File::Seek(u64 position)
{
    LARGE_INTEGER li;
    li.QuadPart = LONGLONG(position);
    return ::SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN) != 0;
}

u64 File::Tell() const
{
    LARGE_INTEGER zero = {}, pos = {};
    ::SetFilePointerEx(m_handle, zero, &pos, FILE_CURRENT);
    return u64(pos.QuadPart);
}

u64 File::Size() const
{
    LARGE_INTEGER size = {};
    ::GetFileSizeEx(m_handle, &size);
    return u64(size.QuadPart);
}

bool File::Flush() { return ::FlushFileBuffers(m_handle) != 0; }

// --- Queries / operations -------------------------------------------------------------------------------------------

bool GetFileInfo(StringView path, FileInfo& out)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    const Array<wchar_t> wpath = WidePath(path);
    if (!::GetFileAttributesExW(wpath.Data(), GetFileExInfoStandard, &data))
        return false;
    out.size = (u64(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.modifiedTime = FileTimeToI64(data.ftLastWriteTime);
    out.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.isReadOnly = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    return true;
}

bool FileExists(StringView path)
{
    FileInfo info;
    return GetFileInfo(path, info) && !info.isDirectory;
}

bool DirectoryExists(StringView path)
{
    FileInfo info;
    return GetFileInfo(path, info) && info.isDirectory;
}

bool CreateDirectoryTree(StringView path)
{
    if (path.Empty())
        return false;
    if (DirectoryExists(path))
        return true;
    // Create parent first.
    const usize sep = path.FindLastOf("/\\");
    if (sep != StringView::NPos && sep > 0) {
        const StringView parent = path.Left(sep);
        if (!(parent.Size() == 2 && parent[1] == ':')) // not a drive root
            CreateDirectoryTree(parent);
    }
    const Array<wchar_t> wpath = WidePath(path);
    return ::CreateDirectoryW(wpath.Data(), nullptr) != 0 || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool DeleteFileAt(StringView path)
{
    const Array<wchar_t> wpath = WidePath(path);
    return ::DeleteFileW(wpath.Data()) != 0;
}

bool DeleteDirectoryTree(StringView path)
{
    Array<DirectoryEntry> entries;
    if (!ListDirectory(path, entries))
        return false;
    bool ok = true;
    for (const DirectoryEntry& e : entries) {
        const String child = String(path) + "/" + e.name.View();
        ok &= e.isDirectory ? DeleteDirectoryTree(child) : DeleteFileAt(child);
    }
    const Array<wchar_t> wpath = WidePath(path);
    return ::RemoveDirectoryW(wpath.Data()) != 0 && ok;
}

bool MoveFileAt(StringView from, StringView to, bool overwrite)
{
    const Array<wchar_t> a = WidePath(from);
    const Array<wchar_t> b = WidePath(to);
    return ::MoveFileExW(a.Data(), b.Data(), MOVEFILE_COPY_ALLOWED | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) != 0;
}

bool CopyFileAt(StringView from, StringView to, bool overwrite)
{
    const Array<wchar_t> a = WidePath(from);
    const Array<wchar_t> b = WidePath(to);
    return ::CopyFileW(a.Data(), b.Data(), overwrite ? FALSE : TRUE) != 0;
}

bool ListDirectory(StringView path, Array<DirectoryEntry>& out)
{
    String pattern(path);
    if (!pattern.EndsWith("/") && !pattern.EndsWith("\\"))
        pattern += "/";
    pattern += "*";
    const Array<wchar_t> wpattern = WidePath(pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileExW(wpattern.Data(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                  FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        DirectoryEntry e;
        e.name = Utf16ToUtf8(fd.cFileName);
        e.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = (u64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.modifiedTime = FileTimeToI64(fd.ftLastWriteTime);
        out.PushBack(move(e));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return true;
}

String GetCurrentDirectoryPath()
{
    wchar_t buffer[2048];
    const DWORD n = ::GetCurrentDirectoryW(ZX_ARRAY_COUNT(buffer), buffer);
    return FromWide(buffer, n);
}

bool SetCurrentDirectoryPath(StringView path)
{
    const Array<wchar_t> w = WidePath(path);
    return ::SetCurrentDirectoryW(w.Data()) != 0;
}

String GetAbsolutePath(StringView path)
{
    const Array<wchar_t> w = WidePath(path);
    wchar_t buffer[2048];
    const DWORD n = ::GetFullPathNameW(w.Data(), ZX_ARRAY_COUNT(buffer), buffer, nullptr);
    if (n == 0 || n >= ZX_ARRAY_COUNT(buffer))
        return String(path);
    return FromWide(buffer, n);
}

String GetUserLocalAppDataDirectory()
{
    PWSTR p = nullptr;
    String result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
        result = FromWide(p, wcslen(p));
    ::CoTaskMemFree(p);
    return result;
}

String GetUserDocumentsDirectory()
{
    PWSTR p = nullptr;
    String result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)))
        result = FromWide(p, wcslen(p));
    ::CoTaskMemFree(p);
    return result;
}

String GetTempDirectory()
{
    wchar_t buffer[MAX_PATH + 1];
    DWORD n = ::GetTempPathW(ZX_ARRAY_COUNT(buffer), buffer);
    String r = FromWide(buffer, n);
    while (r.EndsWith("/"))
        r.PopBack();
    return r;
}

// --- DirectoryWatcher -----------------------------------------------------------------------------------------------

struct DirectoryWatcher::Impl
{
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    HANDLE startedEvent = nullptr; // set once the first ReadDirectoryChangesW is pending
    bool recursive = true;
    Thread thread;
    Mutex mutex;
    Array<Change> pending;
};

DirectoryWatcher::~DirectoryWatcher() { Stop(); }

bool DirectoryWatcher::Start(StringView directory, bool recursive)
{
    Stop();
    const Array<wchar_t> w = WidePath(directory);
    HANDLE dir = ::CreateFileW(w.Data(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE)
        return false;
    m_impl = New<Impl>();
    m_impl->dir = dir;
    m_impl->recursive = recursive;
    m_impl->stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->startedEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Impl* impl = m_impl;
    impl->thread.Start("ZX DirectoryWatcher", [impl] {
        alignas(DWORD) u8 buffer[64 * 1024];
        OVERLAPPED ov = {};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        String pendingOldName;
        for (;;) {
            ::ResetEvent(ov.hEvent);
            const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
            const BOOL issued = ::ReadDirectoryChangesW(impl->dir, buffer, sizeof(buffer), impl->recursive, filter,
                                                        nullptr, &ov, nullptr);
            ::SetEvent(impl->startedEvent);
            if (!issued)
                break;
            HANDLE handles[2] = {ov.hEvent, impl->stopEvent};
            const DWORD r = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0) {
                ::CancelIoEx(impl->dir, &ov);
                DWORD ignored;
                ::GetOverlappedResult(impl->dir, &ov, &ignored, TRUE);
                break;
            }
            DWORD bytes = 0;
            if (!::GetOverlappedResult(impl->dir, &ov, &bytes, FALSE) || bytes == 0)
                continue; // overflow: changes lost
            ScopedLock<Mutex> guard(impl->mutex);
            const u8* p = buffer;
            for (;;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                String name = FromWide(info->FileName, info->FileNameLength / sizeof(wchar_t));
                switch (info->Action) {
                case FILE_ACTION_ADDED:
                    impl->pending.PushBack(Change{Action::Added, move(name), {}});
                    break;
                case FILE_ACTION_REMOVED:
                    impl->pending.PushBack(Change{Action::Removed, move(name), {}});
                    break;
                case FILE_ACTION_MODIFIED:
                    impl->pending.PushBack(Change{Action::Modified, move(name), {}});
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    pendingOldName = move(name);
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    impl->pending.PushBack(Change{Action::Renamed, move(name), move(pendingOldName)});
                    pendingOldName = String();
                    break;
                default:
                    break;
                }
                if (info->NextEntryOffset == 0)
                    break;
                p += info->NextEntryOffset;
            }
        }
        ::SetEvent(impl->startedEvent);
        ::CloseHandle(ov.hEvent);
    });
    // Changes made after Start() returns must be observed: wait for the first watch request to be issued.
    ::WaitForSingleObject(m_impl->startedEvent, 5000);
    return true;
}

void DirectoryWatcher::Stop()
{
    if (!m_impl)
        return;
    ::SetEvent(m_impl->stopEvent);
    m_impl->thread.Join();
    ::CloseHandle(m_impl->dir);
    ::CloseHandle(m_impl->stopEvent);
    ::CloseHandle(m_impl->startedEvent);
    Delete(m_impl);
    m_impl = nullptr;
}

usize DirectoryWatcher::Poll(Array<Change>& out)
{
    if (!m_impl)
        return 0;
    ScopedLock<Mutex> guard(m_impl->mutex);
    const usize n = m_impl->pending.Size();
    for (Change& c : m_impl->pending)
        out.PushBack(move(c));
    m_impl->pending.Clear();
    return n;
}

} // namespace zx::platform
