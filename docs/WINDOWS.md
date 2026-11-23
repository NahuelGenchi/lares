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
| **Share: AirDrop** | confirmed up to the link (issue #98); not yet tried with an Apple device | The Linux AirDrop code inside WSL 2, on a USB Wi-Fi adapter lent by `usbipd-win`; `lares.exe` drives it over pipes and sends the Bluetooth wake-up itself (see *AirDrop through WSL*) | One adapter model so far (Realtek RTL8188EU, 2.4 GHz); needs WSL and a one-time Set up |
| **Settings, Helper & Tray** | confirmed (issue #92) | `lares-pad.exe` background process (autostart via `HKCU\...\Run`), `Shell_NotifyIconW` tray icon + balloon alerts, and `win/src/livelight.cpp`, the port of `src/helper/livelight.c` | Listens to the receiver instead of polling, finds it again on `WM_DEVICECHANGE`, gives the lights back on quit and at `WM_ENDSESSION`. Measured 2.6 to 4.4 MB private across runs and no CPU while idle |

## Windows Dynamic Lighting

Windows 11 paints any HID LampArray itself while Dynamic Lighting is on for it, over whatever the device's own effects are (**confirmed**, issue #92): the HyperX receiver acknowledged a live Solid red and read it back, while the mouse kept glowing the Windows accent colour. Turning Dynamic Lighting off gave the lights back to the mouse at once.

- **Where it shows:** the HyperX receiver's LampArray (`MI_03`, `MI_04` on its cable), and the G715 through a virtual LampArray (`HID_DEVICE_SYSTEM_VHF`, under `USB\VID_046D&PID_C547&LAMPARRAY\…_SLOT00`) made by the **Logitech LampArray Service** (`logi_lamparray_service`, from Windows Update) while it runs. On the development PC that service is set to manual and stopped, so it paints nothing.
- **What Lares reads:** `HKCU\Software\Microsoft\Lighting` `AmbientLightingEnabled` (0 turns it off everywhere) and, per device, `…\Lighting\Devices\<interface path without \\?\>` `AmbientLightingEnabled`. It watches that key with `RegNotifyChangeKeyValue`, so the note goes as soon as it's turned off. **Confirmed** for the global switch; the per-device value follows the same layout but wasn't switched on its own.
- **What Lares does:** the Mouse and Keyboard pages say Windows is painting the lights and offer **Open Dynamic Lighting** (`ms-settings:personalization-lighting`). Lares doesn't change the setting itself, nor fight Windows by sending the LampArray autonomous-mode report.

## AirDrop through WSL

AirDrop's link, AWDL, is made of raw 802.11 frames sent and heard in monitor mode. Windows gives no program that: Native Wifi has no frame injection, and no stock driver has a monitor mode a program can send through. So on Windows the Share section runs the same code as on Linux, inside WSL 2, on a USB Wi-Fi adapter that Windows lends it (issue #98).

| Piece | Where | What it does |
| :--- | :--- | :--- |
| `lares.exe` | Windows | The Share page, the file picker, Downloads, and the Bluetooth LE wake-up (`BluetoothLEAdvertisementPublisher`, the bytes of `src/airdrop/ble.c`) |
| `usbipd-win` | Windows | Lends the USB Wi-Fi adapter to WSL while AirDrop is on |
| `lares-share` | WSL | `src/share.c` without a window (`src/sharecli/main.c`): one command a line on its input, its whole state on its output whenever it changes |
| `lares-awdl-wsl`, `bringup.sh`, `rtlwake` | WSL | Borrow the adapter, start it, run `lares-awdl` on it, and give it back |
| `lares-awdl` | WSL | The AWDL link, as on Linux, in its plain-monitor mode |

`lares.exe` starts `wsl.exe -d <distribution> -u root -e /opt/lares-wsl/bin/run.sh <Downloads>` when Receive is turned on or files are chosen, and talks to it over pipes. Files go between the two systems as paths: `C:\Users\me\a.jpg` is `/mnt/c/Users/me/a.jpg` in WSL, so nothing is copied twice and received files land in the Windows Downloads folder. When nothing uses AirDrop any more, `lares-share` keeps the link for a minute, as on Linux, then ends; the adapter is back with Windows a moment later, and WSL's virtual machine stops by itself when idle. Closing Lares ends the session at once, and Lares detaches the adapter itself if the session is slow to.

**Set up**, one time, from the Share page: installs `usbipd-win` with `winget` and shares the adapter with it (`usbipd bind`), both behind one administrator prompt, then runs `win/wsl/setup.sh` in WSL as root. That installs a compiler, GLib, `iw`, the adapter's firmware and a few tools with `apt`, and builds `lares-awdl`, `lares-share` and `rtlwake` into `/opt/lares-wsl/bin`. The scripts and Linux sources ride beside `lares.exe` in a `wsl` folder with a `VERSION` made from their contents; Lares remembers the version it installed and offers Set up again when a newer Lares brings a newer one. WSL itself, with an Ubuntu or Debian distribution, has to be there first (`wsl --install`).

**The adapter.** The Realtek RTL8188EU (`0bda:8179`) is the one adapter this is built for and measured on. Getting it to work under WSL's kernel took four findings, all in `win/wsl/`:

- WSL's kernel can't read the distribution's compressed firmware, nor find it in the distribution's `/lib/firmware`, so `setup.sh` unpacks it and `bringup.sh` feeds it through the kernel's sysfs loader. The driver is kept from loading by itself, since nobody would bring it the firmware then.
- The Windows driver leaves the chip's MCU IO wrapper in reset (register `0x1d`, bit 3), and `rtl8xxxu`'s firmware then never starts: `rtlwake` releases it over USB first.
- The adapter hears only on its first start after Windows had it. So every session is: Windows has it, attach, bring up, `lares-awdl` starts it once, detach. Windows needs a few seconds with it in between, which a session that follows another waits for.
- It has neither ad-hoc mode nor active monitor; its plain monitor ACKs unicast, which is what AWDL needs (`docs/AIRDROP.md`).

**Checked** on the development PC: Set up from the real window; Receive on, with the link up and the countdown showing 5 to 10 s later; the adapter attached to WSL meanwhile and back with Windows after Receive off, after a send was stopped, and within a second of closing the window; three sessions in a row; AirDrop's Linux tests inside WSL. In the adapter's own measurements it heard 400 to 950 frames in 3 to 4 s on channel 6, and access points answered its probe requests without retries.

**Not checked:** a transfer with an iPhone, iPad or Mac. Nothing here has met an Apple device yet. Also unchecked: the administrator half of Set up, on a PC without `usbipd-win`.

**Limits.** One adapter model, on 2.4 GHz, where an iPhone gives AirDrop little air time. The Bluetooth wake-up needs a radio that can advertise: the development PC's dongle can't (Windows reports no peripheral role, and the publisher aborts), and then the page says to open AirDrop on the other device first. WSL's virtual machine uses memory of its own while AirDrop is on, outside Lares' budget; `lares.exe` grew by about 0.4 MB with a session on.

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
| `win/src/share_win.cpp`, `win/src/ui_share.cpp`, `win/src/ble_wake.cpp` | AirDrop's Windows end: what's set up, the WSL session and what it says, the paths between the two systems (`share_win.cpp`), the Share page (`ui_share.cpp`) and the Bluetooth LE wake-up (`ble_wake.cpp`) |
| `win/wsl/`, `src/sharecli/main.c` | What runs in WSL: `setup.sh`, `run.sh`, `lares-awdl-wsl`, `bringup.sh` and `rtlwake.c`, and `lares-share`, `src/share.c` without a window. Copied beside `lares.exe` with the Linux sources they build |
| `.github/workflows/checks.yml` | Keeps existing Ubuntu 26.04 `make check` job unchanged and adds a parallel `windows-latest` MSVC build + unit-test job once `win/` lands |

### Config and state locations on Windows

- `%APPDATA%\lares\lares.ini` — Remote mode, saved screen/service choices and setup readiness (same INI layout as `~/.config/lares/lares.ini`; dry run uses `lares-dry.ini`).
- `%APPDATA%\lares\pad.ini` — Controller alerts, notices, keep-awake, idle disconnect, tray icon preferences, G715 live lighting, background toggle and nicknames (same INI layout as `~/.config/lares/pad.ini`).
- `%APPDATA%\lares\airdrop-kit.txt`: the version of AirDrop's programs Set up last installed in WSL, and the distribution. In WSL they live in `/opt/lares-wsl`.
- For AirDrop: `LARES_WSL_DISTRO=<name>` picks the distribution (the default one otherwise); in a dry run `LARES_DRY_SHARE=<scene>` shows a scene (`setup`, `setup-failed`, `nowsl`, `noadapter`, `receiving`, `quiet`, `incoming`, `incoming-receiving`, `incoming-done`, `looking`, `sending`) and `LARES_DRY_FILES=<path>;<path>` stands in for the file picker.
- Environment flags matching Linux: `LARES_DRY_RUN=1`, `LARES_DRY_STREAM=1`, `LARES_COLOR_SCHEME=dark|light`, `LARES_SECTION=<id>`.

### Permissions on Windows

- **Hardware, HyperX mouse, Logitech G715, Xbox controller, Bluetooth, display modes, Tailscale user toggles:** zero Administrator privileges required (all HID & storage probes confirmed unprivileged).
- **AirDrop:** installing `usbipd-win` and sharing the Wi-Fi adapter with it ask for Administrator once, in Set up. After that, lending the adapter to WSL and taking it back need none. Inside WSL, Lares' programs run as root, which a WSL user can always become.
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
