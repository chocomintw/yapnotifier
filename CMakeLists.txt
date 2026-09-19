cmake_minimum_required(VERSION 3.24)
project(YapNotifier LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "YapNotifier only builds as x64 Windows (open an x64 VS prompt or use -A x64)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the .asi must not depend on a vcredist the player may not have.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook declares cmake_minimum_required(3.0); tell CMake 4.x to accept it.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.4)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9b)
FetchContent_MakeAvailable(minhook imgui)

# ImGui ships no CMakeLists; core + the two backends we use + std::string InputText helper.
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

add_library(YapNotifier SHARED
  src/main.cpp
  src/log.cpp
  src/config.cpp
  src/hooks.cpp
  src/overlay.cpp
  src/teamspeak.cpp
  YapNotifier.rc)  # FX_ASI_BUILD resources: FiveM refuses the .asi without them
target_link_libraries(YapNotifier PRIVATE minhook imgui ws2_32)
target_compile_definitions(YapNotifier PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(YapNotifier PRIVATE /W4 /permissive- /utf-8)
set_target_properties(YapNotifier PROPERTIES PREFIX "" SUFFIX ".asi")

set(YAPNOTIFIER_DEPLOY_DIR "" CACHE PATH "FiveM plugins directory; when set, the .asi is copied there after every build")
if(YAPNOTIFIER_DEPLOY_DIR)
  add_custom_command(TARGET YapNotifier POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:YapNotifier> "${YAPNOTIFIER_DEPLOY_DIR}/"
    COMMENT "Deploying YapNotifier.asi to ${YAPNOTIFIER_DEPLOY_DIR}")
endif()
