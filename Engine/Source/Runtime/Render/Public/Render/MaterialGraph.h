#pragma once

// Material graphs (.zmat): nodes connected by links, compiled to an HLSL function
//
//   MaterialOutputs EvaluateGraphMaterial(MaterialInputs mi)        (Shaders/Material/MaterialGraph.hlsli)
//
// that ForwardPBR.hlsl calls instead of its fixed metallic-roughness sampling. Every constant/parameter value and
// texture lives in a per-material parameter block (not in the code), so tweaking values never recompiles; only
// structural edits (nodes, links, node options) change the code and its hash.
//
// Values are float1..float4; math nodes take the widest input (scalars broadcast), inputs convert implicitly
// (truncate with a swizzle, or pad with zeros).

#include "Core/Containers/Array.h"
#include "Core/Hash/Guid.h"
#include "Core/Math/Math.h"
#include "Core/Serialization/Json.h"
#include "Core/String/String.h"

namespace zx::render {

enum class MaterialBlendMode : u8
{
    Opaque,
    Masked,
    Translucent,
};

struct MaterialGraphNode
{
    u32 id = 0;
    String type;              // node type (see GetMaterialNodeTypes)
    Vec2 position;            // editor canvas position
    String name;              // parameter name (Scalar/Color/TextureSample); empty = plain constant
    Vec4 value = Vec4(0, 0, 0, 0);
    String texture;           // TextureSample: path relative to the content root
    bool normalMap = false;   // TextureSample: unpack as a tangent-space normal
    Array<Vec4> inputValues;  // values of unconnected inputs (per input pin)
};

struct MaterialGraphLink
{
    u32 fromNode = 0;
    u32 fromPin = 0;
    u32 toNode = 0;
    u32 toPin = 0;
    friend bool operator==(const MaterialGraphLink&, const MaterialGraphLink&) = default;
};

struct MaterialGraph
{
    Guid guid;
    MaterialBlendMode blend = MaterialBlendMode::Opaque;
    bool twoSided = false;
    bool unlit = false;
    f32 alphaCutoff = 0.5f;
    Array<MaterialGraphNode> nodes;
    Array<MaterialGraphLink> links;
    u32 nextId = 1;

    MaterialGraphNode* FindNode(u32 id);
    const MaterialGraphNode* FindNode(u32 id) const;
    /// Adds a node of `type` with its default values; returns it (pointer valid until the next node is added).
    MaterialGraphNode* AddNode(StringView type, Vec2 position);
    void RemoveNode(u32 id); // and its links
    /// Connects (replacing any link into the same input). Returns false for self links or cycles.
    bool Connect(u32 fromNode, u32 fromPin, u32 toNode, u32 toPin);
    void Disconnect(u32 toNode, u32 toPin);
    const MaterialGraphLink* FindInputLink(u32 toNode, u32 toPin) const;
    u32 OutputNode() const; // 0 if none

    JsonValue ToJson() const;
    bool FromJson(const JsonValue& json, Array<String>* warnings = nullptr);

    /// New material: an output node fed by a "BaseColor" color parameter.
    static MaterialGraph CreateDefault();
};

// --- Node library ------------------------------------------------------------------------------------------------------

struct MaterialPinDesc
{
    StringView name;
    u8 dims = 0;         // 1..4; 0 = dynamic (widest connected input)
    Vec4 defaultValue;   // used when unconnected (Vec4(NaN) = special default, see node code)
};

enum class MaterialNodeKind : u8
{
    Output,
    Value,     // value lives in the parameter block (Scalar, Color)
    Texture,   // texture lives in the parameter block
    Input,     // geometry/time inputs
    Math,
    Utility,
};

struct MaterialNodeType
{
    StringView type;
    StringView displayName;
    StringView category;
    MaterialNodeKind kind;
    Span<const MaterialPinDesc> inputs;
    Span<const MaterialPinDesc> outputs;
    StringView description;
};

Span<const MaterialNodeType> GetMaterialNodeTypes();
const MaterialNodeType* FindMaterialNodeType(StringView type);

// --- Compilation ---------------------------------------------------------------------------------------------------------

struct MaterialParamSlot
{
    enum class Kind : u8
    {
        Float4,
        Texture,
    };
    Kind kind = Kind::Float4;
    u32 node = 0;
    u32 offset = 0; // bytes into the material's parameter block
};

struct CompiledMaterialGraph
{
    String code;  // HLSL (includes MaterialGraph.hlsli, defines EvaluateGraphMaterial)
    u64 hash = 0; // of the code and the pipeline-relevant settings
    Array<MaterialParamSlot> params;
    u32 paramBytes = 0;
    Array<String> errors;
    Array<u32> errorNodes;
    bool Ok() const { return errors.Empty(); }
};

CompiledMaterialGraph CompileMaterialGraph(const MaterialGraph& graph);

/// Parameter block for the compiled graph: float4 values, texture slots left as 0xFFFFFFFF (resolved by the renderer).
void WriteMaterialParams(const MaterialGraph& graph, const CompiledMaterialGraph& compiled, Array<u8>& out);

inline constexpr StringView MaterialExtension = ".zmat";

} // namespace zx::render
