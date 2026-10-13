#include "Core/Compression/Compression.h"
#include "Core/Hash/Guid.h"
#include "Core/Math/Random.h"
#include "Core/String/Name.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"

using namespace zx;

namespace {

Array<u8> MakeTestData(usize size, int kind)
{
    Array<u8> d;
    d.ResizeUninitialized(size);
    Random rng(u64(kind) * 977 + size);
    for (usize i = 0; i < size; ++i) {
        switch (kind) {
        case 0: // random (incompressible)
            d[i] = u8(rng.NextU32());
            break;
        case 1: // text-like
            d[i] = u8("the quick brown fox jumps over the lazy dog "[(i * 7 + i / 13) % 44]);
            break;
        case 2: // long runs
            d[i] = u8((i / 1000) & 0xFF);
            break;
        default: // mixed
            d[i] = (i % 100 < 70) ? u8("abcabcabd"[i % 9]) : u8(rng.NextU32() & 0x1F);
            break;
        }
    }
    return d;
}

} // namespace

ZX_TEST(Compression, DeflateRoundTrip)
{
    for (usize size : {usize(0), usize(1), usize(15), usize(100), usize(70000), usize(300000)}) {
        for (int kind = 0; kind < 4; ++kind) {
            const Array<u8> src = MakeTestData(size, kind);
            for (i32 level : {0, 1, 6, 9}) {
                const Array<u8> packed = compression::Deflate(src.View(), level);
                Array<u8> unpacked;
                ZX_REQUIRE(compression::Inflate(packed.View(), unpacked, src.Size()));
                ZX_EXPECT(unpacked == src);
                if (kind == 2 && size > 1000 && level > 0)
                    ZX_EXPECT(packed.Size() < src.Size() / 50);
            }
        }
    }
}

ZX_TEST(Compression, ZlibRoundTripAndChecksum)
{
    const Array<u8> src = MakeTestData(50000, 3);
    Array<u8> z = compression::ZlibCompress(src.View());
    Array<u8> out;
    ZX_REQUIRE(compression::ZlibDecompress(z.View(), out));
    ZX_EXPECT(out == src);
    z[z.Size() - 1] ^= 0xFF; // corrupt Adler-32
    out.Clear();
    ZX_EXPECT(!compression::ZlibDecompress(z.View(), out));
}

ZX_TEST(Compression, InflateReferenceStream)
{
    // zlib.compress(b"Hello, Hello, Hello, ZeqouX3D!") produced by the reference zlib implementation.
    const u8 reference[] = {0x78, 0x9c, 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0xf0, 0x40, 0xa1, 0xa2,
                            0x52, 0x0b, 0xf3, 0x4b, 0x23, 0x8c, 0x5d, 0x14, 0x01, 0x9a, 0xc1, 0x09, 0xc5};
    Array<u8> out;
    ZX_REQUIRE(compression::ZlibDecompress(ByteSpan(reference, sizeof(reference)), out));
    ZX_EXPECT(StringView(reinterpret_cast<const char*>(out.Data()), out.Size()) == "Hello, Hello, Hello, ZeqouX3D!");
}

ZX_TEST(Compression, InflateRejectsGarbage)
{
    Random rng(5);
    int accepted = 0;
    for (int trial = 0; trial < 200; ++trial) {
        u8 junk[64];
        for (u8& b : junk)
            b = u8(rng.NextU32());
        Array<u8> out;
        if (compression::Inflate(ByteSpan(junk, sizeof(junk)), out, 0, 1 << 20))
            ++accepted; // valid-by-chance streams are fine, but must never crash
    }
    ZX_EXPECT(accepted < 200);
}

ZX_TEST(Compression, InflateCorruptStreamTerminates)
{
    // Bit flips in a real stream used to make the decoder emit literals from the zero padding forever.
    Random rng(11);
    Array<u8> text;
    for (u32 i = 0; i < 20000; ++i)
        text.PushBack(u8('a' + (rng.NextU32() % 7)));
    const Array<u8> packed = compression::ZlibCompress(text.View(), 6);
    for (u32 trial = 0; trial < 300; ++trial) {
        Array<u8> bad = packed;
        for (u32 k = 0; k < 4; ++k)
            bad[2 + usize(rng.NextU32()) % (bad.Size() - 6)] ^= u8(1u << (rng.NextU32() & 7));
        Array<u8> out;
        (void)compression::ZlibDecompress(bad.View(), out, text.Size());
        ZX_EXPECT(out.Size() < text.Size() * 4);
    }
}

ZX_TEST(Compression, ZlzRoundTrip)
{
    for (usize size : {usize(0), usize(3), usize(12), usize(1000), usize(65536 * 3 + 17)}) {
        for (int kind = 0; kind < 4; ++kind) {
            const Array<u8> src = MakeTestData(size, kind);
            Array<u8> packed;
            compression::ZlzCompress(src.View(), packed);
            ZX_EXPECT(packed.Size() <= compression::ZlzMaxCompressedSize(size));
            Array<u8> out;
            out.ResizeUninitialized(size);
            ZX_REQUIRE(compression::ZlzDecompress(packed.View(), out.Data(), size));
            ZX_EXPECT(out == src);
            if (kind == 2 && size > 1000)
                ZX_EXPECT(packed.Size() < size / 20);
        }
    }
    // Truncated stream must fail cleanly.
    const Array<u8> src = MakeTestData(5000, 1);
    Array<u8> packed;
    compression::ZlzCompress(src.View(), packed);
    Array<u8> out;
    out.ResizeUninitialized(5000);
    ZX_EXPECT(!compression::ZlzDecompress(packed.View().First(packed.Size() / 2), out.Data(), 5000));
}

ZX_TEST(Hash, GuidAndSha1)
{
    char hex[41];
    Sha1::Hash("abc", 3).ToHex(hex);
    ZX_EXPECT(StringView(hex) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    Sha1::Hash("", 0).ToHex(hex);
    ZX_EXPECT(StringView(hex) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const char* longMsg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha1::Hash(longMsg, StringView(longMsg).Size()).ToHex(hex);
    ZX_EXPECT(StringView(hex) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    const Guid a = Guid::New(), b = Guid::New();
    ZX_EXPECT(a.IsValid());
    ZX_EXPECT(a != b);
    char text[37];
    a.ToChars(text);
    const Optional<Guid> parsed = Guid::Parse(text);
    ZX_REQUIRE(parsed.HasValue());
    ZX_EXPECT(*parsed == a);
    ZX_EXPECT(text[14] == '4'); // version nibble
    ZX_EXPECT(!Guid::Parse("not-a-guid"));
    // RFC 4122 example: v5 of "www.example.com" in the DNS namespace.
    const Guid dns = *Guid::Parse("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
    ZX_EXPECT(Guid::FromName(dns, "www.example.com") == *Guid::Parse("2ed6657d-e927-568b-95e1-2665a8aea6a2"));
    ZX_EXPECT(String::Format("{}", dns) == "6ba7b810-9dad-11d1-80b4-00c04fd430c8");
}

ZX_TEST(String, NameInterning)
{
    const Name a("Position");
    const Name b(String("Position"));
    const Name c("position");
    ZX_EXPECT(a == b);
    ZX_EXPECT(a != c); // case-sensitive
    ZX_EXPECT(a.View() == "Position");
    ZX_EXPECT(StringView(a.CStr()) == "Position");
    ZX_EXPECT(Name().IsNone());
    ZX_EXPECT(Name("").IsNone());
    ZX_EXPECT(Name::Find("Position") == a);
    ZX_EXPECT(Name::Find("NeverInternedName_123").IsNone());
    ZX_EXPECT(String::Format("{}", a) == "Position");
}

ZX_TEST(String, Paths)
{
    ZX_EXPECT(Path::Normalize("a\\b//c/./d/../e/") == "a/b/c/e");
    ZX_EXPECT(Path::Normalize("C:\\Games\\..\\Projects\\X") == "C:/Projects/X");
    ZX_EXPECT(Path::Normalize("/Game/../Engine") == "/Engine");
    ZX_EXPECT(Path::Normalize("../up/x") == "../up/x");
    ZX_EXPECT(Path::Join("/Game", "Textures/Rock.png") == "/Game/Textures/Rock.png");
    ZX_EXPECT(Path::Join("a", "/abs") == "/abs");
    ZX_EXPECT(Path::FileName("dir/file.tar.gz") == "file.tar.gz");
    ZX_EXPECT(Path::Stem("dir/file.tar.gz") == "file.tar");
    ZX_EXPECT(Path::Extension("dir/file.tar.gz") == ".gz");
    ZX_EXPECT(Path::Extension("dir/.hidden").Empty());
    ZX_EXPECT(Path::Parent("dir/sub/file") == "dir/sub");
    ZX_EXPECT(Path::Parent("C:/file") == "C:/");
    ZX_EXPECT(Path::ChangeExtension("a/b.png", "zasset") == "a/b.zasset");
    ZX_EXPECT(Path::HasExtension("a/B.PNG", "png"));
    ZX_EXPECT(Path::IsUnder("/Game/Maps/A.zscene", "/Game"));
    ZX_EXPECT(!Path::IsUnder("/GameX/A", "/Game"));
    ZX_EXPECT(Path::MakeRelative("C:/P/Content/Tex/a.png", "C:/P/Content") == "Tex/a.png");
    ZX_EXPECT(Path::SanitizeFileName("a:b?c") == "a_b_c");
}

#include "Core/HAL/PlatformFile.h"

// Writes zlib streams produced by our encoder for cross-checking with an external zlib:
//   ZXTests ManualCompression.* && python -c "import zlib; ..."
ZX_TEST(ManualCompression, DumpZlibStreams)
{
    const String dir = platform::GetTempDirectory() + "/zx_deflate_check";
    platform::CreateDirectoryTree(dir);
    for (int kind = 0; kind < 4; ++kind) {
        for (i32 level : {1, 6, 9}) {
            const Array<u8> src = MakeTestData(200000, kind);
            const Array<u8> z = compression::ZlibCompress(src.View(), level);
            platform::File f;
            ZX_REQUIRE(f.Open(String::Format("{}/k{}_l{}.raw", dir, kind, level), platform::FileMode::Write));
            f.Write(src.Data(), src.Size());
            platform::File g;
            ZX_REQUIRE(g.Open(String::Format("{}/k{}_l{}.zlib", dir, kind, level), platform::FileMode::Write));
            g.Write(z.Data(), z.Size());
        }
    }
}
