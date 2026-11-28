# The Windows shell

[Back to README](../README.md) · Issue [#139](https://github.com/NahuelGenchi/lares/issues/139)

Lares for Windows is getting its own desktop shell: a top bar, a dock, a launcher, popups, Files and viewers. It comes in as an opt-in module in small steps (stage 2 of the Windows plan). Every part has a switch in Lares' Settings that is off by default, and a Windows account with Lares installed looks exactly like stock Windows until one is turned on. [docs/SAFETY.md](SAFETY.md) covers what happens when the shell stops.

Each claim is marked **confirmed** (a recorded check establishes it), **reported** (documented by Microsoft, not checked here), **planned** or **blocked**.

| Part | Issue | Status |
| :--- | :--- | :--- |
| One drawing engine, measured | [#140](https://github.com/NahuelGenchi/lares/issues/140) | confirmed: GDI+ with Lares' own glyph blending |
| The drawing layer and settings | [#141](https://github.com/NahuelGenchi/lares/issues/141) | confirmed by tests: the layer draws into memory, the settings round-trip; nothing on screen yet |
| `lares-shell.exe`: top bar and dock, under `lares-guard.exe` | [#145](https://github.com/NahuelGenchi/lares/issues/145) | confirmed with tests, on a stand-in taskbar and on the real taskbar of GitHub's Windows runner; not used on a real Windows 11 desktop yet |
| The top bar's popups: calendar, keyboard, Bluetooth, network and Tailscale, media, notifications, power, screenshot menu | [#147](https://github.com/NahuelGenchi/lares/issues/147) | confirmed drawn, light and dark, in a test copy; not clicked on a real desktop |
| Launcher and clipboard history, each with its own switch | [#148](https://github.com/NahuelGenchi/lares/issues/148) | confirmed: drawn in a test copy, light and dark; switches on and off in tests; not used on a real desktop |
| Process list, and the screenshot tool with its own switch | [#149](https://github.com/NahuelGenchi/lares/issues/149) | confirmed: drawn in a test copy, light and dark; switch on and off in tests; no capture taken in a check |
| `lares-view.exe`: the image viewer and the text and Markdown editor, with a Viewers switch | [#151](https://github.com/NahuelGenchi/lares/issues/151) | confirmed drawn with made-up files, light and dark; opened, measured and ended with its last window in the Checks workflow; not used on a real desktop here |
| `lares-files.exe`: Files with tabs, Quick Look and drag and drop, with a Files switch | [#153](https://github.com/NahuelGenchi/lares/issues/153) | confirmed drawn with a made-up folder, light and dark; opened, measured and ended with its last window in the Checks workflow; not used on a real desktop here |
| AirDrop in the top bar, on Lares' own sharing code, with its own switch | [#155](https://github.com/NahuelGenchi/lares/issues/155) | confirmed drawn from made-up state, light and dark; switch on and off in tests; not used with a real device through the bar |
| Device indicators in the bar for a controller, headphones, keyboard and mouse, and Remote mode, with their own switch | [#157](https://github.com/NahuelGenchi/lares/issues/157) | confirmed drawn from made-up state, light and dark; switch on and off in tests; not seen reacting to real devices |
| Per-user file types for Markdown and images (optional) | | planned |

## Budget

| Reading | Target |
| :--- | :--- |
| `lares-shell.exe`, own (private) memory at idle | about 25 MB |
| The same, in active use | under 50 MB |
| Graphics card use at idle | 0% |
| Processor time at idle | near zero |
| Loaded in any shell process | no Direct3D device, WebView2, Chromium or .NET |

## The drawing engine

Two ways of drawing were candidates, and only one is used by every shell part (issue [#140](https://github.com/NahuelGenchi/lares/issues/140)):

- **GDI+ with Lares' own glyph blending.** Shapes and pictures are drawn by GDI+ into a 32-bit bitmap in memory. Text is laid out by DirectWrite, which also turns each line into ClearType coverage; Lares then blends that coverage into the bitmap itself, with gamma 1.8, also where the bitmap is see-through. This is how the shell prototype draws.
- **Direct2D on a software target**, bound to the same kind of bitmap. This is how `lares.exe` draws its window.

`lares-draw-bench.exe` (`win/bench/draw_bench.cpp`) draws the same four screens with both, each engine and screen in a new process of its own: a top bar (1920 x 32, see-through), a dock (640 x 64, see-through, twelve app pictures), a popup (360 x 480, see-through, rows, switches and a wrapped paragraph) and a Files-like list (960 x 640, opaque, 26 rows of four columns and a sidebar), each at 100% and at 150%. It measures the process's own (private) memory before the engine starts and after at least 60 frames and 1.5 seconds of drawing, the processor time a frame takes across all of the process's threads, the threads, and which libraries are loaded. It shows nothing on the screen; putting the finished picture on screen is the same for both engines and is left out. The Checks workflow runs it on every pull request.

```
lares-draw-bench compare [--frames N] [--png DIR]
```

### Readings

On the Windows 11 development PC (x64, 100% display scale):

| Screen | Scale | Engine | Own memory (MB) | Processor per frame (ms) | Threads | Direct3D loaded |
| :--- | ---: | :--- | ---: | ---: | ---: | :--- |
| bar | 100% | GDI+ | 1.80 | 0.25 | 5 | no |
| bar | 100% | Direct2D | 8.11 | 0.31 | 9 | yes |
| dock | 100% | GDI+ | 1.35 | 0.64 | 5 | no |
| dock | 100% | Direct2D | 8.33 | 0.26 | 13 | yes |
| popup | 100% | GDI+ | 2.24 | 0.71 | 5 | no |
| popup | 100% | Direct2D | 11.89 | 0.90 | 21 | yes |
| list | 100% | GDI+ | 1.87 | 3.73 | 5 | no |
| list | 100% | Direct2D | 11.41 | 2.48 | 17 | yes |
| bar | 150% | GDI+ | 1.97 | 0.41 | 5 | no |
| bar | 150% | Direct2D | 8.69 | 0.39 | 12 | yes |
| dock | 150% | GDI+ | 1.34 | 0.98 | 5 | no |
| dock | 150% | Direct2D | 10.09 | 0.46 | 14 | yes |
| popup | 150% | GDI+ | 1.69 | 1.27 | 5 | no |
| popup | 150% | Direct2D | 11.19 | 1.32 | 20 | yes |
| list | 150% | GDI+ | 1.89 | 5.62 | 5 | no |
| list | 150% | Direct2D | 11.62 | 5.13 | 19 | yes |

On GitHub's Windows runner (a clean Windows Server, x64), in the Checks workflow of the pull request that brought the bench in:

| Screen | Scale | Engine | Own memory (MB) | Processor per frame (ms) | Threads | Direct3D loaded |
| :--- | ---: | :--- | ---: | ---: | ---: | :--- |
| bar | 100% | GDI+ | 1.50 | 0.40 | 5 | no |
| bar | 100% | Direct2D | 5.76 | 0.51 | 8 | yes |
| dock | 100% | GDI+ | 1.03 | 1.09 | 5 | no |
| dock | 100% | Direct2D | 5.19 | 0.46 | 8 | yes |
| popup | 100% | GDI+ | 1.48 | 1.17 | 5 | no |
| popup | 100% | Direct2D | 7.85 | 1.40 | 10 | yes |
| list | 100% | GDI+ | 1.56 | 6.28 | 5 | no |
| list | 100% | Direct2D | 7.97 | 4.56 | 8 | yes |
| bar | 150% | GDI+ | 1.68 | 0.70 | 5 | no |
| bar | 150% | Direct2D | 5.95 | 0.68 | 9 | yes |
| dock | 150% | GDI+ | 1.06 | 1.65 | 5 | no |
| dock | 150% | Direct2D | 6.52 | 0.77 | 9 | yes |
| popup | 150% | GDI+ | 1.51 | 2.18 | 5 | no |
| popup | 150% | Direct2D | 7.24 | 1.98 | 9 | yes |
| list | 150% | GDI+ | 1.55 | 9.96 | 5 | no |
| list | 150% | Direct2D | 8.26 | 6.57 | 8 | yes |

"Direct3D loaded" means `d3d11.dll`, `dxgi.dll` and `d3d10warp.dll`: Direct2D's software target runs on WARP, Direct3D's software device, so a process that draws this way holds a Direct3D device even though no graphics card is used. On the development PC another program loads `d2d1.dll` and `dwrite.dll` into every process before it starts (the bench lists them as `modules_at_start`), so those two say nothing there. On the clean runner the GDI+ processes load `gdiplus.dll` and `dwrite.dll` and nothing of Direct2D or Direct3D.

### Text quality

Both engines lay text out with the same DirectWrite formats, so glyphs sit in the same places. Where they differ:

- **On see-through surfaces** (the bar, the dock, every popup) Direct2D cannot use ClearType and falls back to grey-level smoothing, which looks softer. Lares' own blending keeps ClearType there, blended against the surface's own colour.
- **On opaque surfaces** (Files, the viewers) both use ClearType. Lares' blending uses gamma 1.8, which draws light text on dark backgrounds a little fuller than Direct2D's default; the difference is small.
- Pictures: GDI+ scales app icons with high-quality bicubic filtering; Direct2D's software target offers linear filtering only, a little softer when an icon is shrunk.

The pictures behind this were looked at zoomed in on the development PC and are not kept in the repository.

### Decision

**Every shell part draws with GDI+ and Lares' own glyph blending.** Recorded reasons:

1. Direct2D's software target loads a Direct3D device (WARP) in every process that draws, which the shell's budget rules out.
2. It costs 4.2 to 6.7 MB more own memory per drawing process on a clean Windows (5.2 to 8.3 MB against 1.0 to 1.7 MB), and 6.3 to 9.7 MB more on the development PC, and the shell has up to three drawing processes (`lares-shell.exe`, `lares-files.exe`, `lares-view.exe`).
3. It starts 3 to 16 more threads per process.
4. It loses ClearType on every see-through surface, which is most of the shell.

What it costs: Direct2D draws the dock and the large opaque list with less processor time (on the runner 0.46 against 1.09 ms for the dock, and 4.6 against 6.3 ms for the list, at 100%), and spreads the work over several threads, so the list is ready sooner. The bar and popups cost about the same with both. The slowest GDI+ frame measured is the full Files-like list at 150% on the runner, 10 ms; the shell draws only when something on screen changes, so its processor time at idle does not depend on the engine.

`lares.exe` keeps drawing with Direct2D for now; moving it to the same engine is planned in issue [#142](https://github.com/NahuelGenchi/lares/issues/142).

## The drawing layer

`win/shell/draw.c` and `win/shell/palette.c` (issue [#141](https://github.com/NahuelGenchi/lares/issues/141)) are the engine above in plain C, for every shell part to use:

- **Surfaces**: a 32-bit bitmap in memory with GDI+ on it. A see-through one holds premultiplied colour and goes on screen with `UpdateLayeredWindow`; an opaque one is copied into a window's paint and released.
- **Shapes**: rounded rectangles filled or outlined, circles and lines, smoothed.
- **Text**: one line in a box, aligned and cut with an ellipsis when it does not fit, a measured width, and a word-wrapped paragraph with its height. DirectWrite lays it out in the user's own language and turns it into ClearType coverage; draw.c blends that coverage at gamma 1.8, also on see-through surfaces. The face is Inter when it is installed, else Segoe UI Variable Text, else Segoe UI. Symbols come from Windows' own icon font.
- **Colours**: a light and a dark palette, picked from Windows' light or dark mode and its accent colour.

DirectWrite's headers are C++ only, so `win/shell/dwrite_c.h` declares the part the shell calls for C, and `gdiplus_c.h` does the same for GDI+'s flat API. `lares-shell-draw-test` draws on both kinds of surface and reads the pixels back: shapes where they were asked, ClearType edges on both kinds of surface, nothing outside a text's box, premultiplied colour kept, wrapping, measuring and both palettes. The Checks workflow runs it.

| Case | How | Status |
| :--- | :--- | :--- |
| Shapes, text, wrapping, measuring and palettes, on see-through and opaque surfaces | `lares-shell-draw-test`, 38 checks, development PC and the Checks workflow | confirmed |
| Every DirectWrite call draw.c makes goes through `dwrite_c.h` on a real factory | the same test: text that is drawn and measured | confirmed |
| On screen | | planned with the bar and dock |

## Settings

The shell's settings are Lares' own: `win/src/config.cpp` reads and writes `%APPDATA%\lares\shell.ini`, and a dry run (`LARES_DRY_RUN=1`) uses `shell-dry.ini` instead. Nothing is read from the program's own folder, and no setting holds a fixed path, drive letter or screen size. A missing or empty file means every part is off. The shell's C code reaches them through `win/src/shell_prefs.h`, which reads the file again before each write so that two Lares programs writing at once do not undo each other's setting.

```ini
[parts]
top-bar=false
dock=false
launcher=false
launcher-windows-key=false
clipboard-history=false
screenshot=false
files=false
viewers=false

[top-bar]
auto-hide=false
week-starts=sunday

[dock]
hide=when-covered
window-counts=true
middle-click-new-window=true
media=true

[general]
animations=true

[dock-pin 1]
name=Terminal
app-id=Microsoft.WindowsTerminal_8wekyb3d8bbwe!App
```

The dock's `hide` is `always-shown` (it reserves its strip of the screen), `auto-hide` or `when-covered`. A pin names a Store app by its AppUserModelID, or a program file with its command line, and may name a picture. A pin that names nothing to start is dropped, and a value cannot start a new line. Until the dock is changed no pins are written, and the shell picks its first set itself; a dock emptied on purpose stays empty.

## Other programs

`win/src/programs.h` finds the programs the shell and Lares work with the way Windows finds them, never at a fixed place, and says when one is missing so that what needs it hides:

| Program | How it is found |
| :--- | :--- |
| Tailscale's `tailscale.exe` | next to the program Tailscale's Windows service runs, else on the PATH |
| A terminal | Windows Terminal through its app alias (`wt.exe`), else PowerShell 7, else Windows PowerShell, else the command prompt Windows names in `ComSpec` |
| The browser | the program Windows opens `https` links with, and its name |
| Explorer and Windows' own tools | Windows' own folders, wherever Windows is installed |

Lares' Remote section uses the same lookup: Tailscale through its service, and Sunshine's folder through the program its `SunshineService` runs (in Sunshine's `tools` folder or the folder itself). Before, both were read from one fixed folder.

| Case | How | Status |
| :--- | :--- | :--- |
| A service's program folder from its command line: quoted, unquoted with spaces and arguments, `\??\`, environment variables, broken | unit test | confirmed |
| A terminal and Explorer are found on any Windows; Tailscale and a browser, when found, exist | unit test, development PC and the Checks workflow | confirmed |
| Remote finds Tailscale and Sunshine on a PC that has them | | not checked in a window yet |

## The top bar and the dock

`lares-shell.exe` (`win/shell/`, issue [#145](https://github.com/NahuelGenchi/lares/issues/145)) draws the top bar and the dock. It never runs on its own: `lares-guard.exe` starts it and owns hiding the Windows taskbar ([docs/SAFETY.md](SAFETY.md)), so a crash, a hang or a kill of the shell gives the taskbar back.

**Turning them on and off.** Settings has a **Bar and dock** card with two switches, both off:

- **Top bar**: a bar at the top of the screen with the app in front, the clock and quick readings. It sits beside Windows' own taskbar, which stays.
- **Dock**: takes the place of Windows' taskbar, with Start and the pinned and open apps. Only while the dock is on does the shell tell the guard it is ready, and only then is the Windows taskbar hidden. Turning the dock off ends the shell cleanly, the guard gives the taskbar back, and a new shell starts for the bar alone if it is still on.

A switch writes `shell.ini`, then `win/src/shell_service.cpp` puts what runs in line: it starts `lares-guard run lares-shell.exe`, tells a running shell to read its settings again, or ends it cleanly. It also keeps one start-up entry, `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `LaresShell`, set to `lares-guard run --sign-in lares-shell.exe` while either part is on. The entry goes through the change journal, so **Undo all**, `lares undo` and an uninstaller can remove it, and it is written only when a switch changes it: an account where nothing was turned on has no entry and an empty journal. Safe mode keeps both switches off and greyed out. A dry run changes nothing.

**What the prototype gained, kept:**

| Behaviour | Where |
| :--- | :--- |
| Top bar fixed (reserves its strip of the screen) or hidden automatically, back when the pointer touches the top edge; right-click the bar | `topbar.c` |
| Dock always shown (reserves its strip, so maximized windows stop above it), hidden automatically, or hidden when a window covers it; right-click the dock | `dock.c` |
| Drag a pinned app sideways to a new place; the order is saved | `dock.c` |
| Run as Administrator in a dock app's menu, for real program files only | `dock.c` |
| Short slide, fade and launch hop transitions, each a timer that exists only while it runs; **Animations** in either menu turns them off | `topbar.c`, `dock.c` |
| After an Explorer restart both bars reserve their strips again; a real full-screen window hides both | `topbar.c`, `dock.c`, `main.c` |
| Window events merged: only the ones that can change what the bar and dock show get through, into one refresh | `win_tracker.c` |

The dock starts with File Explorer, the browser Windows opens links with, a terminal and Lares, each only when found ([Other programs](#other-programs)); once the user changes it, `shell.ini` keeps the pins. Win+1 to Win+0 bring the dock's apps to the front; the keyboard hook that sees them is only installed while the dock is on.

**Until the rest of the shell arrives**, the dock's Start button opens Windows' own Start menu (or the launcher, when it is on), and the screenshot menu's entries open Windows' snipping unless the screenshot tool is on. The sharing button is left out until the AirDrop panel (part 6). The bar's other buttons open Lares' popups (below).

**Checking a build on a PC someone works on.** `lares-shell --isolated` (or `LARES_SHELL_ISOLATED=1`) runs a copy that hides nothing, reserves no space, puts its windows far off the screen, installs no keyboard hook and opens nothing. `lares-shell --snapshot DIR` does the same with both parts on, draws them once into `DIR\top-bar.png` and `DIR\dock.png`, and ends; `LARES_COLOR_SCHEME=light` or `dark` picks the palette.

| Case | How | Status |
| :--- | :--- | :--- |
| A fresh account: nothing on, no shell, no guard, no start-up entry, an empty journal, the taskbar as it was | `lares-shell-test`, stand-in taskbar, development PC; and the Checks workflow | confirmed |
| Dock on and off; top bar on and off; the dock joining a running bar (same shell); the dock leaving the bar (new shell, taskbar back); start-up entry present and gone | `lares-shell-test`, stand-in taskbar and a shell that hides nothing, once on the development PC (36 checks) | confirmed |
| The same, ten times in a row, on the real taskbar with a shell that reserves its strips | `lares-shell-test --real --cycles 10`, the Checks workflow on GitHub's Windows runner | confirmed: 278 checks, 0 failed, in the workflow of the pull request that brought the bar and dock in (#146) |
| Bar and dock drawn, light and dark | `lares-shell --snapshot`, development PC, looked at | confirmed |
| The Bar and dock card in Settings, light and dark, both off | window check in a dry run, development PC | confirmed |
| Clicking each button, the menus, dragging, auto-hide and the three dock modes on a real desktop | | **not checked**: on the development PC another bar hides the taskbar and must not be disturbed. Needs a Windows 11 test account |
| A real Explorer restart, sign-out, change of screens | | **not checked**, as for the guard ([docs/SAFETY.md](SAFETY.md)) |

### Measured

| Reading | Value | Where | Status |
| :--- | ---: | :--- | :--- |
| `lares-shell.exe` own memory at rest, bar and dock on | 16.6 MB (working set 7.8 MB) | development PC, a copy that hides nothing, 30 seconds after 5 to settle | confirmed |
| Its processor time at rest | 62.5 ms in 30 s (0.2% of one core) | same | confirmed |
| Direct3D, Direct2D, WebView2, Chromium or .NET loaded | none of its own | same; another program loads `d2d1.dll` into every process on that PC | confirmed |
| `lares-shell.exe` own memory at rest, bar and dock on | 14.7 MB (working set 8.3 MB) | GitHub's Windows runner, a clean Windows Server, real taskbar, 30 seconds after 5 to settle (#146) | confirmed |
| Its processor time at rest | 46.9 ms in 30 s (0.16% of one core) | same | confirmed |
| Direct3D, Direct2D, WebView2, Chromium or .NET loaded | none | same | confirmed |

On the development PC about 2.4 MB of every process's own memory is another program's ([docs/SAFETY.md](SAFETY.md)). The budget is about 25 MB at rest.

### The popups

The bar's buttons open one see-through popup at a time (`win/shell/popups.c`, issue [#147](https://github.com/NahuelGenchi/lares/issues/147)), drawn when it opens and when what it shows changes. They are part of the top bar and have no switch of their own.

| Popup | What it holds |
| :--- | :--- |
| Calendar | the time and date, a month to page through, Today; the week starts on Sunday or Monday (`[top-bar] week-starts`) |
| Keyboard | the keyboard layouts Windows has, to switch the window in front to another |
| Bluetooth | the devices Windows already knows, connected first; **Scan** looks for new ones only when pressed, because a scan can make Bluetooth audio stutter |
| Network and Tailscale | the connection, and when Tailscale is installed (found through its service): connect or disconnect, the address to copy, an exit node, and its Windows service, started and stopped on demand with an optional manual start so it costs no memory while off |
| Media | what plays, with its controls, the volume, and the sound outputs |
| Notifications | Windows' notification centre, and **Do Not Disturb**, which opens Windows' notification settings: Windows offers no public way for a program to switch it, so the popup does not pretend to |
| Power | lock, sleep, sign out, restart, shut down, File Explorer, and closing the bar and dock until the next sign-in |
| Screenshot | region, window or screen, through Lares' screenshot tool when it is on (with its shortcuts shown), else Windows' snipping |

Starting or stopping Tailscale's service needs administrator rights on most PCs. The popup first tries without them; when Windows refuses, the shell starts a short-lived copy of itself, `lares-shell --ts-service start|stop [--ts-startup manual|auto]`, with Windows' own approval prompt. That copy touches the Tailscale service and nothing else, and a cancelled prompt is reported in the popup.

| Case | How | Status |
| :--- | :--- | :--- |
| Every popup drawn, light and dark | `lares-shell --snapshot`, development PC, looked at; a test copy draws the popups without showing them or taking the focus | confirmed |
| Clicking in the popups, the keyboard switch, the volume slider, connecting Tailscale | | **not checked**: needs a desktop where the shell is in use |
| Starting and stopping Tailscale's service, with and without the approval prompt | | **not checked**: it would change a service on the development PC |
| The memory a popup takes, given back when it closes | | **not measured** yet |

### The launcher and clipboard history

Two more parts run in `lares-shell.exe`, each with its own switch under **Launcher and clipboard** in Settings, off by default (issue [#148](https://github.com/NahuelGenchi/lares/issues/148)). Either one on is enough for the shell to run, with or without the bar and dock; the Windows taskbar stays unless the dock is on.

- **Launcher** (`win/shell/launcher.c`): Alt+Space opens it. With nothing typed it shows the dock's first nine pinned apps; typing searches the Start menu's shortcuts (the user's and all users'), the installed Store apps Windows lists, and a few Settings pages and Windows tools, and works out sums (`sqrt(144)*2`, percentages such as `200*15%`, `pi`, functions), copying the answer with Enter. It never reads the user's own folders. A second setting, **Open it with the Windows key**, makes a lone tap of the Windows key open it instead of Start; off by default and greyed out while the launcher is off. With the launcher on, the dock's Start button opens it too.
- **Clipboard history** (`win/shell/clipboard.c`): Alt+V opens it; Win+V stays Windows' own. Windows tells the shell when the clipboard changes, so nothing polls. Up to 50 text clips and a few pictures (as PNG) stay in memory, within 6 MB in all; pinned text is kept on this PC in `%APPDATA%\lares\clipboard-pins.txt` (`clipboard-pins-dry.txt` in a dry run). With the switch off nothing is watched or kept; pins already saved stay in that file.

Their shortcuts are registered only while their switch is on, and the keyboard hook for the Windows key only while that setting is on (or the dock, for Win+1 to Win+0). A test copy registers no shortcut, never listens to the PC's clipboard and reads no pins.

Writing `shell.ini` now tries again for up to a second when another Lares program is reading it at that moment: the switch test found the shell reading the file just as Settings replaced it.

| Case | How | Status |
| :--- | :--- | :--- |
| Launcher and clipboard history drawn (grid, search, sum, list), light and dark | `lares-shell --snapshot`, development PC, looked at; the clipboard list holds made-up clips | confirmed |
| Each on and off, alone and together, with the shell staying for the other and ending with the last; no start-up entry or guard left | `lares-shell-test`, stand-in taskbar, once on the development PC (48 checks with the bar and dock cases) | confirmed |
| Alt+Space and Alt+V taken only while their part is on | `lares-shell-test --real`, the Checks workflow | confirmed when the workflow passes |
| Typing, pasting into another window, pinning, the Windows key | | **not checked**: needs a desktop where the shell is in use |

### The process list and the screenshot tool

Issue [#149](https://github.com/NahuelGenchi/lares/issues/149).

- **Processes** (`win/shell/procmon.c`) opens from the bar's processor, graphics and memory readings, the launcher and the dock's Start menu: processor, graphics (with the card's name as Windows reports it) and memory at a glance, how much the shell itself uses, and a list to search and sort by name, number, processor, memory or threads. **End Task** asks first and refuses Windows' own processes (System, csrss, wininit, winlogon, services, lsass, dwm and the like). It is part of the bar, with no switch of its own; its readings run only while it is open, and the dock lists it like an app.
- **Screenshot tool** (`win/shell/screenshot.c`), with its own switch under **Launcher and tools**, off by default: Alt+Shift+S for a region, Alt+Shift+W for the window in front, Alt+Shift+F for the screen; Win+Shift+S and Print Screen stay Windows' own. A capture goes to the clipboard; a preview in the corner can save it as PNG in the user's Pictures\Screenshots folder, which Windows names (no fixed folder), and open it with the program Windows uses for PNG files. Everything it holds goes as soon as the capture is cancelled, saved or dismissed.

| Case | How | Status |
| :--- | :--- | :--- |
| The process list and the screenshot preview drawn, light and dark | `lares-shell --snapshot`, development PC, looked at; the preview shows a made-up picture, never the screen, and the clipboard is not touched | confirmed |
| The screenshot tool on and off beside clipboard history, the shell staying for it and ending with it | `lares-shell-test`, once on the development PC (51 checks in all) | confirmed |
| Alt+Shift+S taken only while the tool is on | `lares-shell-test --real`, the Checks workflow | confirmed when the workflow passes |
| A real capture of each kind, saving, End Task | | **not checked**: a capture of the development PC's screen and ending a process there are left to a test account |

## The viewers

`lares-view.exe` (`win/shell/view_main.c`, `viewer.c`, `textview.c`, issue [#151](https://github.com/NahuelGenchi/lares/issues/151)) holds the image viewer and the text and Markdown editor. It is a program of its own: it starts when a file is opened in it and ends with its last window, so it costs nothing while unused, and a crash in it cannot take the bar or dock down. A second start hands its file to the running one and ends.

```
lares-view --view FILE       the picture, in the image viewer
lares-view --text [FILE]     the file, or a new one, in the editor
lares-view --snapshot DIR    draws a made-up picture and Markdown file into DIR and ends
```

With `LARES_SHELL_ISOLATED=1` it runs as a test copy: its windows sit far off the screen and out of Alt+Tab, with a mutex and host of their own, so a check on a PC someone works on shows nothing and never meets a viewer they have open.

- **Image viewer**: PNG, JPEG, WebP, BMP, GIF and ICO through Windows Imaging Component on the processor, with the longest side held to 2048 px so a very large picture stays small in memory; zoom around the pointer, pan, fit, actual size, rotate, copy, and the next or previous picture in the same folder. The picture's memory goes back the moment the window closes or another picture is shown.
- **Text and Markdown editor**: Markdown shown formatted (headings, emphasis, inline code, fenced code with a Copy button, quotes and alerts, nested lists, task boxes that can be ticked, rules, tables, links) with an outline of the headings and a source mode; code, settings and logs in monospace; find, zoom, word wrap, light or dark; saving keeps the file's own encoding and line endings; files over 2 MB open read-only with their first 512 KB.

The palettes and layout are the prototype's (owner decision in [#124](https://github.com/NahuelGenchi/lares/issues/124)); the comments that named the applications they were modelled on are gone.

The **Viewers** switch (Settings, **Files and viewers**, off by default) lets Lares' own parts use them: the launcher gets a **Text Editor**, and the screenshot preview opens a saved capture in the image viewer. Windows' own apps stay the default for the user's files; making Lares the default is the optional last part of this stage.

| Case | How | Status |
| :--- | :--- | :--- |
| Both drawn with a made-up picture and Markdown file, light and dark | `lares-view --snapshot`, development PC, looked at | confirmed |
| Opened for real with a picture and a Markdown file (the second start handing over to the first), own memory measured, nothing of Direct3D, Direct2D, WebView2, Chromium or .NET loaded, and the program gone once both windows close | the Checks workflow on GitHub's Windows runner | confirmed when the workflow passes; the reading is in its log |
| The same steps as a test copy, off the screen | development PC, once: 7.9 MB own memory (working set 23.7 MB) with both open, both windows found and closed, the program gone. A font tool installed on that PC loads Direct2D into every program, so the module check there is left to the workflow | confirmed |
| Typing, saving, find, zoom and browsing pictures by hand | | **not checked** |

## Files

`lares-files.exe` (`win/shell/files_main.c`, `files.c`, issue [#153](https://github.com/NahuelGenchi/lares/issues/153)) is a file manager of its own, ported from the prototype. Like the viewers it is a separate program: it starts when a folder is opened in it and ends with its last window, so it costs nothing while unused. A second start hands its folder to the running one, which opens it as a tab.

```
lares-files [FOLDER]           the folder (Home when none), or a file's folder with the file selected
lares-files --select FILE      the same as a file's path; /select,FILE is understood too
lares-files --snapshot DIR     draws a made-up folder made inside DIR, then removes it and ends
```

`LARES_SHELL_ISOLATED=1` makes a test copy, as for the viewers: its windows far off the screen and out of Alt+Tab, a mutex and host of its own, and it never takes the focus. A snapshot's path bar starts at the made-up folder, so a picture never shows the folders above it.

- Tabs (up to 8 per window), back and forward, a sidebar with Home, Desktop, Documents, Downloads, Pictures, Music and Videos (found through Windows, so a moved folder is found where it is), the user's bookmarks and the drives with their free space; a path bar that can be typed into, a filter, list and grid views, an info pane, rename (F2), new folder, copy, cut and paste (as a list of files on the clipboard, which File Explorer reads too), and delete to the Recycle Bin.
- **Quick Look** (Space): a preview beside the window that does not take the focus, for pictures, Markdown and text; Up and Down step through the neighbours, Enter opens the file, Space or Esc closes it and frees everything it held. Quick Look is a trademark of Apple Inc.; Lares is not affiliated with Apple.
- Drag and drop inside Files onto folders, bookmarks and drives (moves; Ctrl copies), through Windows' own file operations, so the usual Windows prompts for clashes and the Recycle Bin apply. Dropping files from other programs into Files is not supported yet.
- The sidebar and list columns can be resized; the sizes last while Files runs. Bookmarks are kept in `shell.ini` (`[files-bookmark N]`), nothing in the program's folder.
- A folder is read one level at a time on a worker thread; nothing searches drives in the background. Pictures in the grid come through Windows Imaging Component into a cache of at most 48 thumbnails (under 3 MB). Files draws its own icons and menus: no other program's context menu or icon handler is loaded into it.
- Enter on a picture or a text file opens it in Lares' viewers when the **Viewers** switch is on, else with the program Windows uses for it. **Open in Terminal** uses the terminal found the way [Other programs](#other-programs) describes. Places that only File Explorer can show (This PC, Control Panel, `shell:` folders) are handed to File Explorer.

The **Files** switch (Settings, **Files and viewers**, off by default) makes the dock's Start menu, the power popup and the launcher open Files instead of File Explorer, each labelled **Files** while it is on. File Explorer stays Windows' own: folders opened by other programs, the desktop and Win+E still open File Explorer, and turning the switch off puts the three entries back.

| Case | How | Status |
| :--- | :--- | :--- |
| Drawn with a made-up folder, light and dark | `lares-files --snapshot`, development PC, looked at | confirmed |
| Opened for real on a made-up folder of 41 items, a second start handing over another folder as a tab, own memory measured, nothing of Direct3D, Direct2D, WebView2, Chromium or .NET loaded, and the program gone once its window closes | the Checks workflow on GitHub's Windows runner | confirmed when the workflow passes; the reading is in its log |
| The same steps as a test copy, off the screen | development PC, once: 7.3 MB own memory (working set 23.7 MB) with two folders open in tabs, 0 ms of processor time in 30 idle seconds, the focus left where it was, the program gone after its window closed. The font tool on that PC loads Direct2D here too, so the module check is left to the workflow | confirmed |
| The switch's entries: the power popup says **Files** while it is on | `lares-shell --snapshot`, looked at | confirmed |
| Browsing, Quick Look, renaming, copying, dragging and deleting by hand | | **not checked**: they change real files, and belong on a test account |

## AirDrop in the top bar

Issue [#155](https://github.com/NahuelGenchi/lares/issues/155). With **AirDrop in the top bar** on (Settings, **Bar and dock**, off by default, and only while the bar is on), the bar gets an AirDrop button beside the network one: in the accent colour while something is received or sent, with a red dot while a request waits for an answer. AirDrop is a trademark of Apple Inc.; Lares is not affiliated with Apple.

It is not a second copy of sharing. `win/shell/airdrop.c` reaches Lares' own sharing code through a small C bridge, `win/src/share_bridge.cpp`: the same `ShareManager`, `lares-share.exe`, Wi-Fi adapter, contacts and Bluetooth wake-up that Lares' Share page uses ([docs/WINDOWS.md](WINDOWS.md)).

- **Receiving**: Off, **Contacts Only** (the contacts saved on Lares' Share page) or **Everyone** for 10 minutes, with the time left.
- **Nearby devices**: looked for while the panel is open, with Windows' Bluetooth sending the AirDrop wake-up meanwhile, and a line in the panel when Bluetooth is off; a click on one picks files to send to it, and a click while sending cancels.
- **What comes in**: Accept or Decline, the progress, and **Show in folder** once saved (in Files when that switch is on).
- **Open AirDrop in Lares** ends the bar's session first and opens the Share page, for the set-up, contacts and the other ways to send.

Nothing of sharing runs until the panel is first opened. When nothing is received, sent or looked for, `lares-share.exe` ends by itself and the adapter powers down; turning the switch off or closing the shell ends it at once. Only one program may drive the adapter: `lares-share.exe` now refuses a second session and says why ("Another Lares program is using the AirDrop adapter. Turn AirDrop off there first."), and Lares' Share page and the panel show that line instead of a bare "AirDrop stopped."

| Case | How | Status |
| :--- | :--- | :--- |
| The panel drawn from made-up state (receiving, three made-up devices, a request to accept), light and dark | `lares-shell --snapshot`, development PC, looked at | confirmed |
| The switch on and off beside the bar: the same shell carries on, and no `lares-share.exe` starts | `lares-shell-test`, stand-in taskbar; ten times in the Checks workflow | confirmed when the workflow passes |
| The shell's own memory and processor time at rest with the button on | `lares-shell-test --measure 30`, the Checks workflow | confirmed when the workflow passes; the reading is in its log |
| Receiving, sending and the wake-up with a real iPhone through the panel | | **not checked**: the development PC's adapter and devices are not used by a check; `ShareManager` itself was checked with an iPhone in [#109](https://github.com/NahuelGenchi/lares/issues/109) |
| The second-session refusal, with Lares' Share page and the panel both open | | **not checked** on hardware |

## Device indicators

Issue [#157](https://github.com/NahuelGenchi/lares/issues/157). With **Device indicators** on (Settings, **Bar and dock**, off by default, and only while the bar is on), the left end of the bar's right-hand group shows a small icon for each of these while it is there, and a click opens Lares on its page:

| Icon | Shown while | Opens |
| :--- | :--- | :--- |
| Controller | an Xbox controller answers through XInput; in the danger colour when a wireless one is low or empty | Controller |
| Headphones | Bluetooth headphones or speakers are connected | Headphones |
| Keyboard | a keyboard Lares can light is present (its own HID scan) | Keyboard |
| Mouse | a mouse Lares can set up is present | Mouse |
| Remote | Lares' Remote mode is on, as the Remote page saved it in `lares.ini` | Remote |

They are not built twice: `win/src/indicators.cpp` asks Lares' own code (`hid_find`, the Bluetooth audio scan of `devices.cpp`, the Remote page's `lares.ini` reader) and XInput the way the Controller page loads it, and only reads. It runs on a worker thread once when the switch comes on, then a moment after Windows says devices came or went (so a burst of arrivals is one look) and when the screen changes, which Remote mode does. Nothing polls; between looks the shell holds five flags.

| Case | How | Status |
| :--- | :--- | :--- |
| All five drawn from made-up state, the controller low, light and dark | `lares-shell --snapshot`, development PC, looked at | confirmed |
| The switch on and off beside the bar, the same shell carrying on | `lares-shell-test`, stand-in taskbar; ten times in the Checks workflow | confirmed when the workflow passes |
| The shell's own memory and processor time at rest with bar, dock, the AirDrop button and the indicators on | `lares-shell-test --measure 30`: once on the development PC (a test copy that hides nothing), and in the Checks workflow | development PC: 18.7 MB own memory (working set 9.2 MB), 62.5 ms of processor time in 30 seconds (0.21% of one core); the workflow's reading is in its log |
| An icon appearing and going as a real controller, headphones, keyboard or mouse comes and goes, and with Remote mode | | **not checked**: needs the devices plugged and unplugged on a desktop where the shell is in use |

### Known limits

- The bar and dock are drawn at fixed pixel sizes on the primary screen, as the prototype drew them: at 150% or 200% display scale they are small, and a second screen gets neither. Scaling and more screens belong to the test matrix of stage 4.
- Start opens Windows' own Start menu unless the launcher is on.
