// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar's device indicators (issue #157): indicators.h. */
#include "indicators.h"

#include <fstream>
#include <sstream>
#include <string>

#include <windows.h>
#include <xinput.h>

#include "config.h"
#include "devices.h"
#include "hid_win.h"
#include "remote.h"
#include "shell_prefs.h"

using namespace lares;

namespace {

/* XInput: the slots that answer, and the lowest battery among wireless ones.
 * Loaded for the scan and let go after it, as Lares' controller page loads
 * it (pad.cpp), so the shell carries none of it between scans. */
void scan_pads(LaresIndicators* out)
{
    out->controller_battery = LARES_PAD_BATTERY_UNKNOWN;
    HMODULE dll = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return;
    using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    using GetBattery = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
    auto get_state = reinterpret_cast<GetState>(reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
    auto get_battery =
        reinterpret_cast<GetBattery>(reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetBatteryInformation")));
    for (DWORD i = 0; get_state && i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE st = {};
        if (get_state(i, &st) != ERROR_SUCCESS) continue;
        out->controller = true;
        XINPUT_BATTERY_INFORMATION bi = {};
        if (!get_battery || get_battery(i, BATTERY_DEVTYPE_GAMEPAD, &bi) != ERROR_SUCCESS) continue;
        if (bi.BatteryType != BATTERY_TYPE_ALKALINE && bi.BatteryType != BATTERY_TYPE_NIMH) continue;
        int level = bi.BatteryLevel == BATTERY_LEVEL_EMPTY  ? LARES_PAD_BATTERY_EMPTY
                  : bi.BatteryLevel == BATTERY_LEVEL_LOW    ? LARES_PAD_BATTERY_LOW
                  : bi.BatteryLevel == BATTERY_LEVEL_MEDIUM ? LARES_PAD_BATTERY_MEDIUM
                                                            : LARES_PAD_BATTERY_FULL;
        if (out->controller_battery == LARES_PAD_BATTERY_UNKNOWN || level < out->controller_battery)
            out->controller_battery = level;
    }
    FreeLibrary(dll);
}

/* Remote mode as Lares' Remote page saved it in lares.ini. */
bool remote_mode_on(bool dry_run)
{
    std::wstring path = config_dir_path() + (dry_run ? L"\\lares-dry-run.ini" : L"\\lares.ini");
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    RemoteConfig cfg = parse_lares_ini(ss.str());
    return cfg.mode_known && cfg.mode == RemoteMode::Remote;
}

} // namespace

extern "C" void lares_indicators_scan(LaresIndicators* out)
{
    if (!out) return;
    *out = LaresIndicators{};
    out->controller_battery = LARES_PAD_BATTERY_UNKNOWN;
    if (lares_shell_dry_run()) {
        out->controller = out->headphones = out->keyboard = out->mouse = true;
        out->controller_battery = LARES_PAD_BATTERY_MEDIUM;
        out->remote = remote_mode_on(true);
        return;
    }
    scan_pads(out);
    out->headphones = bluetooth_audio_connected();
    HidFound f = hid_find(hid_collections());
    out->keyboard = !f.hidpp_short.empty() || !f.keyboard_lamps.empty();
    out->mouse = !f.hyperx_receiver.empty() || !f.hyperx_cable.empty();
    out->remote = remote_mode_on(false);
}
