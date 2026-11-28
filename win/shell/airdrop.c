// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar's AirDrop button and panel (issue #155), on Lares' own
 * sharing code through win/src/share_bridge.h: the same ShareManager,
 * lares-share.exe, contacts and Bluetooth wake-up as Lares' Share page.
 *
 * Nothing of it runs until the panel is first opened: then the manager is
 * made, and the panel looks for nearby devices while it is open. When
 * nothing is received, sent or looked for, lares-share.exe ends by itself
 * and the adapter powers down. Turning the switch off, the shell's end, and
 * "Open AirDrop in Lares" free the manager, so Lares' window can take the
 * adapter.
 *
 * AirDrop is a trademark of Apple Inc.; Lares is not affiliated with Apple. */
#include "shell.h"
#include "programs.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>

static bool            g_panel_open = false;
static bool            g_has_test = false;
static LaresShareState g_test;

void airdrop_get(LaresShareState *out) {
    if (!out) return;
    if (g_has_test) {
        *out = g_test;
        return;
    }
    lares_share_get(out);
}

bool airdrop_active(void) {
    if (!g_cfg.airdrop_enabled) return false;
    if (!g_has_test && !lares_share_is_open()) return false;
    LaresShareState s;
    airdrop_get(&s);
    return s.receiving || s.sending || s.has_incoming;
}

bool airdrop_pending(void) {
    if (!g_cfg.airdrop_enabled) return false;
    if (!g_has_test && !lares_share_is_open()) return false;
    LaresShareState s;
    airdrop_get(&s);
    return s.has_incoming && s.in_state == LARES_SHARE_IN_ASKING;
}

void airdrop_panel_opened(void) {
    if (!g_cfg.airdrop_enabled || g_has_test || g_cfg.isolated) return;
    lares_share_open(g_hHostWnd);
    g_panel_open = true;
    lares_share_set_browsing(true);
}

void airdrop_panel_closed(void) {
    if (!g_panel_open) return;
    g_panel_open = false;
    lares_share_set_browsing(false);
}

void airdrop_on_updated(void) {
    lares_share_updated();
    topbar_repaint();
    popups_on_airdrop_updated();
}

void airdrop_apply_settings(void) {
    if (g_cfg.airdrop_enabled) return;
    g_panel_open = false;
    lares_share_close();
}

void airdrop_shutdown(void) {
    g_panel_open = false;
    lares_share_close();
}

void airdrop_set_mode(int mode) {
    lares_share_set_mode(mode);
}

void airdrop_answer(bool accept) { lares_share_answer(accept); }
void airdrop_dismiss(void) { lares_share_dismiss(); }
void airdrop_cancel_send(void) { lares_share_cancel_send(); }

/* Lares' Share page: the bar's session ends first, since only one program
 * may drive the adapter. */
void airdrop_open_lares(void) {
    g_panel_open = false;
    lares_share_close();
    action_open_lares(L"share");
}

/* What came in, selected in its folder: in Lares' Files when that switch is
 * on, else in File Explorer. */
void airdrop_show_saved(void) {
    LaresShareState s;
    airdrop_get(&s);
    if (!s.in_saved[0] || g_has_test) return;
    wchar_t args[MAX_PATH + 16];
    _snwprintf_s(args, MAX_PATH + 16, _TRUNCATE, L"/select,\"%ls\"", s.in_saved);
    wchar_t exe[MAX_PATH];
    if (g_cfg.files_enabled) {
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH && PathRemoveFileSpecW(exe) &&
            wcscat_s(exe, MAX_PATH, L"\\lares-files.exe") == 0 && PathFileExistsW(exe) &&
            (INT_PTR)ShellExecuteW(NULL, L"open", exe, args, NULL, SW_SHOWNORMAL) > 32)
            return;
    }
    if (lares_windows_program(L"explorer.exe", exe, MAX_PATH))
        ShellExecuteW(NULL, L"open", exe, args, NULL, SW_SHOWNORMAL);
}

/* Picks files and offers them to one nearby device. */
void airdrop_send_to(const char *key, HWND owner) {
    if (!key || !key[0] || g_has_test) return;
    enum { PICK = 8192, MAX_FILES = 64 };
    wchar_t *pick = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PICK * sizeof(wchar_t));
    wchar_t *full = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)MAX_FILES * MAX_PATH * sizeof(wchar_t));
    const wchar_t *paths[MAX_FILES];
    int count = 0;
    if (!pick || !full) goto done;

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = pick;
    ofn.nMaxFile = PICK;
    ofn.lpstrTitle = L"Choose what to send";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) goto done;

    const wchar_t *dir = pick;
    const wchar_t *name = pick + wcslen(pick) + 1;
    if (!*name) {
        /* One file: the buffer holds its full path. */
        wcsncpy_s(full, MAX_PATH, pick, _TRUNCATE);
        paths[count++] = full;
    } else {
        /* Several: the folder first, then each name. */
        while (*name && count < MAX_FILES) {
            wchar_t *slot = full + (size_t)count * MAX_PATH;
            _snwprintf_s(slot, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, name);
            paths[count++] = slot;
            name += wcslen(name) + 1;
        }
    }
    if (count > 0) lares_share_send_to(key, paths, count);

done:
    if (pick) HeapFree(GetProcessHeap(), 0, pick);
    if (full) HeapFree(GetProcessHeap(), 0, full);
}

/* Test copies draw the panel from made-up state. */
void airdrop_set_test_state(const LaresShareState *s) {
    if (s) {
        g_test = *s;
        g_has_test = true;
    } else {
        g_has_test = false;
    }
}
