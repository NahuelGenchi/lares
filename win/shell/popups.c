// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The bar's popups (issue #145). Lares' own popups come in part 4 of the
 * shell's import (issue #139); until then each button opens what Windows
 * itself offers for it, and nothing stays open in the shell. */
#include "shell.h"

#include <shellapi.h>

static void open_uri(const wchar_t *uri) {
    ShellExecuteW(NULL, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
}

void popups_init(HINSTANCE hInst) {
    (void)hInst;
}

void popups_toggle(PopupKind kind, int anchor_x, int anchor_y) {
    (void)anchor_x, (void)anchor_y;
    if (g_cfg.isolated) return; /* a test copy opens nothing */
    switch (kind) {
    case POPUP_CALENDAR:
    case POPUP_NOTIFICATIONS:
        open_uri(L"ms-actioncenter:");
        break;
    case POPUP_KEYBOARD:
        open_uri(L"ms-settings:keyboard");
        break;
    case POPUP_BLUETOOTH:
        open_uri(L"ms-settings:bluetooth");
        break;
    case POPUP_NETWORK:
        open_uri(L"ms-availablenetworks:");
        break;
    case POPUP_MEDIA:
    case POPUP_POWER:
        open_uri(L"ms-actioncenter:controlcenter/");
        break;
    case POPUP_SCREENSHOT:
        open_uri(L"ms-screenclip:");
        break;
    case POPUP_SHARE:
        action_open_lares(L"share");
        break;
    case POPUP_NONE:
        break;
    }
}

void popups_close(void) {}
bool popups_is_open(void) { return false; }
bool popups_is_kind_open(PopupKind kind) { (void)kind; return false; }
void popups_refresh_if_open(void) {}
void popups_on_tailscale_updated(void) {}
void popups_on_bt_scan_done(void) {}
void popups_shutdown(void) {}
