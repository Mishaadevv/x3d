#include "Core/FileSystem/Pak.h"
#include "Core/Compression/Compression.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Binary.h"
#include "Core/String/Path.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogPak, Info);

namespace zx::fs {

namespace {

struct Header
{
    u32 magic = PakMagic;
    u32 version = PakVersion;
    u32 fileCount = 0;
    u32 blockCount = 0;
    u64 tocOffset = 0;
    u64 tocSize = 0;
};
static_assert(sizeof(Header) == 32);

String Key(StringView path)
{
    String p = Path::Normalize(path);
    while (p.StartsWith("/"))
        p = p.SubStr(1);
    while (p.EndsWith("/"))
        p = p.SubStr(0, p.Size() - 1);
    return p.ToLower();
}

} // namespace

// --- Writer ------------------------------------------------------------------------------------------------------------

PakWriter::~PakWriter() = default;

bool PakWriter::Open(StringView physicalPath)
{
    platform::CreateDirectoryTree(Path::Parent(physicalPath));
    if (!m_file.Open(physicalPath, platform::FileMode::Write))
        return false;
    const Header placeholder;
    m_offset = sizeof(Header);
    return m_file.Write(&placeholder, sizeof(Header)) == sizeof(Header);
}

bool PakWriter::AddFile(StringView path, ByteSpan data, i64 modifiedTime)
{
    if (!m_file.IsOpen() || m_failed)
        return false;
    FileEntry& f = m_files.AddDefaulted();
    f.path = Path::Normalize(path);
    while (f.path.StartsWith("/"))
        f.path = f.path.SubStr(1);
    f.size = data.Size();
    f.modifiedTime = modifiedTime;
    f.firstBlock = u32(m_blocks.Size());
    f.blockCount = u32((data.Size() + PakBlockSize - 1) / PakBlockSize);
    m_rawBytes += data.Size();
    // Blocks compress independently (in parallel), then go to the file in order.
    Array<Array<u8>> packed;
    packed.Resize(f.blockCount);
    auto compress = [&](u32 begin, u32 end) {
        for (u32 b = begin; b < end; ++b) {
            const usize at = usize(b) * PakBlockSize;
            const ByteSpan raw = data.SubSpan(at, Min<usize>(PakBlockSize, data.Size() - at));
            compression::ZlzCompress(raw, packed[b]);
            if (packed[b].Size() >= raw.Size()) { // incompressible: store
                packed[b].Clear();
                packed[b].Append(raw);
            }
        }
    };
    if (jobs::IsInitialized() && f.blockCount > 2)
        jobs::ParallelFor(f.blockCount, 1, compress);
    else
        compress(0, f.blockCount);
    for (u32 b = 0; b < f.blockCount; ++b) {
        const usize rawSize = Min<usize>(PakBlockSize, data.Size() - usize(b) * PakBlockSize);
        Block block;
        block.offset = m_offset;
        block.packedSize = u32(packed[b].Size());
        block.stored = packed[b].Size() == rawSize;
        if (m_file.Write(packed[b].Data(), packed[b].Size()) != packed[b].Size()) {
            m_failed = true;
            return false;
        }
        m_offset += packed[b].Size();
        m_blocks.PushBack(block);
    }
    return true;
}

bool PakWriter::Finish()
{
    if (!m_file.IsOpen())
        return false;
    Array<u8> toc;
    BinaryWriter w(toc);
    for (const FileEntry& f : m_files) {
        w.WriteString(f.path);
        w.Write<u64>(f.size);
        w.Write<i64>(f.modifiedTime);
        w.Write<u32>(f.firstBlock);
        w.Write<u32>(f.blockCount);
    }
    for (const Block& b : m_blocks) {
        w.Write<u64>(b.offset);
        w.Write<u32>(b.packedSize | (b.stored ? 0x80000000u : 0u));
    }
    Header h;
    h.fileCount = u32(m_files.Size());
    h.blockCount = u32(m_blocks.Size());
    h.tocOffset = m_offset;
    h.tocSize = toc.Size();
    const bool ok = !m_failed && m_file.Write(toc.Data(), toc.Size()) == toc.Size() && m_file.Seek(0) &&
                    m_file.Write(&h, sizeof(h)) == sizeof(h) && m_file.Flush();
    m_file.Close();
    return ok;
}

// --- Reader ------------------------------------------------------------------------------------------------------------

bool PakArchive::Open(StringView physicalPath)
{
    m_path = physicalPath;
    if (!m_file.Open(physicalPath, platform::FileMode::Read))
        return false;
    Header h;
    if (m_file.ReadAt(&h, sizeof(h), 0) != sizeof(h) || h.magic != PakMagic || h.version != PakVersion || h.tocOffset + h.tocSize > m_file.Size()) {
        ZX_LOG_ERROR(LogPak, "{}: not a package of this version", physicalPath);
        return false;
    }
    Array<u8> toc;
    toc.ResizeUninitialized(usize(h.tocSize));
    if (m_file.ReadAt(toc.Data(), toc.Size(), h.tocOffset) != toc.Size())
        return false;
    BinaryReader r(toc.View());
    m_files.Resize(h.fileCount);
    for (FileEntry& f : m_files) {
        f.path = r.ReadString();
        f.size = r.Read<u64>();
        f.modifiedTime = r.Read<i64>();
        f.firstBlock = r.Read<u32>();
        f.blockCount = r.Read<u32>();
    }
    m_blocks.Resize(h.blockCount);
    for (Block& b : m_blocks) {
        b.offset = r.Read<u64>();
        const u32 size = r.Read<u32>();
        b.packedSize = size & 0x7FFFFFFFu;
        b.stored = (size & 0x80000000u) != 0;
    }
    if (r.HasError())
        return false;
    // Index and directory tree.
    m_dirs.InsertOrAssign(String(), Array<platform::DirectoryEntry>());
    for (u32 i = 0; i < m_files.Size(); ++i) {
        const FileEntry& f = m_files[i];
        if (f.firstBlock + f.blockCount > m_blocks.Size())
            return false;
        m_index.InsertOrAssign(Key(f.path), i);
        // Every parent directory lists its child (created on first sight).
        StringView rest = f.path.View();
        String parent;
        for (;;) {
            const usize slash = rest.Find('/');
            const bool isDir = slash != StringView::NPos;
            const StringView name = isDir ? rest.Left(slash) : rest;
            const String parentKey = parent.ToLower();
            Array<platform::DirectoryEntry>* children = m_dirs.Find(parentKey);
            if (!children)
                children = &m_dirs.InsertOrAssign(parentKey, Array<platform::DirectoryEntry>());
            bool listed = false;
            for (const platform::DirectoryEntry& c : *children)
                listed |= c.name.View().EqualsIgnoreCase(name);
            if (!listed)
                children->PushBack(platform::DirectoryEntry{String(name), isDir, isDir ? 0 : f.size, f.modifiedTime});
            if (!isDir)
                break;
            parent = parent.Empty() ? String(name) : parent + "/" + name;
            if (!m_dirs.Find(parent.ToLower()))
                m_dirs.InsertOrAssign(parent.ToLower(), Array<platform::DirectoryEntry>());
            rest = rest.SubStr(slash + 1);
        }
    }
    ZX_LOG_INFO(LogPak, "Opened {} ({} files)", physicalPath, m_files.Size());
    return true;
}

const PakArchive::FileEntry* PakArchive::Find(StringView path) const
{
    const u32* i = m_index.Find(Key(path));
    return i ? &m_files[*i] : nullptr;
}

bool PakArchive::Exists(StringView path) const { return Find(path) || IsDirectory(path); }

bool PakArchive::IsDirectory(StringView path) const { return m_dirs.Find(Key(path)) != nullptr; }

bool PakArchive::GetInfo(StringView path, platform::FileInfo& out) const
{
    if (const FileEntry* f = Find(path)) {
        out = platform::FileInfo{f->size, f->modifiedTime, false, true};
        return true;
    }
    if (IsDirectory(path)) {
        out = platform::FileInfo{0, 0, true, true};
        return true;
    }
    return false;
}

bool PakArchive::Read(StringView path, Array<u8>& out) const
{
    const FileEntry* f = Find(path);
    if (!f)
        return false;
    out.ResizeUninitialized(usize(f->size));
    Array<u8> packed;
    for (u32 b = 0; b < f->blockCount; ++b) {
        const Block& block = m_blocks[f->firstBlock + b];
        const usize at = usize(b) * PakBlockSize;
        const usize rawSize = Min<usize>(PakBlockSize, usize(f->size) - at);
        if (block.stored) {
            if (m_file.ReadAt(out.Data() + at, rawSize, block.offset) != rawSize)
                return false;
            continue;
        }
        packed.ResizeUninitialized(block.packedSize);
        if (m_file.ReadAt(packed.Data(), packed.Size(), block.offset) != packed.Size() ||
            !compression::ZlzDecompress(packed.View(), out.Data() + at, rawSize)) {
            ZX_LOG_ERROR(LogPak, "{}: damaged block in {}", m_path, f->path);
            return false;
        }
    }
    return true;
}

bool PakArchive::List(StringView dir, Array<platform::DirectoryEntry>& out) const
{
    const Array<platform::DirectoryEntry>* children = m_dirs.Find(Key(dir));
    if (!children)
        return false;
    out.Append(children->View());
    return true;
}

RefPtr<PakArchive> OpenPak(StringView physicalPath)
{
    RefPtr<PakArchive> pak = MakeRef<PakArchive>();
    return pak->Open(physicalPath) ? pak : RefPtr<PakArchive>();
}

namespace {

class PakSource final : public MountSource
{
public:
    PakSource(RefPtr<PakArchive> pak, StringView prefix) : m_pak(move(pak)), m_prefix(Key(prefix))
    {
        m_name = String::Format("Pak({}:{})", Path::FileName(m_pak->Path()), m_prefix);
    }
    String Full(StringView rel) const { return m_prefix.Empty() ? String(rel) : rel.Empty() ? m_prefix : m_prefix + "/" + rel; }
    bool Exists(StringView rel) const override { return m_pak->Exists(Full(rel)); }
    bool IsDirectory(StringView rel) const override { return m_pak->IsDirectory(Full(rel)); }
    bool GetInfo(StringView rel, platform::FileInfo& out) const override { return m_pak->GetInfo(Full(rel), out); }
    bool Read(StringView rel, Array<u8>& out) const override { return m_pak->Read(Full(rel), out); }
    bool List(StringView rel, Array<platform::DirectoryEntry>& out) const override { return m_pak->List(Full(rel), out); }
    String PhysicalPath(StringView) const override { return String(); }
    StringView DebugName() const override { return m_name; }

private:
    RefPtr<PakArchive> m_pak;
    String m_prefix;
    String m_name;
};

} // namespace

UniquePtr<MountSource> MakePakSource(RefPtr<PakArchive> pak, StringView prefix)
{
    return MakeUnique<PakSource>(move(pak), prefix);
}

} // namespace zx::fs
