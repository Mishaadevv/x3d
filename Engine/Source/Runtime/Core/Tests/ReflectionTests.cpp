#include "Core/Math/Math.h"
#include "Core/Reflection/ReflectionSerialization.h"
#include "Core/Testing/Test.h"

using namespace zx;

namespace reftest {

enum class LightType : u8
{
    Directional,
    Point,
    Spot
};
ZX_DECLARE_ENUM(LightType);

enum class Features : u32
{
    None = 0,
    Shadows = 1,
    Volumetric = 2,
    Cookie = 4
};
ZX_DECLARE_ENUM(Features);

struct BaseComponent
{
    ZX_REFLECT_STRUCT(BaseComponent)
    bool enabled = true;
    Name tag;
};

struct LightComponent : BaseComponent
{
    ZX_REFLECT_STRUCT(LightComponent)
    LinearColor color = LinearColor::White();
    f32 intensity = 10.0f;
    LightType type = LightType::Point;
    Features features = Features::Shadows;
    Vec3 direction = Vec3::Forward();
    Array<f32> cascadeSplits;
    String label = "Light";
    i32 runtimeCounter = 0; // transient
    Guid id;

    f32 GetScaledIntensity(f32 scale) const { return intensity * scale; }
    void SetIntensity(f32 v) { intensity = v; }
};

struct Node
{
    ZX_REFLECT_STRUCT(Node)
    String name;
    Array<Node> children; // self-referential through Array
    Transform transform;
};

struct Renamed
{
    ZX_REFLECT_STRUCT(Renamed)
    i32 newName = 0;

private:
    i32 m_hidden = 7; // private fields are accessible to the registration

public:
    i32 GetHidden() const { return m_hidden; }
};

ZX_REFLECT_ENUM_BEGIN(LightType)
    ZX_ENUM_VALUE(Directional)
    ZX_ENUM_VALUE(Point)
    ZX_ENUM_VALUE(Spot, Meta::DisplayName("Spot Light"))
ZX_REFLECT_ENUM_END()

ZX_REFLECT_ENUM_BEGIN(Features)
    ZX_ENUM_FLAGS_TYPE()
    ZX_ENUM_VALUE(None)
    ZX_ENUM_VALUE(Shadows)
    ZX_ENUM_VALUE(Volumetric)
    ZX_ENUM_VALUE(Cookie)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(BaseComponent)
    ZX_FIELD(enabled)
    ZX_FIELD(tag)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(LightComponent)
    ZX_BASE(BaseComponent)
    ZX_FIELD(color, Meta::Color(), Meta::HDR())
    ZX_FIELD(intensity, Meta::Range(0, 100000), Meta::Units("lm"), Meta::Tooltip("Luminous power"))
    ZX_FIELD(type)
    ZX_FIELD(features)
    ZX_FIELD(direction)
    ZX_FIELD(cascadeSplits)
    ZX_FIELD(label)
    ZX_FIELD(runtimeCounter, Meta::Transient())
    ZX_FIELD(id)
    ZX_METHOD(GetScaledIntensity, "scale")
    ZX_METHOD(SetIntensity)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(Node)
    ZX_FIELD(name)
    ZX_FIELD(children)
    ZX_FIELD(transform)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(Renamed)
    ZX_FIELD(newName, Meta::FormerName("oldName"))
    ZX_FIELD(m_hidden)
ZX_REFLECT_STRUCT_END()

} // namespace reftest

using namespace reftest;

ZX_TEST(Reflection, TypeInfoBasics)
{
    const TypeInfo& t = TypeOf<LightComponent>();
    ZX_EXPECT(t.name == Name("LightComponent"));
    ZX_EXPECT(t.IsStruct());
    ZX_EXPECT_EQ(t.size, u32(sizeof(LightComponent)));
    ZX_EXPECT(t.base == &TypeOf<BaseComponent>());
    ZX_EXPECT(t.IsA(TypeOf<BaseComponent>()));
    ZX_EXPECT(!TypeOf<BaseComponent>().IsA(t));
    ZX_EXPECT(Reflection::FindType("LightComponent") == &t);
    ZX_EXPECT(Reflection::FindType("f32") == &TypeOf<f32>());

    const FieldInfo* intensity = t.FindField(Name("intensity"));
    ZX_REQUIRE(intensity);
    ZX_EXPECT(intensity->type == &TypeOf<f32>());
    ZX_EXPECT_EQ(intensity->offset, u32(offsetof(LightComponent, intensity)));
    ZX_EXPECT(intensity->meta.GetText(Meta::KeyUnits()) == "lm");
    ZX_EXPECT_EQ(intensity->meta.Find(Meta::KeyRange())->number2, 100000.0);
    ZX_EXPECT(t.FindField(Name("enabled")) != nullptr); // inherited
    ZX_EXPECT(t.FindField(Name("runtimeCounter"))->IsTransient());
    ZX_EXPECT(t.FindField(Name("cascadeSplits"))->type->IsArray());
    ZX_EXPECT(t.FindField(Name("cascadeSplits"))->type->element == &TypeOf<f32>());

    LightComponent light;
    *static_cast<f32*>(intensity->Ptr(&light)) = 42.0f;
    ZX_EXPECT_EQ(light.intensity, 42.0f);

    int fieldCount = 0;
    t.ForEachField([&](const FieldInfo&) { ++fieldCount; });
    ZX_EXPECT_EQ(fieldCount, 11);
    ZX_EXPECT(Reflection::GetDerivedTypes(TypeOf<BaseComponent>()).Contains(&t));

    // Self-referential type resolved without deadlock.
    ZX_EXPECT(TypeOf<Node>().FindField(Name("children"))->type->element == &TypeOf<Node>());
    ZX_EXPECT(TypeOf<Vec3>().fields.Size() == 3);
}

ZX_TEST(Reflection, EnumsAndDisplayNames)
{
    const TypeInfo& e = TypeOf<LightType>();
    ZX_EXPECT(e.IsEnum());
    ZX_EXPECT_EQ(e.enumEntries.Size(), 3u);
    ZX_EXPECT(e.enumEntries[2].displayName == "Spot Light");
    ZX_EXPECT(Reflection::EnumToString(e, 1) == "Point");
    i64 v = -1;
    ZX_EXPECT(Reflection::EnumFromString(e, "spot", v));
    ZX_EXPECT_EQ(v, 2);
    const TypeInfo& f = TypeOf<Features>();
    ZX_EXPECT(Reflection::EnumToString(f, 5) == "Shadows|Cookie");
    ZX_EXPECT(Reflection::EnumFromString(f, "Volumetric | Cookie", v));
    ZX_EXPECT_EQ(v, 6);

    ZX_EXPECT(Reflection::MakeDisplayName("m_castShadows") == "Cast Shadows");
    ZX_EXPECT(Reflection::MakeDisplayName("bEnabled") == "Enabled");
    ZX_EXPECT(Reflection::MakeDisplayName("HDRIntensity") == "HDR Intensity");
    ZX_EXPECT(Reflection::MakeDisplayName("uv0Scale") == "Uv 0 Scale");
    ZX_EXPECT(Reflection::MakeDisplayName("max_speed") == "Max Speed");
}

ZX_TEST(Reflection, Methods)
{
    const TypeInfo& t = TypeOf<LightComponent>();
    const MethodInfo* get = t.FindMethod(Name("GetScaledIntensity"));
    ZX_REQUIRE(get);
    ZX_EXPECT(get->isConst);
    ZX_EXPECT(get->returnType == &TypeOf<f32>());
    ZX_EXPECT_EQ(get->parameterTypes.Size(), 1u);
    ZX_EXPECT(get->parameterNames[0] == Name("scale"));
    LightComponent light;
    f32 scale = 3.0f;
    void* args[] = {&scale};
    f32 result = 0.0f;
    get->invoke(&light, args, &result);
    ZX_EXPECT_EQ(result, 30.0f);
    const MethodInfo* set = t.FindMethod(Name("SetIntensity"));
    f32 newValue = 5.0f;
    void* args2[] = {&newValue};
    set->invoke(&light, args2, nullptr);
    ZX_EXPECT_EQ(light.intensity, 5.0f);
    ZX_EXPECT(set->returnType == &TypeOf<void>());
}

ZX_TEST(Reflection, JsonRoundTripAndDelta)
{
    LightComponent a;
    a.enabled = false;
    a.tag = Name("Sun");
    a.color = LinearColor(1.0f, 0.5f, 0.25f, 1.0f);
    a.intensity = 123.5f;
    a.type = LightType::Spot;
    a.features = Features(3);
    a.direction = Vec3(0, -1, 0);
    a.cascadeSplits = {0.1f, 0.3f, 1.0f};
    a.label = "Главный свет";
    a.runtimeCounter = 99;
    a.id = Guid::New();

    const JsonValue full = Reflection::ToJson(a);
    ZX_EXPECT(full.Get("type").AsString() == "Spot");
    ZX_EXPECT(full.Get("features").AsString() == "Shadows|Volumetric");
    ZX_EXPECT(full.Get("direction").IsArray()); // compact Vec3
    ZX_EXPECT(!full.Contains("runtimeCounter"));

    const String text = ToJsonString(full);
    JsonValue parsed;
    ZX_REQUIRE(ParseJson(text, parsed));
    LightComponent b;
    Array<String> warnings;
    ZX_EXPECT(Reflection::FromJson(parsed, b, &warnings));
    ZX_EXPECT(warnings.Empty());
    b.runtimeCounter = 99;
    ZX_EXPECT(Reflection::ValuesEqual(&a, &b, TypeOf<LightComponent>()));
    ZX_EXPECT(b.label == "Главный свет");

    // Delta: only changed fields are written.
    LightComponent c;
    c.intensity = 1.0f;
    const JsonValue delta = Reflection::ToJson(c, true);
    ZX_EXPECT_EQ(delta.Size(), 1u); // only `intensity` differs from the defaults
    ZX_EXPECT(delta.Contains("intensity"));
}

ZX_TEST(Reflection, TolerantReadingAndRenames)
{
    JsonValue j;
    ZX_REQUIRE(ParseJson(R"({ "oldName": 5, "unknownField": 1, "m_hidden": 3 })", j));
    Renamed r;
    Array<String> warnings;
    Reflection::FromJson(j, r, &warnings);
    ZX_EXPECT_EQ(r.newName, 5);
    ZX_EXPECT_EQ(r.GetHidden(), 3);
    ZX_EXPECT_EQ(warnings.Size(), 1u);

    // Object form for compact struct is accepted too.
    JsonValue v;
    ZX_REQUIRE(ParseJson(R"({ "x": 1, "y": 2, "z": 3 })", v));
    Vec3 vec;
    ZX_EXPECT(Reflection::FromJson(v, vec));
    ZX_EXPECT(vec == Vec3(1, 2, 3));
}

ZX_TEST(Reflection, BinaryAndNested)
{
    Node root;
    root.name = "root";
    root.transform.position = Vec3(1, 2, 3);
    Node child;
    child.name = "child";
    child.children.PushBack(Node{"grandchild", {}, Transform()});
    root.children.PushBack(child);

    Array<u8> bytes;
    BinaryWriter w(bytes);
    Reflection::WriteBinary(w, &root, TypeOf<Node>());
    BinaryReader r(bytes.View());
    Node back;
    ZX_EXPECT(Reflection::ReadBinary(r, &back, TypeOf<Node>()));
    ZX_EXPECT(r.AtEnd());
    ZX_EXPECT(Reflection::ValuesEqual(&root, &back, TypeOf<Node>()));
    ZX_EXPECT(back.children[0].children[0].name == "grandchild");

    // Truncated data fails gracefully.
    BinaryReader truncated(bytes.View().First(bytes.Size() / 2));
    Node partial;
    ZX_EXPECT(!Reflection::ReadBinary(truncated, &partial, TypeOf<Node>()));

    const JsonValue j = Reflection::ToJson(root);
    Node fromJson;
    ZX_EXPECT(Reflection::FromJson(j, fromJson));
    ZX_EXPECT(Reflection::ValuesEqual(&root, &fromJson, TypeOf<Node>()));

    ZX_EXPECT(Reflection::ComputeSchemaHash(TypeOf<Node>()) != Reflection::ComputeSchemaHash(TypeOf<LightComponent>()));
    ZX_EXPECT_EQ(Reflection::ComputeSchemaHash(TypeOf<Node>()), Reflection::ComputeSchemaHash(TypeOf<Node>()));
}

ZX_TEST(Reflection, TextConversion)
{
    f32 f = 0;
    ZX_EXPECT(Reflection::ValueFromString("2.5", &f, TypeOf<f32>()));
    ZX_EXPECT_EQ(f, 2.5f);
    LightType lt = LightType::Point;
    ZX_EXPECT(Reflection::ValueFromString("Directional", &lt, TypeOf<LightType>()));
    ZX_EXPECT(lt == LightType::Directional);
    ZX_EXPECT(Reflection::ValueToString(&lt, TypeOf<LightType>()) == "Directional");
    Vec3 v;
    ZX_EXPECT(Reflection::ValueFromString("[1, 2, 3]", &v, TypeOf<Vec3>()));
    ZX_EXPECT(Reflection::ValueToString(&v, TypeOf<Vec3>()) == "[1.0,2.0,3.0]");
    i16 small = 0;
    ZX_EXPECT(Reflection::ValueFromString("-12", &small, TypeOf<i16>()));
    ZX_EXPECT_EQ(small, i16(-12));
}
