cmake_minimum_required(VERSION 3.24)
project(YapNotifier LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "YapNotifier only builds as x64 Windows (open an x64 VS prompt or use -A x64)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: neither binary may depend on a vcredist the player may not have.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")


set(YAP_COMMON_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set(YAP_COMMON_OPTS /W4 /permissive- /utf-8)

include(FetchContent)
# UI stack: RmlUi (HTML/CSS-like documents) on FreeType, with its SVG plugin on LunaSVG for the
# vector icons. All static; OVERRIDE_FIND_PACKAGE routes RmlUi's find_package() to these fetches.
set(BUILD_SHARED_LIBS OFF)
set(FT_DISABLE_ZLIB ON)
set(FT_DISABLE_BZIP2 ON)
set(FT_DISABLE_PNG ON)
set(FT_DISABLE_HARFBUZZ ON)
set(FT_DISABLE_BROTLI ON)
set(SKIP_INSTALL_ALL ON)
set(LUNASVG_BUILD_EXAMPLES OFF)
set(LUNASVG_DISABLE_LOAD_SYSTEM_FONTS ON)  # never scan the fonts folder from inside the game
set(RMLUI_SAMPLES OFF)
set(RMLUI_SVG_PLUGIN ON)
set(RMLUI_FONT_ENGINE "freetype")
FetchContent_Declare(Freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG        VER-2-14-3
  GIT_SHALLOW    TRUE
  OVERRIDE_FIND_PACKAGE)
FetchContent_Declare(lunasvg
  GIT_REPOSITORY https://github.com/sammycage/lunasvg.git
  GIT_TAG        v3.5.0
  GIT_SHALLOW    TRUE
  OVERRIDE_FIND_PACKAGE)
FetchContent_Declare(RmlUi
  GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
  GIT_TAG        6.3
  GIT_SHALLOW    TRUE)
FetchContent_Declare(ts3sdk
  GIT_REPOSITORY https://github.com/TeamSpeak-Systems/ts3client-pluginsdk.git
  GIT_TAG        4aa90a53aa150cbf81e13bc97e68c0431b26499f)  # plugin API 26, no release tags upstream
FetchContent_MakeAvailable(Freetype lunasvg)
add_library(Freetype::Freetype ALIAS freetype)  # the name RmlUi links against; freetype's tree only exports it on install
FetchContent_MakeAvailable(RmlUi ts3sdk)

# RmlUi's Win32 + DX11 backends ship as loose sources; build them as our own lib (third-party
# code, not held to /W4). The DX11 renderer compiles its HLSL at runtime, hence d3dcompiler.
add_library(rmlui_backend STATIC
  ${rmlui_SOURCE_DIR}/Backends/RmlUi_Platform_Win32.cpp
  ${rmlui_SOURCE_DIR}/Backends/RmlUi_Renderer_DX11.cpp)
target_include_directories(rmlui_backend SYSTEM PUBLIC ${rmlui_SOURCE_DIR}/Backends)
target_link_libraries(rmlui_backend PUBLIC RmlUi::RmlUi d3d11 dxgi d3dcompiler)
target_compile_definitions(rmlui_backend PUBLIC ${YAP_COMMON_DEFS})
set_target_properties(rmlui_core lunasvg plutovg freetype PROPERTIES SYSTEM ON)  # keep /W4 clean on our TUs

# --- FiveM side: YapNotifier.asi ----------------------------------------------
add_library(YapNotifier SHARED
  src/main.cpp
  src/log.cpp
  src/colorpicker.cpp
  src/config.cpp
  src/overlay.cpp
  src/hud.cpp
  src/icons.cpp
  src/menu.cpp
  src/notify.cpp
  src/roster.cpp
  src/teamspeak.cpp
  src/ui_files.cpp
  src/update.cpp
  YapNotifier.rc)  # FX_ASI_BUILD resources: FiveM refuses the .asi without them
target_include_directories(YapNotifier PRIVATE shared)
target_link_libraries(YapNotifier PRIVATE rmlui_backend ws2_32 winhttp bcrypt dcomp dwmapi)
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
add_library(yapnotifier_ts3 SHARED ts3plugin/plugin.cpp ts3plugin/sqlite_read.cpp ts3plugin/ts_contacts.cpp)
target_include_directories(yapnotifier_ts3 PRIVATE shared ts3plugin ${ts3sdk_SOURCE_DIR}/include)
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
add_executable(test_parser tests/test_parser.cpp src/teamspeak.cpp src/update.cpp src/log.cpp src/config.cpp src/notify.cpp src/roster.cpp)
target_include_directories(test_parser PRIVATE src shared)
target_link_libraries(test_parser PRIVATE ws2_32 winhttp bcrypt)
target_compile_definitions(test_parser PRIVATE ${YAP_COMMON_DEFS})
target_compile_options(test_parser PRIVATE ${YAP_COMMON_OPTS})
add_test(NAME parser COMMAND test_parser)

add_executable(test_ui tests/test_ui.cpp src/hud.cpp src/menu.cpp src/colorpicker.cpp src/icons.cpp src/ui_files.cpp src/log.cpp src/config.cpp src/notify.cpp src/roster.cpp src/teamspeak.cpp src/update.cpp)
target_include_directories(test_ui PRIVATE src shared)
target_link_libraries(test_ui PRIVATE RmlUi::RmlUi ws2_32 winhttp bcrypt)
target_compile_definitions(test_ui PRIVATE ${YAP_COMMON_DEFS})
target_compile_options(test_ui PRIVATE ${YAP_COMMON_OPTS})
add_test(NAME ui COMMAND test_ui WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/assets/ui)
