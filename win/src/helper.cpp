// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "helper.h"
extern "C" {
#include "art.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lares {

bool helper_should_show_tray_icon(const std::vector<PadInfo>& pads, const PadPrefs& prefs)
{
    if (!prefs.background || !prefs.topbar) return false;
    if (prefs.topbar_hide && pads.empty()) return false;
    return true;
}

std::string helper_tooltip_text(const std::vector<PadInfo>& pads, const PadPrefs& prefs)
{
    if (pads.empty()) {
        return "Lares \xC2\xB7 No controller connected";
    }
    if (pads.size() == 1) {
        const PadInfo& p = pads[0];
        std::string name = pad_display_name(p);
        if (prefs.topbar_level && p.battery.known) {
            return "Lares \xC2\xB7 " + name + " (" + pad_battery_charge(p.battery) + ")";
        }
        return "Lares \xC2\xB7 " + name;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Lares \xC2\xB7 %zu controllers connected", pads.size());
    return buf;
}

void helper_format_connection_notice(const PadInfo& pad, bool connected,
                                     std::string& out_title, std::string& out_body)
{
    out_title = pad_display_name(pad) + (connected ? " connected" : " disconnected");
    if (connected && pad.battery.known && pad.connection != PadConnection::Usb) {
        out_body = std::string(pad_connection_name(pad.connection)) + " \xC2\xB7 " + pad_battery_text(pad.battery);
    } else {
        out_body = pad_connection_name(pad.connection);
    }
}

void helper_format_battery_alert(const PadInfo& pad, int stage,
                                 std::string& out_title, std::string& out_body)
{
    out_title = pad_display_name(pad) + " battery " + (stage > 1 ? "very low" : "low");
    const char* level = pad_battery_level_name(pad.battery);
    std::string left;
    if (level) {
        std::string low_str(level);
        for (char& c : low_str) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        left = "Its charge is " + low_str + ".";
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d%% left.", static_cast<int>(std::lround(pad.battery.percentage)));
        left = buf;
    }
    out_body = left + " " + (stage > 1
        ? "Charge it or change its batteries before it switches off."
        : "Charge it or change its batteries soon.");
}

bool helper_state_has_activity(const PadState& prev, const PadState& cur)
{
    if (cur.buttons != prev.buttons && cur.buttons != 0) return true;
    if (std::fabs(cur.lx) >= 0.25f || std::fabs(cur.ly) >= 0.25f ||
        std::fabs(cur.rx) >= 0.25f || std::fabs(cur.ry) >= 0.25f ||
        cur.lt >= 0.25f || cur.rt >= 0.25f) {
        return true;
    }
    return false;
}

HICON create_gamepad_tray_icon(bool light_icon)
{
    constexpr int kSize = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = kSize;
    bi.bV5Height = -kSize; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color_bmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color_bmp || !bits) {
        if (color_bmp) DeleteObject(color_bmp);
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    auto* px = static_cast<uint32_t*>(bits);
    std::memset(px, 0, kSize * kSize * sizeof(uint32_t));

    // ART_GAMEPAD is 12x9; draw at 2x scale (24x18) centered in 32x32
    uint32_t solid = light_icon ? 0xFFF3EFE6u : 0xFF1F1C18u;
    uint32_t faint = light_icon ? 0x66F3EFE6u : 0x661F1C18u;
    int ox = 4;
    int oy = 7;

    for (int r = 0; ART_GAMEPAD[r]; ++r) {
        const char* row = ART_GAMEPAD[r];
        for (int c = 0; row[c]; ++c) {
            char ch = row[c];
            if (ch != '#' && ch != '+') continue;
            uint32_t col = (ch == '#') ? solid : faint;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int x = ox + c * 2 + dx;
                    int y = oy + r * 2 + dy;
                    if (x >= 0 && x < kSize && y >= 0 && y < kSize) {
                        px[y * kSize + x] = col;
                    }
                }
            }
        }
    }

    HBITMAP mask_bmp = CreateBitmap(kSize, kSize, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask_bmp;
    ii.hbmColor = color_bmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask_bmp);
    DeleteObject(color_bmp);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

static std::wstring helper_exe_path()
{
    wchar_t self_path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"lares-pad.exe";
    std::wstring dir(self_path);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        return dir.substr(0, slash + 1) + L"lares-pad.exe";
    }
    return L"lares-pad.exe";
}

void pad_service_sync(const PadPrefs& prefs, bool dry_run)
{
    if (dry_run) return;

    bool wanted = padconf_needs_helper(prefs);
    std::wstring exe = helper_exe_path();

    // 1. Update HKCU\Software\Microsoft\Windows\CurrentVersion\Run\LaresPad
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hkey) == ERROR_SUCCESS) {
        if (wanted && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring cmd = L"\"" + exe + L"\"";
            RegSetValueExW(hkey, L"LaresPad", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(cmd.c_str()),
                           static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hkey, L"LaresPad");
        }
        RegCloseKey(hkey);
    }

    // 2. Notify, spawn, or stop running lares-pad.exe instance
    HWND existing = FindWindowW(kHelperWindowClass, nullptr);
    if (wanted) {
        if (existing) {
            PostMessageW(existing, kMsgHelperReloadConfig, 0, 0);
        } else if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            std::wstring cmd = L"\"" + exe + L"\"";
            std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
            mutable_cmd.push_back(L'\0');
            if (CreateProcessW(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    } else if (existing) {
        PostMessageW(existing, WM_CLOSE, 0, 0);
    }
}

} // namespace lares
