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

# The one header of the Linux sources the port includes. Their whole folder
# isn't on the include path: its share.h would hide the C runtime's own.
set(LARES_SHARED_INC ${CMAKE_CURRENT_BINARY_DIR}/shared)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../src/art.h ${LARES_SHARED_INC}/art.h COPYONLY)

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
    src/share_win.cpp
    src/theme.cpp
    ../src/art.c
)
target_include_directories(lares_win_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LARES_SHARED_INC}
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
    src/ui_share.cpp
    src/ble_wake.cpp
    res/lares.rc
    res/lares.manifest
)
target_include_directories(lares PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/res
    ${LARES_SHARED_INC}
)
target_link_libraries(lares PRIVATE
    lares_win_core
    d2d1
    dwrite
    dwmapi
    ole32
    windowsapp
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
    ${LARES_SHARED_INC}
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
    ${LARES_SHARED_INC}
)
target_link_libraries(lares-win-unit PRIVATE
    lares_win_core
)

enable_testing()
add_test(NAME lares-win-unit COMMAND lares-win-unit)

# The kit the Share section sets WSL up with (issue #98): the scripts and the
# Linux sources lares-awdl and lares-share are built from there, beside
# lares.exe, with a VERSION that changes when any of them does.
file(GLOB WSL_KIT_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/wsl/*
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/awdl/*
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/airdrop/*
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sharecli/*
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/share.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/share.h)
set(WSL_KIT_HASH "")
foreach(f ${WSL_KIT_FILES})
    file(SHA256 ${f} h)
    string(APPEND WSL_KIT_HASH ${h})
endforeach()
string(SHA256 WSL_KIT_VERSION "${WSL_KIT_HASH}")
string(SUBSTRING ${WSL_KIT_VERSION} 0 16 WSL_KIT_VERSION)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/wsl-kit-version "${WSL_KIT_VERSION}\n")
add_custom_command(TARGET lares POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/wsl $<TARGET_FILE_DIR:lares>/wsl
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../src/awdl $<TARGET_FILE_DIR:lares>/wsl/src/awdl
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../src/airdrop $<TARGET_FILE_DIR:lares>/wsl/src/airdrop
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../src/sharecli $<TARGET_FILE_DIR:lares>/wsl/src/sharecli
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/../src/share.c ${CMAKE_CURRENT_SOURCE_DIR}/../src/share.h $<TARGET_FILE_DIR:lares>/wsl/src/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/wsl-kit-version $<TARGET_FILE_DIR:lares>/wsl/VERSION)
