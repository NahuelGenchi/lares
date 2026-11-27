// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* What the bar's and dock's buttons open (issue #145): Lares' own parts once
 * they exist and are on, Windows' own until then. Programs are found the
 * way Windows finds them (programs.h). */
#include "shell.h"
#include "programs.h"

#include <shellapi.h>
#include <shlwapi.h>

static void open_file(const wchar_t *file, const wchar_t *args) {
    ShellExecuteW(NULL, L"open", file, args, NULL, SW_SHOWNORMAL);
}

/* The launcher when it is on, else Windows' own Start menu, as Ctrl+Esc opens it. */
void action_open_start(void) {
    if (g_cfg.launcher_enabled) {
        if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_TOGGLE_LAUNCHER, 0, 0);
        return;
    }
    INPUT in[4];
    ZeroMemory(in, sizeof(in));
    for (int i = 0; i < 4; ++i) in[i].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = VK_ESCAPE;
    in[2].ki.wVk = VK_ESCAPE;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

void action_open_files(const wchar_t *folder) {
    wchar_t explorer[MAX_PATH];
    if (!lares_windows_program(L"explorer.exe", explorer, MAX_PATH)) return;
    wchar_t args[MAX_PATH + 4] = L"";
    if (folder && folder[0]) _snwprintf_s(args, MAX_PATH + 4, _TRUNCATE, L"\"%ls\"", folder);
    open_file(explorer, args[0] ? args : NULL);
}

void action_open_terminal(void) {
    wchar_t exe[MAX_PATH];
    if (lares_find_terminal(exe, MAX_PATH, NULL, 0)) open_file(exe, NULL);
}

void action_open_settings(void) {
    open_file(L"ms-settings:", NULL);
}

/* Lares' own window, on a section (its --section ids). */
void action_open_lares(const wchar_t *section) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || !PathRemoveFileSpecW(exe) || wcscat_s(exe, MAX_PATH, L"\\lares.exe") != 0) return;
    if (!PathFileExistsW(exe)) return;
    wchar_t args[64] = L"";
    if (section && section[0]) _snwprintf_s(args, 64, _TRUNCATE, L"--section %ls", section);
    open_file(exe, args[0] ? args : NULL);
}

/* Lares' own process list (issue #149). */
void action_open_processes(void) {
    procmon_open(PROCMON_SORT_MEM);
}

/* Lares' screenshot tool when it is on (issue #149), else Windows' own
 * snipping, which asks for the region, window or screen itself. */
void action_screenshot(int mode) {
    if (g_cfg.screenshot_enabled) {
        screenshot_trigger(mode == 1 ? SCREENSHOT_WINDOW : mode == 2 ? SCREENSHOT_FULL : SCREENSHOT_REGION);
        return;
    }
    open_file(L"ms-screenclip:", NULL);
}
