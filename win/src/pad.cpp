// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pad.h"
#include "../res/resource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace lares {

static bool decode_map_char(char c, uint8_t* cell)
{
    struct LegendEntry {
        char c;
        PadCellKind kind;
        PadButton button;
    };
    static constexpr LegendEntry kLegend[] = {
        { '.', PAD_CELL_EMPTY, PB_A },
        { '#', PAD_CELL_LINE, PB_A },
        { '+', PAD_CELL_FAINT, PB_A },
        { '=', PAD_CELL_GAUGE, PB_A },
        { 'a', PAD_CELL_AREA, PB_A },
        { 'b', PAD_CELL_AREA, PB_B },
        { 'x', PAD_CELL_AREA, PB_X },
        { 'y', PAD_CELL_AREA, PB_Y },
        { 'v', PAD_CELL_AREA, PB_VIEW },
        { 'm', PAD_CELL_AREA, PB_MENU },
        { 's', PAD_CELL_AREA, PB_SHARE },
        { 'A', PAD_CELL_GLYPH, PB_A },
        { 'B', PAD_CELL_GLYPH, PB_B },
        { 'X', PAD_CELL_GLYPH, PB_X },
        { 'Y', PAD_CELL_GLYPH, PB_Y },
        { 'V', PAD_CELL_GLYPH, PB_VIEW },
        { 'M', PAD_CELL_GLYPH, PB_MENU },
        { 'S', PAD_CELL_GLYPH, PB_SHARE },
        { 'H', PAD_CELL_GLYPH, PB_XBOX },
        { '[', PAD_CELL_AREA, PB_LB },
        { ']', PAD_CELL_AREA, PB_RB },
        { '(', PAD_CELL_AREA, PB_LT },
        { ')', PAD_CELL_AREA, PB_RT },
        { 'u', PAD_CELL_AREA, PB_UP },
        { 'd', PAD_CELL_AREA, PB_DOWN },
        { 'l', PAD_CELL_AREA, PB_LEFT },
        { 'r', PAD_CELL_AREA, PB_RIGHT },
        { '<', PAD_CELL_CAP_LINE, PB_LS },
        { '>', PAD_CELL_CAP_LINE, PB_RS },
        { '{', PAD_CELL_CAP_AREA, PB_LS },
        { '}', PAD_CELL_CAP_AREA, PB_RS },
    };
    for (const auto& entry : kLegend) {
        if (entry.c == c) {
            *cell = make_pad_cell(entry.kind, entry.button);
            return true;
        }
    }
    return false;
}

bool pad_map_parse(const char* text, size_t length, PadMap& out, std::string* err)
{
    out = PadMap{};
    if (!text || length == 0) {
        if (err) *err = "the map has no rows";
        return false;
    }

    int width = 0;
    int height = 0;
    std::vector<uint8_t> cells;
    cells.reserve(216 * 162);

    const char* p = text;
    const char* end = text + length;
    while (p < end) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
        if (!eol) eol = end;
        int len = static_cast<int>(eol - p);
        if (len > 0 && p[len - 1] == '\r') --len;
        if (len > 0 && p[0] != ';') {
            if (width != 0 && len != width) {
                if (err) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "row %d is %d pixels long, not %d", height + 1, len, width);
                    *err = buf;
                }
                return false;
            }
            width = len;
            for (int i = 0; i < len; ++i) {
                uint8_t cell = 0;
                if (!decode_map_char(p[i], &cell)) {
                    if (err) {
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "row %d has '%c', which the legend doesn't know", height + 1, p[i]);
                        *err = buf;
                    }
                    return false;
                }
                cells.push_back(cell);
            }
            ++height;
        }
        p = (eol < end) ? (eol + 1) : end;
    }

    if (height == 0) {
        if (err) *err = "the map has no rows";
        return false;
    }

    out.width = width;
    out.height = height;
    out.cells = std::move(cells);
    for (int g = 0; g < 2; ++g) {
        out.gauges[g].x0 = out.width;
        out.gauges[g].x1 = -1;
    }
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            uint8_t cell = out.at(x, y);
            PadButton b = pad_cell_button(cell);
            if (pad_cell_kind(cell) != PAD_CELL_AREA || (b != PB_LT && b != PB_RT)) continue;
            int g = (b == PB_RT) ? 1 : 0;
            out.gauges[g].x0 = std::min(out.gauges[g].x0, x);
            out.gauges[g].x1 = std::max(out.gauges[g].x1, x);
        }
    }
    return true;
}

bool pad_map_load_resource(HINSTANCE hinst, PadMap& out)
{
    HRSRC res = FindResourceW(hinst, MAKEINTRESOURCEW(IDR_PAD_MAP), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(hinst, res);
    if (!mem) return false;
    DWORD size = SizeofResource(hinst, res);
    const char* data = static_cast<const char*>(LockResource(mem));
    if (!data || size == 0) return false;
    return pad_map_parse(data, static_cast<size_t>(size), out, nullptr);
}

PadPaint pad_cell_paint(uint8_t cell, uint32_t held, uint32_t seen)
{
    uint32_t bit = pb_bit(pad_cell_button(cell));
    switch (pad_cell_kind(cell)) {
    case PAD_CELL_LINE:
    case PAD_CELL_GAUGE:
    case PAD_CELL_CAP_LINE:
        return PadPaint::Line;
    case PAD_CELL_FAINT:
        return PadPaint::Faint;
    case PAD_CELL_AREA:
    case PAD_CELL_CAP_AREA:
        if (held & bit) return PadPaint::Lit;
        return (seen & bit) ? PadPaint::Seen : PadPaint::None;
    case PAD_CELL_GLYPH:
        if (pad_cell_button(cell) == PB_XBOX) {
            if (held & bit) return PadPaint::Lit;
            return (seen & bit) ? PadPaint::Seen : PadPaint::Line;
        }
        return (held & bit) ? PadPaint::None : PadPaint::Line;
    default:
        return PadPaint::None;
    }
}

static void cap_offset(float dx, float dy, int out[2])
{
    double reach = std::hypot(static_cast<double>(dx), static_cast<double>(dy));
    if (reach > 1.0) {
        dx = static_cast<float>(dx / reach);
        dy = static_cast<float>(dy / reach);
    }
    out[0] = static_cast<int>(std::lround(dx * kPadMapStickTravel));
    out[1] = static_cast<int>(std::lround(dy * kPadMapStickTravel));
}

PadLook pad_compute_look(const PadMap& map, const PadState& state, uint32_t seen, bool placeholder)
{
    PadLook l;
    if (placeholder) return l;
    l.held = state.buttons & ~(pb_bit(PB_LT) | pb_bit(PB_RT));
    l.seen = seen;
    cap_offset(state.lx, state.ly, l.caps[0]);
    cap_offset(state.rx, state.ry, l.caps[1]);
    float pull[2] = { state.lt, state.rt };
    for (int g = 0; g < 2; ++g) {
        int span = map.gauges[g].x1 - map.gauges[g].x0 + 1;
        if (span > 0) {
            float cl = std::clamp(pull[g], 0.0f, 1.0f);
            l.pulled[g] = static_cast<int>(std::lround(cl * span));
        }
    }
    return l;
}

bool pad_gauge_lit(const PadMap& map, const PadLook& look, PadButton b, int x)
{
    int g = (b == PB_RT) ? 1 : 0;
    if (g == 1) return x > map.gauges[g].x1 - look.pulled[g];
    return x < map.gauges[g].x0 + look.pulled[g];
}

void stick_feed(StickTracker& s, double x, double y, uint64_t now_ms)
{
    s.x = x;
    s.y = y;
    if (std::hypot(x, y) > kStickRestReach) {
        s.anchored = false;
        return;
    }
    if (!s.anchored || std::hypot(x - s.ax, y - s.ay) > kStickRestStill) {
        s.ax = x;
        s.ay = y;
        s.since_ms = now_ms;
        s.anchored = true;
    } else if (now_ms >= s.since_ms && (now_ms - s.since_ms) >= kStickRestMs) {
        s.rx = x;
        s.ry = y;
        s.has_rest = true;
    }
}

bool stick_drifting(const StickTracker& s)
{
    return s.has_rest && std::hypot(s.rx, s.ry) > kStickDeadZone;
}

const char* pad_battery_level_name(const PadBattery& b)
{
    switch (b.level) {
    case 3:  return "Low";
    case 4:  return "Critical";
    case 6:  return "Normal";
    case 7:  return "High";
    case 8:  return "Full";
    default: return nullptr;
    }
}

bool pad_battery_charging(const PadBattery& b)
{
    return b.state == 1 || b.state == 4;
}

int pad_battery_low_stage(const PadBattery& b, int low_level)
{
    if (pad_battery_level_name(b)) {
        return b.level == 3 ? 1 : (b.level == 4 ? 2 : 0);
    }
    if (b.percentage <= std::min(kPadVeryLow, low_level)) return 2;
    return (b.percentage <= low_level) ? 1 : 0;
}

std::string pad_battery_charge(const PadBattery& b)
{
    const char* name = pad_battery_level_name(b);
    if (name) return name;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(b.percentage)));
    return buf;
}

std::string pad_battery_text(const PadBattery& b)
{
    std::string charge = pad_battery_charge(b);
    if (b.state == 1) return charge + " \xC2\xB7 Charging";
    if (b.state == 4) return charge + " \xC2\xB7 Charged";
    return charge;
}

double pad_battery_fraction(const PadBattery& b)
{
    switch (b.level) {
    case 3:  return 0.10;
    case 4:  return 0.05;
    case 6:  return 0.55;
    case 7:  return 0.80;
    case 8:  return 1.00;
    default: return std::clamp(b.percentage / 100.0, 0.0, 1.0);
    }
}

const char* pad_connection_name(PadConnection c)
{
    return (c == PadConnection::Bluetooth) ? "Bluetooth"
         : (c == PadConnection::Adapter)   ? "Xbox adapter"
                                           : "Wired (USB)";
}

std::string pad_display_name(const PadInfo& pad)
{
    return !pad.nickname.empty() ? pad.nickname : pad.name;
}

std::string pad_clean_nickname(const std::string& text)
{
    std::string out;
    bool in_space = false;
    size_t codepoints = 0;

    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t char_len = 1;
        if ((c & 0x80) == 0) char_len = 1;
        else if ((c & 0xE0) == 0xC0) char_len = 2;
        else if ((c & 0xF0) == 0xE0) char_len = 3;
        else if ((c & 0xF8) == 0xF0) char_len = 4;
        if (i + char_len > text.size()) char_len = 1;

        bool is_ws = (char_len == 1 && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'));
        if (is_ws) {
            in_space = true;
            i += char_len;
            continue;
        }
        if (in_space && !out.empty()) {
            if (codepoints + 1 > kPadNicknameLength) break;
            out.push_back(' ');
            ++codepoints;
        }
        in_space = false;
        if (codepoints + 1 > kPadNicknameLength) break;
        out.append(text, i, char_len);
        ++codepoints;
        i += char_len;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string pad_nickname_key(const std::string& identity_id, const std::string& salt)
{
    if (identity_id.empty() || salt.empty()) return "";
    // Keyed FNV-1a 256-bit mixing (4 x 64-bit lanes) so pad.ini never stores raw identifiers
    uint64_t h[4] = {
        0xcbf29ce484222325ULL,
        0x84222325cbf29ce4ULL,
        0x9e3779b97f4a7c15ULL,
        0x6a09e667bb67ae85ULL
    };
    auto feed = [&](const std::string& s, uint64_t tag) {
        for (int lane = 0; lane < 4; ++lane) {
            h[lane] ^= tag + static_cast<uint64_t>(lane) * 0x100000001b3ULL;
            for (unsigned char c : s) {
                h[lane] ^= static_cast<uint64_t>(c);
                h[lane] *= 0x100000001b3ULL;
                h[lane] ^= (h[(lane + 1) & 3] >> 29);
            }
        }
    };
    feed(salt, 0xA5A5A5A5A5A5A5A5ULL);
    feed(identity_id, 0x5A5A5A5A5A5A5A5AULL);

    char hex[65];
    std::snprintf(hex, sizeof(hex), "%016llx%016llx%016llx%016llx",
                  static_cast<unsigned long long>(h[0]),
                  static_cast<unsigned long long>(h[1]),
                  static_cast<unsigned long long>(h[2]),
                  static_cast<unsigned long long>(h[3]));
    return hex;
}

std::string pad_model_for_pid(uint16_t pid)
{
    switch (pid) {
    case 0x02d1: return "1537";
    case 0x02dd: return "1537 / 1697";
    case 0x02e3: return "1698";
    case 0x02ea:
    case 0x02e0:
    case 0x02fd:
    case 0x0b20: return "1708";
    case 0x0b00:
    case 0x0b05:
    case 0x0b22: return "1797";
    case 0x0b0a:
    case 0x0b0c:
    case 0x0b21: return "1836";
    case 0x0b12:
    case 0x0b13: return "1914";
    default:     return "";
    }
}

// ---------------------------------------------------------------------------
// Dynamic XInput bindings (xinput1_4.dll / xinput9_1_0.dll)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct XInputGamepadRaw {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XInputStateRaw {
    DWORD dwPacketNumber;
    XInputGamepadRaw Gamepad;
};

struct XInputVibrationRaw {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};

struct XInputBatteryInfoRaw {
    BYTE BatteryType;
    BYTE BatteryLevel;
};
#pragma pack(pop)

using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XInputStateRaw*);
using PFN_XInputSetState = DWORD(WINAPI*)(DWORD, XInputVibrationRaw*);
using PFN_XInputGetBatteryInformation = DWORD(WINAPI*)(DWORD, BYTE, XInputBatteryInfoRaw*);

struct XInputApi {
    HMODULE dll = nullptr;
    PFN_XInputGetState get_state = nullptr;
    PFN_XInputGetState get_state_ex = nullptr;
    PFN_XInputSetState set_state = nullptr;
    PFN_XInputGetBatteryInformation get_battery = nullptr;

    void ensure_loaded()
    {
        if (dll) return;
        dll = LoadLibraryW(L"xinput1_4.dll");
        if (!dll) dll = LoadLibraryW(L"xinput9_1_0.dll");
        if (!dll) return;
        get_state = reinterpret_cast<PFN_XInputGetState>(
            reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
        // Ordinal #100 in xinput1_4.dll is XInputGetStateEx (includes Guide button 0x0400)
        get_state_ex = reinterpret_cast<PFN_XInputGetState>(
            reinterpret_cast<void*>(GetProcAddress(dll, MAKEINTRESOURCEA(100))));
        if (!get_state_ex) get_state_ex = get_state;
        set_state = reinterpret_cast<PFN_XInputSetState>(
            reinterpret_cast<void*>(GetProcAddress(dll, "XInputSetState")));
        get_battery = reinterpret_cast<PFN_XInputGetBatteryInformation>(
            reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetBatteryInformation")));
    }
};

static XInputApi g_xinput;

static float normalize_axis(SHORT v)
{
    if (v >= 0) return std::clamp(static_cast<float>(v) / 32767.0f, 0.0f, 1.0f);
    return std::clamp(static_cast<float>(v) / 32768.0f, -1.0f, 0.0f);
}

static PadState translate_xinput_state(const XInputGamepadRaw& gp)
{
    PadState s;
    WORD b = gp.wButtons;
    if (b & 0x1000) s.buttons |= pb_bit(PB_A);
    if (b & 0x2000) s.buttons |= pb_bit(PB_B);
    if (b & 0x4000) s.buttons |= pb_bit(PB_X);
    if (b & 0x8000) s.buttons |= pb_bit(PB_Y);
    if (b & 0x0100) s.buttons |= pb_bit(PB_LB);
    if (b & 0x0200) s.buttons |= pb_bit(PB_RB);
    if (b & 0x0020) s.buttons |= pb_bit(PB_VIEW);
    if (b & 0x0010) s.buttons |= pb_bit(PB_MENU);
    if (b & 0x0400) s.buttons |= pb_bit(PB_XBOX);
    if (b & 0x0040) s.buttons |= pb_bit(PB_LS);
    if (b & 0x0080) s.buttons |= pb_bit(PB_RS);
    if (b & 0x0001) s.buttons |= pb_bit(PB_UP);
    if (b & 0x0002) s.buttons |= pb_bit(PB_DOWN);
    if (b & 0x0004) s.buttons |= pb_bit(PB_LEFT);
    if (b & 0x0008) s.buttons |= pb_bit(PB_RIGHT);

    s.lt = std::clamp(static_cast<float>(gp.bLeftTrigger) / 255.0f, 0.0f, 1.0f);
    s.rt = std::clamp(static_cast<float>(gp.bRightTrigger) / 255.0f, 0.0f, 1.0f);
    if (s.lt >= kTriggerPullThreshold) s.buttons |= pb_bit(PB_LT);
    if (s.rt >= kTriggerPullThreshold) s.buttons |= pb_bit(PB_RT);

    s.lx = normalize_axis(gp.sThumbLX);
    // XInput +Y is up; PadState +Y is down (matching Linux evdev ABS_Y)
    s.ly = -normalize_axis(gp.sThumbLY);
    s.rx = normalize_axis(gp.sThumbRX);
    s.ry = -normalize_axis(gp.sThumbRY);
    return s;
}

static uint32_t standard_xbox_checklist()
{
    uint32_t bits = 0;
    for (int b = 0; b < PB_COUNT; ++b) {
        if (b == PB_SHARE) continue; // XInput does not expose Share separately
        bits |= pb_bit(static_cast<PadButton>(b));
    }
    return bits;
}

PadManager::PadManager(bool dry_run)
    : dry_run_(dry_run)
{
    rumble_cancel_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (dry_run_) {
        PadInfo p;
        p.path = "xinput-0";
        p.xinput_slot = 0;
        p.name = "Xbox Wireless Controller";
        p.connection = PadConnection::Bluetooth;
        p.model = "1914";
        p.firmware = "5.23";
        p.driver = "xinput1_4";
        p.event_node = "XInput slot 1";
        p.identity_id = "address sample-dry-run-pad";
        p.rumble = true;
        p.checklist = standard_xbox_checklist();
        p.since = static_cast<int64_t>(std::time(nullptr)) - 1200;
        p.battery.known = true;
        p.battery.percentage = 85.0;
        p.battery.state = 2; // discharging
        p.state.buttons = pb_bit(PB_A) | pb_bit(PB_RB);
        p.state.lx = 0.25f;
        p.state.ly = -0.15f;
        p.state.lt = 0.45f;
        p.state.rt = 0.80f;
        pads_.push_back(p);
    } else {
        g_xinput.ensure_loaded();
        worker_ = std::thread(&PadManager::worker_loop, this);
    }
}

void PadManager::stop_rumble_thread()
{
    if (rumble_cancel_event_) SetEvent(rumble_cancel_event_);
    if (rumble_thread_.joinable()) rumble_thread_.join();
    if (rumble_cancel_event_) ResetEvent(rumble_cancel_event_);
}

PadManager::~PadManager()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
        notify_hwnd_ = nullptr;
    }
    if (wake_) SetEvent(wake_);
    stop_rumble_thread();
    if (worker_.joinable()) worker_.join();
    if (rumble_cancel_event_) {
        CloseHandle(rumble_cancel_event_);
        rumble_cancel_event_ = nullptr;
    }
    if (wake_) {
        CloseHandle(wake_);
        wake_ = nullptr;
    }
}

void PadManager::set_notify_hwnd(HWND hwnd)
{
    std::lock_guard<std::mutex> lk(mtx_);
    notify_hwnd_ = hwnd;
    if (dry_run_ && hwnd) {
        PostMessageW(hwnd, kMsgPadsUpdated, 0, 0);
    }
}

void PadManager::set_wanted(bool list_wanted, bool live_input_wanted)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (list_wanted_ == list_wanted && live_wanted_ == live_input_wanted) return;
        list_wanted_ = list_wanted;
        live_wanted_ = live_input_wanted;
    }
    if (wake_) SetEvent(wake_);
}

void PadManager::request_refresh()
{
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        force_refresh_ = true;
        if (dry_run_) hwnd = notify_hwnd_;
    }
    if (wake_) SetEvent(wake_);
    if (dry_run_ && hwnd) {
        PostMessageW(hwnd, kMsgPadsUpdated, 0, 0);
    }
}

std::vector<PadInfo> PadManager::list() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return pads_;
}

bool PadManager::any_connected() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return !pads_.empty();
}

void PadManager::apply_nicknames(const PadPrefs& prefs)
{
    std::lock_guard<std::mutex> lk(mtx_);
    cached_prefs_ = prefs;
    for (auto& p : pads_) {
        p.nickname.clear();
        std::string key = pad_nickname_key(p.identity_id, prefs.nickname_salt);
        if (!key.empty()) {
            auto it = prefs.nicknames.find(key);
            if (it != prefs.nicknames.end()) {
                size_t tab = it->second.find('\t');
                p.nickname = (tab != std::string::npos) ? it->second.substr(0, tab) : it->second;
            }
        }
    }
}

bool PadManager::set_nickname(PadPrefs& prefs, const std::string& pad_path, const std::string& raw_nickname)
{
    std::string id_str;
    std::string model_name;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& p : pads_) {
            if (p.path == pad_path) {
                id_str = p.identity_id;
                model_name = p.name;
                break;
            }
        }
    }
    if (id_str.empty()) return false;
    if (prefs.nickname_salt.empty()) {
        char salt_buf[64];
        std::snprintf(salt_buf, sizeof(salt_buf), "win-lares-salt-%lld-%u",
                      static_cast<long long>(std::time(nullptr)),
                      static_cast<unsigned>(GetTickCount()));
        prefs.nickname_salt = salt_buf;
    }
    std::string key = pad_nickname_key(id_str, prefs.nickname_salt);
    if (key.empty()) return false;

    std::string clean = pad_clean_nickname(raw_nickname);
    if (!clean.empty()) {
        prefs.nicknames[key] = clean + "\t" + model_name;
    } else {
        prefs.nicknames.erase(key);
    }
    apply_nicknames(prefs);
    return true;
}

void PadManager::rumble_async(const std::string& pad_path, int strong_pct, int weak_pct, int length_ms)
{
    stop_rumble_thread();

    int slot = -1;
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& p : pads_) {
            p.finding = false;
            p.playing_rumble = false;
            if (p.path == pad_path) {
                slot = p.xinput_slot;
                p.playing_rumble = true;
                hwnd = notify_hwnd_;
            }
        }
    }
    if (slot < 0) return;
    if (hwnd) PostMessageW(hwnd, kMsgPadsUpdated, 0, 0);

    bool is_dry = dry_run_;
    HANDLE cancel_ev = rumble_cancel_event_;
    rumble_thread_ = std::thread([this, pad_path, slot, strong_pct, weak_pct, length_ms, is_dry, cancel_ev]() {
        if (!is_dry && g_xinput.set_state) {
            XInputVibrationRaw vib = {};
            vib.wLeftMotorSpeed = static_cast<WORD>(std::clamp(strong_pct, 0, 100) * 65535 / 100);
            vib.wRightMotorSpeed = static_cast<WORD>(std::clamp(weak_pct, 0, 100) * 65535 / 100);
            g_xinput.set_state(static_cast<DWORD>(slot), &vib);
        }
        DWORD dur = static_cast<DWORD>(std::clamp(length_ms, 50, 3000));
        if (cancel_ev) WaitForSingleObject(cancel_ev, dur);
        else Sleep(dur);
        if (!is_dry && g_xinput.set_state) {
            XInputVibrationRaw stop_vib = { 0, 0 };
            g_xinput.set_state(static_cast<DWORD>(slot), &stop_vib);
        }
        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& p : pads_) {
                if (p.path == pad_path) {
                    p.playing_rumble = false;
                    if (!stop_) notify = notify_hwnd_;
                    break;
                }
            }
        }
        if (notify) PostMessageW(notify, kMsgPadsUpdated, 0, 0);
    });
}

void PadManager::toggle_find_controller(const std::string& pad_path)
{
    int slot = -1;
    bool start_finding = false;
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& p : pads_) {
            if (p.path == pad_path) {
                slot = p.xinput_slot;
                start_finding = !p.finding;
                p.finding = start_finding;
                hwnd = notify_hwnd_;
                break;
            }
        }
    }
    if (slot < 0) return;

    stop_rumble_thread();
    if (!start_finding) {
        if (hwnd) PostMessageW(hwnd, kMsgPadsUpdated, 0, 0);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& p : pads_) {
            p.playing_rumble = false;
            p.finding = (p.path == pad_path);
        }
    }
    if (hwnd) PostMessageW(hwnd, kMsgPadsUpdated, 0, 0);

    bool is_dry = dry_run_;
    HANDLE cancel_ev = rumble_cancel_event_;
    rumble_thread_ = std::thread([this, pad_path, slot, is_dry, cancel_ev]() {
        // Ten short, strong buzzes over six seconds
        for (int pulse = 0; pulse < 10; ++pulse) {
            if (cancel_ev && WaitForSingleObject(cancel_ev, 0) == WAIT_OBJECT_0) break;
            if (!is_dry && g_xinput.set_state) {
                XInputVibrationRaw vib = { 65535, 65535 };
                g_xinput.set_state(static_cast<DWORD>(slot), &vib);
            }
            bool cancelled = cancel_ev ? (WaitForSingleObject(cancel_ev, 250) == WAIT_OBJECT_0) : (Sleep(250), false);
            if (!is_dry && g_xinput.set_state) {
                XInputVibrationRaw stop_vib = { 0, 0 };
                g_xinput.set_state(static_cast<DWORD>(slot), &stop_vib);
            }
            if (cancelled) break;
            if (cancel_ev ? (WaitForSingleObject(cancel_ev, 350) == WAIT_OBJECT_0) : (Sleep(350), false)) break;
        }
        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& p : pads_) {
                if (p.path == pad_path) {
                    p.finding = false;
                    if (!stop_) notify = notify_hwnd_;
                    break;
                }
            }
        }
        if (notify) PostMessageW(notify, kMsgPadsUpdated, 0, 0);
    });
}

void PadManager::poll_pads(bool read_live)
{
    if (!g_xinput.get_state_ex) return;

    std::vector<PadInfo> prev;
    PadPrefs prefs;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        prev = pads_;
        prefs = cached_prefs_;
    }

    std::vector<PadInfo> found;
    bool changed = false;

    for (DWORD slot = 0; slot < 4; ++slot) {
        XInputStateRaw st = {};
        if (g_xinput.get_state_ex(slot, &st) != ERROR_SUCCESS) continue;

        char path_buf[32];
        std::snprintf(path_buf, sizeof(path_buf), "xinput-%u", static_cast<unsigned>(slot));

        PadInfo p;
        bool existed = false;
        for (const auto& old : prev) {
            if (old.path == path_buf) {
                p = old;
                existed = true;
                break;
            }
        }

        if (!existed) {
            p.path = path_buf;
            p.xinput_slot = static_cast<int>(slot);
            p.name = "Xbox Controller";
            p.connection = PadConnection::Usb;
            p.driver = "xinput1_4";
            char node_buf[32];
            std::snprintf(node_buf, sizeof(node_buf), "XInput slot %u", static_cast<unsigned>(slot + 1));
            p.event_node = node_buf;
            p.identity_id = std::string("xinput-slot-") + std::to_string(slot);
            p.rumble = true;
            p.checklist = standard_xbox_checklist();
            p.since = static_cast<int64_t>(std::time(nullptr));
            changed = true;
        }

        if (g_xinput.get_battery) {
            XInputBatteryInfoRaw bi = {};
            if (g_xinput.get_battery(slot, 0 /* BATTERY_DEVTYPE_GAMEPAD */, &bi) == ERROR_SUCCESS) {
                if (bi.BatteryType == 1 /* BATTERY_TYPE_WIRED */) {
                    p.connection = PadConnection::Usb;
                    p.battery.known = false;
                } else if (bi.BatteryType == 2 /* ALKALINE */ || bi.BatteryType == 3 /* NIMH */) {
                    p.connection = PadConnection::Bluetooth;
                    p.name = "Xbox Wireless Controller";
                    p.battery.known = true;
                    double pct = (bi.BatteryLevel == 0) ? 10.0
                               : (bi.BatteryLevel == 1) ? 30.0
                               : (bi.BatteryLevel == 2) ? 65.0
                                                        : 100.0;
                    if (std::fabs(p.battery.percentage - pct) > 0.5) {
                        p.battery.percentage = pct;
                        changed = true;
                    }
                }
            }
        }

        if (read_live) {
            PadState new_state = translate_xinput_state(st.Gamepad);
            if (new_state.buttons != p.state.buttons ||
                std::fabs(new_state.lx - p.state.lx) > 0.005f ||
                std::fabs(new_state.ly - p.state.ly) > 0.005f ||
                std::fabs(new_state.rx - p.state.rx) > 0.005f ||
                std::fabs(new_state.ry - p.state.ry) > 0.005f ||
                std::fabs(new_state.lt - p.state.lt) > 0.005f ||
                std::fabs(new_state.rt - p.state.rt) > 0.005f) {
                p.state = new_state;
                changed = true;
            }
        }

        p.nickname.clear();
        std::string key = pad_nickname_key(p.identity_id, prefs.nickname_salt);
        if (!key.empty()) {
            auto it = prefs.nicknames.find(key);
            if (it != prefs.nicknames.end()) {
                size_t tab = it->second.find('\t');
                p.nickname = (tab != std::string::npos) ? it->second.substr(0, tab) : it->second;
            }
        }

        found.push_back(p);
    }

    if (found.size() != prev.size()) changed = true;

    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pads_ = std::move(found);
        if (changed) hwnd = notify_hwnd_;
    }
    if (hwnd) {
        PostMessageW(hwnd, kMsgPadsUpdated, 0, 0);
    }
}

/* XInput can only be asked, so it's asked: every frame while a controller's
 * live input shows, every 1.5 s while a list of them shows or the helper
 * watches them, and not at all otherwise. */
void PadManager::worker_loop()
{
    while (true) {
        bool stop = false;
        bool want_list = false;
        bool want_live = false;
        bool forced = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop = stop_;
            want_list = list_wanted_;
            want_live = live_wanted_;
            forced = force_refresh_;
            force_refresh_ = false;
        }
        if (stop) break;

        if (forced || want_live || want_list) {
            poll_pads(want_live);
        }

        WaitForSingleObject(wake_, want_live ? 16 : want_list ? 1500 : INFINITE);
    }
}

} // namespace lares
