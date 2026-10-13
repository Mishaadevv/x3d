#pragma once

#include "RHI/RHI.h"

namespace zx::rhi::d3d12 {

/// Creates the Direct3D 12 device. Returns null if no suitable GPU is available.
UniquePtr<Device> CreateD3D12Device(const DeviceDesc& desc);

/// Number of D3D12 debug-layer errors seen so far (0 if the debug layer is off). Used by tests.
u32 GetDebugLayerErrorCount(Device& device);

} // namespace zx::rhi::d3d12
