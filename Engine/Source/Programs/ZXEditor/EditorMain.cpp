// ZXEditor entry point.
// Command line: -scene=<gltf|zscene>, -resolution=WxH, -resetlayout, -autotest[=frames], -screenshot=<png>,
// -savescene=<zscene>, -undotest, -pietest, -exec="cmd; cmd", -project=<zproj>, -newproject=<dir/Name>, -projectbrowser,
// -warp, -debuglayer, -novsync.

#include "Core/CoreInit.h"
#include "Core/Memory/Pointers.h"
#include "Editor.h"
#include "Platform/Platform.h"
#include "RHI/ShaderCompiler.h"

int main()
{
    using namespace zx;
    if (!CoreInit(CoreInitParams{.appName = "ZXEditor"}))
        return 1;
    if (!platform::InitializePlatform()) {
        CoreShutdown();
        return 1;
    }
    rhi::ShaderCompiler::Initialize();
    int result = 0;
    {
        UniquePtr<editor::Editor> ed(new editor::Editor());
        if (ed->Initialize()) {
            while (ed->Frame()) {
            }
        } else {
            result = 1;
        }
        ed->Shutdown();
        if (ed->debugLayerErrors > 0)
            result = 3;
        if (ed->selfTestFailed)
            result = 4;
    }
    rhi::ShaderCompiler::Shutdown();
    platform::ShutdownPlatform();
    const u64 leaks = CoreShutdown();
    return result != 0 ? result : (leaks == 0 ? 0 : 2);
}
