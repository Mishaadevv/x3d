#include "Core/Serialization/Binary.h"
#include "Core/Serialization/Json.h"
#include "Core/Testing/Test.h"

using namespace zx;

ZX_TEST(Json, ParseBasics)
{
    JsonValue v;
    JsonParseError err;
    const char* text = R"({
        // comment
        "name": "Sponza", /* block */
        count: 42,
        "big": 18446744073709551615,
        "neg": -9223372036854775808,
        "pi": 3.25,
        "exp": -1.5e-3,
        "flags": [true, false, null,],
        "nested": { "a": [1, [2, [3]]] },
        "esc": "line\nquote\"tab\té🎮",
        "ru": "Привет",
    })";
    ZX_REQUIRE(ParseJson(text, v, &err));
    ZX_EXPECT(v.IsObject());
    ZX_EXPECT(v.Get("name").AsString() == "Sponza");
    ZX_EXPECT_EQ(v.Get("count").AsInt(), 42);
    ZX_EXPECT(v.Get("count").IsInteger());
    ZX_EXPECT_EQ(v.Get("big").AsU64(), U64Max);
    ZX_EXPECT_EQ(v.Get("neg").AsInt(), I64Min);
    ZX_EXPECT_NEAR(v.Get("pi").AsDouble(), 3.25, 1e-12);
    ZX_EXPECT_NEAR(v.Get("exp").AsDouble(), -1.5e-3, 1e-12);
    ZX_EXPECT_EQ(v.Get("flags").Size(), 3u);
    ZX_EXPECT(v.Get("flags")[2].IsNull());
    ZX_EXPECT_EQ(v.Get("nested").Get("a")[1][1][0].AsInt(), 3);
    ZX_EXPECT(v.Get("esc").AsString() == "line\nquote\"tab\t\xc3\xa9\xf0\x9f\x8e\xae");
    ZX_EXPECT(v.Get("ru").AsString() == "Привет");
    ZX_EXPECT(v.Get("missing").IsNull());
    // Member order preserved.
    ZX_EXPECT(v.Members()[0].first == "name");
    ZX_EXPECT(v.Members()[1].first == "count");
}

ZX_TEST(Json, Errors)
{
    JsonValue v;
    JsonParseError err;
    ZX_EXPECT(!ParseJson("{\"a\": }", v, &err));
    ZX_EXPECT_EQ(err.line, 1u);
    ZX_EXPECT(!err.message.Empty());
    ZX_EXPECT(!ParseJson("{\n\n  \"a\": tru }", v, &err));
    ZX_EXPECT_EQ(err.line, 3u);
    ZX_EXPECT(!ParseJson("[1, 2", v, &err));
    ZX_EXPECT(!ParseJson("\"unterminated", v, &err));
    ZX_EXPECT(!ParseJson("{} extra", v, &err));
    ZX_EXPECT(!ParseJson("/* open", v, &err));
    String deep(2000, '[');
    ZX_EXPECT(!ParseJson(deep, v, &err)); // depth limit, no stack overflow
}

ZX_TEST(Json, BuildWriteRoundTrip)
{
    JsonValue root;
    root["version"] = 3;
    root["scale"] = 1.5f;
    root["whole"] = 2.0;
    root["title"] = "Hello \"World\"\n";
    root["tags"].PushBack("a");
    root["tags"].PushBack("b");
    JsonValue& pos = root["position"];
    pos.PushBack(1.0);
    pos.PushBack(2.5);
    pos.PushBack(-3.0);
    root["empty"] = JsonValue::MakeObject();
    root["child"]["deep"]["value"] = true;
    ZX_EXPECT(root.Remove("empty"));
    ZX_EXPECT(!root.Contains("empty"));

    for (bool pretty : {true, false}) {
        JsonWriteOptions opt;
        opt.pretty = pretty;
        const String text = ToJsonString(root, opt);
        JsonValue back;
        ZX_REQUIRE(ParseJson(text, back));
        ZX_EXPECT(back == root);
        ZX_EXPECT(back.Get("whole").IsNumber() && !back.Get("whole").IsInteger()); // "2.0" stays float
    }
    const String compact = ToJsonString(root["position"], JsonWriteOptions{.pretty = false});
    ZX_EXPECT(compact == "[1.0,2.5,-3.0]");

    // Doubles round-trip exactly.
    JsonValue d(0.1 + 0.2);
    JsonValue d2;
    ZX_REQUIRE(ParseJson(ToJsonString(d), d2));
    ZX_EXPECT_EQ(d2.AsDouble(), 0.1 + 0.2);
}

ZX_TEST(Binary, WriterReader)
{
    Array<u8> buffer;
    BinaryWriter w(buffer);
    w.Write<u32>(0xDEADBEEF);
    w.WriteBool(true);
    w.WriteVarU64(300);
    w.WriteVarI64(-5);
    w.WriteString("строка");
    const usize patchAt = w.Reserve<u32>();
    const f32 floats[] = {1.0f, 2.0f, 3.0f};
    w.WriteArray(Span<const f32>(floats));
    w.Align(16);
    w.Patch<u32>(patchAt, 77);
    ZX_EXPECT_EQ(buffer.Size() % 16, 0u);

    BinaryReader r(buffer.View());
    ZX_EXPECT_EQ(r.Read<u32>(), 0xDEADBEEFu);
    ZX_EXPECT(r.ReadBool());
    ZX_EXPECT_EQ(r.ReadVarU64(), 300u);
    ZX_EXPECT_EQ(r.ReadVarI64(), -5);
    ZX_EXPECT(r.ReadStringView() == "строка");
    ZX_EXPECT_EQ(r.Read<u32>(), 77u);
    Array<f32> back;
    ZX_EXPECT(r.ReadArray(back));
    ZX_EXPECT_EQ(back.Size(), 3u);
    ZX_EXPECT_EQ(back[2], 3.0f);
    r.Align(16);
    ZX_EXPECT(r.AtEnd());
    ZX_EXPECT(!r.HasError());

    // Overflow -> error state, zeros, no crash.
    ZX_EXPECT_EQ(r.Read<u64>(), 0u);
    ZX_EXPECT(r.HasError());
    BinaryReader bad(ByteSpan(buffer.Data(), 3));
    Array<f32> huge;
    bad.Seek(0);
    ZX_EXPECT(!bad.ReadArray(huge));
}
