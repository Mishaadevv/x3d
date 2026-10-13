#include "D3D12Common.h"
#include "Core/String/Utf.h"

ZX_DEFINE_LOG_CATEGORY(LogD3D12, Info);

namespace zx::rhi::d3d12 {

bool CheckHR(HRESULT hr, const char* expression, const char* file, int line)
{
    if (SUCCEEDED(hr))
        return true;
    ZX_LOG_ERROR(LogD3D12, "{} failed with HRESULT {:#010x} at {}({})", expression, u32(hr), file, line);
    return false;
}

DXGI_FORMAT ToDxgi(Format f)
{
    switch (f) {
    case Format::Unknown: return DXGI_FORMAT_UNKNOWN;
    case Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case Format::R8_UINT: return DXGI_FORMAT_R8_UINT;
    case Format::R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case Format::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case Format::R8G8B8A8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
    case Format::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
    case Format::R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    case Format::R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
    case Format::R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case Format::R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case Format::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
    case Format::R32_SINT: return DXGI_FORMAT_R32_SINT;
    case Format::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case Format::R32G32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case Format::R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case Format::R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case Format::R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
    case Format::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case Format::R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case Format::R9G9B9E5_SHAREDEXP: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    case Format::D16_UNORM: return DXGI_FORMAT_D16_UNORM;
    case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case Format::D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case Format::BC1_UNORM: return DXGI_FORMAT_BC1_UNORM;
    case Format::BC1_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case Format::BC3_UNORM: return DXGI_FORMAT_BC3_UNORM;
    case Format::BC3_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
    case Format::BC4_UNORM: return DXGI_FORMAT_BC4_UNORM;
    case Format::BC5_UNORM: return DXGI_FORMAT_BC5_UNORM;
    case Format::BC6H_UF16: return DXGI_FORMAT_BC6H_UF16;
    case Format::BC7_UNORM: return DXGI_FORMAT_BC7_UNORM;
    case Format::BC7_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
    case Format::Count: break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT ToDxgiTypeless(Format f)
{
    switch (f) {
    case Format::D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case Format::D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case Format::D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return ToDxgi(f);
    }
}

DXGI_FORMAT ToDxgiSrv(Format f)
{
    switch (f) {
    case Format::D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case Format::D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case Format::D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return ToDxgi(f);
    }
}

D3D12_RESOURCE_STATES ToD3D12(ResourceState s)
{
    if (s == ResourceState::Undefined || s == ResourceState::Common || s == ResourceState::Present)
        return D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES r = D3D12_RESOURCE_STATE_COMMON;
    if (HasAnyFlags(s, ResourceState::VertexBuffer | ResourceState::ConstantBuffer))
        r |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (HasAnyFlags(s, ResourceState::IndexBuffer))
        r |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if (HasAnyFlags(s, ResourceState::ShaderResource))
        r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (HasAnyFlags(s, ResourceState::UnorderedAccess))
        r |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (HasAnyFlags(s, ResourceState::RenderTarget))
        r |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (HasAnyFlags(s, ResourceState::DepthWrite))
        r |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (HasAnyFlags(s, ResourceState::DepthRead))
        r |= D3D12_RESOURCE_STATE_DEPTH_READ;
    if (HasAnyFlags(s, ResourceState::IndirectArgument))
        r |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (HasAnyFlags(s, ResourceState::CopySource))
        r |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (HasAnyFlags(s, ResourceState::CopyDest))
        r |= D3D12_RESOURCE_STATE_COPY_DEST;
    return r;
}

D3D12_COMPARISON_FUNC ToD3D12(CompareOp op)
{
    switch (op) {
    case CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case CompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_FILTER ToD3D12Filter(const SamplerDesc& d)
{
    if (d.minFilter == Filter::Anisotropic || d.magFilter == Filter::Anisotropic)
        return d.comparison ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    const D3D12_FILTER_TYPE minF = d.minFilter == Filter::Point ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
    const D3D12_FILTER_TYPE magF = d.magFilter == Filter::Point ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
    const D3D12_FILTER_TYPE mipF = d.mipFilter == Filter::Point ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
    const D3D12_FILTER_REDUCTION_TYPE red =
        d.comparison ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    return D3D12_ENCODE_BASIC_FILTER(minF, magF, mipF, red);
}

D3D12_TEXTURE_ADDRESS_MODE ToD3D12(AddressMode m)
{
    switch (m) {
    case AddressMode::Wrap: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::Clamp: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AddressMode::Border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_BLEND ToD3D12(BlendFactor f)
{
    switch (f) {
    case BlendFactor::Zero: return D3D12_BLEND_ZERO;
    case BlendFactor::One: return D3D12_BLEND_ONE;
    case BlendFactor::SrcColor: return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::InvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DstColor: return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::InvDstColor: return D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::InvDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP ToD3D12(BlendOp op)
{
    switch (op) {
    case BlendOp::Add: return D3D12_BLEND_OP_ADD;
    case BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    case BlendOp::Min: return D3D12_BLEND_OP_MIN;
    case BlendOp::Max: return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

D3D12_PRIMITIVE_TOPOLOGY ToD3D12(PrimitiveTopology t)
{
    switch (t) {
    case PrimitiveTopology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case PrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3D12TopologyType(PrimitiveTopology t)
{
    switch (t) {
    case PrimitiveTopology::TriangleList:
    case PrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    case PrimitiveTopology::LineList:
    case PrimitiveTopology::LineStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case PrimitiveTopology::PointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

void SetDebugName(ID3D12Object* object, StringView name)
{
    if (!object || name.Empty())
        return;
    const Array<wchar_t> w = Utf8ToUtf16(name);
    object->SetName(w.Data());
}

} // namespace zx::rhi::d3d12
