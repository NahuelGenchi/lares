// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* lares-view.exe (issue #151): the image viewer and the text and Markdown
 * editor, in a program of their own that starts when a file is opened in it
 * and ends with its last window, so they cost nothing while unused.
 *
 *   lares-view --view FILE       the picture, in the image viewer
 *   lares-view --text [FILE]     the file, or a new one, in the editor
 *   lares-view --snapshot DIR    a test copy: opens a made-up picture and a
 *                                made-up Markdown file off the screen, saves
 *                                both windows as PNG files in DIR, and ends
 *
 * A second start hands its file to the running lares-view.exe and ends.
 * LARES_COLOR_SCHEME=light or dark picks the palette.
 */
#include "shell.h"

#include "crash.h"
#include "lares_version.h"

#include <objbase.h>
#include <shellapi.h>

#define VIEW_HOST_CLASS  L"LaresViewHost"
#define VIEW_MUTEX       L"Local\\LaresView"
#define VIEW_COPY_IMAGE  0x4C560001UL
#define VIEW_COPY_TEXT   0x4C560002UL

HINSTANCE       g_hInst = NULL;
HWND            g_hHostWnd = NULL;
HWND            g_hTopBarWnd = NULL;
HWND            g_hDockWnd = NULL;
AppConfig       g_cfg;
DockItemConfig  g_dock_cfg[LARES_MAX_DOCK_ITEMS];
int             g_dock_cfg_count = 0;
SystemTelemetry g_telemetry;

typedef struct {
    wchar_t path[MAX_PATH];
} ViewCopy;

static void end_if_no_window(void) {
    if (!viewer_is_open() && !textview_is_open()) PostQuitMessage(0);
}

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (!cds || cds->cbData != sizeof(ViewCopy) || !cds->lpData) return FALSE;
        ViewCopy copy;
        memcpy(&copy, cds->lpData, sizeof(copy));
        copy.path[MAX_PATH - 1] = L'\0';
        if (cds->dwData == VIEW_COPY_IMAGE && copy.path[0]) viewer_open(copy.path);
        else if (cds->dwData == VIEW_COPY_TEXT) textview_open(copy.path[0] ? copy.path : NULL);
        return TRUE;
    }
    case WM_LARES_WINDOWS_CHANGED:
        end_if_no_window();
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        theme_reload();
        viewer_on_theme_changed();
        textview_on_theme_changed();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void strip_quotes(const wchar_t *src, wchar_t *dst, size_t len) {
    while (*src == L' ' || *src == L'"') src++;
    wcsncpy_s(dst, len, src, _TRUNCATE);
    size_t n = wcslen(dst);
    while (n > 0 && (dst[n - 1] == L' ' || dst[n - 1] == L'"')) dst[--n] = L'\0';
}

/* A made-up picture for checks: a soft gradient with a few shapes. */
static bool write_sample_png(const wchar_t *path) {
    const int w = 960, h = 600;
    BYTE *px = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)w * h * 4);
    if (!px) return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            BYTE *p = px + ((size_t)y * w + x) * 4;
            int dx = x - w / 2, dy = y - h / 2;
            bool disc = dx * dx + dy * dy < 140 * 140;
            p[0] = (BYTE)(disc ? 240 : 140 + x * 80 / w);
            p[1] = (BYTE)(disc ? 200 : 90 + y * 100 / h);
            p[2] = (BYTE)(disc ? 90 : 70 + (x + y) * 60 / (w + h));
            p[3] = 255;
        }
    static const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    GpBitmap *bmp = NULL;
    bool ok = GdipCreateBitmapFromScan0(w, h, w * 4, GP_PIXELFORMAT_32BPP_ARGB, px, &bmp) == GP_OK && bmp &&
              GdipSaveImageToFile((GpImage *)bmp, path, &png, NULL) == GP_OK;
    if (bmp) GdipDisposeImage((GpImage *)bmp);
    HeapFree(GetProcessHeap(), 0, px);
    return ok;
}

static bool write_sample_markdown(const wchar_t *path) {
    static const char text[] =
        "# Notes for the week\n\n"
        "A short **Markdown** file made up for a check, with *emphasis*, `inline code` and a "
        "[link](https://example.com).\n\n"
        "## To do\n\n"
        "- [x] Water the plants\n"
        "- [ ] Write the report\n"
        "- [ ] Call about the boiler\n\n"
        "> [!NOTE]\n> A quote that is also a note.\n\n"
        "```c\nint main(void) { return 0; }\n```\n\n"
        "| Day | Hours |\n| :--- | ---: |\n| Monday | 6 |\n| Tuesday | 7 |\n";
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    BOOL ok = WriteFile(f, text, (DWORD)(sizeof(text) - 1), &n, NULL);
    CloseHandle(f);
    return ok && n == sizeof(text) - 1;
}

static void pump_for(DWORD ms) {
    DWORD until = GetTickCount() + ms;
    MSG m;
    while ((int)(until - GetTickCount()) > 0) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(15);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;
    g_hInst = hInst;
    lares_crash_install("lares-view", LARES_VERSION);

    wchar_t image[MAX_PATH] = {0}, text[MAX_PATH] = {0}, snapshot[MAX_PATH] = {0};
    bool want_text = false;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--view") == 0 && i + 1 < argc) strip_quotes(argv[++i], image, MAX_PATH);
        else if (_wcsicmp(argv[i], L"--text") == 0) {
            want_text = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') strip_quotes(argv[++i], text, MAX_PATH);
        } else if (_wcsicmp(argv[i], L"--snapshot") == 0 && i + 1 < argc) {
            strip_quotes(argv[++i], snapshot, MAX_PATH);
        }
    }
    if (argv) LocalFree(argv);
    if (!image[0] && !want_text && !snapshot[0]) return 0;

    /* LARES_SHELL_ISOLATED=1: a test copy, its windows far off the screen,
     * with a mutex and host of its own so it never meets a viewer the user
     * has open. */
    wchar_t iso[4] = L"";
    bool isolated = snapshot[0] != 0 ||
                    (GetEnvironmentVariableW(L"LARES_SHELL_ISOLATED", iso, 4) == 1 && iso[0] == L'1');
    const wchar_t *host_class = isolated ? VIEW_HOST_CLASS L"Isolated" : VIEW_HOST_CLASS;

    /* A running lares-view.exe takes the file over. */
    HANDLE mutex = NULL;
    if (!snapshot[0]) {
        mutex = CreateMutexW(NULL, TRUE, isolated ? VIEW_MUTEX L"Isolated" : VIEW_MUTEX);
        if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND host = NULL;
            for (int t = 0; t < 40 && !host; ++t) {
                host = FindWindowW(host_class, NULL);
                if (!host) Sleep(50);
            }
            if (host) {
                DWORD pid = 0;
                GetWindowThreadProcessId(host, &pid);
                if (pid) AllowSetForegroundWindow(pid);
                ViewCopy copy;
                ZeroMemory(&copy, sizeof(copy));
                wcsncpy_s(copy.path, MAX_PATH, image[0] ? image : text, _TRUNCATE);
                COPYDATASTRUCT cds = { image[0] ? VIEW_COPY_IMAGE : VIEW_COPY_TEXT, sizeof(copy), &copy };
                DWORD_PTR res = 0;
                SendMessageTimeoutW(host, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &res);
            }
            if (mutex) CloseHandle(mutex);
            return 0;
        }
    }

    ZeroMemory(&g_cfg, sizeof(g_cfg));
    g_cfg.isolated = isolated;
    g_cfg.animations = true;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!render_init()) {
        CoUninitialize();
        if (mutex) CloseHandle(mutex);
        return 1;
    }
    {
        wchar_t scheme[8];
        DWORD n = GetEnvironmentVariableW(L"LARES_COLOR_SCHEME", scheme, 8);
        if (n > 0 && n < 8) {
            if (_wcsicmp(scheme, L"dark") == 0) theme_apply(true, RGB(0, 103, 192));
            else if (_wcsicmp(scheme, L"light") == 0) theme_apply(false, RGB(0, 103, 192));
        }
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = host_proc;
    wc.hInstance = hInst;
    wc.lpszClassName = host_class;
    RegisterClassExW(&wc);
    /* A snapshot's host takes no files from others. */
    g_hHostWnd = CreateWindowExW(0, host_class, L"", 0, 0, 0, 0, 0,
                                 snapshot[0] ? HWND_MESSAGE : NULL, NULL, hInst, NULL);
    ChangeWindowMessageFilterEx(g_hHostWnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);

    viewer_init(hInst);
    textview_init(hInst);

    int rc = 0;
    if (snapshot[0]) {
        wchar_t png[MAX_PATH], md[MAX_PATH], out[MAX_PATH];
        _snwprintf_s(png, MAX_PATH, _TRUNCATE, L"%ls\\sample-picture.png", snapshot);
        _snwprintf_s(md, MAX_PATH, _TRUNCATE, L"%ls\\sample-notes.md", snapshot);
        bool ok = write_sample_png(png) && write_sample_markdown(md);
        viewer_open(png);
        textview_open(md);
        pump_for(1200);
        _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%ls\\viewer.png", snapshot);
        ok = viewer_snapshot(out) && ok;
        _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%ls\\editor.png", snapshot);
        ok = textview_snapshot(out) && ok;
        viewer_shutdown();
        textview_shutdown();
        DeleteFileW(png);
        DeleteFileW(md);
        rc = ok ? 0 : 1;
    } else {
        if (image[0]) viewer_open(image);
        if (want_text) textview_open(text[0] ? text : NULL);
        if (viewer_is_open() || textview_is_open()) {
            MSG msg;
            while (GetMessageW(&msg, NULL, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        viewer_shutdown();
        textview_shutdown();
    }

    if (g_hHostWnd) DestroyWindow(g_hHostWnd);
    render_shutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}
