// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace lares {

inline constexpr int kPadIdleChoices = 4;
inline constexpr int kPadIdleMinutes[kPadIdleChoices] = { 5, 10, 15, 30 };

// Mirrors src/pad/padconf.h and ~/.config/lares/pad.ini keys so Linux and
// Windows keep the same config format.
struct PadPrefs {
    int low_level = 20;
    bool alerts = true;
    bool notices = true;
    bool keep_awake = false;
    bool idle_disconnect = false;
    int idle_minutes = 15;
    bool topbar = false;
    bool topbar_level = false;
    bool topbar_hide = false;
    bool background = true;
    std::string key_light = "own";
    uint8_t key_rgb[3] = { 0xff, 0x00, 0x00 };
    std::string nickname_salt;
    std::map<std::string, std::string> nicknames; // key -> "nickname\tmodel name"
};

bool padconf_needs_helper(const PadPrefs& p);

PadPrefs parse_pad_ini(const std::string& text);
std::string serialize_pad_ini(const PadPrefs& prefs);

std::wstring config_dir_path();
std::wstring pad_ini_path(bool dry_run);
bool load_pad_prefs(const std::wstring& path, PadPrefs& out);
bool save_pad_prefs(const std::wstring& path, const PadPrefs& prefs);

} // namespace lares
