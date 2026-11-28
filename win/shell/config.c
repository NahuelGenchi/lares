// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The shell's settings (issue #145): shell.ini in Lares' own folder through
 * shell_prefs.h, plus the sizes the bar and dock are drawn at. Nothing is
 * read from the program's own folder. */
#include "shell.h"
#include "programs.h"

#include <shlwapi.h>

/* The dock's first set, until the user changes it: Explorer, the browser
 * Windows opens links with, a terminal, and Lares. Each only when found. */
static void first_pins(void) {
    g_dock_cfg_count = 0;
    wchar_t path[MAX_PATH], name[128];

    if (lares_windows_program(L"explorer.exe", path, MAX_PATH)) {
        DockItemConfig *it = &g_dock_cfg[g_dock_cfg_count++];
        ZeroMemory(it, sizeof(*it));
        wcsncpy_s(it->name, 128, L"File Explorer", _TRUNCATE);
        wcsncpy_s(it->path, MAX_PATH, path, _TRUNCATE);
        wcsncpy_s(it->command, MAX_PATH, path, _TRUNCATE);
    }
    if (lares_find_browser(path, MAX_PATH, name, 128)) {
        DockItemConfig *it = &g_dock_cfg[g_dock_cfg_count++];
        ZeroMemory(it, sizeof(*it));
        wcsncpy_s(it->name, 128, name, _TRUNCATE);
        wcsncpy_s(it->path, MAX_PATH, path, _TRUNCATE);
        wcsncpy_s(it->command, MAX_PATH, path, _TRUNCATE);
    }
    if (lares_find_terminal(path, MAX_PATH, name, 128)) {
        DockItemConfig *it = &g_dock_cfg[g_dock_cfg_count++];
        ZeroMemory(it, sizeof(*it));
        wcsncpy_s(it->name, 128, name, _TRUNCATE);
        wcsncpy_s(it->path, MAX_PATH, path, _TRUNCATE);
        wcsncpy_s(it->command, MAX_PATH, path, _TRUNCATE);
    }
    /* Lares sits next to this program. */
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH && PathRemoveFileSpecW(path) &&
        wcscat_s(path, MAX_PATH, L"\\lares.exe") == 0 && PathFileExistsW(path)) {
        DockItemConfig *it = &g_dock_cfg[g_dock_cfg_count++];
        ZeroMemory(it, sizeof(*it));
        wcsncpy_s(it->name, 128, L"Lares", _TRUNCATE);
        wcsncpy_s(it->path, MAX_PATH, path, _TRUNCATE);
        wcsncpy_s(it->command, MAX_PATH, path, _TRUNCATE);
    }
    for (int i = 0; i < g_dock_cfg_count; ++i) {
        g_dock_cfg[i].group = DOCK_GROUP_CENTER;
        g_dock_cfg[i].type = DOCK_ITEM_APP;
        g_dock_cfg[i].pinned = true;
    }
}

void config_load_all(void) {
    LaresShellPrefs p;
    lares_shell_prefs_load(&p);

    g_cfg.polling_interval_ms = 2000;
    g_cfg.animations = p.animations;

    g_cfg.topbar_enabled = p.bar;
    g_cfg.topbar_item_size = 16;
    g_cfg.topbar_padding = 8;
    g_cfg.topbar_margin = 0;
    g_cfg.start_of_week = p.week_starts_monday ? 1 : 0;
    g_cfg.topbar_autohide = p.bar_auto_hide;
    g_cfg.airdrop_enabled = p.bar_airdrop;

    g_cfg.dock_enabled = p.dock;
    g_cfg.dock_item_size = 40;
    g_cfg.dock_padding = 8;
    g_cfg.dock_margin = 8;
    g_cfg.dock_spacing = 8;
    g_cfg.dock_hide_mode = p.dock_hide == LARES_DOCK_ALWAYS_SHOWN ? HIDE_NEVER
                         : p.dock_hide == LARES_DOCK_AUTO_HIDE    ? HIDE_ALWAYS
                                                                  : HIDE_ON_OVERLAP;
    g_cfg.dock_delay_hide_ms = 1000;
    g_cfg.dock_delay_show_ms = 0;
    g_cfg.dock_show_instance_counter = p.dock_window_counts;
    g_cfg.dock_middle_click_new = p.dock_middle_click_new;
    g_cfg.dock_show_media_widget = p.dock_media;

    g_cfg.launcher_enabled = p.launcher;
    g_cfg.launcher_win_key = p.launcher && p.launcher_windows_key;
    g_cfg.clipboard_enabled = p.clipboard;
    g_cfg.screenshot_enabled = p.screenshot;
    g_cfg.viewers_enabled = p.viewers;
    g_cfg.files_enabled = p.files;

    static LaresDockPin pins[LARES_MAX_DOCK_ITEMS];
    int n = lares_shell_pins_load(pins, LARES_MAX_DOCK_ITEMS);
    if (n < 0) {
        first_pins();
    } else {
        g_dock_cfg_count = 0;
        for (int i = 0; i < n; ++i) {
            DockItemConfig *it = &g_dock_cfg[g_dock_cfg_count++];
            ZeroMemory(it, sizeof(*it));
            it->group = DOCK_GROUP_CENTER;
            it->type = DOCK_ITEM_APP;
            it->pinned = true;
            wcsncpy_s(it->name, 128, pins[i].name, _TRUNCATE);
            wcsncpy_s(it->umid, 256, pins[i].app_id, _TRUNCATE);
            wcsncpy_s(it->path, MAX_PATH, pins[i].path, _TRUNCATE);
            wcsncpy_s(it->command, MAX_PATH, pins[i].path, _TRUNCATE);
            wcsncpy_s(it->args, LARES_PIN_ARGS, pins[i].args, _TRUNCATE);
            wcsncpy_s(it->icon_rel, MAX_PATH, pins[i].icon, _TRUNCATE);
        }
    }
    /* The media controls sit at the right end when they are on. */
    if (g_dock_cfg_count < LARES_MAX_DOCK_ITEMS) {
        DockItemConfig *m = &g_dock_cfg[g_dock_cfg_count++];
        ZeroMemory(m, sizeof(*m));
        m->group = DOCK_GROUP_RIGHT;
        m->type = DOCK_ITEM_MEDIA;
        wcsncpy_s(m->name, 128, L"Media", _TRUNCATE);
    }
}

void config_save_dock(void) {
    static LaresDockPin pins[LARES_MAX_DOCK_ITEMS];
    int n = 0;
    for (int i = 0; i < g_dock_cfg_count && n < LARES_MAX_DOCK_ITEMS; ++i) {
        const DockItemConfig *it = &g_dock_cfg[i];
        if (it->type != DOCK_ITEM_APP || !it->pinned) continue;
        LaresDockPin *p = &pins[n++];
        ZeroMemory(p, sizeof(*p));
        wcsncpy_s(p->name, LARES_PIN_NAME, it->name, _TRUNCATE);
        wcsncpy_s(p->app_id, LARES_PIN_APP_ID, it->umid, _TRUNCATE);
        wcsncpy_s(p->path, LARES_PIN_PATH, it->command[0] ? it->command : it->path, _TRUNCATE);
        wcsncpy_s(p->args, LARES_PIN_ARGS, it->args, _TRUNCATE);
        wcsncpy_s(p->icon, LARES_PIN_PATH, it->icon_rel, _TRUNCATE);
    }
    if (!g_cfg.isolated) lares_shell_pins_save(pins, n);
}

/* The bar's and dock's own menus change how they behave; the parts
 * themselves are switched in Lares' Settings. */
void config_save_behaviour(void) {
    if (g_cfg.isolated) return;
    LaresShellPrefs p;
    lares_shell_prefs_load(&p);
    p.animations = g_cfg.animations;
    p.bar_auto_hide = g_cfg.topbar_autohide;
    p.dock_hide = g_cfg.dock_hide_mode == HIDE_NEVER  ? LARES_DOCK_ALWAYS_SHOWN
                : g_cfg.dock_hide_mode == HIDE_ALWAYS ? LARES_DOCK_AUTO_HIDE
                                                      : LARES_DOCK_WHEN_COVERED;
    lares_shell_prefs_save(&p);
}

void config_resolve_path(const wchar_t *rel, wchar_t *out, size_t out_len) {
    if (!rel || !rel[0]) {
        if (out_len > 0) out[0] = L'\0';
        return;
    }
    if (!PathIsRelativeW(rel)) {
        wcsncpy_s(out, out_len, rel, _TRUNCATE);
        return;
    }
    wchar_t dir[MAX_PATH];
    if (!lares_shell_settings_dir(dir, MAX_PATH)) {
        if (out_len > 0) out[0] = L'\0';
        return;
    }
    _snwprintf_s(out, out_len, _TRUNCATE, L"%ls\\%ls", dir, rel);
}
