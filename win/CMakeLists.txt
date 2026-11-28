cmake_minimum_required(VERSION 3.20)
project(lares-windows LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /WX /utf-8 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

    # Keep the builder's folders out of what is built (issue #122): __FILE__
    # starts at the repository instead of the drive, and a program names its
    # symbol file without the folder. tools/privacy-gate.mjs --binaries checks it.
    get_filename_component(LARES_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    file(TO_NATIVE_PATH "${LARES_REPO_ROOT}/" LARES_TRIM_SOURCE)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/" LARES_TRIM_BUILD)
    set(LARES_TRIM_OPTIONS "/d1trimfile:${LARES_TRIM_BUILD}" "/d1trimfile:${LARES_TRIM_SOURCE}")
    add_compile_options(${LARES_TRIM_OPTIONS})
    add_link_options(/PDBALTPATH:%_PDB%)
endif()

# The one header of the Linux sources the port includes. Their whole folder
# isn't on the include path: its share.h would hide the C runtime's own.
set(LARES_SHARED_INC ${CMAKE_CURRENT_BINARY_DIR}/shared)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../src/art.h ${LARES_SHARED_INC}/art.h COPYONLY)

# The version crash reports name (issue #130): "dev" and the commit, taken
# when the build is configured.
set(LARES_VERSION "dev")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE LARES_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        RESULT_VARIABLE LARES_GIT_RESULT)
    if(LARES_GIT_RESULT EQUAL 0 AND LARES_GIT_COMMIT)
        set(LARES_VERSION "dev ${LARES_GIT_COMMIT}")
    endif()
endif()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/crash/version.h.in ${LARES_SHARED_INC}/lares_version.h @ONLY)

# Local crash reports (issue #130): written on this PC only, private details
# removed. Plain Win32 with no C runtime; the guard builds the file itself,
# with its own options.
add_library(lares_crash STATIC crash/crash.c)
target_include_directories(lares_crash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/crash ${LARES_SHARED_INC})

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
    src/programs.cpp
    src/remote.cpp
    src/share_win.cpp
    src/share_bridge.cpp
    src/shell_prefs.cpp
    src/shell_service.cpp
    src/theme.cpp
    ../src/art.c
)
target_include_directories(lares_win_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LARES_SHARED_INC}
)
target_link_libraries(lares_win_core PUBLIC
    lares_journal
    pdh
    advapi32
    shell32
    shlwapi
    user32
    gdi32
    setupapi
    hid
    bthprops
    cfgmgr32
)

# The Bluetooth LE wake-up is the one file that uses C++/WinRT. It is built as
# C++20 on its own: under C++17 its headers fall back to <experimental/coroutine>,
# which newer MSVC toolsets refuse.
add_library(lares_ble OBJECT src/ble_wake.cpp)
set_target_properties(lares_ble PROPERTIES CXX_STANDARD 20)
target_include_directories(lares_ble PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LARES_SHARED_INC}
)

# Main native Win32 + Direct2D software-rendered application
add_executable(lares WIN32
    src/main.cpp
    src/changes.cpp
    src/crash_view.cpp
    guard/safe.c
    src/font.cpp
    src/ui.cpp
    src/ui_pages.cpp
    src/ui_remote.cpp
    src/ui_devices.cpp
    src/ui_share.cpp
    $<TARGET_OBJECTS:lares_ble>
    res/lares.rc
    res/lares.manifest
)
target_include_directories(lares PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/res
    ${CMAKE_CURRENT_SOURCE_DIR}/guard
    ${LARES_SHARED_INC}
)
target_link_libraries(lares PRIVATE
    lares_win_core
    lares_journal
    lares_crash
    d2d1
    dwrite
    dwmapi
    uxtheme
    ole32
    windowscodecs
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
    lares_crash
)

# Unit tests on plain data (CLI, theme interpolation, INI config, shared art)
add_executable(lares-win-unit
    tests/unit.cpp
    src/crash_view.cpp
)
target_include_directories(lares-win-unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LARES_SHARED_INC}
)
target_link_libraries(lares-win-unit PRIVATE
    lares_win_core
    lares_crash
)

enable_testing()
add_test(NAME lares-win-unit COMMAND lares-win-unit)

# ---------------------------------------------------------------------------
# Crash safety (issue #127): lares-guard.exe gives the Windows taskbar back
# when Lares' bar stops. To stay under 1 MB of its own memory it links without
# the C runtime (a Debug build keeps it for its checks) and loads user32 only
# in the short-lived copies of itself that hide and give back.
add_executable(lares-guard WIN32
    guard/guard.c
    guard/safe.c
    guard/state.c
    guard/taskbar.c
    guard/taskbar_journal.c
    journal/journal.c
    crash/crash.c
    guard/guard.manifest
)
target_include_directories(lares-guard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/journal
    ${CMAKE_CURRENT_SOURCE_DIR}/crash
    ${LARES_SHARED_INC})
target_compile_options(lares-guard PRIVATE /GS-)
target_compile_definitions(lares-guard PRIVATE $<$<NOT:$<CONFIG:Debug>>:LARES_NO_CRT>)
target_link_options(lares-guard PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:/NODEFAULTLIB>
    $<$<NOT:$<CONFIG:Debug>>:/ENTRY:lares_guard_entry>
    /DELAYLOAD:user32.dll)
target_link_libraries(lares-guard PRIVATE kernel32 user32 $<$<CONFIG:Debug>:delayimp>)

# Tests only, never installed: a stand-in for the bar that quits, crashes and
# hangs when told, and the program that kills guard, twin and shell in every
# order against a stand-in taskbar.
add_executable(lares-test-shell WIN32
    tests/test_shell.c
    guard/guard_client.c
    guard/state.c
    guard/taskbar.c
    guard/taskbar_journal.c
    journal/journal.c)
target_include_directories(lares-test-shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/guard
    ${CMAKE_CURRENT_SOURCE_DIR}/journal)
target_link_libraries(lares-test-shell PRIVATE lares_crash)
add_executable(lares-guard-test tests/guard_test.c tests/guard_state_test.c)
target_include_directories(lares-guard-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/guard
    ${CMAKE_CURRENT_SOURCE_DIR}/journal)
target_sources(lares-guard-test PRIVATE guard/state.c guard/safe.c journal/journal.c)
target_link_libraries(lares-guard-test PRIVATE shell32 user32 lares_crash)
add_dependencies(lares-guard-test lares-guard lares-test-shell)
add_test(NAME lares-guard-test COMMAND lares-guard-test)
set_tests_properties(lares-guard-test PROPERTIES TIMEOUT 600)

# The change journal (issue #129): every Windows setting Lares changes is
# written down, and flushed, before it is changed, and one routine undoes
# them. journal.c is plain Win32 with no C runtime, so the guard links it.
add_library(lares_journal STATIC
    journal/journal.c
    journal/journal_win.c
    guard/taskbar_journal.c
    guard/taskbar.c)
target_include_directories(lares_journal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/journal
    ${CMAKE_CURRENT_SOURCE_DIR}/guard)
target_link_libraries(lares_journal PUBLIC advapi32 user32)

add_executable(lares-journal-test tests/journal_test.c)
target_link_libraries(lares-journal-test PRIVATE lares_journal)
add_test(NAME lares-journal-test COMMAND lares-journal-test)

# A crash of the stand-in shell, and the report it leaves.
add_executable(lares-crash-test tests/crash_test.c)
target_link_libraries(lares-crash-test PRIVATE lares_crash)
add_dependencies(lares-crash-test lares-test-shell)
add_test(NAME lares-crash-test COMMAND lares-crash-test)

# The drawing engine comparison (issue #140): draws the same shell screens
# with GDI+ and Lares' own glyph blending, and with a Direct2D software
# target, each in a process of its own, and prints what each costs.
add_executable(lares-draw-bench bench/draw_bench.cpp)
# Each engine's libraries load only in the process that uses them.
target_link_libraries(lares-draw-bench PRIVATE gdiplus d2d1 dwrite windowscodecs ole32 psapi gdi32 user32 delayimp)
target_link_options(lares-draw-bench PRIVATE /DELAYLOAD:gdiplus.dll /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll)

# ---------------------------------------------------------------------------
# The desktop shell (stage 2, issue #139). Its drawing layer (issue #141):
# GDI+ for shapes and pictures, DirectWrite glyphs blended by draw.c, the
# engine chosen in #140. Plain C.
add_library(lares_shell_draw STATIC
    shell/draw.c
    shell/palette.c)
target_include_directories(lares_shell_draw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shell)
target_link_libraries(lares_shell_draw PUBLIC gdiplus dwrite gdi32 user32 advapi32)

add_executable(lares-shell-draw-test tests/shell_draw_test.c)
target_link_libraries(lares-shell-draw-test PRIVATE lares_shell_draw)
add_test(NAME lares-shell-draw-test COMMAND lares-shell-draw-test)

# lares-shell.exe (issue #145): the top bar and the dock, started by
# lares-guard.exe, which owns hiding the Windows taskbar. Plain C.
add_executable(lares-shell WIN32
    shell/main.c
    shell/config.c
    shell/actions.c
    shell/popups.c
    shell/launcher.c
    shell/clipboard.c
    shell/procmon.c
    shell/screenshot.c
    shell/win_tracker.c
    shell/telemetry.c
    shell/media.c
    shell/hotkeys.c
    shell/topbar.c
    shell/dock.c
    shell/airdrop.c
    $<TARGET_OBJECTS:lares_ble>
    guard/guard_client.c
    guard/state.c
    shell/shell.manifest)
target_include_directories(lares-shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shell
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/guard
    ${CMAKE_CURRENT_SOURCE_DIR}/journal
    ${CMAKE_CURRENT_SOURCE_DIR}/crash
    ${LARES_SHARED_INC})
target_compile_definitions(lares-shell PRIVATE COBJMACROS CINTERFACE)
target_link_libraries(lares-shell PRIVATE
    lares_shell_draw lares_win_core lares_journal lares_crash
    shell32 shlwapi ole32 oleaut32 uuid dwmapi propsys pdh iphlpapi ws2_32 wlanapi bthprops
    runtimeobject psapi windowscodecs comctl32 powrprof comdlg32 windowsapp)

# The shell's switches turned on and off from the outside, through the code
# Lares' Settings use, against a stand-in taskbar unless --real (issue #145).
add_executable(lares-shell-test tests/shell_test.cpp)
target_link_libraries(lares-shell-test PRIVATE lares_win_core lares_journal psapi)
add_dependencies(lares-shell-test lares-shell lares-guard)
# lares-view.exe (issue #151): the image viewer and the text and Markdown
# editor, started when a file is opened in them and ended with their last
# window. Plain C, on the shell's drawing layer.
add_executable(lares-view WIN32
    shell/view_main.c
    shell/viewer.c
    shell/textview.c
    shell/filetypes.c
    shell/shell.manifest)
target_include_directories(lares-view PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shell
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/crash
    ${LARES_SHARED_INC})
target_compile_definitions(lares-view PRIVATE COBJMACROS CINTERFACE)
target_link_libraries(lares-view PRIVATE
    lares_shell_draw lares_crash
    shell32 shlwapi ole32 oleaut32 uuid dwmapi windowscodecs comctl32 comdlg32)
# lares-files.exe (issue #153): Files, started when a folder is opened in
# it and ended with its last window. Plain C, on the shell's drawing layer.
add_executable(lares-files WIN32
    shell/files_main.c
    shell/files.c
    shell/filetypes.c
    shell/shell.manifest)
target_include_directories(lares-files PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shell
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/crash
    ${LARES_SHARED_INC})
target_compile_definitions(lares-files PRIVATE COBJMACROS CINTERFACE)
target_link_libraries(lares-files PRIVATE
    lares_shell_draw lares_win_core lares_crash
    shell32 shlwapi ole32 oleaut32 uuid dwmapi windowscodecs comctl32 propsys)
add_test(NAME lares-shell-test COMMAND lares-shell-test)
set_tests_properties(lares-shell-test PROPERTIES TIMEOUT 300)

# ---------------------------------------------------------------------------
# AirDrop without WSL (issue #109): lares-share.exe runs the Wi-Fi adapter
# over WinUSB, AWDL, IPv6 and AirDrop itself, and talks to lares.exe in
# lines (win/share/main.cpp).

# Vendored libraries: zlib 1.3.1 (zlib licence) and lwIP 2.2.1's IPv6, UDP
# and TCP (BSD-3-Clause). Their own warnings aren't Lares' to fix.
file(GLOB LARES_ZLIB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib/*.c)
add_library(lares_zlib STATIC ${LARES_ZLIB_SOURCES})
set_property(TARGET lares_zlib PROPERTY COMPILE_OPTIONS /W3 /utf-8 /wd4996 /wd4267 /wd4244 ${LARES_TRIM_OPTIONS})
target_include_directories(lares_zlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib)

set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/src)
add_library(lares_lwip STATIC
    ${LWIP_DIR}/core/def.c ${LWIP_DIR}/core/inet_chksum.c ${LWIP_DIR}/core/init.c ${LWIP_DIR}/core/ip.c
    ${LWIP_DIR}/core/mem.c ${LWIP_DIR}/core/memp.c ${LWIP_DIR}/core/netif.c ${LWIP_DIR}/core/pbuf.c
    ${LWIP_DIR}/core/stats.c ${LWIP_DIR}/core/sys.c ${LWIP_DIR}/core/tcp.c ${LWIP_DIR}/core/tcp_in.c
    ${LWIP_DIR}/core/tcp_out.c ${LWIP_DIR}/core/timeouts.c ${LWIP_DIR}/core/udp.c
    ${LWIP_DIR}/core/ipv6/icmp6.c ${LWIP_DIR}/core/ipv6/inet6.c ${LWIP_DIR}/core/ipv6/ip6.c
    ${LWIP_DIR}/core/ipv6/ip6_addr.c ${LWIP_DIR}/core/ipv6/ip6_frag.c ${LWIP_DIR}/core/ipv6/mld6.c
    ${LWIP_DIR}/core/ipv6/nd6.c
    ${LWIP_DIR}/api/api_lib.c ${LWIP_DIR}/api/api_msg.c ${LWIP_DIR}/api/err.c ${LWIP_DIR}/api/if_api.c
    ${LWIP_DIR}/api/netbuf.c ${LWIP_DIR}/api/sockets.c ${LWIP_DIR}/api/tcpip.c
    share/net/sys_arch.c)
set_property(TARGET lares_lwip PROPERTY COMPILE_OPTIONS /W3 /utf-8 /wd4996 /wd4267 /wd4244 /wd4312 ${LARES_TRIM_OPTIONS})
target_include_directories(lares_lwip PUBLIC ${LWIP_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/share/net)

# The Realtek driver, ported from Linux rtl8xxxu: GPL-2.0-only.
set(RTL_SOURCES
    share/rtl8188eu.c
    share/rtl8188eu_tables.c)
# AirDrop's formats on plain bytes: plists, CPIO, compression, HTTP, mDNS
# messages, names.
set(AIRDROP_FORMAT_SOURCES
    share/airdrop/names.cpp
    share/airdrop/plist.cpp
    share/airdrop/cpio.cpp
    share/airdrop/compress.cpp
    share/airdrop/http.cpp
    share/airdrop/mdns.cpp
    share/airdrop/record.cpp)
add_library(lares_share_core STATIC
    share/rtl_usb.c
    ../src/awdl/frame.c
    ../src/awdl/node.c
    ${RTL_SOURCES}
    ${AIRDROP_FORMAT_SOURCES})
# The Linux AWDL sources build as they are; casts that gcc takes quietly
# don't get rewritten for MSVC.
set_source_files_properties(../src/awdl/frame.c ../src/awdl/node.c PROPERTIES COMPILE_OPTIONS "/wd4310")
target_include_directories(lares_share_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/share
    ${CMAKE_CURRENT_SOURCE_DIR}/share/airdrop
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/awdl)
target_link_libraries(lares_share_core PUBLIC lares_lwip lares_zlib winusb setupapi cfgmgr32 secur32 crypt32 ncrypt bcrypt advapi32 shell32 ole32)

# What only the program itself uses: the driver switch, the link, the IP
# stack's interface, TLS and the session.
add_library(lares_share_link STATIC
    share/driver_setup.cpp
    share/link.c
    share/net.c
    share/tls.cpp
    share/model.cpp
    share/airdrop/session.cpp
    share/airdrop/thumb.cpp
    share/airdrop/mdns_io.cpp)
target_link_libraries(lares_share_link PUBLIC lares_share_core newdev windowscodecs gdi32)

add_executable(lares-share share/main.cpp share/firmware.rc)
target_link_libraries(lares-share PRIVATE lares_share_link)

add_executable(lares-share-unit
    tests/share_unit.cpp
    tests/share_driver.cpp
    tests/share_rtl.cpp
    tests/share_formats.cpp
    tests/share_net.cpp)
target_include_directories(lares-share-unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(lares-share-unit PRIVATE lares_share_link)
add_test(NAME lares-share-unit COMMAND lares-share-unit)

# lares.exe starts lares-share.exe from beside itself; the firmware's licence
# travels with it.
add_dependencies(lares lares-share)
add_custom_command(TARGET lares-share POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/share/firmware/LICENCE.rtlwifi_firmware.txt $<TARGET_FILE_DIR:lares-share>/)
