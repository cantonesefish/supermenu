cmake_minimum_required(VERSION 3.25)
project(SuperMenu VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SUPERMENU_BUILD_TESTS "Build native tray regression checks and probes" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The glass HLSL is compiled at runtime with d3dcompiler_47.dll. Embed the source
# as a C string at configure time so the exe is self-contained.
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})
file(READ ${SHADER_DIR}/glass.hlsl GLASS_HLSL_SOURCE)
configure_file(${SHADER_DIR}/glass_shader_data.h.in ${GENERATED_DIR}/glass_shader_data.h @ONLY)

# The build version is defined once in this project() line. The generated header
# feeds the startup log and the About dialog, and the generated .rc carries the
# same numbers into the executable's version resource.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/supermenu_version.h.in ${GENERATED_DIR}/supermenu_version.h @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc.in ${GENERATED_DIR}/app.rc @ONLY)

add_executable(SuperMenuBar WIN32
    src/main.cpp
    src/app.cpp
    src/bar_window.cpp
    src/popup_window.cpp
    src/menu.cpp
    src/settings.cpp
    src/json.cpp
    src/log.cpp
    src/atrium/log.cpp
    src/atrium/shell/shell_icon.cpp
    src/atrium/shell/tray_host.cpp
    src/atrium/shell/tray_model.cpp
    src/atrium/shell/tray_match.cpp
    src/gpu.cpp
    src/capture.cpp
    src/glass_surface.cpp
    src/system_status.cpp
    src/media_monitor.cpp
    src/ui_util.cpp
    src/icons.cpp
    ${GENERATED_DIR}/app.rc
)

target_include_directories(SuperMenuBar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/atrium
    ${GENERATED_DIR}
)

target_compile_definitions(SuperMenuBar PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_compile_options(SuperMenuBar PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
    -Wno-unused-function -Wno-missing-field-initializers
    -municode -finput-charset=UTF-8 -fexec-charset=UTF-8)

target_link_options(SuperMenuBar PRIVATE -static -municode -mwindows)

target_link_libraries(SuperMenuBar PRIVATE
    d3d11 dxgi d2d1 dwrite dcomp d3dcompiler
    windowscodecs ole32 oleaut32 uuid
    user32 gdi32 shell32 shlwapi dwmapi version propsys advapi32 comctl32 imm32
    uiautomationcore
    wlanapi iphlpapi dxva2 powrprof wbemuuid shcore wevtapi taskschd
)

# Portable output folder next to the exe: fonts + shader docs + settings app.
set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/dist)
add_custom_command(TARGET SuperMenuBar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}/Fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/HarmonyOS_Sans_SC_Light.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/HarmonyOS_Sans_SC_Regular.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/HarmonyOS_Sans_SC_Medium.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/HarmonyOS_Sans_SC_Bold.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/LICENSE-HarmonyOS-Sans.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/SegoeIcons.ttf
        ${OUT_DIR}/Fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SuperMenuBar> ${OUT_DIR}/SuperMenuBar.exe
    VERBATIM
)

if(SUPERMENU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
