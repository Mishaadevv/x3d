#pragma once

// Package Project: turns a project into a standalone game folder.
//
//   <output>/<Name>.exe       ZXGame (the runtime), renamed
//   <output>/Game.json        name, startup scene, window, cook settings
//   <output>/Content.zpak     Game/ (the project's Content), DDC/ (cooked for the packaged paths), Shaders/,
//                             ShaderCache/ (warm cache), Project/Config/
//   <output>/dxcompiler.dll, dxil.dll, the C++ game module DLL (if the project has one)
//
// Cooking: the asset manager's cache keys contain the source path, so the project is cooked again with its Content
// mounted at "/Game" - exactly the paths the packaged game registers - into a clean staging cache.

#include "Core/Containers/Array.h"
#include "Core/Containers/Function.h"
#include "Core/Result.h"
#include "Core/String/String.h"

namespace zx {

struct PackageOptions
{
    String name;           // game / executable name
    String projectRoot;    // physical
    String contentDir;     // physical (<project>/Content)
    String engineRoot;     // physical (Engine/Shaders, Saved/ShaderCache)
    String binariesDir;    // where ZXGame.exe and the DXC DLLs are (the editor's own directory)
    String outputDir;      // physical; created / cleared of a previous package
    String startupScene;   // relative to contentDir ('/' separators)
    String gameModuleDll;  // physical path of the built C++ module (empty = none)
    u32 textureMaxSize = 0; // 0 = full resolution; 1024 = Lite
    bool fullscreen = true;
    Array<String> disabledFeatures;
};

struct PackageReport
{
    String executable;
    u32 files = 0;        // in the package
    u32 cookedAssets = 0; // assets in the staging cache
    u64 rawBytes = 0;
    u64 packageBytes = 0;
    f64 seconds = 0.0;
};

/// Blocking (run it on a worker thread). `status` receives progress lines (any thread).
Result<PackageReport> PackageProject(const PackageOptions& options, const Function<void(StringView)>& status = nullptr);

} // namespace zx
