// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
// The shell's settings for its C code: a thin bridge to config.cpp (#141).
#include "shell_prefs.h"

#include "config.h"

#include <windows.h>

#include <cwchar>
#include <string>

namespace {

std::wstring wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const wchar_t* s)
{
    if (!s || !*s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

void copy(wchar_t* dst, size_t cch, const std::string& src)
{
    wcsncpy_s(dst, cch, wide(src).c_str(), _TRUNCATE);
}

std::wstring path_in_use()
{
    return lares::shell_ini_path(lares_shell_dry_run());
}

lares::ShellPrefs load_now()
{
    lares::ShellPrefs p;
    lares::load_shell_prefs(path_in_use(), p);
    return p;
}

} // namespace

extern "C" bool lares_shell_dry_run(void)
{
    wchar_t buf[16];
    DWORD n = GetEnvironmentVariableW(L"LARES_DRY_RUN", buf, 16);
    return n > 0 && n < 16 && buf[0] != L'\0' && wcscmp(buf, L"0") != 0;
}

extern "C" void lares_shell_prefs_load(LaresShellPrefs* out)
{
    if (!out) return;
    lares::ShellPrefs p = load_now();
    out->bar = p.bar;
    out->dock = p.dock;
    out->launcher = p.launcher;
    out->launcher_windows_key = p.launcher_windows_key;
    out->clipboard = p.clipboard;
    out->screenshot = p.screenshot;
    out->files = p.files;
    out->viewers = p.viewers;
    out->bar_auto_hide = p.bar_auto_hide;
    out->week_starts_monday = p.week_starts_monday;
    out->bar_airdrop = p.bar_airdrop;
    out->bar_indicators = p.bar_indicators;
    out->dock_hide = p.dock_hide == lares::DockHide::AlwaysShown ? LARES_DOCK_ALWAYS_SHOWN
                     : p.dock_hide == lares::DockHide::AutoHide  ? LARES_DOCK_AUTO_HIDE
                                                                 : LARES_DOCK_WHEN_COVERED;
    out->dock_window_counts = p.dock_window_counts;
    out->dock_middle_click_new = p.dock_middle_click_new;
    out->dock_media = p.dock_media;
    out->animations = p.animations;
}

extern "C" bool lares_shell_prefs_save(const LaresShellPrefs* in)
{
    if (!in) return false;
    lares::ShellPrefs p = load_now();
    p.bar = in->bar;
    p.dock = in->dock;
    p.launcher = in->launcher;
    p.launcher_windows_key = in->launcher_windows_key;
    p.clipboard = in->clipboard;
    p.screenshot = in->screenshot;
    p.files = in->files;
    p.viewers = in->viewers;
    p.bar_auto_hide = in->bar_auto_hide;
    p.week_starts_monday = in->week_starts_monday;
    p.bar_airdrop = in->bar_airdrop;
    p.bar_indicators = in->bar_indicators;
    p.dock_hide = in->dock_hide == LARES_DOCK_ALWAYS_SHOWN ? lares::DockHide::AlwaysShown
                  : in->dock_hide == LARES_DOCK_AUTO_HIDE  ? lares::DockHide::AutoHide
                                                           : lares::DockHide::WhenCovered;
    p.dock_window_counts = in->dock_window_counts;
    p.dock_middle_click_new = in->dock_middle_click_new;
    p.dock_media = in->dock_media;
    p.animations = in->animations;
    return lares::save_shell_prefs(path_in_use(), p);
}

extern "C" int lares_shell_pins_load(LaresDockPin* out, int max)
{
    lares::ShellPrefs p = load_now();
    if (!p.pins_saved) return -1;
    int n = 0;
    for (const lares::DockPin& pin : p.pins) {
        if (!out || n >= max) break;
        LaresDockPin* d = &out[n++];
        copy(d->name, LARES_PIN_NAME, pin.name);
        copy(d->app_id, LARES_PIN_APP_ID, pin.app_id);
        copy(d->path, LARES_PIN_PATH, pin.path);
        copy(d->args, LARES_PIN_ARGS, pin.args);
        copy(d->icon, LARES_PIN_PATH, pin.icon);
    }
    return n;
}

extern "C" bool lares_shell_pins_save(const LaresDockPin* pins, int count)
{
    lares::ShellPrefs p = load_now();
    p.pins_saved = true;
    p.pins.clear();
    for (int i = 0; pins && i < count && p.pins.size() < lares::kMaxDockPins; ++i) {
        lares::DockPin pin;
        pin.name = narrow(pins[i].name);
        pin.app_id = narrow(pins[i].app_id);
        pin.path = narrow(pins[i].path);
        pin.args = narrow(pins[i].args);
        pin.icon = narrow(pins[i].icon);
        if (pin.app_id.empty() && pin.path.empty()) continue;
        p.pins.push_back(pin);
    }
    return lares::save_shell_prefs(path_in_use(), p);
}

extern "C" int lares_shell_bookmarks_load(LaresBookmark* out, int max)
{
    lares::ShellPrefs p = load_now();
    int n = 0;
    for (const lares::FilesBookmark& b : p.bookmarks) {
        if (!out || n >= max) break;
        LaresBookmark* d = &out[n++];
        copy(d->name, LARES_BOOKMARK_NAME, b.name);
        copy(d->path, LARES_PIN_PATH, b.path);
    }
    return n;
}

extern "C" bool lares_shell_bookmarks_save(const LaresBookmark* marks, int count)
{
    lares::ShellPrefs p = load_now();
    p.bookmarks.clear();
    for (int i = 0; marks && i < count && p.bookmarks.size() < lares::kMaxFilesBookmarks; ++i) {
        lares::FilesBookmark b;
        b.name = narrow(marks[i].name);
        b.path = narrow(marks[i].path);
        if (!b.path.empty()) p.bookmarks.push_back(b);
    }
    return lares::save_shell_prefs(path_in_use(), p);
}

extern "C" bool lares_shell_settings_dir(wchar_t* out, size_t cch)
{
    if (!out || !cch) return false;
    std::wstring dir = lares::config_dir_path();
    return wcsncpy_s(out, cch, dir.c_str(), _TRUNCATE) == 0;
}

extern "C" bool lares_shell_settings_file(wchar_t* out, size_t cch)
{
    if (!out || !cch) return false;
    return wcsncpy_s(out, cch, path_in_use().c_str(), _TRUNCATE) == 0;
}
