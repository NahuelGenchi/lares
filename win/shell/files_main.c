// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* lares-files.exe (issue #153): Files, in a program of its own that starts
 * when a folder is opened in it and ends with its last window.
 *
 *   lares-files [FOLDER]           Files at the folder (the home folder when none)
 *   lares-files --select FILE      Files at the file's folder, with the file selected
 *   lares-files --snapshot DIR     a test copy: makes a small made-up folder in
 *                                  DIR, draws Files on it off the screen into
 *                                  DIR\files.png, and ends
 *
 * A second start hands its folder to the running lares-files.exe and ends.
 * Windows' own places (Control Panel, the Recycle Bin, shell: names) go to
 * File Explorer. LARES_COLOR_SCHEME=light or dark picks the palette.
 */
#include "shell.h"
#include "programs.h"

#include "crash.h"
#include "lares_version.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>

#define FILES_HOST_CLASS L"LaresFilesHost"
#define FILES_MUTEX      L"Local\\LaresFiles"
#define FILES_COPY_OPEN  0x4C460001UL

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
    wchar_t select_name[MAX_PATH];
} FilesCopy;

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (!cds || cds->dwData != FILES_COPY_OPEN || cds->cbData != sizeof(FilesCopy) || !cds->lpData) return FALSE;
        FilesCopy copy;
        memcpy(&copy, cds->lpData, sizeof(copy));
        copy.path[MAX_PATH - 1] = L'\0';
        copy.select_name[MAX_PATH - 1] = L'\0';
        files_open_ex(copy.path[0] ? copy.path : NULL, copy.select_name[0] ? copy.select_name : NULL, true);
        return TRUE;
    }
    case WM_LARES_WINDOWS_CHANGED:
        if (!files_has_open_windows()) PostQuitMessage(0);
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        theme_reload();
        files_on_theme_changed();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void clean_path(const wchar_t *src, wchar_t *dst, size_t len) {
    while (*src == L' ' || *src == L'\t' || *src == L'"') src++;
    wcsncpy_s(dst, len, src, _TRUNCATE);
    size_t n = wcslen(dst);
    while (n > 0 && (dst[n - 1] == L' ' || dst[n - 1] == L'\t' || dst[n - 1] == L'"')) dst[--n] = L'\0';
    if (n == 2 && dst[1] == L':' && len > 3) { /* "D:" means the drive's root */
        dst[2] = L'\\';
        dst[3] = L'\0';
    }
}

static bool is_virtual_place(const wchar_t *path) {
    return path && path[0] && (wcsstr(path, L"::") != NULL || _wcsnicmp(path, L"shell:", 6) == 0);
}

static void write_text(const wchar_t *path, const char *text) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, text, (DWORD)strlen(text), &n, NULL);
    CloseHandle(f);
}

/* A small made-up folder for checks: a subfolder, notes and a list. */
static bool make_sample_folder(const wchar_t *root, wchar_t *out, size_t len) {
    _snwprintf_s(out, len, _TRUNCATE, L"%ls\\Sample folder", root);
    CreateDirectoryW(out, NULL);
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\Projects", out);
    CreateDirectoryW(p, NULL);
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\Recipes", out);
    CreateDirectoryW(p, NULL);
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\Meeting notes.md", out);
    write_text(p, "# Meeting notes\n\n- Budget\n- Plan\n");
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\Shopping list.txt", out);
    write_text(p, "Bread\nMilk\nApples\n");
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\settings.ini", out);
    write_text(p, "[window]\nsize=100\n");
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

static void remove_sample_folder(const wchar_t *folder) {
    static const wchar_t *files[] = { L"Meeting notes.md", L"Shopping list.txt", L"settings.ini" };
    static const wchar_t *dirs[] = { L"Projects", L"Recipes" };
    wchar_t p[MAX_PATH];
    for (int i = 0; i < 3; ++i) {
        _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\%ls", folder, files[i]);
        DeleteFileW(p);
    }
    for (int i = 0; i < 2; ++i) {
        _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\%ls", folder, dirs[i]);
        RemoveDirectoryW(p);
    }
    RemoveDirectoryW(folder);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd; (void)nShow;
    g_hInst = hInst;
    lares_crash_install("lares-files", LARES_VERSION);

    wchar_t path[MAX_PATH] = {0}, select[MAX_PATH] = {0}, snapshot[MAX_PATH] = {0};
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const wchar_t *a = argv[i];
        if (_wcsicmp(a, L"--snapshot") == 0 && i + 1 < argc) clean_path(argv[++i], snapshot, MAX_PATH);
        else if ((_wcsicmp(a, L"--select") == 0 || _wcsicmp(a, L"-select") == 0) && i + 1 < argc)
            clean_path(argv[++i], select, MAX_PATH);
        else if (_wcsnicmp(a, L"/select,", 8) == 0) clean_path(a + 8, select, MAX_PATH);
        else if (_wcsicmp(a, L"--open") == 0 && i + 1 < argc) clean_path(argv[++i], path, MAX_PATH);
        else if (a[0] != L'-' && a[0] != L'/') clean_path(a, path, MAX_PATH);
    }
    if (argv) LocalFree(argv);

    if (is_virtual_place(path)) {
        wchar_t explorer[MAX_PATH];
        if (lares_windows_program(L"explorer.exe", explorer, MAX_PATH)) {
            wchar_t args[MAX_PATH + 4];
            _snwprintf_s(args, MAX_PATH + 4, _TRUNCATE, L"\"%ls\"", path);
            ShellExecuteW(NULL, L"open", explorer, args, NULL, SW_SHOWNORMAL);
        }
        return 0;
    }
    if (select[0] && !path[0]) wcsncpy_s(path, MAX_PATH, select, _TRUNCATE);

    /* LARES_SHELL_ISOLATED=1: a test copy, its windows far off the screen,
     * with a mutex and host of its own so it never meets a Files the user
     * has open. */
    wchar_t iso[4] = L"";
    bool isolated = snapshot[0] != 0 ||
                    (GetEnvironmentVariableW(L"LARES_SHELL_ISOLATED", iso, 4) == 1 && iso[0] == L'1');
    const wchar_t *host_class = isolated ? FILES_HOST_CLASS L"Isolated" : FILES_HOST_CLASS;

    HANDLE mutex = NULL;
    if (!snapshot[0]) {
        mutex = CreateMutexW(NULL, TRUE, isolated ? FILES_MUTEX L"Isolated" : FILES_MUTEX);
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
                FilesCopy copy;
                ZeroMemory(&copy, sizeof(copy));
                wcsncpy_s(copy.path, MAX_PATH, path, _TRUNCATE);
                COPYDATASTRUCT cds = { FILES_COPY_OPEN, sizeof(copy), &copy };
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
    OleInitialize(NULL);
    if (!render_init()) {
        OleUninitialize();
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
    /* A snapshot's host takes no folders from others. */
    g_hHostWnd = CreateWindowExW(0, host_class, L"", 0, 0, 0, 0, 0,
                                 snapshot[0] ? HWND_MESSAGE : NULL, NULL, hInst, NULL);
    ChangeWindowMessageFilterEx(g_hHostWnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);

    files_init(hInst);

    int rc = 0;
    if (snapshot[0]) {
        wchar_t folder[MAX_PATH], out[MAX_PATH];
        bool ok = make_sample_folder(snapshot, folder, MAX_PATH);
        files_set_crumb_root(snapshot);
        files_open_ex(folder, NULL, false);
        MSG m;
        for (int t = 0; t < 200 && !files_loaded(); ++t) {
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(15);
        }
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%ls\\files.png", snapshot);
        ok = files_loaded() && files_snapshot(out) && ok;
        files_shutdown();
        remove_sample_folder(folder);
        rc = ok ? 0 : 1;
    } else {
        /* A file's path opens its folder with the file selected. */
        files_open_ex(path[0] ? path : NULL, NULL, false);
        if (files_has_open_windows()) {
            MSG msg;
            while (GetMessageW(&msg, NULL, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        files_shutdown();
    }

    if (g_hHostWnd) DestroyWindow(g_hHostWnd);
    render_shutdown();
    OleUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}
