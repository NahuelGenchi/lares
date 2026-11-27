// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"

/*
 * Keyboard shortcuts (issues #145, #148), each only while its part is on:
 *   - Win + 1..0       -> brings dock app 1..10 to the front, or starts it (the dock)
 *   - Alt + Space      -> the launcher
 *   - a lone Win tap   -> the launcher, when its own setting is on
 *   - Alt + V          -> clipboard history (Win+V stays Windows' own)
 *   - Alt + Shift + S, W, F -> a screenshot of a region, the window, the
 *     screen (Win+Shift+S and Print Screen stay Windows' own)
 * Every other Windows-key combination (Win+D, Win+R, Win+L, Win+Tab,
 * Win+Shift+S, Win+E, ...) passes through untouched. The keyboard hook is
 * only installed while one of these is on.
 */

static HHOOK g_kb_hook = NULL;
static HWND  g_hk_host = NULL;
static bool  g_win_down = false;
static bool  g_win_other_key = false;
static bool  g_suppress_start_injected = false;

static void inject_dummy_mask_key(void) {
    /* Sending an unassigned VK (0xFF) masks solo Win key-up so Explorer does NOT open Start Menu */
    INPUT inp[2];
    ZeroMemory(inp, sizeof(inp));
    inp[0].type = INPUT_KEYBOARD;
    inp[0].ki.wVk = 0xFF;
    inp[1].type = INPUT_KEYBOARD;
    inp[1].ki.wVk = 0xFF;
    inp[1].ki.dwFlags = KEYEVENTF_KEYUP;
    g_suppress_start_injected = true;
    SendInput(2, inp, sizeof(INPUT));
    g_suppress_start_injected = false;
}

static LRESULT CALLBACK low_level_kb_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && !g_suppress_start_injected) {
        const KBDLLHOOKSTRUCT *kb = (const KBDLLHOOKSTRUCT *)lParam;
        bool is_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool is_up   = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        DWORD vk = kb->vkCode;

        if (vk == VK_LWIN || vk == VK_RWIN) {
            if (is_down) {
                if (!g_win_down) {
                    g_win_down = true;
                    g_win_other_key = false;
                }
            } else if (is_up) {
                bool was_solo = g_win_down && !g_win_other_key;
                g_win_down = false;
                g_win_other_key = false;
                if (was_solo && g_cfg.launcher_win_key && g_hk_host) {
                    inject_dummy_mask_key();
                    PostMessageW(g_hk_host, WM_LARES_TOGGLE_LAUNCHER, 0, 0);
                }
            }
        } else if (g_win_down) {
            if (is_down && vk >= '0' && vk <= '9') {
                /* Do not steal Ctrl+Win+digit or Shift+Win+digit */
                bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                bool alt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                if (!ctrl && !shift && !alt) {
                    g_win_other_key = true;
                    if (!g_cfg.dock_enabled) return CallNextHookEx(g_kb_hook, nCode, wParam, lParam);
                    inject_dummy_mask_key();
                    int idx = (vk == '0') ? 9 : (int)(vk - '1');
                    if (g_hk_host) PostMessageW(g_hk_host, WM_LARES_DOCK_SHORTCUT, (WPARAM)idx, 0);
                    return 1; /* consume digit so Explorer doesn't run native taskbar pin */
                }
            }
            if (is_down && vk != 0xFF) {
                g_win_other_key = true;
            }
        }
    }
    return CallNextHookEx(g_kb_hook, nCode, wParam, lParam);
}

bool hotkeys_init(HWND host_hwnd) {
    g_hk_host = host_hwnd;
    if (g_cfg.launcher_enabled) RegisterHotKey(host_hwnd, LARES_HOTKEY_LAUNCHER, MOD_ALT | MOD_NOREPEAT, VK_SPACE);
    if (g_cfg.clipboard_enabled) RegisterHotKey(host_hwnd, LARES_HOTKEY_CLIPBOARD, MOD_ALT | MOD_NOREPEAT, 'V');
    if (g_cfg.screenshot_enabled) {
        RegisterHotKey(host_hwnd, LARES_HOTKEY_SNIP_REGION, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S');
        RegisterHotKey(host_hwnd, LARES_HOTKEY_SNIP_WINDOW, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'W');
        RegisterHotKey(host_hwnd, LARES_HOTKEY_SNIP_FULL, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'F');
    }
    if (!g_cfg.dock_enabled && !g_cfg.launcher_win_key) return true;
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_kb_proc, GetModuleHandleW(NULL), 0);
    return g_kb_hook != NULL;
}

void hotkeys_shutdown(void) {
    if (g_kb_hook) {
        UnhookWindowsHookEx(g_kb_hook);
        g_kb_hook = NULL;
    }
    if (g_hk_host) {
        UnregisterHotKey(g_hk_host, LARES_HOTKEY_LAUNCHER);
        UnregisterHotKey(g_hk_host, LARES_HOTKEY_CLIPBOARD);
        UnregisterHotKey(g_hk_host, LARES_HOTKEY_SNIP_REGION);
        UnregisterHotKey(g_hk_host, LARES_HOTKEY_SNIP_WINDOW);
        UnregisterHotKey(g_hk_host, LARES_HOTKEY_SNIP_FULL);
    }
    g_hk_host = NULL;
}
