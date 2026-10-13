#ifndef ZX_BINDLESS_HLSLI
#define ZX_BINDLESS_HLSLI

// Bindless resource access. Keep in sync with D3D12Device::CreateRootSignature.
//
//   b0          : push constants (32 x uint)        -> declare with ZX_PUSH_CONSTANTS(MyStruct, g_Push)
//   b1, b2      : per-pass / per-view constant buffers
//   t0 space1-5 : Texture2D / Texture2DArray / TextureCube / Texture3D / ByteAddressBuffer (one heap, any index)
//   u0 space6-9 : RWTexture2D / RWByteAddressBuffer / RWTexture2DArray / RWTexture3D
//   s0..s7      : static samplers, s0 space1: bindless samplers

Texture2D<float4>        g_Texture2D[]        : register(t0, space1);
Texture2DArray<float4>   g_Texture2DArray[]   : register(t0, space2);
TextureCube<float4>      g_TextureCube[]      : register(t0, space3);
Texture3D<float4>        g_Texture3D[]        : register(t0, space4);
ByteAddressBuffer        g_Buffer[]           : register(t0, space5);
RWTexture2D<float4>      g_RWTexture2D[]      : register(u0, space6);
RWByteAddressBuffer      g_RWBuffer[]         : register(u0, space7);
RWTexture2DArray<float4> g_RWTexture2DArray[] : register(u0, space8);
RWTexture3D<float4>      g_RWTexture3D[]      : register(u0, space9);
SamplerState             g_Samplers[]         : register(s0, space1);

SamplerState           g_PointClamp   : register(s0);
SamplerState           g_PointWrap    : register(s1);
SamplerState           g_LinearClamp  : register(s2);
SamplerState           g_LinearWrap   : register(s3);
SamplerState           g_AnisoWrap    : register(s4);
SamplerState           g_AnisoClamp   : register(s5);
SamplerComparisonState g_ShadowSampler : register(s6);
SamplerState           g_LinearMirror : register(s7);

#define ZX_PUSH_CONSTANTS(Type, name) ConstantBuffer<Type> name : register(b0, space0)
#define ZX_PASS_CONSTANTS(Type, name) ConstantBuffer<Type> name : register(b1, space0)
#define ZX_VIEW_CONSTANTS(Type, name) ConstantBuffer<Type> name : register(b2, space0)

#define ZX_INVALID_INDEX 0xFFFFFFFFu

// Uniform access: the index must be the same for the whole draw/dispatch (push constants, constant buffers, data
// indexed by such values). This is the fast path - NonUniformResourceIndex costs a waterfall loop on some GPUs.
Texture2D<float4> GetTexture2D(uint index) { return g_Texture2D[index]; }
Texture2DArray<float4> GetTexture2DArray(uint index) { return g_Texture2DArray[index]; }
TextureCube<float4> GetTextureCube(uint index) { return g_TextureCube[index]; }
Texture3D<float4> GetTexture3D(uint index) { return g_Texture3D[index]; }
ByteAddressBuffer GetBuffer(uint index) { return g_Buffer[index]; }
RWTexture2D<float4> GetRWTexture2D(uint index) { return g_RWTexture2D[index]; }
RWByteAddressBuffer GetRWBuffer(uint index) { return g_RWBuffer[index]; }
RWTexture2DArray<float4> GetRWTexture2DArray(uint index) { return g_RWTexture2DArray[index]; }
RWTexture3D<float4> GetRWTexture3D(uint index) { return g_RWTexture3D[index]; }
SamplerState GetSampler(uint index) { return g_Samplers[index]; }

// Divergent access: the index may differ between lanes (per-vertex/per-pixel data, GPU-driven batches).
Texture2D<float4> GetTexture2DNonUniform(uint index) { return g_Texture2D[NonUniformResourceIndex(index)]; }
ByteAddressBuffer GetBufferNonUniform(uint index) { return g_Buffer[NonUniformResourceIndex(index)]; }
SamplerState GetSamplerNonUniform(uint index) { return g_Samplers[NonUniformResourceIndex(index)]; }

// Typed loads from raw buffers.
float3 LoadFloat3(ByteAddressBuffer b, uint byteOffset) { return asfloat(b.Load3(byteOffset)); }
float2 LoadFloat2(ByteAddressBuffer b, uint byteOffset) { return asfloat(b.Load2(byteOffset)); }
float4 LoadFloat4(ByteAddressBuffer b, uint byteOffset) { return asfloat(b.Load4(byteOffset)); }

#endif // ZX_BINDLESS_HLSLI
