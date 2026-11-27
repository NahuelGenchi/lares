// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "config.h"
#include "pad.h"

struct lares_journal; // win/journal/journal.h

namespace lares {

inline constexpr wchar_t kHelperWindowClass[] = L"LaresPadHelperClass";
inline constexpr wchar_t kHelperWindowClassDry[] = L"LaresPadHelperClassDry";
inline constexpr UINT kMsgHelperReloadConfig = WM_APP + 40;
inline constexpr UINT kMsgHelperTrayCallback = WM_APP + 41;

bool helper_should_show_tray_icon(const std::vector<PadInfo>& pads, const PadPrefs& prefs);
std::string helper_tooltip_text(const std::vector<PadInfo>& pads, const PadPrefs& prefs);

void helper_format_connection_notice(const PadInfo& pad, bool connected,
                                     std::string& out_title, std::string& out_body);
void helper_format_battery_alert(const PadInfo& pad, int stage,
                                 std::string& out_title, std::string& out_body);
bool helper_state_has_activity(const PadState& prev, const PadState& cur);

HICON create_gamepad_tray_icon(bool light_icon);
void pad_service_sync(const PadPrefs& prefs, bool dry_run);

// The helper's start-up entry, a Windows setting, goes through the change
// journal (win/journal/journal.h) under this feature name.
inline constexpr char kPadHelperFeature[] = "pad-helper";
inline constexpr wchar_t kPadAutostartTarget[] = L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run|LaresPad";

// Sets the start-up entry `target` to `command` when `wanted`, removes it
// otherwise, through `journal`. An entry that is already there with nothing
// in the journal for it was made by a Lares from before the journal: it is
// written down as Lares' own first, so that an undo removes it rather than
// putting it back. Returns false when the journal could not be written, and
// then nothing is changed.
bool pad_autostart_apply(::lares_journal* journal, const wchar_t* target, bool wanted, const std::wstring& command);

} // namespace lares
