# Lares on Windows

[Back to README](../README.md) · Research issue [#70](https://github.com/NahuelGenchi/lares/issues/70)

This is the design for a native Windows 11 port of Lares in C and C++ using Win32, Direct2D and DirectWrite, with no third-party UI runtime or bundled DLL pack. Each claim is marked **confirmed** (checked on the Windows 11 development PC or in the installed Windows 11 SDK), **reported** (documented by Microsoft or another project, not checked on live hardware here), **planned** (what Lares on Windows will do) or **blocked**.

Windows, Direct2D, XInput and Xbox are trademarks of Microsoft Corporation. HyperX, Logitech, Tailscale, LizardByte and Moonlight are trademarks of their respective owners. Lares is not affiliated with any of them.

## Goals and lightness budget

Lares on Windows follows the same rules as Linux (`AGENTS.md`):

- **Small footprint (planned):** target ~15 to 30 MB private working set for `lares.exe` and ~2 to 4 MB for `lares-pad.exe`. Measure before and after each milestone phase.
- **Software rendering by default (planned):** draw with Direct2D on a WIC/GDI software target (`D2D1_RENDER_TARGET_TYPE_SOFTWARE` / `ID2D1DCRenderTarget`) so the window never loads a 3D GPU swapchain into the UI process.
- **No idle polling (confirmed for devices, issue #92):** read hardware and device settings only while their section is visible and the window isn't minimized; watch connect/disconnect events through `RegisterDeviceNotificationW` (`WM_DEVICECHANGE`), HID overlapped I/O (`ReadFile` + `WaitForMultipleObjects`) and `ReadDirectoryChangesW` for config files; run all blocking I/O on worker threads.
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
| **Mouse: HyperX Pulsefire Haste 2 Wireless** | confirmed (issue #92) | Win32 `SetupAPI` + `hid.dll` on vendor collection `UsagePage 0xFF00`, `Usage 0x0001` (64-byte reports); events (DPI button, sleep, battery) heard on an overlapped read while Mouse shows | **Confirmed** unprivileged R/W on `03F0:0F98`; a Solid colour and Breathing set from the window showed on the mouse once Windows Dynamic Lighting was off (2026-11-23). Its receiver also has a HID LampArray (`UsagePage 0x59`, `MI_03`; `MI_04` on the cable), which Dynamic Lighting paints over the mouse's own effects: see below |
| **Keyboard: Logitech G715 (HID++ 2.0)** | confirmed (issue #92) | Win32 `SetupAPI` + `hid.dll` on receiver vendor collections `0xFF00:0x0001` (7-byte) and `0xFF00:0x0002` (20-byte), which Windows splits one HID++ interface into | **Confirmed** unprivileged R/W on `046D:C547`, and the receiver's wireless notifications on (register `0x00` = `00 09 00`). A blue Ripple stored from the window, and a live Solid played by `lares-pad.exe` that dims after the profile's dim time, goes dark at its lights-off time and comes back on a key, all showed on the keyboard (2026-11-23). Keeps `hidpp_allowed` and `HIDPP_SW_ID = 0x0c` / `HIDPP_SW_HELPER = 0x0e` |
| **Remote: Screen switching** | confirmed | `EnumDisplaySettingsW` + `ChangeDisplaySettingsExW` (without `CDS_UPDATEREGISTRY`, so changes stay temporary) | **Confirmed** on `\\.\DISPLAY1` for 16:9 (`1920×1080`) and 16:10 (`1680×1050`) modes |
| **Remote: Tailscale & Sunshine** | confirmed | `tailscale.exe status --json` / `up` / `down`; `OpenSCManagerW` / `QueryServiceStatusEx` on `SunshineService`; `sunshine.log` for active stream; `sunshine_state.json` checked only for non-empty `named_devices` and zeroed with `SecureZeroMemory` | Starting/stopping `SunshineService` needs Administrator once to grant `SERVICE_START \| SERVICE_STOP` to the user (one-time **Allow** button via UAC) |
| **Remote CLI** | planned | `lares home`, `lares remote`, `lares status`, `lares restore`, `--section` | Same commands and exit semantics as Linux |
| **Share: AirDrop** | confirmed up to the link (issue #109); not yet tried natively with an Apple device | `lares-share.exe` drives a USB Wi-Fi adapter over Windows' WinUSB and runs AWDL, IPv6 (lwIP), TLS (Schannel) and AirDrop itself; `lares.exe` sends the Bluetooth wake-up (see *AirDrop without WSL*) | One adapter model so far (Realtek RTL8188EU, 2.4 GHz); a one-time Set up |
| **Settings, Helper & Tray** | confirmed (issue #92) | `lares-pad.exe` background process (autostart via `HKCU\...\Run`), `Shell_NotifyIconW` tray icon + balloon alerts, and `win/src/livelight.cpp`, the port of `src/helper/livelight.c` | Listens to the receiver instead of polling, finds it again on `WM_DEVICECHANGE`, gives the lights back on quit and at `WM_ENDSESSION`. Measured 2.6 to 4.4 MB private across runs and no CPU while idle |
| **Settings: Accessibility, Size** | confirmed in dry runs (issue #102); Windows only | One scale for the whole window: Windows' display scale (`GetDpiForWindow`) times the Size, 90% to 150%, through the Direct2D transform, pointer coordinates and the smallest window (640 by 480 layout pixels, at most the work area). A new size resizes the window by the same ratio, inside the work area, unless it is maximised | The window's private memory, fresh on Overview at 96 DPI: 13.3 MB at 100%, 14.1 MB at 100% stretched to 1350 by 1050, 22.1 MB at 150%: what grows is drawing at a larger scale, as at Windows' own 150%. Switching sizes 28 times in a row levels off near 31 MB |

## Windows Dynamic Lighting

Windows 11 paints any HID LampArray itself while Dynamic Lighting is on for it, over whatever the device's own effects are (**confirmed**, issue #92): the HyperX receiver acknowledged a live Solid red and read it back, while the mouse kept glowing the Windows accent colour. Turning Dynamic Lighting off gave the lights back to the mouse at once.

- **Where it shows:** the HyperX receiver's LampArray (`MI_03`, `MI_04` on its cable), and the G715 through a virtual LampArray (`HID_DEVICE_SYSTEM_VHF`, under `USB\VID_046D&PID_C547&LAMPARRAY\…_SLOT00`) made by the **Logitech LampArray Service** (`logi_lamparray_service`, from Windows Update) while it runs. On the development PC that service is set to manual and stopped, so it paints nothing.
- **What Lares reads:** `HKCU\Software\Microsoft\Lighting` `AmbientLightingEnabled` (0 turns it off everywhere) and, per device, `…\Lighting\Devices\<interface path without \\?\>` `AmbientLightingEnabled`. It watches that key with `RegNotifyChangeKeyValue`, so the note goes as soon as it's turned off. **Confirmed** for the global switch; the per-device value follows the same layout but wasn't switched on its own.
- **What Lares does:** the Mouse and Keyboard pages say Windows is painting the lights and offer **Open Dynamic Lighting** (`ms-settings:personalization-lighting`). Lares doesn't change the setting itself, nor fight Windows by sending the LampArray autonomous-mode report.

## AirDrop without WSL

AirDrop's link, AWDL, is made of raw 802.11 frames sent and heard in monitor mode. Windows gives no program that: Native Wifi has no frame injection, and no stock driver has a monitor mode a program can send through. So Lares drives a USB Wi-Fi adapter itself, from user mode, over Windows' own WinUSB driver, and runs everything above it in `lares-share.exe` (issue #109). Nothing of it uses WSL, usbipd, a virtual network adapter or a driver of Lares' own.

| Piece | Code | What it does |
| :--- | :--- | :--- |
| `lares.exe` | `win/src/share_win.cpp`, `ui_share.cpp`, `ble_wake.cpp` | The Share page, the file picker, Set up, and the Bluetooth LE wake-up (`BluetoothLEAdvertisementPublisher`, the bytes of `src/airdrop/ble.c`) |
| `lares-share.exe` | `win/share/main.cpp`, `model.cpp` | `src/share.c`'s model without a window: one command a line on its input, its whole state on its output whenever it changes. It runs only while AirDrop is in use |
| The driver | `win/share/rtl8188eu.c`, `rtl_usb.c` | The RTL8188EU over WinUSB, ported from Linux `rtl8xxxu` (GPL-2.0-only): firmware, tables, calibration, a monitor with the chip's TSF, and frames out at the rate and retry count each one names |
| The link | `win/share/link.c` | `src/awdl`'s node and frames, unchanged, on a thread of their own with a high-resolution waitable timer: Windows' default tick, 15.6 ms, is five times AWDL's 3 TU guard |
| IPv6 | `win/share/net.c`, lwIP (`win/third_party/`) | Link-local IPv6, UDP for mDNS and TCP, inside `lares-share.exe` on an interface of its own that Windows never sees: no adapter to install, no firewall rule, no clash with Windows' mDNS, and the PC's name never goes out |
| AirDrop | `win/share/tls.cpp`, `win/share/airdrop/` | Schannel TLS with a P-256 key kept by Windows for the user, and `src/airdrop`'s formats, mDNS, receiver and sender in C++, with their rules and #107's fixes |

`lares.exe` starts `lares-share.exe` from beside itself when Receive is turned on or files are chosen, and talks to it over pipes. Files go as Windows paths, and received ones land in Downloads, never over an existing file; a name Windows can't hold (a `:` from an iPhone, `CON`) is saved with `_` in its place. When nothing uses AirDrop any more, the link stays a minute, as on Linux, then `lares-share.exe` powers the chip down and ends. Closing Lares ends it at once.

**Set up**, one time, from the Share page, gives the adapter Windows' in-box *WinUsb Device* driver (`winusb.inf`, Microsoft's and signed: nothing is added to the driver store), behind one administrator prompt. Windows lists that driver only for its *Universal Serial Bus devices* class, so the adapter is moved there first, as Device Manager does when a driver of another class is picked. Windows' Wi-Fi then no longer uses that adapter, and opening it needs no rights. **Give back to Windows**, on the same page, reinstalls Windows' own driver for it (`netrtwlanu.inf`) the same way. Nothing changes per session.

**The adapter.** The Realtek RTL8188EU (`0bda:8179`) is the one adapter this is built for and measured on. What it needs, measured over WSL in #107 and natively in #109:

- The chip ACKs only the address in `REG_MACID` (0x0610), which holds the AWDL address.
- Unicast goes once: the retry limit is 0, and the descriptor marks frames nobody ACKs. Every frame goes at the rate its descriptor names, never at the chip's own choice.
- After Windows' driver had the chip, its MCU IO wrapper (register 0x1d, bit 3) is held and has to be released before the firmware starts.
- Linux's power-off for this chip leaves out a step of Realtek's own sequence: turning the MAC off through the hardware state machine (register 0x04, bit 9). Without it the next power-on finds the MAC still on and its steps do nothing. The radio then hears energy but decodes almost nothing (measured: 2 good frames of about 170 in 3 s), which is why Linux's driver heard only on its first start after Windows had the chip. `lares-share.exe` takes the MAC off properly at every power-on and power-off.
- Frames are timed by the chip's TSF, mapped to the host clock twice a second, rather than by when USB delivered them.

**Checked** on the development PC, 2026-11-24:

- Set up's switch, run as administrator: the adapter moved to *WinUsb Device*, and opened afterwards without administrator rights.
- Bring-up in 0.6 s: firmware, tables, calibration.
- Three sessions in a row, each ending in the chip's power-off: 266, 311 and 307 frames heard in 4 s on channel 6, 85 to 147 of them OFDM.
- The full link sending AWDL's own frames, 24 a second with none failed, while still hearing about 60 a second.
- 50 probe requests sent from the AWDL address drew 42 probe responses to that address, 1 with the retry bit: frames go out, frames to our address come in, and the chip ACKs them.
- `lares-share.exe` driven as `lares.exe` drives it: Receive on, the link up, the receiver listening, mDNS announcements sent, Receive off, the chip powered down, exit.
- With the link up, `lares-share.exe` used 5.2 MB of private memory and no measurable CPU over 10 s.
- Unit tests (`lares-share-unit`): the driver's descriptors and its bring-up on a fake register file, AirDrop's formats with Linux's vectors (their output byte for byte the same as Linux's), and whole transfers, a photo and a folder and a decline, over TLS 1.3 between two links in one process.

**Not checked:** anything with an Apple device: hearing an iPhone's AWDL frames natively, the link with it, a transfer either way. Also unchecked: Give back to Windows on the real adapter.

**Limits.** One adapter model, on 2.4 GHz, where an iPhone gives AirDrop little air time. The Bluetooth wake-up needs Windows' Bluetooth on. The ported driver is GPL-2.0-only, so `lares-share.exe` as a whole is GPL-2.0-only; Lares' own files stay GPL-2.0-or-later.
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
| `win/src/ui.cpp`, `win/src/ui_pages.cpp`, `win/src/ui_remote.cpp`, `win/src/ui_devices.cpp`, `win/src/ui_internal.h` | The window and its pages, laid out on the 9 px grid of `data/style.css` as the GTK views lay theirs (issue #94): text at 18, 27 and 36 px, heights from the text's own line height. Native Win32 window, Direct2D/DirectWrite software renderer, Monocraft font loader, palette & Home/Remote colour transitions, sidebar, spring switch, cards, meters, graphs and pixel-art controller canvas |
| `win/src/hw.c` | CPU, RAM, disk space, NVMe temp (`IOCTL_STORAGE_QUERY_PROPERTY`), AMD ADL / NVIDIA NVML / PDH GPU telemetry |
| `win/src/remote.c` | Display modes (`EnumDisplaySettingsW` / `ChangeDisplaySettingsExW`), Tailscale CLI, `SunshineService` SCM + one-time Allow ACL, `sunshine.log` / `sunshine_state.json`, `lares.ini` in `%APPDATA%\lares\` |
| `win/src/hid_win.cpp`, `win/src/devices.cpp` | Win32 `SetupAPI` + `hid.dll` exchanges for HyperX and Logitech G715 (`hid_win.cpp`), the shared codecs and the window's device manager (`devices.cpp`), Bluetooth discovery |
| `win/src/pad/` | XInput + `Windows.Gaming.Input` live state, rumble, battery, nicknames, Bluetooth disconnect, keep-awake |
| `win/src/helper_main.cpp`, `win/src/livelight.cpp` | `lares-pad.exe`: hidden window, `Shell_NotifyIconW` tray icon and menu, G715 live lighting, alerts, keep-awake |
| `win/src/share_win.cpp`, `win/src/ui_share.cpp`, `win/src/ble_wake.cpp` | AirDrop's end in `lares.exe`: what's set up, the session with `lares-share.exe` and what it says (`share_win.cpp`), the Share page (`ui_share.cpp`) and the Bluetooth LE wake-up (`ble_wake.cpp`) |
| `win/share/` | `lares-share.exe`: the RTL8188EU driver, the AWDL link, IPv6, TLS and AirDrop (see *AirDrop without WSL*), with `src/awdl/node.c` and `frame.c` built as they are; tested by `lares-share-unit` (`win/tests/share_*.cpp`) |
| `win/third_party/` | lwIP 2.2.1 and zlib 1.3.1, unchanged, for `lares-share.exe` only (`win/third_party/README.md`) |
| `.github/workflows/checks.yml` | Keeps existing Ubuntu 26.04 `make check` job unchanged and adds a parallel `windows-latest` MSVC build + unit-test job once `win/` lands |

### Config and state locations on Windows

- `%APPDATA%\lares\lares.ini` — Remote mode, saved screen/service choices and setup readiness (same INI layout as `~/.config/lares/lares.ini`; dry run uses `lares-dry.ini`).
- `%APPDATA%\lares\pad.ini` — Controller alerts, notices, keep-awake, idle disconnect, tray icon preferences, G715 live lighting, background toggle and nicknames (same INI layout as `~/.config/lares/pad.ini`), and the window's Size as `[window] size=<percent>`, written only when it isn't 100 (Windows only; Linux ignores the group).
- `%LOCALAPPDATA%\lares\share.log` and `lares-awdl.log`: when each step of AirDrop happened and what the link counted, started afresh each session, with no names or addresses, on this PC only.
- For AirDrop: in a dry run `LARES_DRY_SHARE=<scene>` shows a scene (`setup`, `setup-failed`, `noadapter`, `receiving`, `quiet`, `incoming`, `incoming-receiving`, `incoming-done`, `looking`, `sending`) and `LARES_DRY_FILES=<path>;<path>` stands in for the file picker.
- Environment flags matching Linux: `LARES_DRY_RUN=1`, `LARES_DRY_STREAM=1`, `LARES_COLOR_SCHEME=dark|light`, `LARES_SECTION=<id>`.

### Permissions on Windows

- **Hardware, HyperX mouse, Logitech G715, Xbox controller, Bluetooth, display modes, Tailscale user toggles:** zero Administrator privileges required (all HID & storage probes confirmed unprivileged).
- **AirDrop:** giving the Wi-Fi adapter Windows' WinUSB driver asks for Administrator once, in Set up, and so does giving it back. Using it needs none: `lares-share.exe` runs as the user.
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
