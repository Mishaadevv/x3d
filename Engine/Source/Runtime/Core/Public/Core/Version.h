#pragma once

#include "Core/Types.h"

namespace zx {

inline constexpr const char* EngineName = "ZeqouX3D";
// Defined by CMake from project(VERSION) (CMakeLists.txt); history in docs/CHANGELOG.md.
inline constexpr u32 EngineVersionMajor = ZX_VERSION_MAJOR;
inline constexpr u32 EngineVersionMinor = ZX_VERSION_MINOR;
inline constexpr u32 EngineVersionPatch = ZX_VERSION_PATCH;
inline constexpr const char* EngineVersionString = ZX_VERSION_STRING;

#if ZX_BUILD_DEBUG
inline constexpr const char* BuildConfigurationName = "Debug";
#elif ZX_BUILD_DEVELOPMENT
inline constexpr const char* BuildConfigurationName = "Development";
#else
inline constexpr const char* BuildConfigurationName = "Shipping";
#endif

} // namespace zx
