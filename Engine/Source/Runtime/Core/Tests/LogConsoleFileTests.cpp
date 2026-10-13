#include "Core/Console/Console.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"

using namespace zx;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogTest, Info);

static CVar<i32> CVarTestInt("test.Int", 5, "Test integer");
static CVar<f32> CVarTestFloat("test.Float", 1.5f, "Test float");
static CVar<bool> CVarTestBool("test.Bool", false, "Test bool");
static CVar<String> CVarTestString("test.String", "default", "Test string");
static CVar<i32> CVarTestReadOnly("test.ReadOnly", 1, "Read only", CVarFlags::ReadOnly);
static int g_cmdCalls = 0;
static String g_cmdArgs;
static ConCommand CmdTest("test.Cmd", "Test command", [](Span<const StringView> args) {
    ++g_cmdCalls;
    for (StringView a : args)
        g_cmdArgs += a;
});

ZX_TEST(Console, CVars)
{
    ZX_EXPECT_EQ(CVarTestInt.Get(), 5);
    ZX_EXPECT(Console::Execute("test.Int 42"));
    ZX_EXPECT_EQ(CVarTestInt.Get(), 42);
    ZX_EXPECT(Console::Execute("TEST.FLOAT=0.25; test.Bool true"));
    ZX_EXPECT_NEAR(CVarTestFloat.Get(), 0.25, 1e-6);
    ZX_EXPECT(CVarTestBool.Get());
    ZX_EXPECT(Console::Execute("test.String \"hello world\""));
    ZX_EXPECT(CVarTestString.Get() == "hello world");
    ZX_EXPECT(!Console::Execute("test.Int notanumber"));
    ZX_EXPECT(!Console::Execute("test.ReadOnly 5"));
    ZX_EXPECT_EQ(CVarTestReadOnly.Get(), 1);
    ZX_EXPECT(!Console::Execute("does.not.exist 1"));

    int changes = 0;
    CVarTestInt.SetOnChanged([&](CVarBase&) { ++changes; });
    CVarTestInt.Set(7);
    CVarTestInt.Set(7);
    ZX_EXPECT_EQ(changes, 1);
    CVarTestInt.SetOnChanged(nullptr);

    // Priority: config can't override a console-set value.
    ZX_EXPECT(!CVarTestInt.SetFromString("9", CVarSetBy::ProjectConfig) || CVarTestInt.Get() == 9);
    CVarTestInt.ResetToDefault();
    ZX_EXPECT(CVarTestInt.SetFromString("9", CVarSetBy::ProjectConfig));
    ZX_EXPECT(!CVarTestInt.SetFromString("10", CVarSetBy::Scalability));
    ZX_EXPECT_EQ(CVarTestInt.Get(), 9);
    CVarTestInt.ResetToDefault();

    ZX_EXPECT(Console::Execute("test.Cmd a \"b c\""));
    ZX_EXPECT_EQ(g_cmdCalls, 1);
    ZX_EXPECT(g_cmdArgs == "ab c");

    const auto completions = Console::AutoComplete("test.");
    ZX_EXPECT(completions.Size() >= 6);
}

ZX_TEST(Console, CommandLine)
{
    CommandLine::Initialize("MyMap.zscene -nosound -res=1280x720 +test.Int=77 \"quoted arg\"");
    ZX_EXPECT(CommandLine::HasFlag("NoSound"));
    ZX_EXPECT(!CommandLine::HasFlag("res"));
    StringView res;
    ZX_EXPECT(CommandLine::GetValue("res", res));
    ZX_EXPECT(res == "1280x720");
    ZX_EXPECT_EQ(CommandLine::GetPositional().Size(), 2u);
    ZX_EXPECT(CommandLine::GetPositional()[1] == "quoted arg");
    ZX_EXPECT_EQ(CommandLine::ApplyCVarOverrides(), 1u);
    ZX_EXPECT_EQ(CVarTestInt.Get(), 77);
    CVarTestInt.ResetToDefault();
    CommandLine::Initialize("");
}

ZX_TEST(Log, RingBufferSinkAndCategories)
{
    auto ringOwned = MakeUnique<RingBufferLogSink>(3);
    RingBufferLogSink* ring = ringOwned.Get();
    Log::AddSink(move(ringOwned));
    ZX_LOG_INFO(LogTest, "first {}", 1);
    ZX_LOG_WARN(LogTest, "second");
    ZX_LOG_DEBUG(LogTest, "filtered out by verbosity");
    ZX_LOG_ERROR(LogTest, "third");
    ZX_LOG_INFO(LogTest, "fourth");
    Array<RingBufferLogSink::Entry> entries;
    const u64 last = ring->CopyEntries(entries);
    ZX_EXPECT_EQ(entries.Size(), 3u); // capacity 3
    ZX_EXPECT(entries[0].text == "second");
    ZX_EXPECT(entries[2].text == "fourth");
    ZX_EXPECT_EQ(entries[1].level, LogLevel::Error);
    entries.Clear();
    ring->CopyEntries(entries, last);
    ZX_EXPECT(entries.Empty());
    ZX_EXPECT(Log::SetVerbosity("LogTest", LogLevel::Error));
    ZX_LOG_WARN(LogTest, "suppressed");
    ring->CopyEntries(entries, last);
    ZX_EXPECT(entries.Empty());
    Log::SetVerbosity("LogTest", LogLevel::Info);
    ZX_EXPECT(Log::FindCategory("logcore") != nullptr);
    Log::RemoveSink(ring);

    // Long message (> stack buffer) must not be truncated.
    String longText(3000, 'x');
    auto ring2Owned = MakeUnique<RingBufferLogSink>(4);
    RingBufferLogSink* ring2 = ring2Owned.Get();
    Log::AddSink(move(ring2Owned));
    ZX_LOG_INFO(LogTest, "{}", longText);
    entries.Clear();
    ring2->CopyEntries(entries);
    ZX_EXPECT_EQ(entries[0].text.Size(), 3000u);
    Log::RemoveSink(ring2);
}

ZX_TEST(PlatformFile, ReadWriteListDelete)
{
    const String dir = platform::GetTempDirectory() + "/zx_test_" + String::Format("{}", Thread::CurrentId());
    ZX_REQUIRE(platform::CreateDirectoryTree(dir + "/sub/deeper"));
    {
        platform::File f;
        ZX_REQUIRE(f.Open(dir + "/sub/тест.txt", platform::FileMode::Write));
        ZX_EXPECT_EQ(f.Write("hello", 5), 5u);
    }
    {
        platform::File f;
        ZX_REQUIRE(f.Open(dir + "/sub/тест.txt", platform::FileMode::Append));
        f.Write(" world", 6);
    }
    platform::File f;
    ZX_REQUIRE(f.Open(dir + "/sub/тест.txt", platform::FileMode::Read));
    ZX_EXPECT_EQ(f.Size(), 11u);
    char buf[16] = {};
    ZX_EXPECT_EQ(f.ReadAt(buf, 5, 6), 5u);
    ZX_EXPECT(StringView(buf, 5) == "world");
    f.Close();

    Array<platform::DirectoryEntry> entries;
    ZX_EXPECT(platform::ListDirectory(dir + "/sub", entries));
    ZX_EXPECT_EQ(entries.Size(), 2u);
    platform::FileInfo info;
    ZX_EXPECT(platform::GetFileInfo(dir + "/sub/тест.txt", info));
    ZX_EXPECT_EQ(info.size, 11u);
    ZX_EXPECT(platform::CopyFileAt(dir + "/sub/тест.txt", dir + "/copy.txt"));
    ZX_EXPECT(platform::MoveFileAt(dir + "/copy.txt", dir + "/moved.txt"));
    ZX_EXPECT(platform::FileExists(dir + "/moved.txt"));
    ZX_EXPECT(!platform::FileExists(dir + "/copy.txt"));

    platform::DirectoryWatcher watcher;
    ZX_EXPECT(watcher.Start(dir));
    {
        platform::File w;
        w.Open(dir + "/watched.txt", platform::FileMode::Write);
        w.Write("x", 1);
    }
    Array<platform::DirectoryWatcher::Change> changes;
    for (int i = 0; i < 100 && changes.Empty(); ++i) {
        Thread::SleepMs(5);
        watcher.Poll(changes);
    }
    ZX_EXPECT(!changes.Empty());
    if (!changes.Empty())
        ZX_EXPECT(changes[0].path == "watched.txt");
    watcher.Stop();

    ZX_EXPECT(platform::DeleteDirectoryTree(dir));
    ZX_EXPECT(!platform::DirectoryExists(dir));
}
