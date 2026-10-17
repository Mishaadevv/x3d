#pragma once

// RHI common types: formats, resource descriptions, states, pipeline state descriptions.

#include "Core/Containers/Array.h"
#include "Core/Containers/Span.h"
#include "Core/Math/Color.h"
#include "Core/String/String.h"

namespace zx::rhi {

enum class Format : u8
{
    Unknown,
    // 8-bit
    R8_UNORM,
    R8_UINT,
    R8G8_UNORM,
    R8G8B8A8_UNORM,
    R8G8B8A8_SRGB,
    R8G8B8A8_SNORM,
    R8G8B8A8_UINT,
    B8G8R8A8_UNORM,
    B8G8R8A8_SRGB,
    // 16-bit
    R16_UNORM,
    R16_UINT,
    R16_FLOAT,
    R16G16_UNORM,
    R16G16_FLOAT,
    R16G16B16A16_UNORM,
    R16G16B16A16_FLOAT,
    // 32-bit
    R32_UINT,
    R32_SINT,
    R32_FLOAT,
    R32G32_UINT,
    R32G32_FLOAT,
    R32G32B32_FLOAT,
    R32G32B32A32_UINT,
    R32G32B32A32_FLOAT,
    // Packed
    R10G10B10A2_UNORM,
    R11G11B10_FLOAT,
    R9G9B9E5_SHAREDEXP,
    // Depth
    D16_UNORM,
    D24_UNORM_S8_UINT,
    D32_FLOAT,
    D32_FLOAT_S8X24_UINT,
    // Block compressed
    BC1_UNORM,
    BC1_SRGB,
    BC3_UNORM,
    BC3_SRGB,
    BC4_UNORM,
    BC5_UNORM,
    BC6H_UF16,
    BC7_UNORM,
    BC7_SRGB,
    Count
};

struct FormatInfo
{
    const char* name;
    u8 bytesPerBlock; // bytes per pixel for uncompressed formats
    u8 blockSize;     // 1 for uncompressed, 4 for BCn
    bool isDepth;
    bool hasStencil;
    bool isSRGB;
    bool isCompressed;
};

const FormatInfo& GetFormatInfo(Format format);
/// Row pitch / slice size of a mip level in bytes (tightly packed).
u64 GetRowPitch(Format format, u32 width);
u64 GetSliceSize(Format format, u32 width, u32 height);

enum class QueueType : u8
{
    Graphics,
    Compute,
    Copy,
    Count
};

enum class MemoryType : u8
{
    GpuOnly,  // device local
    Upload,   // CPU write, GPU read (write-combined)
    Readback, // GPU write, CPU read
};

enum class BufferUsage : u32
{
    None = 0,
    Vertex = 1 << 0,     // vertex pulling via ByteAddressBuffer works with ShaderResource; kept for clarity
    Index = 1 << 1,
    Constant = 1 << 2,
    ShaderResource = 1 << 3, // bindless SRV (raw ByteAddressBuffer / structured)
    UnorderedAccess = 1 << 4,
    Indirect = 1 << 5,
    CopySource = 1 << 6,
    CopyDest = 1 << 7,
};
ZX_ENUM_FLAGS(BufferUsage)

/// One draw of DrawIndirect (D3D12_DRAW_ARGUMENTS).
struct IndirectDrawArgsPlain
{
    u32 vertexCount;
    u32 instanceCount;
    u32 firstVertex;
    u32 firstInstance;
};
static_assert(sizeof(IndirectDrawArgsPlain) == 16);

/// One draw of DrawIndexedIndirect: a draw id written to push constant dword IndirectDrawIdConstant, then the
/// arguments of DrawIndexed.
struct IndirectDrawArgs
{
    u32 drawId = 0;
    u32 indexCount = 0;
    u32 instanceCount = 0;
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    u32 firstInstance = 0;
};
static_assert(sizeof(IndirectDrawArgs) == 24);
inline constexpr u32 IndirectDrawIdConstant = 4; // dword of the push constants (b0) that receives drawId

enum class TextureUsage : u32
{
    None = 0,
    ShaderResource = 1 << 0,
    UnorderedAccess = 1 << 1,
    RenderTarget = 1 << 2,
    DepthStencil = 1 << 3,
    CopySource = 1 << 4,
    CopyDest = 1 << 5,
};
ZX_ENUM_FLAGS(TextureUsage)

enum class TextureDimension : u8
{
    Texture2D,
    Texture2DArray,
    TextureCube,
    TextureCubeArray,
    Texture3D,
};

/// Resource access states. Barriers transition between them.
enum class ResourceState : u32
{
    Undefined = 0,
    Common = 1 << 0,
    VertexBuffer = 1 << 1,
    IndexBuffer = 1 << 2,
    ConstantBuffer = 1 << 3,
    ShaderResource = 1 << 4, // pixel + non-pixel
    UnorderedAccess = 1 << 5,
    RenderTarget = 1 << 6,
    DepthWrite = 1 << 7,
    DepthRead = 1 << 8,
    IndirectArgument = 1 << 9,
    CopySource = 1 << 10,
    CopyDest = 1 << 11,
    Present = 1 << 12,
};
ZX_ENUM_FLAGS(ResourceState)

struct BufferDesc
{
    u64 size = 0;
    BufferUsage usage = BufferUsage::ShaderResource;
    MemoryType memory = MemoryType::GpuOnly;
    /// Structured stride (0 = raw ByteAddressBuffer views).
    u32 stride = 0;
    StringView debugName;
};

struct ClearValue
{
    LinearColor color = LinearColor(0, 0, 0, 0);
    f32 depth = 0.0f; // reversed-Z: far = 0
    u8 stencil = 0;
};

struct TextureDesc
{
    TextureDimension dimension = TextureDimension::Texture2D;
    u32 width = 1;
    u32 height = 1;
    u32 depthOrArraySize = 1; // depth for 3D, array layers (x6 for cubes is implicit: give cube count)
    u32 mipLevels = 1;
    u32 sampleCount = 1;
    Format format = Format::R8G8B8A8_UNORM;
    TextureUsage usage = TextureUsage::ShaderResource;
    ClearValue clearValue;
    StringView debugName;
};

/// Initial data for one subresource (mip/layer), tightly packed rows unless rowPitch given.
struct SubresourceData
{
    const void* data = nullptr;
    u64 rowPitch = 0;
    u64 slicePitch = 0;
};

enum class Filter : u8
{
    Point,
    Linear,
    Anisotropic
};

enum class AddressMode : u8
{
    Wrap,
    Clamp,
    Mirror,
    Border
};

enum class CompareOp : u8
{
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

struct SamplerDesc
{
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    Filter mipFilter = Filter::Linear;
    AddressMode addressU = AddressMode::Wrap;
    AddressMode addressV = AddressMode::Wrap;
    AddressMode addressW = AddressMode::Wrap;
    f32 mipLodBias = 0.0f;
    u32 maxAnisotropy = 16;
    bool comparison = false;
    CompareOp compareOp = CompareOp::GreaterEqual;
    LinearColor borderColor = LinearColor(0, 0, 0, 0);
    f32 minLod = 0.0f;
    f32 maxLod = 1000.0f;
};

// --- Pipeline state -------------------------------------------------------------------------------------------------

enum class ShaderStage : u8
{
    Vertex,
    Pixel,
    Compute,
    Count
};

/// Compiled shader bytecode (DXIL / SPIR-V).
struct ShaderBytecode
{
    Array<u8> code;
    ShaderStage stage = ShaderStage::Vertex;
    String entryPoint;
    u64 hash = 0;
};

enum class PrimitiveTopology : u8
{
    TriangleList,
    TriangleStrip,
    LineList,
    LineStrip,
    PointList
};

enum class CullMode : u8
{
    None,
    Front,
    Back
};

enum class FillMode : u8
{
    Solid,
    Wireframe
};

struct RasterizerState
{
    CullMode cullMode = CullMode::Back;
    FillMode fillMode = FillMode::Solid;
    bool frontCounterClockwise = false; // left-handed: clockwise front faces
    i32 depthBias = 0;
    f32 slopeScaledDepthBias = 0.0f;
    f32 depthBiasClamp = 0.0f;
    bool depthClip = true;
    bool conservative = false;
};

struct DepthStencilState
{
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompare = CompareOp::GreaterEqual; // reversed-Z
    bool stencil = false;
};

enum class BlendFactor : u8
{
    Zero,
    One,
    SrcColor,
    InvSrcColor,
    SrcAlpha,
    InvSrcAlpha,
    DstColor,
    InvDstColor,
    DstAlpha,
    InvDstAlpha
};

enum class BlendOp : u8
{
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max
};

struct BlendTarget
{
    bool enable = false;
    BlendFactor srcColor = BlendFactor::SrcAlpha;
    BlendFactor dstColor = BlendFactor::InvSrcAlpha;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::InvSrcAlpha;
    BlendOp alphaOp = BlendOp::Add;
    u8 writeMask = 0xF;

    static BlendTarget AlphaBlend()
    {
        BlendTarget b;
        b.enable = true;
        return b;
    }
    static BlendTarget Additive()
    {
        BlendTarget b;
        b.enable = true;
        b.srcColor = BlendFactor::One;
        b.dstColor = BlendFactor::One;
        b.srcAlpha = BlendFactor::One;
        b.dstAlpha = BlendFactor::One;
        return b;
    }
    static BlendTarget Premultiplied()
    {
        BlendTarget b;
        b.enable = true;
        b.srcColor = BlendFactor::One;
        return b;
    }
};

inline constexpr u32 MaxRenderTargets = 8;

struct GraphicsPipelineDesc
{
    const ShaderBytecode* vertexShader = nullptr;
    const ShaderBytecode* pixelShader = nullptr;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    RasterizerState rasterizer;
    DepthStencilState depthStencil;
    BlendTarget blend[MaxRenderTargets];
    Format colorFormats[MaxRenderTargets] = {};
    u32 colorCount = 0;
    Format depthFormat = Format::Unknown;
    u32 sampleCount = 1;
    StringView debugName;
};

struct ComputePipelineDesc
{
    const ShaderBytecode* computeShader = nullptr;
    StringView debugName;
};

// --- Bindless conventions (shared with Engine/Shaders/Common/Bindless.hlsli) ---------------------------------------

/// Root constants available to every draw/dispatch (32-bit values).
inline constexpr u32 MaxPushConstants = 32;
/// Invalid bindless descriptor index.
inline constexpr u32 InvalidDescriptor = U32Max;

} // namespace zx::rhi
