#include "Core/Log/Log.h"
#include "Platform/Platform.h"

#include "Win32/Win32Include.h"

#include <objbase.h>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogPlatform, Info);

namespace zx::platform {

bool RegisterWindowClass();
void UnregisterWindowClass();

namespace {
bool g_comInitialized = false;
}

bool InitializePlatform()
{
    if (!::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        ZX_LOG_WARN(LogPlatform, "Per-monitor DPI awareness v2 unavailable (already set by manifest?)");
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_comInitialized = SUCCEEDED(hr);
    // Prevent the system from sleeping/turning off the display while the engine runs is left to the app.
    if (!RegisterWindowClass()) {
        ZX_LOG_ERROR(LogPlatform, "Failed to register window class");
        return false;
    }
    return true;
}

void ShutdownPlatform()
{
    UnregisterWindowClass();
    if (g_comInitialized)
        ::CoUninitialize();
    g_comInitialized = false;
}

} // namespace zx::platform
