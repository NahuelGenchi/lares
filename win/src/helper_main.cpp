// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "config.h"
#include "devices.h"
#include "helper.h"
#include "hw.h"
#include "pad.h"
#include "theme.h"
#include "ui.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <shellapi.h>
#include <string>
#include <vector>
#include <windows.h>

namespace lares {

static std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

static constexpr UINT_PTR kHelperTimerId = 1;
static constexpr UINT kTrayIconId = 1;

static constexpr UINT kMenuCmdRumble     = 1001;
static constexpr UINT kMenuCmdFind       = 1002;
static constexpr UINT kMenuCmdController = 1003;
static constexpr UINT kMenuCmdOpenLares  = 1004;
static constexpr UINT kMenuCmdQuit       = 1005;

class HelperApp {
public:
    HelperApp(HINSTANCE hinst, bool dry_run)
        : hinst_(hinst), dry_run_(dry_run), pads_(dry_run), devices_(dry_run)
    {
        pad_path_ = pad_ini_path(dry_run_);
        load_pad_prefs(pad_path_, prefs_);
        if (dry_run_) {
            prefs_.background = true;
            prefs_.topbar = true;
            prefs_.topbar_level = true;
        }
        pads_.apply_nicknames(prefs_);
        pad_list_ = pads_.list();
        for (const auto& p : pad_list_) {
            known_pads_[p.path] = p;
            prev_states_[p.path] = p.state;
        }
        scanned_once_ = true;
    }

    ~HelperApp()
    {
        remove_tray_icon();
        if (keeping_awake_) {
            SetThreadExecutionState(ES_CONTINUOUS);
            keeping_awake_ = false;
        }
        if (icon_) {
            DestroyIcon(icon_);
            icon_ = nullptr;
        }
    }

    bool should_stay_alive() const
    {
        return dry_run_ || padconf_needs_helper(prefs_);
    }

    bool init()
    {
        const wchar_t* cls_name = dry_run_ ? kHelperWindowClassDry : kHelperWindowClass;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HelperApp::wnd_proc;
        wc.hInstance = hinst_;
        wc.lpszClassName = cls_name;
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(
            0, cls_name, L"Lares Background Helper",
            WS_OVERLAPPEDWINDOW,
            0, 0, 0, 0,
            nullptr, nullptr, hinst_, this);
        if (!hwnd_) return false;

        icon_ = create_gamepad_tray_icon(system_prefers_dark());
        pads_.set_notify_hwnd(hwnd_);
        pads_.set_wanted(true, prefs_.keep_awake || prefs_.idle_disconnect);
        devices_.set_notify_hwnd(hwnd_);
        devices_.set_wanted(prefs_.key_light != "own", false, prefs_.key_light != "own");

        sync_tray_icon();
        sync_keyboard_live_light();
        SetTimer(hwnd_, kHelperTimerId, 2000, nullptr);
        return true;
    }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        HelperApp* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<HelperApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<HelperApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->handle_msg(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    void sync_tray_icon()
    {
        if (!hwnd_ || dry_run_ || !FindWindowW(L"Shell_TrayWnd", nullptr)) return;
        bool want = helper_should_show_tray_icon(pad_list_, prefs_);
        if (!want) {
            remove_tray_icon();
            return;
        }

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kMsgHelperTrayCallback;
        nid.hIcon = icon_;

        std::wstring tip = utf8_to_wide(helper_tooltip_text(pad_list_, prefs_));
        std::wcsncpy(nid.szTip, tip.c_str(), std::size(nid.szTip) - 1);

        if (!tray_added_) {
            tray_added_ = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
        } else {
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
    }

    void remove_tray_icon()
    {
        if (!tray_added_ || !hwnd_) return;
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        tray_added_ = false;
    }

    void show_balloon(const std::string& title, const std::string& body, DWORD info_flags)
    {
        if (dry_run_ || !hwnd_) return;
        if (!tray_added_) {
            NOTIFYICONDATAW add_nid = {};
            add_nid.cbSize = sizeof(add_nid);
            add_nid.hWnd = hwnd_;
            add_nid.uID = kTrayIconId;
            add_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            add_nid.uCallbackMessage = kMsgHelperTrayCallback;
            add_nid.hIcon = icon_;
            std::wcsncpy(add_nid.szTip, L"Lares", std::size(add_nid.szTip) - 1);
            tray_added_ = (Shell_NotifyIconW(NIM_ADD, &add_nid) != FALSE);
        }
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = info_flags;
        std::wstring tw = utf8_to_wide(title);
        std::wstring bw = utf8_to_wide(body);
        std::wcsncpy(nid.szInfoTitle, tw.c_str(), std::size(nid.szInfoTitle) - 1);
        std::wcsncpy(nid.szInfo, bw.c_str(), std::size(nid.szInfo) - 1);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void sync_keyboard_live_light()
    {
        if (dry_run_) return;
        auto devs = devices_.list();
        for (const auto& d : devs) {
            if (d.kind != DevKind::Keyboard) continue;
            KeyboardSettings want = d.keyboard;
            if (prefs_.key_light == "off") want.light = KeyLight::Off;
            else if (prefs_.key_light == "solid") want.light = KeyLight::Solid;
            else if (prefs_.key_light == "breathing") want.light = KeyLight::Breathing;
            else if (prefs_.key_light == "cycle") want.light = KeyLight::Cycle;
            else continue;
            std::memcpy(want.rgb, prefs_.key_rgb, 3);
            devices_.apply_keyboard_async(d.id, want);
        }
    }

    void open_lares_section(const char* section)
    {
        const wchar_t* main_cls = dry_run_ ? kWindowClassDry : kWindowClass;
        HWND main_hwnd = FindWindowW(main_cls, nullptr);
        if (main_hwnd) {
            COPYDATASTRUCT cds = {};
            cds.dwData = kCopyDataSection;
            cds.cbData = static_cast<DWORD>(std::strlen(section) + 1);
            cds.lpData = const_cast<char*>(section);
            SendMessageW(main_hwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(hwnd_), reinterpret_cast<LPARAM>(&cds));
            return;
        }
        wchar_t self_path[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(nullptr, self_path, MAX_PATH);
        std::wstring exe = L"lares.exe";
        if (n > 0 && n < MAX_PATH) {
            std::wstring dir(self_path);
            size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                exe = dir.substr(0, slash + 1) + L"lares.exe";
            }
        }
        SetEnvironmentVariableW(L"LARES_SECTION", utf8_to_wide(section).c_str());
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void show_tray_menu()
    {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;

        if (pad_list_.empty()) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No controller connected");
        } else {
            const PadInfo& p = pad_list_[0];
            std::string head = pad_display_name(p);
            if (p.battery.known) {
                head += " \xE2\x80\x94 " + pad_battery_text(p.battery);
            }
            head += std::string(" \xC2\xB7 ") + pad_connection_name(p.connection);
            std::wstring head_w = utf8_to_wide(head);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, head_w.c_str());
            if (p.rumble) {
                AppendMenuW(menu, MF_STRING, kMenuCmdRumble, L"Test vibration");
                if (p.connection != PadConnection::Usb) {
                    AppendMenuW(menu, MF_STRING, kMenuCmdFind,
                                p.finding ? L"Stop finding" : L"Find controller");
                }
            }
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuCmdController, L"Controller settings");
        AppendMenuW(menu, MF_STRING, kMenuCmdOpenLares, L"Open Lares");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuCmdQuit, L"Quit helper");

        POINT pt = {};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                  pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (cmd) {
        case kMenuCmdRumble:
            if (!pad_list_.empty()) {
                pads_.rumble_async(pad_list_[0].path, kPadRumbleStrength, kPadRumbleStrength, kPadRumbleMs);
            }
            break;
        case kMenuCmdFind:
            if (!pad_list_.empty()) {
                pads_.toggle_find_controller(pad_list_[0].path);
            }
            break;
        case kMenuCmdController:
            open_lares_section("controller");
            break;
        case kMenuCmdOpenLares:
            open_lares_section("overview");
            break;
        case kMenuCmdQuit:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        default:
            break;
        }
    }

    void on_pads_updated()
    {
        auto current = pads_.list();
        ULONGLONG now_ms = GetTickCount64();

        // 1. Connection notices (ported from connection_notices in src/pad/padbg.c)
        if (scanned_once_ && prefs_.notices) {
            for (const auto& kv : known_pads_) {
                bool still_here = false;
                for (const auto& c : current) {
                    if (c.path == kv.first) { still_here = true; break; }
                }
                if (!still_here) {
                    std::string title, body;
                    helper_format_connection_notice(kv.second, false, title, body);
                    show_balloon(title, body, NIIF_INFO);
                }
            }
            for (const auto& c : current) {
                if (known_pads_.find(c.path) == known_pads_.end()) {
                    std::string title, body;
                    helper_format_connection_notice(c, true, title, body);
                    show_balloon(title, body, NIIF_INFO);
                }
            }
        }

        known_pads_.clear();
        for (const auto& c : current) {
            known_pads_[c.path] = c;
        }
        scanned_once_ = true;

        // 2. Low-battery alerts (ported from battery_alerts in src/pad/padbg.c)
        for (const auto& c : current) {
            if (!c.battery.known || c.connection == PadConnection::Usb) continue;
            int stage = pad_battery_low_stage(c.battery, prefs_.low_level);
            bool charging = pad_battery_charging(c.battery);
            if (stage == 0 && (charging || pad_battery_low_stage(c.battery, prefs_.low_level + 10) == 0)) {
                told_stage_.erase(c.path);
                continue;
            }
            int prev_told = told_stage_[c.path];
            if (prefs_.alerts && !charging && stage > prev_told) {
                told_stage_[c.path] = stage;
                std::string title, body;
                helper_format_battery_alert(c, stage, title, body);
                show_balloon(title, body, NIIF_WARNING);
            }
        }

        // 3. Keep screen awake on controller activity (HOLD_SECONDS = 10s)
        if (prefs_.keep_awake) {
            for (const auto& c : current) {
                if (helper_state_has_activity(prev_states_[c.path], c.state)) {
                    last_activity_ms_ = now_ms;
                    if (!keeping_awake_) {
                        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
                        keeping_awake_ = true;
                    }
                }
                prev_states_[c.path] = c.state;
            }
        }

        pad_list_ = std::move(current);
        sync_tray_icon();
    }

    void reload_config()
    {
        load_pad_prefs(pad_path_, prefs_);
        if (dry_run_) {
            prefs_.background = true;
            prefs_.topbar = true;
            prefs_.topbar_level = true;
        }
        if (!should_stay_alive()) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        }
        pads_.apply_nicknames(prefs_);
        pads_.set_wanted(true, prefs_.keep_awake || prefs_.idle_disconnect);
        devices_.set_wanted(prefs_.key_light != "own", false, prefs_.key_light != "own");
        pad_list_ = pads_.list();
        sync_tray_icon();
        sync_keyboard_live_light();
    }

    LRESULT handle_msg(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_TIMER:
            if (wp == kHelperTimerId) {
                if (keeping_awake_ && (GetTickCount64() - last_activity_ms_ > 10000)) {
                    SetThreadExecutionState(ES_CONTINUOUS);
                    keeping_awake_ = false;
                }
                reload_config();
            }
            return 0;
        case kMsgHelperReloadConfig:
            reload_config();
            return 0;
        case kMsgPadsUpdated:
            on_pads_updated();
            return 0;
        case kMsgDevicesUpdated:
            sync_keyboard_live_light();
            return 0;
        case kMsgHelperTrayCallback:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                show_tray_menu();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kHelperTimerId);
            pads_.set_wanted(false, false);
            pads_.set_notify_hwnd(nullptr);
            devices_.set_wanted(false, false, false);
            devices_.set_notify_hwnd(nullptr);
            remove_tray_icon();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
    }

    HINSTANCE hinst_ = nullptr;
    bool dry_run_ = false;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    bool tray_added_ = false;

    std::wstring pad_path_;
    PadPrefs prefs_;
    PadManager pads_;
    DevicesManager devices_;
    std::vector<PadInfo> pad_list_;

    bool scanned_once_ = false;
    std::map<std::string, PadInfo> known_pads_;
    std::map<std::string, int> told_stage_;
    std::map<std::string, PadState> prev_states_;
    ULONGLONG last_activity_ms_ = 0;
    bool keeping_awake_ = false;
};

} // namespace lares

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR cmd_line, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t dry_buf[16] = {};
    bool dry_run = (GetEnvironmentVariableW(L"LARES_DRY_RUN", dry_buf, 16) > 0 && dry_buf[0] != L'\0');

    if (cmd_line && std::wcsstr(cmd_line, L"--status")) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            std::freopen("CONOUT$", "w", stdout);
        }
        lares::PadPrefs prefs;
        lares::load_pad_prefs(lares::pad_ini_path(dry_run), prefs);
        lares::PadManager pm(dry_run);
        pm.apply_nicknames(prefs);
        auto pads = pm.list();
        std::printf("helper-needed: %s\n", lares::padconf_needs_helper(prefs) ? "yes" : "no");
        std::printf("tray-visible: %s\n", lares::helper_should_show_tray_icon(pads, prefs) ? "yes" : "no");
        std::printf("tooltip: %s\n", lares::helper_tooltip_text(pads, prefs).c_str());
        std::printf("controllers: %zu\n", pads.size());
        return 0;
    }

    const wchar_t* cls_name = dry_run ? lares::kHelperWindowClassDry : lares::kHelperWindowClass;
    HWND existing = FindWindowW(cls_name, nullptr);
    if (existing) {
        PostMessageW(existing, lares::kMsgHelperReloadConfig, 0, 0);
        return 0;
    }

    lares::HelperApp app(hinst, dry_run);
    if (!app.should_stay_alive()) {
        return 0;
    }
    if (!app.init()) {
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
