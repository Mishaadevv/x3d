#include "Core/FileSystem/FileSystem.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"

#include <atomic>

using namespace zx;

namespace {
struct FsScope
{
    String root;
    FsScope()
    {
        fs::Initialize(2);
        root = platform::GetTempDirectory() + String::Format("/zx_fs_test_{}", Thread::CurrentId());
        platform::CreateDirectoryTree(root + "/content/textures");
        platform::CreateDirectoryTree(root + "/patch");
    }
    ~FsScope()
    {
        fs::Shutdown();
        platform::DeleteDirectoryTree(root);
    }
};
} // namespace

ZX_TEST(FileSystem, MountReadWrite)
{
    FsScope scope;
    ZX_REQUIRE(fs::Mount("/Game", scope.root + "/content"));
    ZX_EXPECT(fs::IsMounted("/Game"));
    ZX_EXPECT(!fs::Mount("/Missing", scope.root + "/does_not_exist"));

    ZX_EXPECT(fs::WriteTextFile("/Game/textures/info.txt", "rock texture"));
    ZX_EXPECT(fs::WriteTextFile("/Game/maps/level1.zscene", "{}")); // creates directories
    ZX_EXPECT(fs::Exists("/Game/textures/info.txt"));
    ZX_EXPECT(fs::IsDirectory("/Game/maps"));
    ZX_EXPECT(platform::FileExists(scope.root + "/content/textures/info.txt"));

    String text;
    ZX_EXPECT(fs::ReadTextFile("/Game/textures/info.txt", text));
    ZX_EXPECT(text == "rock texture");
    ZX_EXPECT(fs::ReadTextFile(scope.root + "/content/textures/info.txt", text)); // physical path works too

    ZX_EXPECT(fs::ToPhysicalPath("/Game/a/b.png") == Path::Normalize(platform::GetAbsolutePath(scope.root + "/content")) + "/a/b.png");
    ZX_EXPECT(fs::ToVirtualPath(scope.root + "/content/textures/info.txt") == "/Game/textures/info.txt");

    // Overlay with higher priority shadows files for reads.
    platform::File patch;
    ZX_REQUIRE(patch.Open(scope.root + "/patch/textures/info.txt", platform::FileMode::Write) ||
               (platform::CreateDirectoryTree(scope.root + "/patch/textures") &&
                patch.Open(scope.root + "/patch/textures/info.txt", platform::FileMode::Write)));
    patch.Write("patched", 7);
    patch.Close();
    ZX_REQUIRE(fs::Mount("/Game", scope.root + "/patch", 10, false));
    ZX_EXPECT(fs::ReadTextFile("/Game/textures/info.txt", text));
    ZX_EXPECT(text == "patched");

    Array<platform::DirectoryEntry> entries;
    ZX_EXPECT(fs::List("/Game", entries, fs::ListOptions{.recursive = true, .includeDirectories = false}));
    usize txtCount = 0;
    for (const auto& e : entries)
        txtCount += e.name == "textures/info.txt" ? 1 : 0;
    ZX_EXPECT_EQ(txtCount, 1u); // deduplicated across overlapping sources
    entries.Clear();
    ZX_EXPECT(fs::List("/Game", entries, fs::ListOptions{.recursive = true, .extension = ".zscene"}));
    ZX_EXPECT_EQ(entries.Size(), 1u);

    ZX_EXPECT(fs::Copy("/Game/textures/info.txt", "/Game/copy.txt"));
    ZX_EXPECT(fs::Move("/Game/copy.txt", "/Game/moved.txt"));
    ZX_EXPECT(fs::Exists("/Game/moved.txt"));
    ZX_EXPECT(fs::Delete("/Game/moved.txt"));
    ZX_EXPECT(!fs::Exists("/Game/moved.txt"));
    fs::Unmount("/Game");
    ZX_EXPECT(!fs::IsMounted("/Game"));
}

ZX_TEST(FileSystem, AsyncReads)
{
    FsScope scope;
    ZX_REQUIRE(fs::Mount("/Data", scope.root + "/content"));
    for (int i = 0; i < 20; ++i)
        ZX_REQUIRE(fs::WriteTextFile(String::Format("/Data/file{}.bin", i), String(usize(1000 + i), char('a' + i % 26))));

    std::atomic<int> callbacks{0};
    Array<fs::IORequestRef> requests;
    for (int i = 0; i < 20; ++i)
        requests.PushBack(fs::ReadFileAsync(String::Format("/Data/file{}.bin", i), [&](fs::IORequest&) { callbacks.fetch_add(1); },
                                            i % 2 ? fs::IOPriority::High : fs::IOPriority::Low));
    fs::IORequestRef missing = fs::ReadFileAsync("/Data/nope.bin");
    for (int i = 0; i < 20; ++i) {
        ZX_REQUIRE(requests[usize(i)]->Wait(5000));
        ZX_EXPECT(requests[usize(i)]->Succeeded());
        ZX_EXPECT_EQ(requests[usize(i)]->Data().Size(), usize(1000 + i));
    }
    ZX_EXPECT(missing->Wait(5000));
    ZX_EXPECT(missing->GetState() == fs::IORequest::State::Failed);
    ZX_EXPECT_EQ(callbacks.load(), 20);
    for (int i = 0; i < 100 && fs::PendingIORequests() > 0; ++i)
        Thread::SleepMs(1);
    ZX_EXPECT_EQ(fs::PendingIORequests(), 0u);
}

ZX_TEST(FileSystem, TempAndAppDataAreWritable)
{
    // Even at Low integrity (an executable under a folder with a Low mandatory label) the engine must be able to write
    // its scratch files and settings: the platform layer moves both to LocalLow then.
    for (const String& base : {platform::GetTempDirectory(), platform::GetUserLocalAppDataDirectory() + "/ZeqouX3D"}) {
        ZX_REQUIRE(!base.Empty());
        const String dir = String::Format("{}/zx_writable_test_{}", base, Thread::CurrentId());
        ZX_EXPECT(platform::CreateDirectoryTree(dir));
        const String file = dir + "/probe.txt";
        platform::File f;
        ZX_EXPECT(f.Open(file, platform::FileMode::Write));
        f.Close();
        ZX_EXPECT(platform::FileExists(file));
        platform::DeleteDirectoryTree(dir);
    }
}

#if defined(_WIN32)
#include <windows.h>
ZX_TEST(FileSystem, WritingThroughAShortNameKeepsTheLongName)
{
    // Settings saved through an 8.3 alias (a path passed as AI_SET~1.JSO) replace the real file instead of creating
    // a file literally named after the alias.
    const String dir = String::Format("{}/zx_shortname_test_{}", platform::GetTempDirectory(), Thread::CurrentId());
    platform::DeleteDirectoryTree(dir);
    ZX_REQUIRE(platform::CreateDirectoryTree(dir));
    const String longPath = dir + "/assistant_settings.json";
    ZX_REQUIRE(fs::WriteTextFile(longPath, "{\"v\": 1}"));
    wchar_t wideLong[512], wideShort[512];
    String native = longPath;
    native.ReplaceChar('/', '\\');
    ::MultiByteToWideChar(CP_UTF8, 0, native.Data(), int(native.Size()) + 1, wideLong, 512);
    const DWORD n = ::GetShortPathNameW(wideLong, wideShort, 512);
    char shortUtf8[1024] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, wideShort, int(n) + 1, shortUtf8, sizeof(shortUtf8), nullptr, nullptr);
    const String shortPath(shortUtf8);
    if (n == 0 || Path::FileName(shortPath) == Path::FileName(native)) {
        platform::DeleteDirectoryTree(dir);
        return; // no 8.3 names on this volume
    }
    ZX_EXPECT(fs::WriteTextFile(shortPath, "{\"v\": 2}"));
    Array<platform::DirectoryEntry> entries;
    ZX_REQUIRE(platform::ListDirectory(dir, entries));
    ZX_EXPECT_EQ(entries.Size(), usize(1));
    ZX_EXPECT(!entries.Empty() && entries[0].name == "assistant_settings.json");
    String text;
    ZX_EXPECT(fs::ReadTextFile(longPath, text) && text.Contains("2"));
    platform::DeleteDirectoryTree(dir);
}
#endif
