#pragma once

#include "Core/Types.h"

namespace zx::platform {

/// Initializes the platform layer: per-monitor DPI awareness (v2), COM (STA) for shell dialogs,
/// window class registration. Call once at startup on the main thread, after CoreInit.
bool InitializePlatform();
void ShutdownPlatform();

} // namespace zx::platform
