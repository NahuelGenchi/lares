cmake_minimum_required(VERSION 3.20)
project(lares-windows LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /WX /utf-8 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Shared pure-C core modules (zero GLib or Win32 dependencies)
add_library(lares_win_core STATIC
    src/cli.cpp
    src/config.cpp
    src/devices.cpp
    src/helper.cpp
    src/hid_win.cpp
    src/livelight.cpp
    src/hw.cpp
    src/pad.cpp
    src/remote.cpp
    src/theme.cpp
    ../src/art.c
)
target_include_directories(lares_win_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(lares_win_core PUBLIC
    pdh
    advapi32
    shell32
    user32
    gdi32
    setupapi
    hid
    bthprops
    cfgmgr32
)

# Main native Win32 + Direct2D software-rendered application
add_executable(lares WIN32
    src/main.cpp
    src/font.cpp
    src/ui.cpp
    src/ui_pages.cpp
    src/ui_remote.cpp
    src/ui_devices.cpp
    res/lares.rc
    res/lares.manifest
)
target_include_directories(lares PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/res
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(lares PRIVATE
    lares_win_core
    d2d1
    dwrite
    dwmapi
    shell32
    shlwapi
    user32
    gdi32
    advapi32
)

# Background helper & system-tray controller service (no Direct2D window overhead)
add_executable(lares-pad WIN32
    src/helper_main.cpp
    res/lares.rc
    res/lares.manifest
)
target_include_directories(lares-pad PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/res
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(lares-pad PRIVATE
    lares_win_core
)

# Unit tests on plain data (CLI, theme interpolation, INI config, shared art)
add_executable(lares-win-unit
    tests/unit.cpp
)
target_include_directories(lares-win-unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(lares-win-unit PRIVATE
    lares_win_core
)

enable_testing()
add_test(NAME lares-win-unit COMMAND lares-win-unit)
