# The Windows shell

[Back to README](../README.md) · Issue [#139](https://github.com/NahuelGenchi/lares/issues/139)

Lares for Windows is getting its own desktop shell: a top bar, a dock, a launcher, popups, Files and viewers. It comes in as an opt-in module in small steps (stage 2 of the Windows plan). Every part has a switch in Lares' Settings that is off by default, and a Windows account with Lares installed looks exactly like stock Windows until one is turned on. [docs/SAFETY.md](SAFETY.md) covers what happens when the shell stops.

Each claim is marked **confirmed** (a recorded check establishes it), **reported** (documented by Microsoft, not checked here), **planned** or **blocked**.

| Part | Issue | Status |
| :--- | :--- | :--- |
| One drawing engine, measured | [#140](https://github.com/NahuelGenchi/lares/issues/140) | confirmed: GDI+ with Lares' own glyph blending |
| The drawing layer and settings | | planned |
| `lares-shell.exe`: top bar and dock, under `lares-guard.exe` | | planned |
| Launcher, popups, clipboard history, process list, screenshot tool | | planned |
| `lares-files.exe` and `lares-view.exe` | | planned |
| AirDrop panel on Lares' own sharing code; device indicators | | planned |
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
