# Global build configuration: configurations, compiler flags, output dirs.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # we don't use C++20 modules; skip dependency scanning

# Exceptions are disabled engine-wide (see TZ 2.2): drop CMake's default /EHsc.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Configurations: Debug / Development / Shipping
set(CMAKE_CONFIGURATION_TYPES "Debug;Development;Shipping" CACHE STRING "" FORCE)

set(CMAKE_CXX_FLAGS_DEVELOPMENT "/O2 /Ob2 /Zi" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_SHIPPING "/O2 /Ob2 /GL /Zi" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_SHIPPING "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF /LTCG" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT "${CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_SHIPPING "${CMAKE_EXE_LINKER_FLAGS_SHIPPING}" CACHE STRING "" FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_SHIPPING "/LTCG" CACHE STRING "" FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_DEVELOPMENT "" CACHE STRING "" FORCE)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Output layout: Binaries/Win64/<Config>/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/Win64/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/Win64/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Lib/$<CONFIG>")

# Common compile options applied to every ZX target (see zx_apply_common_options).
set(ZX_COMMON_COMPILE_OPTIONS
    /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline /utf-8
    /W4 /WX
    /EHs-c- /GR-
    /wd4201   # nameless struct/union (used by math types)
    /wd4324   # structure padded due to alignment specifier (intentional cache-line alignment)
)
set(ZX_COMMON_DEFINES
    ZX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ZX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ZX_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ZX_VERSION_STRING="${PROJECT_VERSION}"
    _HAS_EXCEPTIONS=0
    _CRT_SECURE_NO_WARNINGS
    UNICODE _UNICODE
    $<$<CONFIG:Debug>:ZX_BUILD_DEBUG=1>
    $<$<CONFIG:Development>:ZX_BUILD_DEVELOPMENT=1>
    $<$<CONFIG:Shipping>:ZX_BUILD_SHIPPING=1>
)
