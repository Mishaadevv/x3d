cmake_minimum_required(VERSION 3.28)

# Engine version (single source of truth: Core/Version.h and the executables' version resources read it).
# Scheme: 0.<last closed milestone>.<release within it> until 1.0 (see docs/CHANGELOG.md).
project(ZeqouX3D VERSION 0.10.2 LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "ZeqouX3D currently supports only MSVC on Windows.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ZXConfig)
include(ZXModule)

set(ZX_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(ZX_ENGINE_DIR "${ZX_ROOT_DIR}/Engine")
set(ZX_SOURCE_DIR "${ZX_ENGINE_DIR}/Source")

# Runtime modules (order = dependency order)
add_subdirectory(Engine/Source/Runtime/Core)
add_subdirectory(Engine/Source/Runtime/Script)
add_subdirectory(Engine/Source/Runtime/Audio)
add_subdirectory(Engine/Source/Runtime/Platform)
add_subdirectory(Engine/Source/Runtime/RHI)
add_subdirectory(Engine/Source/Runtime/RHI_D3D12)
add_subdirectory(Engine/Source/Runtime/Asset)
add_subdirectory(Engine/Source/Runtime/Scene)
add_subdirectory(Engine/Source/Runtime/Physics)
add_subdirectory(Engine/Source/Runtime/Render)
add_subdirectory(Engine/Source/Runtime/Engine)
add_subdirectory(Engine/Source/Runtime/UI)

# Programs
add_subdirectory(Engine/Source/Programs/ZXEditor)

# Samples
add_subdirectory(Samples/HelloTriangle)
add_subdirectory(Samples/SceneDemo)
add_subdirectory(Samples/MeshViewer)
add_subdirectory(Samples/UIDemo)

# Tests (collects Tests/*.cpp from every module registered via zx_add_module)
add_subdirectory(Tests)
