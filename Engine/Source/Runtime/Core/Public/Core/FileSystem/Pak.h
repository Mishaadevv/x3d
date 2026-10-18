#pragma once

// .zpak packages: many files in one archive, each split into 64 KB blocks compressed with ZLZ (stored when that
// doesn't help). Layout: header | blocks | table of contents (paths, sizes, block table). Mounted as a read-only
// MountSource, so a packaged game reads "/Game/..." from the archive exactly like from a directory.
//
//   PakWriter w;
//   w.Open("Builds/MyGame/Content.zpak");
//   w.AddFile("Game/Scenes/Main.zscene", bytes);
//   w.Finish();
//   fs::MountSourceAt("/Game", MakePakSource(OpenPak("Content.zpak"), "Game"));

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Memory/Pointers.h"

namespace zx::fs {

inline constexpr u32 PakMagic = 0x4B41505A; // "ZPAK"
inline constexpr u32 PakVersion = 1;
inline constexpr u32 PakBlockSize = 64 * 1024;

class PakWriter
{
public:
    ~PakWriter();
    bool Open(StringView physicalPath);
    /// `path` inside the package ('/' separators, no leading slash). Compresses on the job system.
    bool AddFile(StringView path, ByteSpan data, i64 modifiedTime = 0);
    /// Writes the table of contents. Returns false on an IO error.
    bool Finish();

    u64 RawBytes() const { return m_rawBytes; }
    u64 PackedBytes() const { return m_offset; }
    u32 FileCount() const { return u32(m_files.Size()); }

private:
    struct FileEntry
    {
        String path;
        u64 size = 0;
        i64 modifiedTime = 0;
        u32 firstBlock = 0, blockCount = 0;
    };
    struct Block
    {
        u64 offset = 0;
        u32 packedSize = 0;
        bool stored = false;
    };
    platform::File m_file;
    Array<FileEntry> m_files;
    Array<Block> m_blocks;
    u64 m_offset = 0;
    u64 m_rawBytes = 0;
    bool m_failed = false;
};

/// An open package (shared by the mount sources made from it).
class PakArchive : public RefCounted
{
public:
    /// `baseOffset`: where the package starts inside the file (a package appended to an executable, e.g. the installer).
    bool Open(StringView physicalPath, u64 baseOffset = 0);
    bool Exists(StringView path) const;
    bool IsDirectory(StringView path) const;
    bool GetInfo(StringView path, platform::FileInfo& out) const;
    bool Read(StringView path, Array<u8>& out) const;
    bool List(StringView dir, Array<platform::DirectoryEntry>& out) const;
    u32 FileCount() const { return u32(m_files.Size()); }
    StringView Path() const { return m_path; }

private:
    struct FileEntry
    {
        String path;
        u64 size = 0;
        i64 modifiedTime = 0;
        u32 firstBlock = 0, blockCount = 0;
    };
    struct Block
    {
        u64 offset = 0;
        u32 packedSize = 0;
        bool stored = false;
    };
    const FileEntry* Find(StringView path) const;

    String m_path;
    mutable platform::File m_file;
    u64 m_base = 0;
    Array<FileEntry> m_files;
    Array<Block> m_blocks;
    HashMap<String, u32> m_index;                         // lower-case path -> file
    HashMap<String, Array<platform::DirectoryEntry>> m_dirs; // lower-case dir ("" = root) -> children
};

/// Opens a package; null if it can't be read.
RefPtr<PakArchive> OpenPak(StringView physicalPath, u64 baseOffset = 0);
/// Read-only mount source for the files under `prefix` (a directory inside the package, "" = all).
UniquePtr<MountSource> MakePakSource(RefPtr<PakArchive> pak, StringView prefix);

} // namespace zx::fs
