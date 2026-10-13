# ZXTests: one executable with tests from every module (Engine/Source/**/Tests/*.cpp).
get_property(test_sources GLOBAL PROPERTY ZX_TEST_SOURCES)
get_property(test_modules GLOBAL PROPERTY ZX_TEST_MODULES)

add_executable(ZXTests TestsMain.cpp ${test_sources})
zx_apply_common_options(ZXTests)
foreach(module IN LISTS test_modules)
    target_link_libraries(ZXTests PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${module}>")
endforeach()
set_target_properties(ZXTests PROPERTIES FOLDER "Tests")
zx_copy_dxc_runtime(ZXTests)
