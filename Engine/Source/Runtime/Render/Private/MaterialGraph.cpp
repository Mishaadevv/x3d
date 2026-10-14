#include "Render/MaterialGraph.h"
#include "Core/Containers/HashMap.h"
#include "Core/Hash/Hash.h"

namespace zx::render {

namespace {

// --- Node table --------------------------------------------------------------------------------------------------------

const MaterialPinDesc OutputInputs[] = {
    {"Base Color", 3, Vec4(0.8f, 0.8f, 0.8f, 1)}, {"Metallic", 1, Vec4(0)}, {"Roughness", 1, Vec4(0.5f)},
    {"Normal", 3, Vec4(0, 0, 1, 0)},              {"Emissive", 3, Vec4(0)},  {"Occlusion", 1, Vec4(1)},
    {"Opacity", 1, Vec4(1)},                      {"Opacity Mask", 1, Vec4(1)}, {"Subsurface", 3, Vec4(0)},
};
const MaterialPinDesc ScalarOut[] = {{"Value", 1, {}}};
const MaterialPinDesc ColorOut[] = {{"RGB", 3, {}}, {"R", 1, {}}, {"G", 1, {}}, {"B", 1, {}}, {"A", 1, {}}, {"RGBA", 4, {}}};
const MaterialPinDesc TextureIn[] = {{"UV", 2, Vec4(0)}};
const MaterialPinDesc TexCoordOut[] = {{"UV", 2, {}}};
const MaterialPinDesc TimeOut[] = {{"Time", 1, {}}};
const MaterialPinDesc VertexColorOut[] = {{"RGB", 3, {}}, {"A", 1, {}}};
const MaterialPinDesc Vec3Out[] = {{"XYZ", 3, {}}};
const MaterialPinDesc PannerIn[] = {{"UV", 2, Vec4(0)}, {"Speed", 2, Vec4(0.1f, 0, 0, 0)}};
const MaterialPinDesc DynOut[] = {{"Result", 0, {}}};
const MaterialPinDesc OneOut[] = {{"Result", 1, {}}};
const MaterialPinDesc AddIn[] = {{"A", 0, Vec4(0)}, {"B", 0, Vec4(0)}};
const MaterialPinDesc MulIn[] = {{"A", 0, Vec4(1)}, {"B", 0, Vec4(1)}};
const MaterialPinDesc LerpIn[] = {{"A", 0, Vec4(0)}, {"B", 0, Vec4(1)}, {"Alpha", 1, Vec4(0.5f)}};
const MaterialPinDesc XIn[] = {{"X", 0, Vec4(0)}};
const MaterialPinDesc PowerIn[] = {{"Base", 0, Vec4(1)}, {"Exponent", 1, Vec4(2)}};
const MaterialPinDesc ClampIn[] = {{"X", 0, Vec4(0)}, {"Min", 1, Vec4(0)}, {"Max", 1, Vec4(1)}};
const MaterialPinDesc AppendIn[] = {{"A", 0, Vec4(0)}, {"B", 0, Vec4(0)}};
const MaterialPinDesc DesaturateIn[] = {{"Color", 3, Vec4(1)}, {"Fraction", 1, Vec4(1)}};
const MaterialPinDesc Vec3OutResult[] = {{"Result", 3, {}}};
const MaterialPinDesc FresnelIn[] = {{"Exponent", 1, Vec4(5)}, {"Base Reflect", 1, Vec4(0.04f)}};
const MaterialPinDesc MaskIn[] = {{"X", 4, Vec4(0)}};
const MaterialPinDesc GridIn[] = {{"Cell Size", 1, Vec4(1)}, {"Line Width", 1, Vec4(0.03f)}};
const MaterialPinDesc GridOut[] = {{"Lines", 1, {}}, {"Checker", 1, {}}};
const MaterialPinDesc NoiseIn[] = {{"Position", 3, Vec4(0)}, {"Scale", 1, Vec4(1)}};
const MaterialPinDesc ParallaxIn[] = {{"UV", 2, Vec4(0)}, {"Height", 1, Vec4(0.04f)}, {"Steps", 1, Vec4(16)}};

#define ZX_PINS(a) Span<const MaterialPinDesc>(a, ZX_ARRAY_COUNT(a))
const Span<const MaterialPinDesc> NoPins;

const MaterialNodeType NodeTypes[] = {
    {"Output", "Material Output", "Output", MaterialNodeKind::Output, ZX_PINS(OutputInputs), NoPins, "Final surface properties"},
    {"Scalar", "Scalar", "Values", MaterialNodeKind::Value, NoPins, ZX_PINS(ScalarOut), "A number (named = material parameter)"},
    {"Color", "Color", "Values", MaterialNodeKind::Value, NoPins, ZX_PINS(ColorOut), "A color / vector (named = material parameter)"},
    {"TextureSample", "Texture Sample", "Textures", MaterialNodeKind::Texture, ZX_PINS(TextureIn), ZX_PINS(ColorOut),
     "Samples a texture (UV defaults to the mesh's first UV set)"},
    {"ParallaxOcclusion", "Parallax Occlusion", "Textures", MaterialNodeKind::Texture, ZX_PINS(ParallaxIn), ZX_PINS(TexCoordOut),
     "Relief from a height map (white = high): offsets the UV the other texture samples use"},
    {"TexCoord", "Texture Coordinate", "Inputs", MaterialNodeKind::Input, NoPins, ZX_PINS(TexCoordOut), "Mesh UV (value: tiling U, V; set)"},
    {"Time", "Time", "Inputs", MaterialNodeKind::Input, NoPins, ZX_PINS(TimeOut), "Seconds since start"},
    {"VertexColor", "Vertex Color", "Inputs", MaterialNodeKind::Input, NoPins, ZX_PINS(VertexColorOut), "Mesh vertex color"},
    {"WorldPosition", "World Position", "Inputs", MaterialNodeKind::Input, NoPins, ZX_PINS(Vec3Out), "Pixel position (m)"},
    {"CameraVector", "Camera Vector", "Inputs", MaterialNodeKind::Input, NoPins, ZX_PINS(Vec3Out), "Direction from the pixel to the camera"},
    {"VertexNormal", "Vertex Normal", "Inputs", MaterialNodeKind::Input, NoPins, ZX_PINS(Vec3Out), "Interpolated world-space normal"},
    {"Panner", "Panner", "Utility", MaterialNodeKind::Utility, ZX_PINS(PannerIn), ZX_PINS(TexCoordOut), "UV + Speed * Time"},
    {"Fresnel", "Fresnel", "Utility", MaterialNodeKind::Utility, ZX_PINS(FresnelIn), ZX_PINS(OneOut), "Schlick Fresnel term (edges -> 1)"},
    {"WorldGrid", "World Grid", "Utility", MaterialNodeKind::Utility, ZX_PINS(GridIn), ZX_PINS(GridOut),
     "World-aligned grid lines and checker (no UVs needed; prototype surfaces)"},
    {"Noise", "Noise", "Utility", MaterialNodeKind::Utility, ZX_PINS(NoiseIn), ZX_PINS(OneOut),
     "Fractal noise 0..1 (Position defaults to the world position)"},
    {"Desaturate", "Desaturate", "Utility", MaterialNodeKind::Utility, ZX_PINS(DesaturateIn), ZX_PINS(Vec3OutResult), "Blends towards grey"},
    {"ComponentMask", "Component Mask", "Utility", MaterialNodeKind::Utility, ZX_PINS(MaskIn), ZX_PINS(DynOut),
     "Keeps the channels enabled in the node value (R, G, B, A)"},
    {"Append", "Append", "Utility", MaterialNodeKind::Utility, ZX_PINS(AppendIn), ZX_PINS(DynOut), "Concatenates A and B (max 4 channels)"},
    {"Add", "Add", "Math", MaterialNodeKind::Math, ZX_PINS(AddIn), ZX_PINS(DynOut), "A + B"},
    {"Subtract", "Subtract", "Math", MaterialNodeKind::Math, ZX_PINS(AddIn), ZX_PINS(DynOut), "A - B"},
    {"Multiply", "Multiply", "Math", MaterialNodeKind::Math, ZX_PINS(MulIn), ZX_PINS(DynOut), "A * B"},
    {"Divide", "Divide", "Math", MaterialNodeKind::Math, ZX_PINS(MulIn), ZX_PINS(DynOut), "A / B"},
    {"Lerp", "Lerp", "Math", MaterialNodeKind::Math, ZX_PINS(LerpIn), ZX_PINS(DynOut), "A + (B - A) * Alpha"},
    {"OneMinus", "One Minus", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(DynOut), "1 - X"},
    {"Power", "Power", "Math", MaterialNodeKind::Math, ZX_PINS(PowerIn), ZX_PINS(DynOut), "Base ^ Exponent"},
    {"Clamp", "Clamp", "Math", MaterialNodeKind::Math, ZX_PINS(ClampIn), ZX_PINS(DynOut), "Clamps X to [Min, Max]"},
    {"Saturate", "Saturate", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(DynOut), "Clamps X to [0, 1]"},
    {"Abs", "Abs", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(DynOut), "|X|"},
    {"Floor", "Floor", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(DynOut), "Rounds down"},
    {"Frac", "Frac", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(DynOut), "Fractional part"},
    {"Sine", "Sine", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(DynOut), "sin(X) (radians)"},
    {"Cosine", "Cosine", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(DynOut), "cos(X) (radians)"},
    {"Min", "Min", "Math", MaterialNodeKind::Math, ZX_PINS(AddIn), ZX_PINS(DynOut), "min(A, B)"},
    {"Max", "Max", "Math", MaterialNodeKind::Math, ZX_PINS(AddIn), ZX_PINS(DynOut), "max(A, B)"},
    {"Dot", "Dot", "Math", MaterialNodeKind::Math, ZX_PINS(AddIn), ZX_PINS(OneOut), "dot(A, B)"},
    {"Normalize", "Normalize", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(DynOut), "X / |X|"},
    {"Length", "Length", "Math", MaterialNodeKind::Math, ZX_PINS(XIn), ZX_PINS(OneOut), "|X|"},
};
#undef ZX_PINS

const char* TypeName(u32 dims)
{
    static const char* names[5] = {"float", "float", "float2", "float3", "float4"};
    return names[Min(dims, 4u)];
}

String Lit(f32 v)
{
    if (!(v == v) || v > 1e30f || v < -1e30f)
        v = 0.0f;
    String s = String::Format("{}", v);
    if (s.View().FindFirstOf(".eE") == StringView::NPos)
        s += ".0";
    return s;
}

String Literal(Vec4 v, u32 dims)
{
    if (dims <= 1)
        return Lit(v.x);
    String s = String::Format("{}({}", TypeName(dims), Lit(v.x));
    const f32 c[4] = {v.x, v.y, v.z, v.w};
    for (u32 i = 1; i < dims; ++i)
        s += String::Format(", {}", Lit(c[i]));
    return s + ")";
}

struct Expr
{
    String code;
    u32 dims = 1;
};

String Convert(const Expr& e, u32 to)
{
    if (to == 0 || e.dims == to)
        return e.code;
    if (e.dims == 1)
        return String::Format("(({})({}))", TypeName(to), e.code);
    if (e.dims > to) {
        static const char* sw[5] = {"", "x", "xy", "xyz", "xyzw"};
        return String::Format("({}).{}", e.code, sw[to]);
    }
    String s = String::Format("{}({}", TypeName(to), e.code);
    for (u32 i = e.dims; i < to; ++i)
        s += i == 3 ? ", 1.0" : ", 0.0";
    return s + ")";
}

u64 Key(u32 node, u32 pin) { return (u64(node) << 8) | pin; }

class Compiler
{
public:
    Compiler(const MaterialGraph& g, CompiledMaterialGraph& out) : m_graph(g), m_out(out) {}

    void Run()
    {
        const u32 output = m_graph.OutputNode();
        if (!output) {
            Error(0, "The graph has no Material Output node");
            return;
        }
        // Parameter slots for every value/texture node (even unconnected ones, so the block layout is stable).
        for (const MaterialGraphNode& n : m_graph.nodes) {
            const MaterialNodeType* t = FindMaterialNodeType(n.type);
            if (t && (t->kind == MaterialNodeKind::Value || t->kind == MaterialNodeKind::Texture)) {
                MaterialParamSlot slot;
                slot.kind = t->kind == MaterialNodeKind::Texture ? MaterialParamSlot::Kind::Texture : MaterialParamSlot::Kind::Float4;
                slot.node = n.id;
                slot.offset = m_out.paramBytes;
                m_out.paramBytes += 16;
                m_slotOf.InsertOrAssign(n.id, slot.offset);
                m_out.params.PushBack(slot);
            }
        }
        Visit(output);
        String code;
        code += "// Generated by the ZeqouX3D material graph compiler. Do not edit.\n";
        code += "#include \"/Shaders/Material/MaterialGraph.hlsli\"\n\n";
        code += "MaterialOutputs EvaluateGraphMaterial(MaterialInputs mi)\n{\n    MaterialOutputs o = DefaultMaterialOutputs();\n";
        code += m_body;
        code += "    return o;\n}\n";
        m_out.code = move(code);
    }

private:
    void Error(u32 node, StringView message)
    {
        m_out.errors.PushBack(node ? String::Format("Node {}: {}", node, message) : String(message));
        if (node && !m_out.errorNodes.Contains(node))
            m_out.errorNodes.PushBack(node);
    }

    Expr Input(const MaterialGraphNode& n, const MaterialNodeType& t, u32 pin)
    {
        if (const MaterialGraphLink* link = m_graph.FindInputLink(n.id, pin))
            if (const Expr* e = m_values.Find(Key(link->fromNode, link->fromPin)))
                return *e;
        const MaterialPinDesc& p = t.inputs[pin];
        const Vec4 v = pin < n.inputValues.Size() ? n.inputValues[pin] : p.defaultValue;
        const u32 dims = p.dims == 0 ? 1 : p.dims;
        return Expr{Literal(v, dims), dims};
    }
    bool Connected(const MaterialGraphNode& n, u32 pin) const { return m_graph.FindInputLink(n.id, pin) != nullptr; }

    void Visit(u32 id)
    {
        u8& state = *m_state.Insert(id, 0).first;
        if (state == 2)
            return;
        if (state == 1) {
            Error(id, "is part of a cycle");
            return;
        }
        state = 1;
        for (const MaterialGraphLink& l : m_graph.links)
            if (l.toNode == id)
                Visit(l.fromNode);
        *m_state.Find(id) = 2;
        Emit(id);
    }

    void Set(u32 node, u32 pin, String code, u32 dims) { m_values.InsertOrAssign(Key(node, pin), Expr{move(code), dims}); }

    /// Declares a local for the node's main value and returns its name.
    String Local(const MaterialGraphNode& n, u32 dims, StringView code)
    {
        const String name = String::Format("n{}", n.id);
        m_body += String::Format("    {} {} = {};\n", TypeName(dims), name, code);
        return name;
    }

    void EmitColorOutputs(const MaterialGraphNode& n, const String& v4)
    {
        Set(n.id, 0, String::Format("{}.rgb", v4), 3);
        Set(n.id, 1, String::Format("{}.r", v4), 1);
        Set(n.id, 2, String::Format("{}.g", v4), 1);
        Set(n.id, 3, String::Format("{}.b", v4), 1);
        Set(n.id, 4, String::Format("{}.a", v4), 1);
        Set(n.id, 5, v4, 4);
    }

    void Emit(u32 id)
    {
        const MaterialGraphNode* np = m_graph.FindNode(id);
        if (!np)
            return;
        const MaterialGraphNode& n = *np;
        const MaterialNodeType* tp = FindMaterialNodeType(n.type);
        if (!tp) {
            Error(id, String::Format("unknown node type '{}'", n.type));
            return;
        }
        const MaterialNodeType& t = *tp;
        const StringView type = t.type;
        const u32* slot = m_slotOf.Find(id);

        auto in = [&](u32 pin) { return Input(n, t, pin); };
        auto widest = [&](std::initializer_list<u32> pins) {
            u32 d = 1;
            for (u32 p : pins)
                d = Max(d, in(p).dims);
            return d;
        };

        if (type == "Output") {
            static const char* fields[9] = {"baseColor", "metallic", "roughness", "normal", "emissive", "occlusion", "opacity", "opacityMask",
                                            "subsurface"};
            for (u32 p = 0; p < 9; ++p)
                if (Connected(n, p) || (p < n.inputValues.Size() && !(n.inputValues[p] == t.inputs[p].defaultValue)))
                    m_body += String::Format("    o.{} = {};\n", fields[p], Convert(in(p), t.inputs[p].dims));
        } else if (type == "Scalar") {
            Set(id, 0, Local(n, 1, String::Format("ParamFloat4(mi, {}).x", *slot)), 1);
        } else if (type == "Color") {
            EmitColorOutputs(n, Local(n, 4, String::Format("ParamFloat4(mi, {})", *slot)));
        } else if (type == "TextureSample") {
            const String uv = Connected(n, 0) ? Convert(in(0), 2) : String("mi.uv0");
            const String fn = n.normalMap ? "SampleParamNormalMap" : "SampleParamTexture";
            EmitColorOutputs(n, Local(n, 4, String::Format("{}(mi, {}, {})", fn, *slot, uv)));
        } else if (type == "ParallaxOcclusion") {
            const String uv = Connected(n, 0) ? Convert(in(0), 2) : String("mi.uv0");
            Set(id, 0, Local(n, 2, String::Format("ParallaxOcclusion(mi, {}, {}, {}, {})", *slot, uv, Convert(in(1), 1), Convert(in(2), 1))), 2);
        } else if (type == "TexCoord") {
            const Vec2 tiling(n.value.x == 0.0f ? 1.0f : n.value.x, n.value.y == 0.0f ? 1.0f : n.value.y);
            Set(id, 0, Local(n, 2, String::Format("{} * float2({}, {})", n.value.z >= 0.5f ? "mi.uv1" : "mi.uv0", Lit(tiling.x), Lit(tiling.y))), 2);
        } else if (type == "Time") {
            Set(id, 0, "mi.time", 1);
        } else if (type == "VertexColor") {
            Set(id, 0, "mi.vertexColor.rgb", 3);
            Set(id, 1, "mi.vertexColor.a", 1);
        } else if (type == "WorldPosition") {
            Set(id, 0, "mi.worldPos", 3);
        } else if (type == "CameraVector") {
            Set(id, 0, "mi.viewDir", 3);
        } else if (type == "VertexNormal") {
            Set(id, 0, "mi.normal", 3);
        } else if (type == "Panner") {
            const String uv = Connected(n, 0) ? Convert(in(0), 2) : String("mi.uv0");
            Set(id, 0, Local(n, 2, String::Format("{} + {} * mi.time", uv, Convert(in(1), 2))), 2);
        } else if (type == "Fresnel") {
            Set(id, 0,
                Local(n, 1, String::Format("Fresnel(mi, {}, {})", Convert(in(0), 1), Convert(in(1), 1))), 1);
        } else if (type == "WorldGrid") {
            const String grid = Local(n, 2, String::Format("WorldGrid(mi, {}, {})", Convert(in(0), 1), Convert(in(1), 1)));
            Set(id, 0, grid + ".x", 1);
            Set(id, 1, grid + ".y", 1);
        } else if (type == "Noise") {
            const String pos = Connected(n, 0) ? Convert(in(0), 3) : String("mi.worldPos");
            Set(id, 0, Local(n, 1, String::Format("FractalNoise({}, {})", pos, Convert(in(1), 1))), 1);
        } else if (type == "Desaturate") {
            Set(id, 0, Local(n, 3, String::Format("Desaturate({}, {})", Convert(in(0), 3), Convert(in(1), 1))), 3);
        } else if (type == "ComponentMask") {
            String sw;
            const f32 c[4] = {n.value.x, n.value.y, n.value.z, n.value.w};
            for (u32 i = 0; i < 4; ++i)
                if (c[i] != 0.0f)
                    sw += "rgba"[i];
            if (sw.Empty()) {
                Error(id, "no channel selected");
                sw = "r";
            }
            const Expr x = in(0);
            Set(id, 0, Local(n, u32(sw.Size()), String::Format("({}).{}", Convert(x, 4), sw)), u32(sw.Size()));
        } else if (type == "Append") {
            const Expr a = in(0), b = in(1);
            const u32 dims = a.dims + b.dims;
            if (dims > 4) {
                Error(id, "the result would have more than 4 channels");
                Set(id, 0, "0.0", 1);
                return;
            }
            Set(id, 0, Local(n, dims, String::Format("{}({}, {})", TypeName(dims), a.code, b.code)), dims);
        } else if (type == "Add" || type == "Subtract" || type == "Multiply" || type == "Divide" || type == "Min" || type == "Max") {
            const u32 d = widest({0, 1});
            const String a = Convert(in(0), d), b = Convert(in(1), d);
            String code;
            if (type == "Add") code = String::Format("{} + {}", a, b);
            else if (type == "Subtract") code = String::Format("{} - {}", a, b);
            else if (type == "Multiply") code = String::Format("{} * {}", a, b);
            else if (type == "Divide") code = String::Format("{} / {}", a, b);
            else if (type == "Min") code = String::Format("min({}, {})", a, b);
            else code = String::Format("max({}, {})", a, b);
            Set(id, 0, Local(n, d, code), d);
        } else if (type == "Lerp") {
            const u32 d = widest({0, 1});
            Set(id, 0, Local(n, d, String::Format("lerp({}, {}, {})", Convert(in(0), d), Convert(in(1), d), Convert(in(2), 1))), d);
        } else if (type == "Power") {
            const u32 d = in(0).dims;
            Set(id, 0, Local(n, d, String::Format("pow(max({}, 0.0), {})", in(0).code, Convert(in(1), 1))), d);
        } else if (type == "Clamp") {
            const u32 d = in(0).dims;
            Set(id, 0, Local(n, d, String::Format("clamp({}, {}, {})", in(0).code, Convert(in(1), 1), Convert(in(2), 1))), d);
        } else if (type == "Dot") {
            const u32 d = widest({0, 1});
            Set(id, 0, Local(n, 1, String::Format("dot({}, {})", Convert(in(0), d), Convert(in(1), d))), 1);
        } else if (type == "Length") {
            Set(id, 0, Local(n, 1, String::Format("length({})", in(0).code)), 1);
        } else {
            // One-input math with the same width out.
            static const struct
            {
                const char* type;
                const char* fmt;
            } unary[] = {{"OneMinus", "1.0 - {}"}, {"Saturate", "saturate({})"}, {"Abs", "abs({})"},     {"Floor", "floor({})"},
                         {"Frac", "frac({})"},     {"Sine", "sin({})"},           {"Cosine", "cos({})"}, {"Normalize", "SafeNormalize({})"}};
            for (const auto& u : unary)
                if (type == u.type) {
                    const Expr x = in(0);
                    Set(id, 0, Local(n, x.dims, String::Format(u.fmt, x.code)), x.dims);
                    return;
                }
            Error(id, String::Format("node type '{}' has no code generator", type));
        }
    }

    const MaterialGraph& m_graph;
    CompiledMaterialGraph& m_out;
    HashMap<u32, u8> m_state;
    HashMap<u64, Expr> m_values;
    HashMap<u32, u32> m_slotOf;
    String m_body;
};

StringView BlendName(MaterialBlendMode b)
{
    return b == MaterialBlendMode::Masked ? "Masked" : b == MaterialBlendMode::Translucent ? "Translucent" : "Opaque";
}

JsonValue Vec4Json(Vec4 v)
{
    JsonValue a = JsonValue::MakeArray();
    a.PushBack(v.x);
    a.PushBack(v.y);
    a.PushBack(v.z);
    a.PushBack(v.w);
    return a;
}

Vec4 JsonVec4(const JsonValue& j, Vec4 fallback = Vec4(0))
{
    if (!j.IsArray())
        return fallback;
    const f32 x = j.Size() > 0 ? j[0].AsFloat() : fallback.x, y = j.Size() > 1 ? j[1].AsFloat() : fallback.y,
              z = j.Size() > 2 ? j[2].AsFloat() : fallback.z, w = j.Size() > 3 ? j[3].AsFloat() : fallback.w;
    return Vec4(x, y, z, w);
}

} // namespace

// --- Node types ----------------------------------------------------------------------------------------------------------

Span<const MaterialNodeType> GetMaterialNodeTypes() { return Span<const MaterialNodeType>(NodeTypes, ZX_ARRAY_COUNT(NodeTypes)); }

const MaterialNodeType* FindMaterialNodeType(StringView type)
{
    for (const MaterialNodeType& t : NodeTypes)
        if (t.type == type)
            return &t;
    return nullptr;
}

// --- Graph ----------------------------------------------------------------------------------------------------------------

MaterialGraphNode* MaterialGraph::FindNode(u32 id)
{
    for (MaterialGraphNode& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

const MaterialGraphNode* MaterialGraph::FindNode(u32 id) const { return const_cast<MaterialGraph*>(this)->FindNode(id); }

MaterialGraphNode* MaterialGraph::AddNode(StringView type, Vec2 position)
{
    const MaterialNodeType* t = FindMaterialNodeType(type);
    MaterialGraphNode& n = nodes.AddDefaulted();
    n.id = nextId++;
    n.type = type;
    n.position = position;
    if (t)
        for (const MaterialPinDesc& p : t->inputs)
            n.inputValues.PushBack(p.defaultValue);
    if (type == "Scalar")
        n.value = Vec4(1, 0, 0, 0);
    else if (type == "Color")
        n.value = Vec4(1, 1, 1, 1);
    else if (type == "TexCoord")
        n.value = Vec4(1, 1, 0, 0);
    else if (type == "ComponentMask")
        n.value = Vec4(1, 1, 0, 0);
    return &n;
}

void MaterialGraph::RemoveNode(u32 id)
{
    for (usize i = links.Size(); i > 0; --i)
        if (links[i - 1].fromNode == id || links[i - 1].toNode == id)
            links.RemoveAt(i - 1);
    for (usize i = 0; i < nodes.Size(); ++i)
        if (nodes[i].id == id) {
            nodes.RemoveAt(i);
            return;
        }
}

const MaterialGraphLink* MaterialGraph::FindInputLink(u32 toNode, u32 toPin) const
{
    for (const MaterialGraphLink& l : links)
        if (l.toNode == toNode && l.toPin == toPin)
            return &l;
    return nullptr;
}

void MaterialGraph::Disconnect(u32 toNode, u32 toPin)
{
    for (usize i = 0; i < links.Size(); ++i)
        if (links[i].toNode == toNode && links[i].toPin == toPin) {
            links.RemoveAt(i);
            return;
        }
}

bool MaterialGraph::Connect(u32 fromNode, u32 fromPin, u32 toNode, u32 toPin)
{
    if (fromNode == toNode || !FindNode(fromNode) || !FindNode(toNode))
        return false;
    // A cycle appears if `toNode` already feeds `fromNode`.
    Array<u32> stack{fromNode};
    Array<u32> seen;
    while (!stack.Empty()) {
        const u32 n = stack.PopBackValue();
        if (n == toNode)
            return false;
        if (seen.Contains(n))
            continue;
        seen.PushBack(n);
        for (const MaterialGraphLink& l : links)
            if (l.toNode == n)
                stack.PushBack(l.fromNode);
    }
    Disconnect(toNode, toPin);
    links.PushBack(MaterialGraphLink{fromNode, fromPin, toNode, toPin});
    return true;
}

u32 MaterialGraph::OutputNode() const
{
    for (const MaterialGraphNode& n : nodes)
        if (n.type == "Output")
            return n.id;
    return 0;
}

JsonValue MaterialGraph::ToJson() const
{
    JsonValue doc = JsonValue::MakeObject();
    doc.Set("format", "zmat");
    doc.Set("version", i64(1));
    doc.Set("guid", String::Format("{}", guid));
    doc.Set("blend", BlendName(blend));
    doc.Set("twoSided", twoSided);
    doc.Set("unlit", unlit);
    doc.Set("alphaCutoff", alphaCutoff);
    JsonValue list = JsonValue::MakeArray();
    for (const MaterialGraphNode& n : nodes) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("id", n.id);
        o.Set("type", n.type);
        JsonValue pos = JsonValue::MakeArray();
        pos.PushBack(n.position.x);
        pos.PushBack(n.position.y);
        o.Set("pos", move(pos));
        if (!n.name.Empty())
            o.Set("name", n.name);
        if (!(n.value == Vec4(0)))
            o.Set("value", Vec4Json(n.value));
        if (!n.texture.Empty())
            o.Set("texture", n.texture);
        if (n.normalMap)
            o.Set("normalMap", true);
        const MaterialNodeType* t = FindMaterialNodeType(n.type);
        bool custom = false;
        for (usize i = 0; t && i < n.inputValues.Size() && i < t->inputs.Size(); ++i)
            custom |= !(n.inputValues[i] == t->inputs[i].defaultValue);
        if (custom) {
            JsonValue inputs = JsonValue::MakeArray();
            for (const Vec4& v : n.inputValues)
                inputs.PushBack(Vec4Json(v));
            o.Set("inputs", move(inputs));
        }
        list.PushBack(move(o));
    }
    doc.Set("nodes", move(list));
    JsonValue linkList = JsonValue::MakeArray();
    for (const MaterialGraphLink& l : links) {
        JsonValue a = JsonValue::MakeArray();
        a.PushBack(l.fromNode);
        a.PushBack(l.fromPin);
        a.PushBack(l.toNode);
        a.PushBack(l.toPin);
        linkList.PushBack(move(a));
    }
    doc.Set("links", move(linkList));
    return doc;
}

bool MaterialGraph::FromJson(const JsonValue& doc, Array<String>* warnings)
{
    if (doc.Get("format").AsString() != "zmat")
        return false;
    *this = MaterialGraph();
    if (const Optional<Guid> g = Guid::Parse(doc.Get("guid").AsString()))
        guid = *g;
    const StringView b = doc.Get("blend").AsString("Opaque");
    blend = b == "Masked" ? MaterialBlendMode::Masked : b == "Translucent" ? MaterialBlendMode::Translucent : MaterialBlendMode::Opaque;
    twoSided = doc.Get("twoSided").AsBool();
    unlit = doc.Get("unlit").AsBool();
    alphaCutoff = doc.Get("alphaCutoff").AsFloat(0.5f);
    for (const JsonValue& o : doc.Get("nodes").Elements()) {
        MaterialGraphNode n;
        n.id = u32(o.Get("id").AsInt());
        n.type = o.Get("type").AsString();
        const JsonValue& pos = o.Get("pos");
        n.position = Vec2(pos.Size() > 0 ? pos[0].AsFloat() : 0.0f, pos.Size() > 1 ? pos[1].AsFloat() : 0.0f);
        n.name = o.Get("name").AsString();
        n.value = JsonVec4(o.Get("value"));
        n.texture = o.Get("texture").AsString();
        n.normalMap = o.Get("normalMap").AsBool();
        const MaterialNodeType* t = FindMaterialNodeType(n.type);
        if (!t && warnings)
            warnings->PushBack(String::Format("Unknown material node type '{}'", n.type));
        const JsonValue& inputs = o.Get("inputs");
        if (t)
            for (usize i = 0; i < t->inputs.Size(); ++i)
                n.inputValues.PushBack(i < inputs.Size() ? JsonVec4(inputs[i], t->inputs[i].defaultValue) : t->inputs[i].defaultValue);
        nextId = Max(nextId, n.id + 1);
        nodes.PushBack(move(n));
    }
    for (const JsonValue& l : doc.Get("links").Elements())
        if (l.Size() == 4)
            links.PushBack(MaterialGraphLink{u32(l[0].AsInt()), u32(l[1].AsInt()), u32(l[2].AsInt()), u32(l[3].AsInt())});
    return true;
}

MaterialGraph MaterialGraph::CreateDefault()
{
    MaterialGraph g;
    g.guid = Guid::New();
    const u32 output = g.AddNode("Output", Vec2(320, 0))->id;
    MaterialGraphNode* color = g.AddNode("Color", Vec2(0, 0));
    color->name = "Base Color";
    color->value = Vec4(0.8f, 0.8f, 0.8f, 1.0f);
    g.Connect(color->id, 0, output, 0);
    return g;
}

// --- Compilation ----------------------------------------------------------------------------------------------------------

CompiledMaterialGraph CompileMaterialGraph(const MaterialGraph& graph)
{
    CompiledMaterialGraph out;
    Compiler(graph, out).Run();
    u64 h = HashXX64(out.code.View());
    h = HashCombine(h, u64(graph.blend));
    h = HashCombine(h, u64(graph.twoSided));
    h = HashCombine(h, u64(graph.unlit));
    out.hash = h;
    return out;
}

void WriteMaterialParams(const MaterialGraph& graph, const CompiledMaterialGraph& compiled, Array<u8>& out)
{
    out.ResizeZeroed(compiled.paramBytes);
    for (const MaterialParamSlot& s : compiled.params) {
        const MaterialGraphNode* n = graph.FindNode(s.node);
        if (s.kind == MaterialParamSlot::Kind::Texture) {
            const u32 invalid = 0xFFFFFFFFu;
            MemCopy(out.Data() + s.offset, &invalid, 4);
        } else if (n) {
            MemCopy(out.Data() + s.offset, &n->value, 16);
        }
    }
}

} // namespace zx::render
