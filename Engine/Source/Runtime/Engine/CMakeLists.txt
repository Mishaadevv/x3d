zx_add_module(Engine DEPS Platform Render Scene Script Audio)
# The C++ Game SDK header (game modules include it too).
target_include_directories(ZXEngine PUBLIC "${CMAKE_SOURCE_DIR}/Engine/Source/GameSDK")
