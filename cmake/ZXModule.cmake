# zx_add_module(<Name> [DEPS <modules...>] [SYSTEM_LIBS <libs...>])
#   Creates static library ZX<Name> from <dir>/Public and <dir>/Private sources.
#   Public/ is the include root exposed to dependents; Private/ is internal.
#   Tests/*.cpp are not compiled into the library; they are appended to the
#   global ZX_TEST_SOURCES property and built into ZXTests.
#   Engine modules are always linked with WHOLE_ARCHIVE so that objects containing only self-registering
#   statics (reflection types, CVars, log categories, operator new) are never dropped by the linker.
#
# zx_add_program(<Name> [DEPS <modules...>] [WIN32])
#   Creates an executable from <dir>/*.cpp (recursively).

function(zx_apply_common_options target)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${ZX_COMMON_COMPILE_OPTIONS}>)
    target_compile_definitions(${target} PRIVATE ${ZX_COMMON_DEFINES})
endfunction()

function(zx_add_module name)
    cmake_parse_arguments(ARG "" "" "DEPS;SYSTEM_LIBS" ${ARGN})
    set(target ZX${name})

    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.inl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.inl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp")

    add_library(${target} STATIC ${sources})
    target_include_directories(${target}
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Public"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
    zx_apply_common_options(${target})

    foreach(dep IN LISTS ARG_DEPS)
        target_link_libraries(${target} PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,ZX${dep}>")
    endforeach()
    if(ARG_SYSTEM_LIBS)
        target_link_libraries(${target} PUBLIC ${ARG_SYSTEM_LIBS})
    endif()

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${sources})
    set_target_properties(${target} PROPERTIES FOLDER "Engine/Runtime")

    file(GLOB_RECURSE tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/*.cpp")
    if(tests)
        set_property(GLOBAL APPEND PROPERTY ZX_TEST_SOURCES ${tests})
        set_property(GLOBAL APPEND PROPERTY ZX_TEST_MODULES ${target})
    endif()
endfunction()

# Copies dxcompiler.dll + dxil.dll (shader compiler runtime from the Windows SDK) next to a target's executable.
function(zx_copy_dxc_runtime target)
    set(sdk_bin "$ENV{WindowsSdkVerBinPath}")
    if(NOT sdk_bin)
        set(sdk_bin "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/")
    endif()
    file(TO_CMAKE_PATH "${sdk_bin}/x64" dxc_dir)
    if(NOT EXISTS "${dxc_dir}/dxcompiler.dll")
        message(WARNING "dxcompiler.dll not found in ${dxc_dir}; runtime shader compilation will be unavailable")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dxc_dir}/dxcompiler.dll" "${dxc_dir}/dxil.dll"
                "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

function(zx_add_program name)
    cmake_parse_arguments(ARG "WIN32" "" "DEPS" ${ARGN})
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.rc")
    if(ARG_WIN32)
        add_executable(${name} WIN32 ${sources})
        # Keep the standard main() entry point for GUI-subsystem executables.
        target_link_options(${name} PRIVATE "/ENTRY:mainCRTStartup")
    else()
        add_executable(${name} ${sources})
    endif()
    zx_apply_common_options(${name})
    foreach(dep IN LISTS ARG_DEPS)
        target_link_libraries(${name} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,ZX${dep}>")
    endforeach()
    set_target_properties(${name} PROPERTIES FOLDER "Programs")
    if("RHI" IN_LIST ARG_DEPS OR "RHI_D3D12" IN_LIST ARG_DEPS)
        zx_copy_dxc_runtime(${name})
    endif()
endfunction()
