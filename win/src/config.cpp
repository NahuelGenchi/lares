// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace lares {

static std::string trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static bool parse_bool(const std::string& v, bool fallback)
{
    if (v == "true" || v == "1" || v == "TRUE" || v == "True") return true;
    if (v == "false" || v == "0" || v == "FALSE" || v == "False") return false;
    return fallback;
}

static int parse_int_clamped(const std::string& v, int fallback, int lo, int hi)
{
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (!end || end == v.c_str() || *end != '\0') return fallback;
    if (n < lo) return lo;
    if (n > hi) return hi;
    return static_cast<int>(n);
}

PadPrefs parse_pad_ini(const std::string& text)
{
    PadPrefs p;
    std::istringstream in(text);
    std::string line;
    std::string group;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            group = trim(t.substr(1, t.size() - 2));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));

        if (group == "pad") {
            if (key == "low-battery-level") p.low_level = parse_int_clamped(val, p.low_level, 5, 50);
            else if (key == "low-battery-alerts") p.alerts = parse_bool(val, p.alerts);
            else if (key == "connection-notices") p.notices = parse_bool(val, p.notices);
            else if (key == "keep-awake") p.keep_awake = parse_bool(val, p.keep_awake);
            else if (key == "idle-disconnect") p.idle_disconnect = parse_bool(val, p.idle_disconnect);
            else if (key == "idle-disconnect-minutes") p.idle_minutes = parse_int_clamped(val, p.idle_minutes, 1, 120);
            else if (key == "topbar") p.topbar = parse_bool(val, p.topbar);
            else if (key == "topbar-battery-level") p.topbar_level = parse_bool(val, p.topbar_level);
            else if (key == "topbar-hide-without-controller") p.topbar_hide = parse_bool(val, p.topbar_hide);
            else if (key == "background") p.background = parse_bool(val, p.background);
            else if (key == "nickname-salt") p.nickname_salt = val;
        } else if (group == "keyboard") {
            if (key == "light") {
                if (val == "off" || val == "solid" || val == "breathing" || val == "cycle") {
                    p.key_light = val;
                } else {
                    p.key_light = "own";
                }
            } else if (key == "colour" && val.size() == 6) {
                char* end = nullptr;
                unsigned long rgb = std::strtoul(val.c_str(), &end, 16);
                if (end && *end == '\0') {
                    p.key_rgb[0] = static_cast<uint8_t>((rgb >> 16) & 0xff);
                    p.key_rgb[1] = static_cast<uint8_t>((rgb >> 8) & 0xff);
                    p.key_rgb[2] = static_cast<uint8_t>(rgb & 0xff);
                }
            }
        } else if (group == "window") {
            if (key == "size") p.size = parse_int_clamped(val, p.size, kSizeMin, kSizeMax);
        } else if (group == "share") {
            if (key == "media-format") {
                if (val == "original") p.share_original_media = true;
                else if (val == "compatible") p.share_original_media = false;
            }
        } else if (group == "nicknames") {
            if (!key.empty() && !val.empty()) {
                // Support both literal tab and GLib keyfile \t / \\ escapes
                std::string unescaped;
                for (size_t i = 0; i < val.size(); ++i) {
                    if (val[i] == '\\' && i + 1 < val.size()) {
                        if (val[i + 1] == 't') { unescaped.push_back('\t'); ++i; continue; }
                        if (val[i + 1] == '\\') { unescaped.push_back('\\'); ++i; continue; }
                    }
                    unescaped.push_back(val[i]);
                }
                p.nicknames[key] = unescaped;
            }
        }
    }
    return p;
}

// ---------------------------------------------------------------------------
// The Windows shell (issue #141)

static const char* dock_hide_name(DockHide h)
{
    switch (h) {
    case DockHide::AlwaysShown: return "always-shown";
    case DockHide::AutoHide: return "auto-hide";
    case DockHide::WhenCovered: break;
    }
    return "when-covered";
}

ShellPrefs parse_shell_ini(const std::string& text)
{
    ShellPrefs p;
    std::istringstream in(text);
    std::string line;
    std::string group;
    DockPin* pin = nullptr;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            group = trim(t.substr(1, t.size() - 2));
            pin = nullptr;
            if (group.rfind("dock-pin", 0) == 0) {
                p.pins_saved = true;
                if (p.pins.size() < kMaxDockPins) {
                    p.pins.emplace_back();
                    pin = &p.pins.back();
                }
            } else if (group == "dock-pins") {
                // An empty list the user left on purpose.
                p.pins_saved = true;
            }
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));

        if (group == "parts") {
            if (key == "top-bar") p.bar = parse_bool(val, p.bar);
            else if (key == "dock") p.dock = parse_bool(val, p.dock);
            else if (key == "launcher") p.launcher = parse_bool(val, p.launcher);
            else if (key == "launcher-windows-key") p.launcher_windows_key = parse_bool(val, p.launcher_windows_key);
            else if (key == "clipboard-history") p.clipboard = parse_bool(val, p.clipboard);
            else if (key == "screenshot") p.screenshot = parse_bool(val, p.screenshot);
            else if (key == "files") p.files = parse_bool(val, p.files);
            else if (key == "viewers") p.viewers = parse_bool(val, p.viewers);
        } else if (group == "top-bar") {
            if (key == "auto-hide") p.bar_auto_hide = parse_bool(val, p.bar_auto_hide);
            else if (key == "week-starts") p.week_starts_monday = (val == "monday");
        } else if (group == "dock") {
            if (key == "hide") {
                if (val == "always-shown") p.dock_hide = DockHide::AlwaysShown;
                else if (val == "auto-hide") p.dock_hide = DockHide::AutoHide;
                else p.dock_hide = DockHide::WhenCovered;
            } else if (key == "window-counts") p.dock_window_counts = parse_bool(val, p.dock_window_counts);
            else if (key == "middle-click-new-window") p.dock_middle_click_new = parse_bool(val, p.dock_middle_click_new);
            else if (key == "media") p.dock_media = parse_bool(val, p.dock_media);
        } else if (group == "general") {
            if (key == "animations") p.animations = parse_bool(val, p.animations);
        } else if (pin) {
            if (key == "name") pin->name = val;
            else if (key == "app-id") pin->app_id = val;
            else if (key == "path") pin->path = val;
            else if (key == "args") pin->args = val;
            else if (key == "icon") pin->icon = val;
        }
    }
    // A pin that names nothing to start is dropped.
    p.pins.erase(std::remove_if(p.pins.begin(), p.pins.end(),
                                [](const DockPin& d) { return d.app_id.empty() && d.path.empty(); }),
                 p.pins.end());
    return p;
}

// One value on one line: line breaks would start a new key.
static std::string one_line(const std::string& v)
{
    std::string out;
    for (char c : v) out.push_back((c == '\r' || c == '\n') ? ' ' : c);
    return trim(out);
}

std::string serialize_shell_ini(const ShellPrefs& p)
{
    auto b = [](bool v) { return v ? "true" : "false"; };
    std::ostringstream out;
    out << "[parts]\n"
        << "top-bar=" << b(p.bar) << "\n"
        << "dock=" << b(p.dock) << "\n"
        << "launcher=" << b(p.launcher) << "\n"
        << "launcher-windows-key=" << b(p.launcher_windows_key) << "\n"
        << "clipboard-history=" << b(p.clipboard) << "\n"
        << "screenshot=" << b(p.screenshot) << "\n"
        << "files=" << b(p.files) << "\n"
        << "viewers=" << b(p.viewers) << "\n"
        << "\n[top-bar]\n"
        << "auto-hide=" << b(p.bar_auto_hide) << "\n"
        << "week-starts=" << (p.week_starts_monday ? "monday" : "sunday") << "\n"
        << "\n[dock]\n"
        << "hide=" << dock_hide_name(p.dock_hide) << "\n"
        << "window-counts=" << b(p.dock_window_counts) << "\n"
        << "middle-click-new-window=" << b(p.dock_middle_click_new) << "\n"
        << "media=" << b(p.dock_media) << "\n"
        << "\n[general]\n"
        << "animations=" << b(p.animations) << "\n";
    if (p.pins_saved) {
        if (p.pins.empty()) out << "\n[dock-pins]\n";
        size_t n = 0;
        for (const DockPin& pin : p.pins) {
            if (pin.app_id.empty() && pin.path.empty()) continue;
            if (++n > kMaxDockPins) break;
            out << "\n[dock-pin " << n << "]\n";
            out << "name=" << one_line(pin.name) << "\n";
            if (!pin.app_id.empty()) out << "app-id=" << one_line(pin.app_id) << "\n";
            if (!pin.path.empty()) out << "path=" << one_line(pin.path) << "\n";
            if (!pin.args.empty()) out << "args=" << one_line(pin.args) << "\n";
            if (!pin.icon.empty()) out << "icon=" << one_line(pin.icon) << "\n";
        }
    }
    return out.str();
}

std::wstring shell_ini_path(bool dry_run)
{
    return config_dir_path() + (dry_run ? L"\\shell-dry.ini" : L"\\shell.ini");
}

static bool read_whole_file(const std::wstring& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Written to a temporary file next to it and moved into place, so a crash
// leaves the old file or the new one, never half of one.
static bool replace_file(const std::wstring& path, const std::string& content)
{
#ifdef _WIN32
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out.good()) return false;
    }
    // Another Lares program may be reading the file this very moment (the
    // shell reads shell.ini when told to): try again for a little while.
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DWORD err = GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) break;
        Sleep(25);
    }
    DeleteFileW(tmp.c_str());
    return false;
#else
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
#endif
}

bool load_shell_prefs(const std::wstring& path, ShellPrefs& out)
{
    std::string text;
    if (!read_whole_file(path, text)) {
        out = ShellPrefs{};
        return false;
    }
    out = parse_shell_ini(text);
    return true;
}

bool save_shell_prefs(const std::wstring& path, const ShellPrefs& prefs)
{
    return replace_file(path, serialize_shell_ini(prefs));
}

bool padconf_needs_helper(const PadPrefs& p)
{
    if (!p.background) return false;
    return p.topbar || p.alerts || p.notices || p.keep_awake || p.idle_disconnect || (p.key_light != "own");
}

std::string serialize_pad_ini(const PadPrefs& p)
{
    std::ostringstream out;
    out << "[pad]\n"
        << "low-battery-level=" << std::clamp(p.low_level, 5, 50) << "\n"
        << "low-battery-alerts=" << (p.alerts ? "true" : "false") << "\n"
        << "connection-notices=" << (p.notices ? "true" : "false") << "\n"
        << "keep-awake=" << (p.keep_awake ? "true" : "false") << "\n"
        << "idle-disconnect=" << (p.idle_disconnect ? "true" : "false") << "\n"
        << "idle-disconnect-minutes=" << std::clamp(p.idle_minutes, 1, 120) << "\n"
        << "topbar=" << (p.topbar ? "true" : "false") << "\n"
        << "topbar-battery-level=" << (p.topbar_level ? "true" : "false") << "\n"
        << "topbar-hide-without-controller=" << (p.topbar_hide ? "true" : "false") << "\n"
        << "background=" << (p.background ? "true" : "false") << "\n";
    if (!p.nickname_salt.empty()) {
        out << "nickname-salt=" << p.nickname_salt << "\n";
    }

    if (p.key_light != "own") {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "%02x%02x%02x", p.key_rgb[0], p.key_rgb[1], p.key_rgb[2]);
        out << "\n[keyboard]\n"
            << "light=" << p.key_light << "\n"
            << "colour=" << hex << "\n";
    }

    int size = std::clamp(p.size, kSizeMin, kSizeMax);
    if (size != 100) {
        out << "\n[window]\n"
            << "size=" << size << "\n";
    }

    if (p.share_original_media) {
        out << "\n[share]\n"
            << "media-format=original\n";
    }

    if (!p.nicknames.empty()) {
        out << "\n[nicknames]\n";
        for (const auto& kv : p.nicknames) {
            std::string escaped;
            for (char c : kv.second) {
                if (c == '\\') escaped += "\\\\";
                else if (c == '\t') escaped += "\\t";
                else escaped.push_back(c);
            }
            out << kv.first << "=" << escaped << "\n";
        }
    }
    return out.str();
}

std::wstring config_dir_path()
{
#ifdef _WIN32
    // Tests keep their settings in a folder of their own.
    wchar_t test_dir[MAX_PATH];
    DWORD td = GetEnvironmentVariableW(L"LARES_CONFIG_DIR", test_dir, MAX_PATH);
    if (td > 0 && td < MAX_PATH) return std::wstring(test_dir);
    PWSTR appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
        std::wstring dir(appdata);
        CoTaskMemFree(appdata);
        dir += L"\\lares";
        return dir;
    }
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::wstring(buf) + L"\\lares";
    }
#endif
    return L".";
}

std::wstring pad_ini_path(bool dry_run)
{
    std::wstring dir = config_dir_path();
    return dir + (dry_run ? L"\\pad-dry.ini" : L"\\pad.ini");
}

bool load_pad_prefs(const std::wstring& path, PadPrefs& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        out = PadPrefs{};
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = parse_pad_ini(ss.str());
    return true;
}

bool save_pad_prefs(const std::wstring& path, const PadPrefs& prefs)
{
#ifdef _WIN32
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring dir = path.substr(0, slash);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        std::string content = serialize_pad_ini(prefs);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out.good()) return false;
    }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(tmp.c_str());
    return false;
#else
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    std::string content = serialize_pad_ini(prefs);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
#endif
}

} // namespace lares
