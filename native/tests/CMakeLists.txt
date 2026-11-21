# Native tray regression checks. Configure with -DSUPERMENU_BUILD_TESTS=ON.
# The pure-function suites (tray_match, tray_click, tray_protocol) link no Win32
# tray code; tray_probe drives the production atrium::TrayModel adapter against
# the live shell (README/docs/TRAY.md).

set(TR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(TR_SRC ${TR_ROOT}/src)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-unused-function -Wno-missing-field-initializers)
add_link_options(-static)

function(tray_test target source)
    add_executable(${target} ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    target_include_directories(${target} PRIVATE ${TR_SRC} ${TR_SRC}/atrium)
    target_link_libraries(${target} PRIVATE user32 shell32 ole32 oleaut32 uuid)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

tray_test(tray_match_tests tray_match_test.cpp)
target_sources(tray_match_tests PRIVATE ${TR_SRC}/atrium/shell/tray_match.cpp)
tray_test(tray_click_tests tray_click_test.cpp)
tray_test(tray_protocol_tests tray_protocol_test.cpp)
tray_test(tray_pointer_tests tray_pointer_test.cpp)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tray_probe.cpp)
    add_executable(tray_probe
        ${CMAKE_CURRENT_SOURCE_DIR}/tray_probe.cpp
        ${TR_SRC}/atrium/shell/tray_model.cpp
        ${TR_SRC}/atrium/shell/tray_match.cpp
        ${TR_SRC}/atrium/shell/tray_host.cpp
        ${TR_SRC}/atrium/shell/shell_icon.cpp
        ${TR_SRC}/atrium/log.cpp
        ${TR_SRC}/log.cpp
    )
    target_include_directories(tray_probe PRIVATE ${TR_SRC} ${TR_SRC}/atrium)
    target_link_libraries(tray_probe PRIVATE
        windowscodecs ole32 oleaut32 uuid user32 gdi32 shell32 shlwapi version
        advapi32 comctl32 uiautomationcore)
endif()
