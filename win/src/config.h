// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lares {

inline constexpr int kPadIdleChoices = 4;
inline constexpr int kPadIdleMinutes[kPadIdleChoices] = { 5, 10, 15, 30 };

// Settings > Accessibility > Size: how much larger or smaller than Windows'
// own display scale the window is drawn, in percent.
inline constexpr int kSizeChoices = 7;
inline constexpr int kSizePercents[kSizeChoices] = { 90, 100, 110, 120, 130, 140, 150 };
inline constexpr int kSizeMin = 90;
inline constexpr int kSizeMax = 150;

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
    int size = 100; // [window] size, Windows only
    bool share_original_media = false; // [share] media-format=original (HEIC/HEVC vs JPEG/H.264)
    std::string key_light = "own";
    uint8_t key_rgb[3] = { 0xff, 0x00, 0x00 };
    std::string nickname_salt;
    std::map<std::string, std::string> nicknames; // key -> "nickname\tmodel name"
};

// The Windows shell's settings (issue #141), in %APPDATA%\lares\shell.ini
// (shell-dry.ini in a dry run). Every part is off until it is turned on in
// Settings; a missing or empty file means stock Windows.
enum class DockHide { AlwaysShown, AutoHide, WhenCovered };

// One app pinned to the dock. app_id is a Store app's AppUserModelID; path
// is a program file, args its command line. Either may be empty.
struct DockPin {
    std::string name;
    std::string app_id;
    std::string path;
    std::string args;
    std::string icon; // a picture file, absolute or in the settings folder
};

inline constexpr size_t kMaxDockPins = 64;

// A folder bookmarked in Files' sidebar.
struct FilesBookmark {
    std::string name;
    std::string path;
};

inline constexpr size_t kMaxFilesBookmarks = 16;

struct ShellPrefs {
    // [parts]
    bool bar = false;
    bool dock = false;
    bool launcher = false;
    bool launcher_windows_key = false; // a lone tap of the Windows key opens the launcher
    bool clipboard = false;
    bool screenshot = false;
    bool files = false;
    bool viewers = false;
    // [top-bar]
    bool bar_auto_hide = false;
    bool week_starts_monday = false;
    // [dock]
    DockHide dock_hide = DockHide::WhenCovered;
    bool dock_window_counts = true;
    bool dock_middle_click_new = true;
    bool dock_media = true;
    // [general]
    bool animations = true;
    // [dock-pin N]: when none were ever saved, the shell picks its own first set.
    bool pins_saved = false;
    std::vector<DockPin> pins;
    // [files-bookmark N]
    std::vector<FilesBookmark> bookmarks;

    bool any_part() const
    {
        return bar || dock || launcher || clipboard || screenshot || files || viewers;
    }
};

ShellPrefs parse_shell_ini(const std::string& text);
std::string serialize_shell_ini(const ShellPrefs& prefs);
std::wstring shell_ini_path(bool dry_run);
bool load_shell_prefs(const std::wstring& path, ShellPrefs& out);
bool save_shell_prefs(const std::wstring& path, const ShellPrefs& prefs);

bool padconf_needs_helper(const PadPrefs& p);

PadPrefs parse_pad_ini(const std::string& text);
std::string serialize_pad_ini(const PadPrefs& prefs);

std::wstring config_dir_path();
std::wstring pad_ini_path(bool dry_run);
bool load_pad_prefs(const std::wstring& path, PadPrefs& out);
bool save_pad_prefs(const std::wstring& path, const PadPrefs& prefs);

} // namespace lares
