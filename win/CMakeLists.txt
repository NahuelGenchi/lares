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
    ${LARES_SHARED_INC}
)
target_link_libraries(lares PRIVATE
    lares_win_core
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

# ---------------------------------------------------------------------------
# AirDrop without WSL (issue #109): lares-share.exe runs the Wi-Fi adapter
# over WinUSB, AWDL, IPv6 and AirDrop itself, and talks to lares.exe in
# lines (win/share/main.cpp).

# Vendored libraries: zlib 1.3.1 (zlib licence) and lwIP 2.2.1's IPv6, UDP
# and TCP (BSD-3-Clause). Their own warnings aren't Lares' to fix.
file(GLOB LARES_ZLIB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib/*.c)
add_library(lares_zlib STATIC ${LARES_ZLIB_SOURCES})
set_property(TARGET lares_zlib PROPERTY COMPILE_OPTIONS /W3 /utf-8 /wd4996 /wd4267 /wd4244)
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
set_property(TARGET lares_lwip PROPERTY COMPILE_OPTIONS /W3 /utf-8 /wd4996 /wd4267 /wd4244 /wd4312)
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
    share/airdrop/mdns_io.cpp)
target_link_libraries(lares_share_link PUBLIC lares_share_core newdev)

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
