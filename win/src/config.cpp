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
