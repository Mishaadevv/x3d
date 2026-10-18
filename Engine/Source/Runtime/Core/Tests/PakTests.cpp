// .zpak packages: files of every size round-trip (empty, under / over a block, incompressible), mount as read-only
// sources (with a writable directory on top: reads fall through), directory listings, case-insensitive lookups.

#include "Core/FileSystem/Pak.h"
#include "Core/Math/Random.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"

using namespace zx;

namespace {
struct PakScope
{
    String root;
    PakScope()
    {
        fs::Initialize(2);
        root = platform::GetTempDirectory() + String::Format("/zx_pak_test_{}", Thread::CurrentId());
        platform::CreateDirectoryTree(root + "/saved");
    }
    ~PakScope()
    {
        fs::Shutdown();
        platform::DeleteDirectoryTree(root);
    }
};

Array<u8> Bytes(StringView text) { return Array<u8>(ByteSpan(reinterpret_cast<const u8*>(text.Data()), text.Size())); }
} // namespace

ZX_TEST(Pak, RoundTripMountAndList)
{
    PakScope scope;
    Array<u8> text;
    for (u32 i = 0; i < 20000; ++i) // compressible, several blocks
        text.Append(Bytes("level geometry, ").View());
    Array<u8> noise;
    Random rng(7);
    for (u32 i = 0; i < 150000; ++i) // incompressible: stored blocks
        noise.PushBack(u8(rng.NextU32()));
    const Array<u8> small = Bytes("{\"format\":\"zscene\"}");
    {
        fs::PakWriter w;
        ZX_REQUIRE(w.Open(scope.root + "/Content.zpak"));
        ZX_EXPECT(w.AddFile("Game/Scenes/Main.zscene", small.View()));
        ZX_EXPECT(w.AddFile("Game/Scripts/Player.zs", text.View()));
        ZX_EXPECT(w.AddFile("Game/Textures/noise.bin", noise.View()));
        ZX_EXPECT(w.AddFile("Game/Empty.txt", ByteSpan()));
        ZX_EXPECT(w.AddFile("DDC/0123456789abcdef.zmesh", small.View()));
        ZX_REQUIRE(w.Finish());
        ZX_EXPECT(w.PackedBytes() < w.RawBytes()); // the text compressed
    }
    RefPtr<fs::PakArchive> pak = fs::OpenPak(scope.root + "/Content.zpak");
    ZX_REQUIRE(pak);
    ZX_EXPECT_EQ(pak->FileCount(), 5u);
    fs::MountSourceAt("/Game", fs::MakePakSource(pak, "Game"));
    fs::MountSourceAt("/DDC", fs::MakePakSource(pak, "DDC"));
    Array<u8> out;
    ZX_REQUIRE(fs::ReadFile("/Game/Scripts/Player.zs", out));
    ZX_EXPECT(out == text);
    ZX_REQUIRE(fs::ReadFile("/Game/Textures/noise.bin", out));
    ZX_EXPECT(out == noise);
    ZX_REQUIRE(fs::ReadFile("/game/scenes/MAIN.zscene", out)); // like the file system: case-insensitive
    ZX_EXPECT(out == small);
    ZX_REQUIRE(fs::ReadFile("/Game/Empty.txt", out));
    ZX_EXPECT(out.Empty());
    ZX_EXPECT(fs::Exists("/DDC/0123456789abcdef.zmesh"));
    ZX_EXPECT(!fs::Exists("/Game/0123456789abcdef.zmesh")); // prefixes keep the roots apart
    ZX_EXPECT(fs::IsDirectory("/Game/Scripts"));
    ZX_EXPECT(!fs::Exists("/Game/Missing.zs"));
    platform::FileInfo info;
    ZX_REQUIRE(fs::GetInfo("/Game/Textures/noise.bin", info));
    ZX_EXPECT_EQ(info.size, u64(noise.Size()));
    // Listings: immediate children only.
    Array<platform::DirectoryEntry> entries;
    ZX_REQUIRE(fs::List("/Game", entries));
    u32 dirs = 0, files = 0;
    for (const platform::DirectoryEntry& e : entries)
        (e.isDirectory ? dirs : files)++;
    ZX_EXPECT_EQ(dirs, 3u);  // Scenes, Scripts, Textures
    ZX_EXPECT_EQ(files, 1u); // Empty.txt
    // Read-only: writes go to a writable directory mounted over the same root, reads fall back to the package.
    ZX_EXPECT(!fs::WriteTextFile("/Game/New.txt", "x"));
    ZX_REQUIRE(fs::Mount("/Game", scope.root + "/saved", 10, true));
    ZX_EXPECT(fs::WriteTextFile("/Game/New.txt", "x"));
    ZX_EXPECT(fs::Exists("/Game/New.txt") && fs::Exists("/Game/Scripts/Player.zs"));
    // Not a package.
    ZX_EXPECT(!fs::OpenPak(scope.root + "/saved/New.txt"));
}

ZX_TEST(Pak, AppendedToAnotherFile)
{
    // The installer: an executable with the package appended (opened at its offset).
    PakScope scope;
    const Array<u8> text = Bytes("ZeqouX3D installer payload");
    {
        fs::PakWriter w;
        ZX_REQUIRE(w.Open(scope.root + "/Payload.zpak"));
        ZX_EXPECT(w.AddFile("Files/readme.txt", text.View()));
        ZX_REQUIRE(w.Finish());
    }
    Array<u8> pakBytes, combined;
    ZX_REQUIRE(fs::ReadFile(scope.root + "/Payload.zpak", pakBytes));
    for (u32 i = 0; i < 1234; ++i)
        combined.PushBack(u8(i * 7));
    combined.Append(pakBytes.View());
    ZX_REQUIRE(fs::WriteFile(scope.root + "/Setup.exe", combined.View()));
    RefPtr<fs::PakArchive> pak = fs::OpenPak(scope.root + "/Setup.exe", 1234);
    ZX_REQUIRE(pak);
    Array<u8> out;
    ZX_REQUIRE(pak->Read("Files/readme.txt", out));
    ZX_EXPECT(out == text);
    ZX_EXPECT(!fs::OpenPak(scope.root + "/Setup.exe", 0)); // not at the start
}
