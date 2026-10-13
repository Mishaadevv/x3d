#include "Core/Assert.h"
#include "Core/String/Format.h"
#include "Core/Testing/Test.h"

using namespace zx;

namespace {

template <typename... Args>
StringView Fmt(char (&buf)[256], StringView fmt, const Args&... args)
{
    const usize n = FormatToBuffer(buf, sizeof(buf), fmt, args...);
    return StringView(buf, n);
}

struct Point
{
    int x, y;
};

} // namespace

template <>
struct zx::Formatter<Point>
{
    static void Format(FormatSink& sink, const Point& p, const FormatSpec&) { FormatTo(sink, "({}, {})", p.x, p.y); }
};

ZX_TEST(Format, Basics)
{
    char b[256];
    ZX_EXPECT(Fmt(b, "hello") == "hello");
    ZX_EXPECT(Fmt(b, "{} + {} = {}", 1, 2, 3) == "1 + 2 = 3");
    ZX_EXPECT(Fmt(b, "{{}} {}", "x") == "{} x");
    ZX_EXPECT(Fmt(b, "{1} {0}", "a", "b") == "b a");
    ZX_EXPECT(Fmt(b, "{} {}", true, false) == "true false");
    ZX_EXPECT(Fmt(b, "{}", 'c') == "c");
    ZX_EXPECT(Fmt(b, "{}", StringView("view")) == "view");
    ZX_EXPECT(Fmt(b, "{} {}", 1) == "1 {?}");
}

ZX_TEST(Format, Integers)
{
    char b[256];
    ZX_EXPECT(Fmt(b, "{}", -42) == "-42");
    ZX_EXPECT(Fmt(b, "{}", I64Min) == "-9223372036854775808");
    ZX_EXPECT(Fmt(b, "{}", U64Max) == "18446744073709551615");
    ZX_EXPECT(Fmt(b, "{:x}", 255u) == "ff");
    ZX_EXPECT(Fmt(b, "{:#X}", 255u) == "0XFF");
    ZX_EXPECT(Fmt(b, "{:#010x}", 255u) == "0x000000ff");
    ZX_EXPECT(Fmt(b, "{:b}", 5u) == "101");
    ZX_EXPECT(Fmt(b, "{:5}", 42) == "   42");
    ZX_EXPECT(Fmt(b, "{:<5}|", 42) == "42   |");
    ZX_EXPECT(Fmt(b, "{:^6}|", 42) == "  42  |");
    ZX_EXPECT(Fmt(b, "{:*>5}", 7) == "****7");
    ZX_EXPECT(Fmt(b, "{:+}", 7) == "+7");
    ZX_EXPECT(Fmt(b, "{:05}", -7) == "-0007");
}

ZX_TEST(Format, Floats)
{
    char b[256];
    ZX_EXPECT(Fmt(b, "{}", 1.5f) == "1.5");
    ZX_EXPECT(Fmt(b, "{}", 0.1f) == "0.1");
    ZX_EXPECT(Fmt(b, "{}", 0.1) == "0.1");
    ZX_EXPECT(Fmt(b, "{:.2f}", 3.14159) == "3.14");
    ZX_EXPECT(Fmt(b, "{:.3f}", -0.5f) == "-0.500");
    ZX_EXPECT(Fmt(b, "{:8.2f}", 2.0) == "    2.00");
    ZX_EXPECT(Fmt(b, "{:e}", 1234.5) == "1.234500e+03");
    ZX_EXPECT(Fmt(b, "{}", 1e30) == "1e+30");
}

ZX_TEST(Format, StringsAndCustom)
{
    char b[256];
    ZX_EXPECT(Fmt(b, "[{:6}]", "ab") == "[ab    ]");
    ZX_EXPECT(Fmt(b, "[{:>6}]", "ab") == "[    ab]");
    ZX_EXPECT(Fmt(b, "[{:.2}]", "abcdef") == "[ab]");
    ZX_EXPECT(Fmt(b, "p={}", Point{3, -4}) == "p=(3, -4)");
    ZX_EXPECT(Fmt(b, "Привет, {}!", "мир") == "Привет, мир!");
}

ZX_TEST(Format, Truncation)
{
    char small[8];
    const usize n = FormatToBuffer(small, sizeof(small), "0123456789");
    ZX_EXPECT_EQ(n, 7u);
    ZX_EXPECT(StringView(small) == "0123456");
}

ZX_TEST(Assert, ExpectAssertCatchesFailure)
{
    int value = 1;
    ZX_EXPECT_ASSERT(ZX_ASSERT(value == 2, "value is {}", value));
}

ZX_TEST(StringView, Operations)
{
    const StringView s = "  Hello, World  ";
    ZX_EXPECT(s.Trim() == "Hello, World");
    ZX_EXPECT(s.Trim().StartsWith("Hello"));
    ZX_EXPECT(s.Trim().EndsWith("World"));
    ZX_EXPECT_EQ(s.Find("World"), 9u);
    ZX_EXPECT_EQ(s.Find("xyz"), StringView::NPos);
    ZX_EXPECT_EQ(StringView("a/b/c").RFind('/'), 3u);
    ZX_EXPECT(StringView("ABC").EqualsIgnoreCase("abc"));
    StringView l, r;
    ZX_EXPECT(StringView("key=value").Split('=', l, r));
    ZX_EXPECT(l == "key");
    ZX_EXPECT(r == "value");
    static_assert(StringView("abc").Size() == 3);
    static_assert(StringView("abcdef").SubStr(2, 2) == "cd");
}
