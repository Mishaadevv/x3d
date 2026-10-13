#include "D3D12Device.h"

namespace zx::rhi::d3d12 {

D3D12SwapChain::~D3D12SwapChain()
{
    if (m_swapChain) {
        m_device.WaitIdle();
        ReleaseBackBuffers();
    }
}

bool D3D12SwapChain::Init(const SwapChainDesc& desc)
{
    m_hwnd = static_cast<HWND>(desc.windowHandle);
    m_width = Max(1u, desc.width);
    m_height = Max(1u, desc.height);
    m_bufferCount = Clamp(desc.bufferCount, 2u, 4u);
    m_format = desc.format;
    m_vsync = desc.vsync;
    m_tearing = m_device.GetCaps().tearingSupported;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = m_width;
    sd.Height = m_height;
    sd.Format = ToDxgi(m_format);
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = m_bufferCount;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> sc1;
    if (!ZX_DX_CHECK(m_device.Factory()->CreateSwapChainForHwnd(m_device.GraphicsQueue(), m_hwnd, &sd, nullptr, nullptr,
                                                                sc1.ReleaseAndGetAddressOf())))
        return false;
    if (!ZX_DX_CHECK(sc1.As(m_swapChain)))
        return false;
    // Fullscreen is handled by the window (borderless); disable DXGI's Alt+Enter handling.
    m_device.Factory()->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    CreateBackBuffers();
    return true;
}

void D3D12SwapChain::CreateBackBuffers()
{
    for (u32 i = 0; i < m_bufferCount; ++i) {
        ComPtr<ID3D12Resource> buffer;
        ZX_DX_CHECK(m_swapChain->GetBuffer(i, ZX_DX_PPV(buffer)));
        m_backBuffers.PushBack(m_device.WrapTexture(buffer.Get(), m_format, String::Format("BackBuffer {}", i)));
    }
}

void D3D12SwapChain::ReleaseBackBuffers()
{
    for (D3D12Texture* t : m_backBuffers)
        m_device.DestroyImmediate(t);
    m_backBuffers.Clear();
}

Texture* D3D12SwapChain::GetCurrentBackBuffer() { return m_backBuffers[m_swapChain->GetCurrentBackBufferIndex()]; }

void D3D12SwapChain::Resize(u32 width, u32 height)
{
    if (width == 0 || height == 0) {
        RECT r;
        ::GetClientRect(m_hwnd, &r);
        width = u32(r.right - r.left);
        height = u32(r.bottom - r.top);
    }
    if (width == 0 || height == 0 || (width == m_width && height == m_height))
        return; // minimized or unchanged
    m_device.WaitIdle();
    ReleaseBackBuffers();
    if (!ZX_DX_CHECK(m_swapChain->ResizeBuffers(m_bufferCount, width, height, DXGI_FORMAT_UNKNOWN,
                                                m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
        return;
    m_width = width;
    m_height = height;
    CreateBackBuffers();
}

void D3D12SwapChain::Present()
{
    const UINT sync = m_vsync ? 1 : 0;
    const UINT flags = (!m_vsync && m_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT hr = m_swapChain->Present(sync, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        m_device.OnDeviceRemoved(hr);
    else if (FAILED(hr))
        ZX_DX_CHECK(hr);
}

} // namespace zx::rhi::d3d12
