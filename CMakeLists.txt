cmake_minimum_required(VERSION 3.24)
project(YapNotifier LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "YapNotifier only builds as x64 Windows (open an x64 VS prompt or use -A x64)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: neither binary may depend on a vcredist the player may not have.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")


include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9b)
FetchContent_Declare(ts3sdk
  GIT_REPOSITORY https://github.com/TeamSpeak-Systems/ts3client-pluginsdk.git
  GIT_TAG        4aa90a53aa150cbf81e13bc97e68c0431b26499f)  # plugin API 26, no release tags upstream
FetchContent_MakeAvailable(imgui ts3sdk)

# ImGui ships no CMakeLists; core + the two backends we use.
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

set(YAP_COMMON_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set(YAP_COMMON_OPTS /W4 /permissive- /utf-8)

# --- FiveM side: YapNotifier.asi ----------------------------------------------
add_library(YapNotifier SHARED
  src/main.cpp
  src/log.cpp
  src/config.cpp
  src/overlay.cpp
  src/teamspeak.cpp
  src/update.cpp
  YapNotifier.rc)  # FX_ASI_BUILD resources: FiveM refuses the .asi without them
target_include_directories(YapNotifier PRIVATE shared)
target_link_libraries(YapNotifier PRIVATE imgui ws2_32 winhttp bcrypt)
target_compile_definitions(YapNotifier PRIVATE ${YAP_COMMON_DEFS})
target_compile_options(YapNotifier PRIVATE ${YAP_COMMON_OPTS})
set_target_properties(YapNotifier PROPERTIES PREFIX "" SUFFIX ".asi")
# Always emit a PDB so FiveM crash offsets (YapNotifier.asi+0x...) resolve to source lines.
target_compile_options(YapNotifier PRIVATE /Zi)
target_link_options(YapNotifier PRIVATE /DEBUG /OPT:REF /OPT:ICF)

set(YAPNOTIFIER_DEPLOY_DIR "" CACHE PATH "FiveM plugins directory; when set, the .asi is copied there after every build")
if(YAPNOTIFIER_DEPLOY_DIR)
  add_custom_command(TARGET YapNotifier POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:YapNotifier> "${YAPNOTIFIER_DEPLOY_DIR}/"
    COMMENT "Deploying YapNotifier.asi to ${YAPNOTIFIER_DEPLOY_DIR}")
endif()

# --- TeamSpeak side: yapnotifier_ts3_win64.dll packaged as YapNotifier.ts3_plugin
add_library(yapnotifier_ts3 SHARED ts3plugin/plugin.cpp)
target_include_directories(yapnotifier_ts3 PRIVATE shared ${ts3sdk_SOURCE_DIR}/include)
target_link_libraries(yapnotifier_ts3 PRIVATE ws2_32)
target_compile_definitions(yapnotifier_ts3 PRIVATE ${YAP_COMMON_DEFS})
target_compile_options(yapnotifier_ts3 PRIVATE ${YAP_COMMON_OPTS})
set_target_properties(yapnotifier_ts3 PROPERTIES PREFIX "" OUTPUT_NAME "yapnotifier_ts3_win64")

# A .ts3_plugin is a zip of package.ini + plugins/<dll>; TeamSpeak installs it on double-click.
set(TS3_PKG_DIR ${CMAKE_BINARY_DIR}/ts3_package)
file(MAKE_DIRECTORY ${TS3_PKG_DIR}/plugins)  # WORKING_DIRECTORY must exist before the first command runs
add_custom_target(ts3_package ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/ts3plugin/package.ini ${TS3_PKG_DIR}/package.ini
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:yapnotifier_ts3> ${TS3_PKG_DIR}/plugins/
  COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/YapNotifier.ts3_plugin --format=zip -- package.ini plugins
  WORKING_DIRECTORY ${TS3_PKG_DIR}
  DEPENDS yapnotifier_ts3
  COMMENT "Packaging YapNotifier.ts3_plugin")

# --- Tests --------------------------------------------------------------------
enable_testing()
add_executable(test_parser tests/test_parser.cpp src/teamspeak.cpp src/update.cpp src/log.cpp)
target_include_directories(test_parser PRIVATE src shared)
target_link_libraries(test_parser PRIVATE ws2_32 winhttp bcrypt)
target_compile_definitions(test_parser PRIVATE ${YAP_COMMON_DEFS})
target_compile_options(test_parser PRIVATE ${YAP_COMMON_OPTS})
add_test(NAME parser COMMAND test_parser)
