// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "config.h"
#include "pad.h"

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

} // namespace lares
