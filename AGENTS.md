# Agent instructions

## The project

Lares is a native GNOME app for Linux, in C with GTK 4, that checks and manages a PC and its devices: an Overview, live Hardware readings, Xbox controllers (the work of the retired Padlight extension), Bluetooth headphones, keyboards and mice, AirDrop with iPhones, iPads and Macs, remote play with Tailscale, Sunshine and Moonlight, and Settings. A GLib-only helper, `lares-pad`, runs the controller features that work while the window is closed, and a thin JavaScript addon shows a controller icon in GNOME's top bar. `README.md` describes the features.

Lightness is a requirement: the window stays around 30 MB of its own memory and the helper under 1 MB. Nothing reads hardware unless something on screen needs it; nothing polls what can be watched; nothing blocks the main thread (a slow sysfs file, such as amdgpu's `power1_average`, is read in a worker thread). Measure before and after a change that could cost memory, CPU or GPU.

It is an independent project, not affiliated with Microsoft, Tailscale, LizardByte or Moonlight. Xbox is a trademark of Microsoft.

Never describe something as working or complete unless a recorded check establishes it. A build is not proof that the window behaves, and a simulated controller is not proof that real hardware behaves.

## Layout

| Path | Contents |
| :--- | :--- |
| `src/main.c`, `src/window.c` | Start-up, the command line (`lares home`, `remote`, `status`, `restore`, `--section`), the window, its sidebar and lazily built sections |
| `src/widgets.c` | Lares' own widgets: pixel icons, the spring-driven switch and sidebar highlight, wrap, clamp, meters and graphs |
| `src/overview.c`, `src/hardware.c`, `src/hardware_view.c` | Overview, and the hardware readings with their section |
| `src/devices.c`, `src/hyperx.c`, `src/hidpp.c`, `src/keylight.h`, `src/device_view.c` | Bluetooth, battery and USB input devices from BlueZ, UPower and sysfs, with their model and connection; HyperX mouse batteries and settings over hidraw; Logitech keyboard brightness, lighting, dim and lights-off timers and startup and shutdown effects over HID++, kept on the keyboard or played live; and the Headphones, Keyboard and Mouse sections |
| `src/pad/` | The controller core, shared with the helper and free of GTK: detection, batteries, nicknames, evdev input, rumble and Find, the programs using a pad, pairing, settings and the background features |
| `src/pad_view.c`, `src/pad_picture.c`, `src/pad_map.c`, `src/pad_service.c` | The Controller section, its live pixel-art picture and the picture's map (`data/pad/controller.txt`), and switching the helper and the addon |
| `src/share.c`, `src/share_view.c` | The Share section: AirDrop with Apple devices, the Wi-Fi card lent to `lares-awdl`, and the Allow step |
| `src/airdrop/` | AirDrop above the link, GLib only: plists, CPIO, gzip and DVZip, HTTP, mDNS, the Bluetooth LE wake-up, and the TLS receiver and sender |
| `src/awdl/` | `lares-awdl`, the libc-only AWDL link program that holds `CAP_NET_ADMIN` and `CAP_NET_RAW` (`docs/AIRDROP.md`) |
| `win/share/`, `win/third_party/` | `lares-share.exe`, AirDrop on Windows without WSL: an RTL8188EU driver over WinUSB ported from Linux `rtl8xxxu` (GPL-2.0-only), `src/awdl`'s node, lwIP for IPv6, Schannel, and `src/airdrop` and `src/share.c` in C++ (`docs/WINDOWS.md`) |
| `src/remote_model.c`, `src/remote_view.c` | Remote play: the screen, Sunshine and Tailscale, the Home and Remote modes, and the setup guide |
| `src/settings_view.c` | The Settings section: Run in the background and the top-bar icon |
| `src/helper/lares-pad.c`, `src/helper/livelight.c` | The background helper and its D-Bus interface for the addon, and the keyboard lighting it plays live |
| `data/` | Stylesheet, artwork, the bundled Monocraft font, the desktop file, the helper's unit and the addon (`data/addon/`) |
| `tests/unit.c`, `tests/addon.mjs` | Unit tests on plain data, and on the addon's controller picture under Node, run by `make check` |
| `tests/awdl.c`, `tests/formats.c`, `tests/discovery.c`, `tests/share.c` | AirDrop's tests: AWDL frames, the formats, mDNS and Bluetooth LE, and whole transfers over TLS on loopback |
| `tools/pad-map.py` | Draws the controller picture's map from the artwork; needs Pillow and NumPy, building doesn't |
| `tools/privacy-gate.mjs`, `tools/privacy-allow.txt` | The privacy gate the Checks workflow runs over tracked files, commits and built Windows programs, and its exceptions (`CONTRIBUTING.md`) |
| `tools/measure.ps1`, `docs/GAINS.md` | The Windows before and after measurement (memory, processes, idle processor and graphics use, start-up entries, named programs), its method and the measured results |
| `tools/hidpp-probe.py` | Lists a Logitech device's HID++ 2.0 features and lighting state behind its receiver, with read requests only |
| `.github/` | Issue form, PR template, the Checks and Project tracking workflows |

## Commands

`make`, `make check`, `make install` (into `~/.local`, no administrator rights), `make uninstall`. `LARES_DRY_RUN=1 build/lares` pretends every change and keeps its own settings; `LARES_SECTION=controller` opens on a section; `LARES_COLOR_SCHEME=dark` forces dark; `LARES_CSS=data/style.css` loads the stylesheet from disk.

## Working well here

- Start from the active issue, then read only the files involved.
- Try changes in a dry run or a hidden GNOME Shell with its own settings and buses, never the real session: a real run switches screens, services and Bluetooth devices. The installed copy of Lares is single-instance, so close it before running a test copy.
- A GNOME Shell started by the user's session loads new or changed top-bar addons only at the next log-in.
- Desktop themes that ship a user `gtk.css` override app styles; Lares' stylesheet sits above them on purpose.

## Private data

Bluetooth addresses, serial numbers, Tailscale addresses and machine names, personal paths, logs that contain them, and account credentials stay out of commits, issues, PRs and pictures. Sunshine's state file also holds its login: Lares only checks whether its device list is empty, and nothing else from it is ever kept, logged or shown.

## Tracking and delivery

Every change (code, bug fixes, enhancements, docs, research and repository operations) belongs to an issue with a milestone in `NahuelGenchi/lares`, reused or created before editing. Every work item gets its own new branch created from the latest `origin/main`: `<type>/<issue>-<slug>` (`feat`, `fix`, `enh`, `docs`, `research`, `ops`). Never reuse a branch for a second item, never mix two issues on one branch, and never push to `main`. Use `type: summary (#N)` commits. PRs follow `.github/pull_request_template.md`: a standalone `Closes #N` line, the issue's milestone, and `Refs #N` for unfinished parent work. Decisions, exact changed paths and blockers go in the issue or PR. See `CONTRIBUTING.md` and `docs/TRACKING.md`.

## Final report

Report the issue, the PR and what changed. Keep these separate: `make check`, window checks (how Lares ran, which sections, light or dark, and the outcome, or "none run"), helper and addon checks, hardware checks, and the source/diff review scope.
