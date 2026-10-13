// Focused edge-case tests: one behavior per test.

#include "Core/Compression/Compression.h"
#include "Core/Console/Console.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Containers/Function.h"
#include "Core/Containers/HashMap.h"
#include "Core/Containers/RingBuffer.h"
#include "Core/Hash/Guid.h"
#include "Core/Math/Math.h"
#include "Core/Memory/Allocator.h"
#include "Core/Memory/OffsetAllocator.h"
#include "Core/Reflection/ReflectionSerialization.h"
#include "Core/Serialization/Binary.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Name.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"
#include "Core/String/Utf.h"
#include "Core/Testing/Test.h"

using namespace zx;

// --- Format ---------------------------------------------------------------------------------------------------------

ZX_TEST(FormatEdge, NegativeZeroAndSpecialFloats)
{
    ZX_EXPECT(String::Format("{}", -0.0f) == "-0");
    ZX_EXPECT(String::Format("{}", Infinity) == "inf");
    ZX_EXPECT(String::Format("{}", -Infinity) == "-inf");
    ZX_EXPECT(String::Format("{}", std::nanf("")) == "nan");
}

ZX_TEST(FormatEdge, PointerFormatting)
{
    const void* p = reinterpret_cast<const void*>(uptr(0x1234));
    ZX_EXPECT(String::Format("{}", p) == "0x0000000000001234");
    ZX_EXPECT(String::Format("{}", nullptr) == "0x0000000000000000");
}

ZX_TEST(FormatEdge, CharAndBoolAsNumbers)
{
    ZX_EXPECT(String::Format("{:d}", 'A') == "65");
    ZX_EXPECT(String::Format("{:x}", 'A') == "41");
    ZX_EXPECT(String::Format("{:d}", true) == "1");
}

ZX_TEST(FormatEdge, MalformedBraceIsLiteral)
{
    ZX_EXPECT(String::Format("value {", 1) == "value {");
    ZX_EXPECT(String::Format("}} {}", 2) == "} 2");
}

ZX_TEST(FormatEdge, EnumsFormatAsIntegers)
{
    enum class E : i8
    {
        A = -3
    };
    ZX_EXPECT(String::Format("{}", E::A) == "-3");
}

// --- StringView / String --------------------------------------------------------------------------------------------

ZX_TEST(StringViewEdge, EmptyOperations)
{
    const StringView e;
    ZX_EXPECT(e.Empty());
    ZX_EXPECT(e.Trim().Empty());
    ZX_EXPECT_EQ(e.Find('x'), StringView::NPos);
    ZX_EXPECT_EQ(e.Find(""), 0u);
    ZX_EXPECT(e.SubStr(10).Empty());
    ZX_EXPECT(e.StartsWith(""));
    ZX_EXPECT(StringView(nullptr).Empty());
}

ZX_TEST(StringViewEdge, RFindAndFindLastOf)
{
    const StringView s = "a/b\\c/d";
    ZX_EXPECT_EQ(s.FindLastOf("/\\"), 5u);
    ZX_EXPECT_EQ(s.RFind("/"), 5u);
    ZX_EXPECT_EQ(s.FindFirstOf("\\"), 3u);
    ZX_EXPECT_EQ(StringView("abcabc").RFind("abc"), 3u);
}

ZX_TEST(StringViewEdge, CompareOrdering)
{
    ZX_EXPECT(StringView("abc") < StringView("abd"));
    ZX_EXPECT(StringView("ab") < StringView("abc"));
    ZX_EXPECT_EQ(StringView("ABC").CompareIgnoreCase("abc"), 0);
    ZX_EXPECT(StringView("z") < StringView("\xC3\xA9")); // unsigned byte comparison
}

ZX_TEST(StringEdge, ReplaceAllWithLongerText)
{
    String s = "a.b.c";
    ZX_EXPECT_EQ(s.ReplaceAll(".", "::"), 2u);
    ZX_EXPECT(s == "a::b::c");
    ZX_EXPECT_EQ(s.ReplaceAll("", "x"), 0u);
    ZX_EXPECT_EQ(s.ReplaceAll("zzz", "y"), 0u);
}

ZX_TEST(StringEdge, AssignFromSelfSubstring)
{
    String s = "0123456789012345678901234567890123456789";
    s = StringView(s).SubStr(10, 5);
    ZX_EXPECT(s == "01234");
    String t = "short";
    t.Assign(t.View().SubStr(1));
    ZX_EXPECT(t == "hort");
}

ZX_TEST(StringEdge, InsertAtEndsAndErasePastEnd)
{
    String s = "mid";
    s.Insert(0, "<");
    s.Insert(s.Size(), ">");
    ZX_EXPECT(s == "<mid>");
    s.Erase(2, 1000);
    ZX_EXPECT(s == "<m");
}

ZX_TEST(StringEdge, ComparisonsWithViewsAndLiterals)
{
    const String s = "text";
    ZX_EXPECT(s == "text");
    ZX_EXPECT(s == StringView("text"));
    ZX_EXPECT(StringView("text") == s);
    ZX_EXPECT(String("a") < String("b"));
}

ZX_TEST(UtfEdge, TruncatedSequencesBecomeReplacement)
{
    usize pos = 0;
    const StringView bad("\xE2\x82", 2); // truncated 3-byte sequence
    ZX_EXPECT_EQ(Utf8Decode(bad, pos), ReplacementCodepoint);
    ZX_EXPECT_EQ(pos, 1u);
    ZX_EXPECT(!Utf8IsValid("\xED\xA0\x80")); // UTF-8 encoded surrogate
    ZX_EXPECT(Utf8IsValid("\xEF\xBF\xBD"));  // literal U+FFFD is valid
}

ZX_TEST(UtfEdge, EncodeRejectsSurrogatesAndOutOfRange)
{
    char buf[4];
    ZX_EXPECT_EQ(Utf8Encode(0xD800, buf), 0u);
    ZX_EXPECT_EQ(Utf8Encode(0x110000, buf), 0u);
    ZX_EXPECT_EQ(Utf8Encode(0x10FFFF, buf), 4u);
}

ZX_TEST(UtfEdge, LoneSurrogatesInUtf16)
{
    const wchar_t lone[] = {0xD800, L'a', 0};
    const String s = Utf16ToUtf8(lone);
    ZX_EXPECT(s == "\xEF\xBF\xBD" "a");
}

ZX_TEST(ParseEdge, Whitespace)
{
    ZX_EXPECT_EQ(*ParseInt("\t 7 \n"), 7);
    ZX_EXPECT(!ParseInt("- 7"));
    ZX_EXPECT(!ParseInt("0x"));
}

ZX_TEST(ParseEdge, FloatsSpecial)
{
    ZX_EXPECT(ParseFloat("1e40").HasValue() == false || IsFinite(*ParseFloat("1e40")) == false);
    ZX_EXPECT_NEAR(*ParseDouble("+2.5"), 2.5, 1e-12);
    f64 out = 0;
    ZX_EXPECT_EQ(ParseDoublePrefix("3.5abc", out), 3u);
    ZX_EXPECT_NEAR(out, 3.5, 1e-12);
}

// --- Paths / Names / GUID -------------------------------------------------------------------------------------------

ZX_TEST(PathEdge, RootsAndEmpty)
{
    ZX_EXPECT(Path::Normalize("") == "");
    ZX_EXPECT(Path::Normalize("/") == "/");
    ZX_EXPECT(Path::Normalize("C:/") == "C:/");
    ZX_EXPECT(Path::Normalize("./") == ".");
    ZX_EXPECT(Path::Normalize("/..") == "/");
    ZX_EXPECT(Path::Parent("file").Empty());
    ZX_EXPECT(Path::Parent("/file") == "/");
}

ZX_TEST(PathEdge, ExtensionsWithDotsInDirectories)
{
    ZX_EXPECT(Path::Extension("dir.v2/file").Empty());
    ZX_EXPECT(Path::Stem("dir.v2/file") == "file");
    ZX_EXPECT(Path::ChangeExtension("a/b", ".png") == "a/b.png");
    ZX_EXPECT(Path::ChangeExtension("a/b.png", "") == "a/b");
}

ZX_TEST(NameEdge, LongAndUnicodeNames)
{
    const String longText(500, 'n');
    const Name a(longText);
    ZX_EXPECT(a.View() == longText.View());
    const Name u("Свет_01");
    ZX_EXPECT(u.View() == "Свет_01");
    ZX_EXPECT(Hasher<Name>::Hash(u) == Hasher<Name>::Hash(Name("Свет_01")));
}

ZX_TEST(GuidEdge, ParseVariants)
{
    ZX_EXPECT(Guid::Parse("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}").HasValue());
    ZX_EXPECT(Guid::Parse("6BA7B8109DAD11D180B400C04FD430C8").HasValue());
    ZX_EXPECT(!Guid::Parse("6ba7b810-9dad-11d1-80b4-00c04fd430c"));  // too short
    ZX_EXPECT(!Guid::Parse("6ba7b810-9dad-11d1-80b4-00c04fd430c8a")); // too long
    ZX_EXPECT(!Guid().IsValid());
}

ZX_TEST(HashEdge, Sha1StreamingEqualsOneShot)
{
    const char* text = "The quick brown fox jumps over the lazy dog, repeated to cross a block boundary....";
    const usize n = StringView(text).Size();
    Sha1 s;
    for (usize i = 0; i < n; i += 7)
        s.Update(text + i, Min<usize>(7, n - i));
    ZX_EXPECT(s.Finish() == Sha1::Hash(text, n));
}

ZX_TEST(HashEdge, XXHashSeedChangesResult)
{
    ZX_EXPECT(HashXX64("abc", 3, 0) != HashXX64("abc", 3, 1));
    ZX_EXPECT(HashXX64("abc", 3, 0) != HashXX64("abd", 3, 0));
}

// --- Containers -----------------------------------------------------------------------------------------------------

ZX_TEST(ArrayEdge, ReserveDoesNotChangeSize)
{
    Array<int> a;
    a.Reserve(100);
    ZX_EXPECT(a.Empty());
    ZX_EXPECT(a.Capacity() >= 100u);
    a.ShrinkToFit();
    ZX_EXPECT_EQ(a.Capacity(), 0u);
}

ZX_TEST(ArrayEdge, InsertRangeInMiddle)
{
    Array<int> a = {1, 5};
    const int mid[] = {2, 3, 4};
    a.Insert(1, Span<const int>(mid));
    ZX_EXPECT(a == (Array<int>{1, 2, 3, 4, 5}));
}

ZX_TEST(ArrayEdge, AddUniqueAndFindLast)
{
    Array<int> a = {1, 2, 1};
    ZX_EXPECT_EQ(a.AddUnique(2), 1u);
    ZX_EXPECT_EQ(a.AddUnique(9), 3u);
    ZX_EXPECT_EQ(a.FindLast(1), 2u);
}

ZX_TEST(ArrayEdge, ResizeZeroedAndUninitialized)
{
    Array<u32> a;
    a.ResizeZeroed(10);
    ZX_EXPECT_EQ(a[9], 0u);
    a.ResizeUninitialized(3);
    ZX_EXPECT_EQ(a.Size(), 3u);
}

ZX_TEST(ArrayEdge, CustomAllocator)
{
    LinearAllocator linear(1024);
    Array<int> a(&linear);
    for (int i = 0; i < 300; ++i)
        a.PushBack(i);
    ZX_EXPECT_EQ(a.Back(), 299);
    ZX_EXPECT(linear.BytesUsed() >= 300 * sizeof(int));
}

ZX_TEST(SpanEdge, SubSpanAndBytes)
{
    const int data[] = {1, 2, 3, 4};
    const Span<const int> s(data);
    ZX_EXPECT_EQ(s.First(2).Size(), 2u);
    ZX_EXPECT_EQ(s.Last(1)[0], 4);
    ZX_EXPECT_EQ(s.SubSpan(1, 100).Size(), 3u);
    ZX_EXPECT_EQ(s.AsBytes().Size(), sizeof(data));
    ZX_EXPECT(s.Contains(3));
}

ZX_TEST(FixedArrayEdge, FillAndCompare)
{
    FixedArray<int, 3> a{};
    a.Fill(7);
    FixedArray<int, 3> b{{7, 7, 7}};
    ZX_EXPECT(a == b);
    ZX_EXPECT_EQ(a.Size(), 3u);
}

ZX_TEST(HashMapEdge, ReserveAvoidsRehashAndKeysValues)
{
    HashMap<int, int> m;
    m.Reserve(100);
    const usize cap = m.Capacity();
    for (int i = 0; i < 80; ++i)
        m.Insert(i, -i);
    ZX_EXPECT_EQ(m.Capacity(), cap);
    ZX_EXPECT_EQ(m.Keys().Size(), 80u);
    ZX_EXPECT_EQ(m.Values().Size(), 80u);
    ZX_EXPECT_EQ(m.FindOr(1000, 5), 5);
}

ZX_TEST(HashMapEdge, ClearKeepsWorking)
{
    HashMap<String, int> m;
    m["a"] = 1;
    m.Clear();
    ZX_EXPECT(m.Empty());
    ZX_EXPECT(!m.Contains("a"));
    m["a"] = 2;
    ZX_EXPECT_EQ(m.At("a"), 2);
}

ZX_TEST(HashMapEdge, MoveConstruction)
{
    HashMap<int, String> a;
    a.Insert(1, String("one"));
    HashMap<int, String> b = move(a);
    ZX_EXPECT(b.Find(1) && *b.Find(1) == "one");
    ZX_EXPECT(a.Empty());
}

ZX_TEST(BitArrayEdge, ExactWordBoundaries)
{
    BitArray b(128);
    b.Set(63);
    b.Set(64);
    b.Set(127);
    ZX_EXPECT_EQ(b.FindFirstSet(0), 63u);
    ZX_EXPECT_EQ(b.FindFirstSet(65), 127u);
    ZX_EXPECT_EQ(b.FindFirstSet(128), NPos);
    b.SetAll(true);
    ZX_EXPECT_EQ(b.FindFirstClear(), NPos);
    ZX_EXPECT_EQ(b.CountSet(), 128u);
}

ZX_TEST(FunctionEdge, EmptyAndReassign)
{
    Function<int()> f;
    ZX_EXPECT(!f);
    f = [] { return 1; };
    ZX_EXPECT_EQ(f(), 1);
    f = nullptr;
    ZX_EXPECT(!f.IsBound());
    int (*fp)() = nullptr;
    Function<int()> fromNullPtr = fp;
    ZX_EXPECT(!fromNullPtr);
}

ZX_TEST(DelegateEdge, RemoveDuringBroadcast)
{
    MulticastDelegate<> d;
    int calls = 0;
    DelegateHandle second;
    d.Add([&] {
        ++calls;
        d.Remove(second);
    });
    second = d.Add([&] { calls += 100; });
    d.Broadcast();
    ZX_EXPECT_EQ(calls, 1); // second removed before it ran
    d.Broadcast();
    ZX_EXPECT_EQ(calls, 2);
    ZX_EXPECT_EQ(d.Count(), 1u);
}

ZX_TEST(SortEdge, StableSortOfStrings)
{
    Array<String> a = {"b", "a", "c", "a"};
    StableSort(a);
    ZX_EXPECT(a[0] == "a" && a[3] == "c");
    Reverse(a.View());
    ZX_EXPECT(a[0] == "c");
    ZX_EXPECT_EQ(CountIf(a.View(), [](const String& s) { return s == "a"; }), 2u);
}

// --- Memory ---------------------------------------------------------------------------------------------------------

ZX_TEST(MemoryEdge, LinearReallocateInPlace)
{
    LinearAllocator a(1024);
    void* p = a.Allocate(16, 16);
    void* q = a.Reallocate(p, 16, 64, 16);
    ZX_EXPECT(p == q); // last allocation grows in place
}

ZX_TEST(MemoryEdge, OffsetAllocatorExhaustion)
{
    OffsetAllocator a(1000, 16);
    auto x = a.Allocate(600);
    ZX_EXPECT(x.IsValid());
    ZX_EXPECT(!a.Allocate(600).IsValid());
    a.Free(x);
    ZX_EXPECT(a.Allocate(1000).IsValid());
}

ZX_TEST(MemoryEdge, OffsetAllocatorNodeLimit)
{
    OffsetAllocator a(1 << 20, 4);
    OffsetAllocator::Allocation allocs[4];
    for (auto& al : allocs)
        al = a.Allocate(16);
    ZX_EXPECT(!a.Allocate(16).IsValid()); // max allocations reached
    for (auto& al : allocs)
        a.Free(al);
    ZX_EXPECT_EQ(a.GetStats().freeBlockCount, 1u);
}

ZX_TEST(MemoryEdge, VirtualArenaRewind)
{
    VirtualArena arena(16_MB);
    const usize mark = arena.GetPosition();
    arena.Allocate(1000, 16);
    arena.RewindTo(mark);
    ZX_EXPECT_EQ(arena.GetPosition(), mark);
}

// --- Math -----------------------------------------------------------------------------------------------------------

ZX_TEST(MathEdge, Mat3InverseAndSingular)
{
    const Mat3 m = Mat3::FromQuat(Quat::FromEuler(0.3f, 0.2f, 0.1f)) * Mat3::Scale(Vec3(2, 3, 4));
    const Mat3 i = Inverse(m) * m;
    for (usize c = 0; c < 3; ++c)
        ZX_EXPECT(IsNearlyEqual(i.columns[c], Mat3::Identity().columns[c], 1e-5f));
    ZX_EXPECT(Inverse(Mat3::Scale(Vec3(0))) == Mat3::Identity());
}

ZX_TEST(MathEdge, NormalMatrixHandlesNonUniformScale)
{
    const Mat4 m = Mat4::Scale(Vec3(2, 1, 1));
    const Vec3 n = Normalize(NormalMatrix(m) * Normalize(Vec3(1, 1, 0)));
    // Normal of the plane x - y = 0 after scaling x by 2 becomes (1, 2, 0) normalized.
    ZX_EXPECT(IsNearlyEqual(n, Normalize(Vec3(1, 2, 0)), 1e-5f));
}

ZX_TEST(MathEdge, DecomposeMirroredMatrix)
{
    const Mat4 m = Mat4::Scale(Vec3(-1, 1, 1));
    Vec3 t, s;
    Quat q;
    ZX_EXPECT(Decompose(m, t, q, s));
    ZX_EXPECT(IsNearlyEqual(Mat4::TRS(t, q, s), m, 1e-5f));
}

ZX_TEST(MathEdge, WrapAngleAndDelta)
{
    ZX_EXPECT_NEAR(Abs(WrapAngle(3.0f * Pi)), Pi, 1e-4f); // +-Pi are the same angle
    ZX_EXPECT_NEAR(WrapAngle(DegToRad(370.0f)), DegToRad(10.0f), 1e-4f);
    ZX_EXPECT_NEAR(WrapAngle(DegToRad(-190.0f)), DegToRad(170.0f), 1e-4f);
    ZX_EXPECT_NEAR(AngleDelta(DegToRad(350.0f), DegToRad(10.0f)), DegToRad(20.0f), 1e-4f);
}

ZX_TEST(MathEdge, IntegerHelpers)
{
    ZX_EXPECT_EQ(NextPowerOfTwo(0), 1u);
    ZX_EXPECT_EQ(NextPowerOfTwo(17), 32u);
    ZX_EXPECT_EQ(CeilLog2(17), 5u);
    ZX_EXPECT_EQ(FloorLog2(17), 4u);
    ZX_EXPECT_EQ(MipCount(1920, 1080), 11u);
    ZX_EXPECT_EQ(DivideRoundUp(10, 3), 4u);
}

ZX_TEST(MathEdge, SmoothDampConverges)
{
    f32 v = 0.0f, vel = 0.0f;
    for (int i = 0; i < 600; ++i)
        v = SmoothDamp(v, 10.0f, vel, 0.3f, 1.0f / 60.0f);
    ZX_EXPECT_NEAR(v, 10.0f, 1e-3f);
}

ZX_TEST(MathEdge, OBBCornersAndBounds)
{
    const OBB o = OBB::FromAABB(AABB(Vec3(-1), Vec3(1)), Mat4::RotationZ(Pi / 4.0f));
    Vec3 corners[8];
    o.GetCorners(corners);
    const AABB b = o.GetAABB();
    for (const Vec3& c : corners)
        ZX_EXPECT(b.Inflated(1e-4f).Contains(c));
    ZX_EXPECT_NEAR(b.max.x, Sqrt(2.0f), 1e-4f);
}

ZX_TEST(MathEdge, RectOperations)
{
    const Rect a = Rect::FromXYWH(0, 0, 10, 10);
    const Rect b = Rect::FromXYWH(5, 5, 10, 10);
    ZX_EXPECT(a.Intersects(b));
    ZX_EXPECT(a.Intersect(b) == Rect::FromXYWH(5, 5, 5, 5));
    ZX_EXPECT(a.Union(b) == Rect::FromXYWH(0, 0, 15, 15));
    ZX_EXPECT(a.Contains(Vec2(0, 0)));
    ZX_EXPECT(!a.Contains(Vec2(10, 10))); // max is exclusive
}

ZX_TEST(MathEdge, AABBEmptyAndUnion)
{
    AABB e;
    ZX_EXPECT(!e.IsValid());
    e.Expand(Vec3(1, 2, 3));
    ZX_EXPECT(e.IsValid());
    ZX_EXPECT(e.Size() == Vec3(0));
    const AABB u = Union(e, AABB(Vec3(-1), Vec3(0)));
    ZX_EXPECT(u.min == Vec3(-1) && u.max == Vec3(1, 2, 3));
    ZX_EXPECT(!AABB().Transformed(Mat4::Identity()).IsValid());
}

ZX_TEST(MathEdge, CurveConstantAndSingleKey)
{
    Curve c = Curve::Constant(3.0f);
    ZX_EXPECT_EQ(c.Evaluate(100.0f), 3.0f);
    Curve steps;
    steps.AddKey(Curve::Key{0.0f, 1.0f, 0, 0, CurveInterp::Constant});
    steps.AddKey(Curve::Key{1.0f, 2.0f, 0, 0, CurveInterp::Constant});
    ZX_EXPECT_EQ(steps.Evaluate(0.99f), 1.0f);
    ZX_EXPECT_EQ(steps.Evaluate(1.0f), 2.0f);
    ZX_EXPECT_EQ(Curve().Evaluate(1.0f), 0.0f);
}

ZX_TEST(MathEdge, RandomBoundedIsUniform)
{
    Random r(11);
    u32 hist[5] = {};
    for (int i = 0; i < 50000; ++i)
        ++hist[r.NextBounded(5)];
    for (u32 h : hist)
        ZX_EXPECT(h > 9000 && h < 11000);
    ZX_EXPECT_EQ(r.NextBounded(0), 0u);
}

ZX_TEST(MathEdge, ColorPackingOrder)
{
    const Color c(1, 2, 3, 4);
    ZX_EXPECT_EQ(c.ToRGBA32(), 0x01020304u);
    ZX_EXPECT_EQ(c.ToPackedABGR(), 0x04030201u);
    ZX_EXPECT(Color::FromRGBA32(0x01020304u) == c);
    ZX_EXPECT(LinearColor::FromHex("zz") == LinearColor(1, 0, 1, 1));
}

// --- Serialization --------------------------------------------------------------------------------------------------

ZX_TEST(JsonEdge, EscapesControlCharacters)
{
    const String text = ToJsonString(JsonValue(StringView("a\x01" "b", 3)), JsonWriteOptions{.pretty = false});
    ZX_EXPECT(text == "\"a\\u0001b\"");
}

ZX_TEST(JsonEdge, NonFiniteWritesNull)
{
    ZX_EXPECT(ToJsonString(JsonValue(f64(Infinity)), JsonWriteOptions{.pretty = false}) == "null");
}

ZX_TEST(JsonEdge, DuplicateKeysLastWins)
{
    JsonValue v;
    ZX_REQUIRE(ParseJson("{\"a\":1,\"a\":2}", v));
    ZX_EXPECT_EQ(v.Size(), 1u);
    ZX_EXPECT_EQ(v.Get("a").AsInt(), 2);
}

ZX_TEST(JsonEdge, BomAndWhitespaceOnly)
{
    JsonValue v;
    ZX_EXPECT(ParseJson("\xEF\xBB\xBF  [1]  ", v));
    ZX_EXPECT(!ParseJson("   ", v));
}

ZX_TEST(JsonEdge, TypeMismatchFallbacks)
{
    JsonValue v("text");
    ZX_EXPECT_EQ(v.AsInt(7), 7);
    ZX_EXPECT(v.AsBool(true));
    ZX_EXPECT_EQ(v.Size(), 0u);
    ZX_EXPECT(v.Find("x") == nullptr);
    ZX_EXPECT(JsonValue(5).AsString("fallback") == "fallback");
}

ZX_TEST(BinaryEdge, OverlongVarIntIsError)
{
    u8 bytes[11];
    MemSet(bytes, 0xFF, sizeof(bytes));
    BinaryReader r(ByteSpan(bytes, sizeof(bytes)));
    r.ReadVarU64();
    ZX_EXPECT(r.HasError());
}

ZX_TEST(BinaryEdge, VarIntBoundaries)
{
    Array<u8> buf;
    BinaryWriter w(buf);
    const u64 values[] = {0, 127, 128, 16383, 16384, U64Max};
    for (u64 v : values)
        w.WriteVarU64(v);
    w.WriteVarI64(I64Min);
    w.WriteVarI64(I64Max);
    BinaryReader r(buf.View());
    for (u64 v : values)
        ZX_EXPECT_EQ(r.ReadVarU64(), v);
    ZX_EXPECT_EQ(r.ReadVarI64(), I64Min);
    ZX_EXPECT_EQ(r.ReadVarI64(), I64Max);
    ZX_EXPECT(r.AtEnd());
}

ZX_TEST(CompressionEdge, StoredBlocksLargerThan64K)
{
    Array<u8> src;
    src.ResizeUninitialized(200000);
    Random rng(1);
    for (u8& b : src)
        b = u8(rng.NextU32());
    const Array<u8> packed = compression::Deflate(src.View(), 0);
    Array<u8> out;
    ZX_REQUIRE(compression::Inflate(packed.View(), out));
    ZX_EXPECT(out == src);
}

ZX_TEST(CompressionEdge, InflateRespectsMaxOutput)
{
    Array<u8> zeros;
    zeros.ResizeZeroed(100000);
    const Array<u8> packed = compression::Deflate(zeros.View(), 9);
    Array<u8> out;
    ZX_EXPECT(!compression::Inflate(packed.View(), out, 0, 1000)); // "zip bomb" protection
}

ZX_TEST(CompressionEdge, ZlzRejectsBadOffset)
{
    // token: 1 literal + match, literal 'a', offset 5 (beyond output start)
    const u8 bad[] = {0x10, 'a', 0x05, 0x00};
    u8 out[16];
    ZX_EXPECT(!compression::ZlzDecompress(ByteSpan(bad, sizeof(bad)), out, 6));
}

// --- Console / Reflection -------------------------------------------------------------------------------------------

ZX_TEST(ConsoleEdge, TokenizeQuotesAndSpaces)
{
    const auto t = Console::Tokenize("  cmd   \"a b\"  c  \"\" ");
    ZX_EXPECT_EQ(t.Size(), 4u);
    ZX_EXPECT(t[1] == "a b");
    ZX_EXPECT(t[3].Empty());
}

ZX_TEST(ReflectionEdge, ArrayOfStructsJson)
{
    Array<Vec2> points = {Vec2(1, 2), Vec2(3, 4)};
    const JsonValue j = Reflection::ToJson(points);
    ZX_EXPECT(ToJsonString(j, JsonWriteOptions{.pretty = false}) == "[[1.0,2.0],[3.0,4.0]]");
    Array<Vec2> back;
    ZX_EXPECT(Reflection::FromJson(j, back));
    ZX_EXPECT(back.Size() == 2 && back[1] == Vec2(3, 4));
}

ZX_TEST(ReflectionEdge, DefaultObjectIsDefaultConstructed)
{
    const Transform* t = static_cast<const Transform*>(Reflection::GetDefaultObject(TypeOf<Transform>()));
    ZX_REQUIRE(t);
    ZX_EXPECT(*t == Transform::Identity());
    ZX_EXPECT(Reflection::GetDefaultObject(TypeOf<Transform>()) == t); // cached
}

ZX_TEST(ReflectionEdge, BuiltinTypeNames)
{
    ZX_EXPECT(TypeOf<i32>().name == Name("i32"));
    ZX_EXPECT(TypeOf<u64>().size == 8u);
    ZX_EXPECT(TypeOf<String>().kind == TypeKind::String);
    ZX_EXPECT(TypeOf<Array<String>>().name == Name("Array<String>"));
    ZX_EXPECT(Reflection::FindType("Array<String>") == &TypeOf<Array<String>>());
}

ZX_TEST(ReflectionEdge, WrongJsonTypesReportWarnings)
{
    JsonValue j;
    ZX_REQUIRE(ParseJson("{\"position\": \"oops\", \"scale\": [2, 2, 2]}", j));
    Transform t;
    Array<String> warnings;
    ZX_EXPECT(!Reflection::FromJson(j, t, &warnings));
    ZX_EXPECT(!warnings.Empty());
    ZX_EXPECT(t.scale == Vec3(2)); // valid fields still applied
}
