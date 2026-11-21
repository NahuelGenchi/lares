# Lares on Windows

[Back to README](../README.md) · Research issue [#70](https://github.com/NahuelGenchi/lares/issues/70)

This is the design for a native Windows 11 port of Lares in C and C++ using Win32, Direct2D and DirectWrite, with no third-party UI runtime or bundled DLL pack. Each claim is marked **confirmed** (checked on the Windows 11 development PC or in the installed Windows 11 SDK), **reported** (documented by Microsoft or another project, not checked on live hardware here), **planned** (what Lares on Windows will do) or **blocked**.

Windows, Direct2D, XInput and Xbox are trademarks of Microsoft Corporation. HyperX, Logitech, Tailscale, LizardByte and Moonlight are trademarks of their respective owners. Lares is not affiliated with any of them.

## Goals and lightness budget

Lares on Windows follows the same rules as Linux (`AGENTS.md`):

- **Small footprint (planned):** target ~15 to 30 MB private working set for `lares.exe` and ~2 to 4 MB for `lares-pad.exe`. Measure before and after each milestone phase.
- **Software rendering by default (planned):** draw with Direct2D on a WIC/GDI software target (`D2D1_RENDER_TARGET_TYPE_SOFTWARE` / `ID2D1DCRenderTarget`) so the window never loads a 3D GPU swapchain into the UI process.
- **No idle polling (planned):** read hardware and device settings only while their section is visible and the window isn't minimized; watch connect/disconnect events through `RegisterDeviceNotificationW` (`WM_DEVICECHANGE`), HID overlapped I/O (`ReadFile` + `WaitForMultipleObjects`) and `ReadDirectoryChangesW` for config files; run all blocking I/O on worker threads.
- **Single-language native toolchain (confirmed):** builds with Visual Studio 2022 Build Tools (`MSVC 14.44` x64), Windows 11 SDK (`10.0.26100.0`) and CMake, with `/W4 /WX` (warnings as errors, matching Linux's `-Werror`). Zero external libraries beyond standard Windows system DLLs.
- **Exact visual language (planned):** embed `data/fonts/Monocraft.ttf` (`RCDATA`, loaded via `IDWriteFactory5::CreateInMemoryFontFileLoader`, `liga=0, calt=0`) and `data/pad/controller.txt` directly into the binary; match the 9 px grid (18, 27, 36 px), Home warm and Remote blue palettes in light and dark modes (`data/style.css`), spring-driven mode switch and sidebar highlight, and Windows 11 title-bar tinting (`DwmSetWindowAttribute`).

## Feature parity and real OS boundaries

| Section / feature | Status | Windows mechanism | Notes / limits |
| :--- | :--- | :--- | :--- |
| **Overview** | planned | Aggregates live state from Hardware, Devices, Pads and Remote | Same calm card layout and click-to-open navigation |
| **Hardware: CPU load, RAM, disk space** | confirmed | `GetSystemTimes`, `GlobalMemoryStatusEx`, `GetDiskFreeSpaceExW` | Unprivileged; zero drivers |
| **Hardware: GPU load & temp** | confirmed (AMD R9 380) / reported (RDNA, NVIDIA) | `atiadlxx.dll` (Overdrive5/ADL2) for AMD; `nvml.dll` for NVIDIA; PDH `\GPU Engine(*)\Utilization Percentage` universal load fallback | Unprivileged; `atiadlxx.dll` confirmed reading temperature and load on the development PC |
| **Hardware: NVMe temperature** | confirmed | `CreateFileW("\\.\PhysicalDrive0", 0, ...)` + `IOCTL_STORAGE_QUERY_PROPERTY` (`StorageDeviceTemperatureProperty`, ID `52`) | Unprivileged when opened with `dwDesiredAccess = 0`; confirmed reading composite and sensor temps on the development PC |
| **Hardware: CPU temperature** | blocked (without driver) | Windows exposes no unprivileged user-mode CPU package temp API; `MSAcpi_ThermalZoneTemperature` returns `Access denied` unprivileged and desktop boards rarely populate it | Lares ships no kernel driver (no WinRing0 / MSR driver), so CPU temperature displays as unavailable (`--`) unless a safe user-mode source is available |
| **Controller: Buttons, Sticks, Vibration** | reported / planned | `xinput1_4.dll` (`XInputGetStateEx` ordinal `#100` for Guide, `XInputSetState` for motors) + `Windows.Gaming.Input` / Raw Input for model metadata; shared pixel map `data/pad/controller.txt` | Up to 4 XInput slots; `WM_DEVICECHANGE` for hotplug |
| **Controller: Battery & BT disconnect** | reported / planned | `XInputGetBatteryInformation` + PnP property `{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2` (Bluetooth HFP/BAS battery); `IOCTL_BTH_DISCONNECT_DEVICE` for idle/manual Bluetooth disconnect | Keep-awake uses `SetThreadExecutionState(ES_DISPLAY_REQUIRED)` |
| **Controller: Programs holding pad open** | blocked | Windows has no unprivileged equivalent to scanning `/proc/<pid>/fd` for input nodes | Omitted on Windows rather than requiring elevated handle-table inspection |
| **Controller: Firmware update hint** | reported / planned | USB/BT `bcdDevice` via `HidD_GetAttributes` / PnP `DEVPKEY_Device_HardwareIds` | Same hint threshold when version is reported |
| **Headphones (Bluetooth / AirPods)** | reported / planned | `BluetoothFindFirstDevice` + PnP `BTHENUM`/`BTHLEDevice` battery property + `Windows.Devices.Bluetooth` connect/disconnect | Same card layout; shown only when paired |
| **Mouse: HyperX Pulsefire Haste 2 Wireless** | confirmed (HID access) / planned (protocol) | Win32 `SetupAPI` + `hid.dll` on vendor collection `UsagePage 0xFF00`, `Usage 0x0001` (64-byte reports) | **Confirmed** unprivileged R/W (`err=0`) on `03F0:0F98`; Windows keeps standard mouse/keyboard collections closed (`err=5`). No Allow step needed |
| **Keyboard: Logitech G715 (HID++ 2.0)** | confirmed (HID access) / planned (protocol) | Win32 `SetupAPI` + `hid.dll` on receiver vendor collections `0xFF00:0x0001` (7-byte) and `0xFF00:0x0002` (20-byte) | **Confirmed** unprivileged R/W (`err=0`) on `046D:C547`; Windows keeps keyboard keystroke interface (`0x0001:0x0006`) closed (`err=5`). Keeps `hidpp_allowed` safety allowlist and `HIDPP_SW_ID = 0x0c` / `HIDPP_SW_HELPER = 0x0e`. No Allow step needed |
| **Remote: Screen switching** | confirmed | `EnumDisplaySettingsW` + `ChangeDisplaySettingsExW` (without `CDS_UPDATEREGISTRY`, so changes stay temporary) | **Confirmed** on `\\.\DISPLAY1` for 16:9 (`1920×1080`) and 16:10 (`1680×1050`) modes |
| **Remote: Tailscale & Sunshine** | confirmed | `tailscale.exe status --json` / `up` / `down`; `OpenSCManagerW` / `QueryServiceStatusEx` on `SunshineService`; `sunshine.log` for active stream; `sunshine_state.json` checked only for non-empty `named_devices` and zeroed with `SecureZeroMemory` | Starting/stopping `SunshineService` needs Administrator once to grant `SERVICE_START \| SERVICE_STOP` to the user (one-time **Allow** button via UAC) |
| **Remote CLI** | planned | `lares home`, `lares remote`, `lares status`, `lares restore`, `--section` | Same commands and exit semantics as Linux |
| **Settings, Helper & Tray** | planned | `lares-pad.exe` background process (autostart via `HKCU\...\Run`), `Shell_NotifyIconW` tray icon + custom Direct2D popup menu + balloon/toast alerts | Replaces `systemd --user`, D-Bus and GNOME Shell JS addon (`data/addon/`) |

## Hardware probes on the Windows 11 development PC

On 2026-11-21, read-only user-mode probes were run on a Windows 11 Home (Build 26200) PC with no Administrator elevation. **Confirmed:**

- **Toolchain & SDK:** Visual Studio 2022 Build Tools (`MSVC 14.44.35207` x64) and Windows 11 SDK `10.0.26100.0` are present with all required headers (`d2d1_1.h`, `dwrite_3.h`, `dwmapi.h`, `shellapi.h`, `hidsdi.h`, `SetupAPI.h`, `Xinput.h`, `bluetoothapis.h`, `pdh.h`, `winsvc.h`, `cfgmgr32.h`, `winrt/windows.gaming.input.h`, `winrt/windows.devices.bluetooth.h`).
- **AMD GPU (`atiadlxx.dll`):** `ADL_Main_Control_Create` succeeded unprivileged on an AMD Radeon R9 380, reporting `50.0 °C` via `ADL_Overdrive5_Temperature_Get` and `15%` via `ADL_Overdrive5_CurrentActivity_Get`. PDH `\GPU Engine(*)\Utilization Percentage` returned `474` counter instances unprivileged.
- **NVMe temperature:** `CreateFileW("\\.\PhysicalDrive0", 0, ...)` + `IOCTL_STORAGE_QUERY_PROPERTY` (`StorageDeviceTemperatureProperty`, PropertyId `52`) succeeded unprivileged on a Lexar NM620 NVMe SSD, returning 3 sensors (`36 °C` composite, `90 °C` warning, `95 °C` critical). Opening with `GENERIC_READ` returned `ERROR_ACCESS_DENIED (5)`.
- **CPU temperature:** `MSAcpi_ThermalZoneTemperature` failed unprivileged with `Access denied`.
- **HyperX Pulsefire Haste 2 Wireless (`03F0:0F98`):** `SetupDiEnumDeviceInterfaces` + `HidD_GetAttributes` + `HidP_GetCaps` found the vendor collection `UP=0xFF00 U=0x0001` (`In=64 Out=64 Feat=64`), which opened `GENERIC_READ | GENERIC_WRITE` unprivileged (`err=0`). The standard mouse (`0x0001:0x0002`) and keyboard (`0x0001:0x0006`) collections returned `err=5`.
- **Logitech G715 receiver (`046D:C547`):** found both vendor collections `UP=0xFF00 U=0x0001` (`In=7 Out=7`) and `UP=0xFF00 U=0x0002` (`In=20 Out=20`), both opening `GENERIC_READ | GENERIC_WRITE` unprivileged (`err=0`), while the keyboard interface (`0x0001:0x0006`) returned `err=5`.
- **Display switching:** `EnumDisplaySettingsW` enumerated 32-bit modes including `1920×1080@60/75Hz` and `1680×1050@60/75Hz`; `ChangeDisplaySettingsExW` with `CDS_TEST` succeeded on `\\.\DISPLAY1` for both `1680×1050@60Hz` and `1920×1080@60Hz`.
- **Smart App Control:** Windows 11 Smart App Control (`VerifiedAndReputablePolicyState = 1`) blocks unsigned executables. Local development builds run with Smart App Control off; any future signed release binary would require a code-signing certificate.

## Repository layout and code sharing

The Linux build (`Makefile` and Ubuntu 26.04 `make check`) stays completely intact and dependency-free. Windows code lives under `win/` with a `CMakeLists.txt`, sharing assets from `data/` and pure C data/protocol code cleanly:

| Path | Ownership |
| :--- | :--- |
| `data/fonts/Monocraft.ttf`, `data/pad/controller.txt`, `data/com.nahuelgenchi.Lares.svg` | Shared directly by Linux and Windows (embedded as Win32 resources on Windows) |
| `src/core/` *(planned extraction, optional per module)* or portable C99 protocol modules | Pure C logic with no GLib or Win32 headers: `pad_map` parser, HyperX packet framing/decode/merge, Logitech HID++ 2.0 packet framing/CRC/`hidpp_allowed`/profile/NV codecs, Remote mode ordering/spec parsing. Shared directly by both Linux `tests/unit.c` and Windows `win/tests/` |
| `win/CMakeLists.txt` | Builds `lares.exe`, `lares-pad.exe` and `lares-win-unit.exe` with MSVC `/W4 /WX` |
| `win/src/ui/` | Native Win32 window, Direct2D/DirectWrite software renderer, Monocraft font loader, palette & Home/Remote colour transitions, sidebar, spring switch, cards, meters, graphs and pixel-art controller canvas |
| `win/src/hw.c` | CPU, RAM, disk space, NVMe temp (`IOCTL_STORAGE_QUERY_PROPERTY`), AMD ADL / NVIDIA NVML / PDH GPU telemetry |
| `win/src/remote.c` | Display modes (`EnumDisplaySettingsW` / `ChangeDisplaySettingsExW`), Tailscale CLI, `SunshineService` SCM + one-time Allow ACL, `sunshine.log` / `sunshine_state.json`, `lares.ini` in `%APPDATA%\lares\` |
| `win/src/hid_win.c`, `win/src/devices.c` | Win32 `SetupAPI` + `hid.dll` transport for HyperX and Logitech G715, Bluetooth discovery/battery/connect, USB input enumeration |
| `win/src/pad/` | XInput + `Windows.Gaming.Input` live state, rumble, battery, nicknames, Bluetooth disconnect, keep-awake |
| `win/src/helper/` | `lares-pad.exe`: message-only window, `Shell_NotifyIconW` system tray icon + pixel-art popup menu, G715 live lighting daemon, alerts, idle disconnect, single-instance named pipe IPC |
| `.github/workflows/checks.yml` | Keeps existing Ubuntu 26.04 `make check` job unchanged and adds a parallel `windows-latest` MSVC build + unit-test job once `win/` lands |

### Config and state locations on Windows

- `%APPDATA%\lares\lares.ini` — Remote mode, saved screen/service choices and setup readiness (same INI layout as `~/.config/lares/lares.ini`; dry run uses `lares-dry.ini`).
- `%APPDATA%\lares\pad.ini` — Controller alerts, notices, keep-awake, idle disconnect, tray icon preferences, G715 live lighting, background toggle and nicknames (same INI layout as `~/.config/lares/pad.ini`).
- Environment flags matching Linux: `LARES_DRY_RUN=1`, `LARES_DRY_STREAM=1`, `LARES_COLOR_SCHEME=dark|light`, `LARES_SECTION=<id>`.

### Permissions on Windows

- **Hardware, HyperX mouse, Logitech G715, Xbox controller, Bluetooth, display modes, Tailscale user toggles:** zero Administrator privileges required (all HID & storage probes confirmed unprivileged).
- **Sunshine service toggle (`SunshineService`):** starting and stopping a Windows service requires service DACL permission. The Remote section offers a one-time **Allow** button that launches a small elevated helper (`ShellExecuteW("runas")`) to grant `SERVICE_START | SERVICE_STOP` on `SunshineService` to the current user's SID — mirroring Linux's one-time `pkexec` Allow buttons. Uninstalling or revoking removes that ACE.

## Phased implementation plan (Milestone M5)

Each phase will be tracked as its own issue and PR under **M5 — Windows port**:

1. **M5.1 — Foundation, UI shell, Settings & Windows CI:** `win/CMakeLists.txt`, `windows-latest` CI job (`/W4 /WX`), Win32 + Direct2D software window with embedded Monocraft font, light/dark Home & Remote palettes, sidebar, `LARES_DRY_RUN=1`, single-instance activation, Settings section, and CLI dispatch (`--section`).
2. **M5.2 — Overview & Hardware:** `win/src/hw.c` (CPU, RAM, disk space, NVMe temp, AMD ADL / PDH GPU load & temp) with worker-thread reads, Overview summary cards, and live Hardware graphs.
3. **M5.3 — Remote section & CLI:** `win/src/remote.c` (`lares home` / `remote` / `status` / `restore`), three-step setup guide, Home/Remote spring switch, Win32 display mode picker, Tailscale + `SunshineService` control (with one-time **Allow** DACL helper), and Moonlight live badge.
4. **M5.4 — Mouse (HyperX) & Keyboard (Logitech G715) & Headphones:** shared pure-C protocol codecs + Win32 HID transport (`win/src/hid_win.c`), HyperX Pulsefire Haste 2 Wireless (receiver & cable) battery/DPI/polling/LOD/lighting, Logitech G715 HID++ 2.0 brightness/onboard profile/timers/startup & shutdown effects, and Bluetooth Headphones card.
5. **M5.5 — Controller section:** shared `pad_map` pixel-art controller canvas (`data/pad/controller.txt`), XInput + `Windows.Gaming.Input` Buttons/Sticks/Vibration tabs, battery, nicknames, Bluetooth pairing & disconnect, and controller settings.
6. **M5.6 — Background helper & System Tray (`lares-pad.exe`):** hidden background helper, `Shell_NotifyIconW` tray icon & custom controller popup menu, low-battery & connection notifications, keep-awake, idle disconnect, G715 live lighting playback, and autostart toggle.

## Decisions before building

1. **Shared pure-C protocol modules vs copying into `win/`:** Extracting the GLib-free byte codecs from `src/pad_map.c`, `src/hyperx.c` and `src/hidpp.c` into small pure-C files shared by both `Makefile` and `win/CMakeLists.txt` avoids protocol drift (especially `hidpp_allowed` safety rules) while touching Linux files minimally; alternatively `win/` can start standalone and share later. Recommended: share pure-C protocol codecs from M5.4 onward, keeping `Makefile` and `make check` green at every step.
2. **CPU temperature on Windows:** Accept `--` (unavailable) on Windows rather than bundling a third-party kernel driver (WinRing0 / PawnIO), keeping Lares driverless, unprivileged and lightweight.
3. **Sunshine service Allow DACL:** Grant `SERVICE_START | SERVICE_STOP` on `SunshineService` to the current user SID once via UAC rather than running Lares itself elevated.
4. **Smart App Control for local vs shipped binaries:** Local builds run without signing once Smart App Control is off; code signing (e.g. Microsoft Trusted Signing) is deferred to M4 / release packaging.

## Sources

- Installed Windows 11 SDK `10.0.26100.0` headers (`winioctl.h`, `hidsdi.h`, `d2d1_1.h`, `dwrite_3.h`, `Xinput.h`, `winsvc.h`, `bluetoothapis.h`).
- Microsoft Learn: Direct2D Render Targets (`D2D1_RENDER_TARGET_TYPE_SOFTWARE`), `IOCTL_STORAGE_QUERY_PROPERTY` (`StorageDeviceTemperatureProperty`), `ChangeDisplaySettingsExW`, `Shell_NotifyIconW`, `XInput`, `Windows.Gaming.Input`, `SetServiceObjectSecurity`.
- AMD Display Library (ADL) SDK (`ADL_Overdrive5_Temperature_Get`, `ADL_Overdrive5_CurrentActivity_Get`).
- Lares source (`src/hardware.h`, `src/devices.h`, `src/hyperx.h`, `src/hidpp.h`, `src/remote_model.h`, `src/remote_model.c`, `src/pad/padconf.h`, `src/pad_map.h`, `data/style.css`).
