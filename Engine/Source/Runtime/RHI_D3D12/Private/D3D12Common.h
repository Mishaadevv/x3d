#pragma once

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#undef CreateWindow
#undef Yield

#include "Core/Log/Log.h"
#include "RHI/RHI.h"

ZX_DECLARE_LOG_CATEGORY(LogD3D12);

namespace zx::rhi::d3d12 {

/// Minimal COM smart pointer.
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    explicit ComPtr(T* p) : m_ptr(p) {}
    ComPtr(const ComPtr& o) : m_ptr(o.m_ptr)
    {
        if (m_ptr)
            m_ptr->AddRef();
    }
    ComPtr(ComPtr&& o) noexcept : m_ptr(o.m_ptr) { o.m_ptr = nullptr; }
    ~ComPtr() { Reset(); }
    ComPtr& operator=(const ComPtr& o)
    {
        if (this != &o) {
            Reset();
            m_ptr = o.m_ptr;
            if (m_ptr)
                m_ptr->AddRef();
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept
    {
        if (this != &o) {
            Reset();
            m_ptr = o.m_ptr;
            o.m_ptr = nullptr;
        }
        return *this;
    }
    void Reset()
    {
        if (m_ptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }
    T* Get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }
    /// For out-parameters: releases the current pointer.
    T** ReleaseAndGetAddressOf()
    {
        Reset();
        return &m_ptr;
    }
    template <typename U>
    HRESULT As(ComPtr<U>& out) const
    {
        return m_ptr ? m_ptr->QueryInterface(IID_PPV_ARGS(out.ReleaseAndGetAddressOf())) : E_POINTER;
    }

private:
    T* m_ptr = nullptr;
};

#define ZX_DX_PPV(comptr) IID_PPV_ARGS((comptr).ReleaseAndGetAddressOf())

/// Logs a failed HRESULT with context. Returns true on success.
bool CheckHR(HRESULT hr, const char* expression, const char* file, int line);
#define ZX_DX_CHECK(expr) ::zx::rhi::d3d12::CheckHR((expr), #expr, __FILE__, __LINE__)

DXGI_FORMAT ToDxgi(Format format);
/// Typeless resource format for depth textures that are also sampled.
DXGI_FORMAT ToDxgiTypeless(Format format);
/// SRV format for depth formats (e.g. D32_FLOAT -> R32_FLOAT).
DXGI_FORMAT ToDxgiSrv(Format format);
D3D12_RESOURCE_STATES ToD3D12(ResourceState state);
D3D12_COMPARISON_FUNC ToD3D12(CompareOp op);
D3D12_FILTER ToD3D12Filter(const SamplerDesc& desc);
D3D12_TEXTURE_ADDRESS_MODE ToD3D12(AddressMode mode);
D3D12_BLEND ToD3D12(BlendFactor f);
D3D12_BLEND_OP ToD3D12(BlendOp op);
D3D12_PRIMITIVE_TOPOLOGY ToD3D12(PrimitiveTopology t);
D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3D12TopologyType(PrimitiveTopology t);

void SetDebugName(ID3D12Object* object, StringView name);

} // namespace zx::rhi::d3d12
