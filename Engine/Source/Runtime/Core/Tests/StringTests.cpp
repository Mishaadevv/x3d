#include "Core/Hash/Hash.h"
#include "Core/String/Parse.h"
#include "Core/String/String.h"
#include "Core/String/Utf.h"
#include "Core/Testing/Test.h"

using namespace zx;

ZX_TEST(String, SsoAndHeap)
{
    String s;
    ZX_EXPECT(s.Empty());
    ZX_EXPECT_EQ(StringView(s.CStr()).Size(), 0u);
    s = "short";
    ZX_EXPECT_EQ(s.Capacity(), String::InlineCapacity);
    String exact(23, 'x');
    ZX_EXPECT_EQ(exact.Size(), 23u);
    ZX_EXPECT_EQ(exact.Capacity(), String::InlineCapacity);
    ZX_EXPECT_EQ(exact.CStr()[23], '\0');
    exact += 'y';
    ZX_EXPECT_EQ(exact.Size(), 24u);
    ZX_EXPECT(exact.Capacity() > 23);
    ZX_EXPECT(exact.EndsWith("xy"));
    String moved = move(exact);
    ZX_EXPECT(exact.Empty());
    ZX_EXPECT_EQ(moved.Size(), 24u);
    moved.Resize(3);
    moved.ShrinkToFit();
    ZX_EXPECT(moved == "xxx");
    ZX_EXPECT_EQ(moved.Capacity(), String::InlineCapacity);
}

ZX_TEST(String, Operations)
{
    String s = "Hello";
    s += ", ";
    s += String("World");
    ZX_EXPECT(s == "Hello, World");
    s.Insert(5, "!!!");
    ZX_EXPECT(s == "Hello!!!, World");
    s.Erase(5, 3);
    ZX_EXPECT(s == "Hello, World");
    ZX_EXPECT_EQ(s.ReplaceAll("o", "0"), 2u);
    ZX_EXPECT(s == "Hell0, W0rld");
    ZX_EXPECT(s.ToUpper() == "HELL0, W0RLD");
    ZX_EXPECT(String("  pad  ").Trimmed() == "pad");
    ZX_EXPECT(String::Format("{}-{}", 1, "two") == "1-two");
    String self = "abcdefghijklmnopqrstuvwxyz";
    self.Append(self.View()); // self-append with reallocation
    ZX_EXPECT_EQ(self.Size(), 52u);
    ZX_EXPECT(self.SubStr(26, 3) == "abc");
    ZX_EXPECT(String("b") + StringView("c") == "bc");

    const auto parts = SplitString("a,b,,c", ',');
    ZX_EXPECT_EQ(parts.Size(), 4u);
    ZX_EXPECT(parts[2].Empty());
    ZX_EXPECT_EQ(SplitString("a,b,,c", ',', true).Size(), 3u);
    ZX_EXPECT(JoinStrings(Span<const StringView>(parts.View()), "|") == "a|b||c");
    ZX_EXPECT_EQ(SplitString("a::b::c", "::").Size(), 3u);
}

ZX_TEST(String, Utf8)
{
    const StringView ru = "Привет, 世界 🎮";
    ZX_EXPECT_EQ(Utf8Length(ru), 12u);
    ZX_EXPECT(Utf8IsValid(ru));
    ZX_EXPECT(!Utf8IsValid(StringView("\xC3\x28", 2)));
    ZX_EXPECT(!Utf8IsValid(StringView("\xC0\xAF", 2))); // overlong

    usize pos = 0;
    ZX_EXPECT_EQ(Utf8Decode(ru, pos), 0x41Fu); // 'П'
    ZX_EXPECT_EQ(pos, 2u);

    String s;
    Utf8Append(s, 0x1F3AE);
    ZX_EXPECT(s == "🎮");

    const Array<wchar_t> wide = Utf8ToUtf16(ru);
    ZX_EXPECT_EQ(wide.Size(), 13u); // emoji = surrogate pair
    ZX_EXPECT_EQ(wide.Data()[wide.Size()], L'\0');
    ZX_EXPECT(Utf16ToUtf8(wide.Data(), wide.Size()) == ru);

    ZX_EXPECT_EQ(Utf8TruncateBytes("Пр", 3), 2u);

    u32 count = 0;
    for (u32 cp : Utf8Codepoints("aБ"))
        count += cp > 0 ? 1 : 0;
    ZX_EXPECT_EQ(count, 2u);
}

ZX_TEST(String, Parse)
{
    ZX_EXPECT_EQ(*ParseInt("42"), 42);
    ZX_EXPECT_EQ(*ParseInt(" -17 "), -17);
    ZX_EXPECT_EQ(*ParseInt("0x1F"), 31);
    ZX_EXPECT_EQ(*ParseInt("-9223372036854775808"), I64Min);
    ZX_EXPECT(!ParseInt("9223372036854775808"));
    ZX_EXPECT(!ParseInt("12abc"));
    ZX_EXPECT(!ParseInt(""));
    ZX_EXPECT_EQ(*ParseUInt("0b101"), 5u);
    ZX_EXPECT_NEAR(*ParseDouble("3.25"), 3.25, 1e-12);
    ZX_EXPECT_NEAR(*ParseFloat("-1e-3"), -1e-3, 1e-9);
    ZX_EXPECT(!ParseFloat("1.0f"));
    ZX_EXPECT(*ParseBool("Yes"));
    ZX_EXPECT(!*ParseBool("off"));
    ZX_EXPECT(!ParseBool("maybe"));
}

ZX_TEST(Hash, KnownVectors)
{
    ZX_EXPECT_EQ(HashXX64("", 0, 0), 0xEF46DB3751D8E999ull);
    ZX_EXPECT_EQ(Crc32("123456789", 9), 0xCBF43926u);
    ZX_EXPECT_EQ(Adler32("Wikipedia", 9), 0x11E60398u);
    static_assert(HashFnv1a32("") == 0x811c9dc5u);
    static_assert(HashFnv1a64("a") == 0xaf63dc4c8601ec8cull);

    // Streaming == one-shot for every split point.
    char data[200];
    for (int i = 0; i < 200; ++i)
        data[i] = char(i * 31 + 7);
    const u64 expected = HashXX64(data, sizeof(data), 99);
    for (usize split = 0; split <= 200; split += 13) {
        XXHash64Stream st(99);
        st.Update(data, split);
        st.Update(data + split, 200 - split);
        ZX_EXPECT_EQ(st.Digest(), expected);
    }
    // Running CRC over chunks.
    ZX_EXPECT_EQ(Crc32("56789", 5, Crc32("1234", 4)), 0xCBF43926u);
}
