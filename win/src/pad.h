// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "config.h"

namespace lares {

inline constexpr UINT kMsgPadsUpdated = WM_APP + 15;

inline constexpr int kPadMapStickTravel = 3;
inline constexpr int kPadVeryLow = 10;
inline constexpr size_t kPadNicknameLength = 32;
inline constexpr int kPadRumbleStrength = 75;
inline constexpr int kPadRumbleMs = 400;

inline constexpr double kStickDeadZone = 0.10;
inline constexpr double kStickRestStill = 0.02;
inline constexpr uint64_t kStickRestMs = 600;
inline constexpr double kStickRestReach = 0.40;
inline constexpr float kTriggerPullThreshold = 0.90f;

enum class PadConnection {
    Usb,
    Bluetooth,
    Adapter,
};

enum PadButton : uint8_t {
    PB_A = 0,
    PB_B,
    PB_X,
    PB_Y,
    PB_LB,
    PB_RB,
    PB_LT,
    PB_RT,
    PB_VIEW,
    PB_MENU,
    PB_XBOX,
    PB_SHARE,
    PB_LS,
    PB_RS,
    PB_UP,
    PB_DOWN,
    PB_LEFT,
    PB_RIGHT,
    PB_COUNT
};

inline constexpr uint32_t pb_bit(PadButton b)
{
    return 1u << static_cast<uint32_t>(b);
}

enum PadCellKind : uint8_t {
    PAD_CELL_EMPTY = 0,
    PAD_CELL_LINE,
    PAD_CELL_FAINT,
    PAD_CELL_GAUGE,
    PAD_CELL_AREA,
    PAD_CELL_GLYPH,
    PAD_CELL_CAP_LINE,
    PAD_CELL_CAP_AREA,
};

inline constexpr uint8_t make_pad_cell(PadCellKind kind, PadButton button)
{
    return static_cast<uint8_t>((static_cast<uint8_t>(kind) << 5) | (static_cast<uint8_t>(button) & 31u));
}

inline constexpr PadCellKind pad_cell_kind(uint8_t cell)
{
    return static_cast<PadCellKind>(cell >> 5);
}

inline constexpr PadButton pad_cell_button(uint8_t cell)
{
    return static_cast<PadButton>(cell & 31u);
}

enum class PadPaint : uint8_t {
    None = 0,
    Line,
    Faint,
    Lit,
    Seen,
};

struct PadGaugeSpan {
    int x0 = 0;
    int x1 = -1;
};

struct PadMap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> cells;
    PadGaugeSpan gauges[2]; // 0 = LT, 1 = RT

    uint8_t at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return make_pad_cell(PAD_CELL_EMPTY, PB_A);
        }
        return cells[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    }
};

bool pad_map_parse(const char* text, size_t length, PadMap& out, std::string* err = nullptr);
bool pad_map_load_resource(HINSTANCE hinst, PadMap& out);
PadPaint pad_cell_paint(uint8_t cell, uint32_t held, uint32_t seen);

struct PadState {
    uint32_t buttons = 0;      // pb_bit of each button held now
    float lx = 0.0f;           // -1..1
    float ly = 0.0f;           // -1..1
    float rx = 0.0f;           // -1..1
    float ry = 0.0f;           // -1..1
    float lt = 0.0f;           // 0..1
    float rt = 0.0f;           // 0..1
};

struct PadLook {
    uint32_t held = 0;
    uint32_t seen = 0;
    int caps[2][2] = { { 0, 0 }, { 0, 0 } };
    int pulled[2] = { 0, 0 };
};

PadLook pad_compute_look(const PadMap& map, const PadState& state, uint32_t seen, bool placeholder);
bool pad_gauge_lit(const PadMap& map, const PadLook& look, PadButton b, int x);

struct StickTracker {
    double x = 0.0;
    double y = 0.0;
    double ax = 0.0;
    double ay = 0.0;
    uint64_t since_ms = 0;
    bool anchored = false;
    bool has_rest = false;
    double rx = 0.0;
    double ry = 0.0;
};

void stick_feed(StickTracker& s, double x, double y, uint64_t now_ms);
bool stick_drifting(const StickTracker& s);

struct PadBattery {
    bool known = false;
    double percentage = 0.0;
    uint32_t state = 0; // 1 = charging, 4 = fully charged
    uint32_t level = 0; // UPower level words: 3=Low, 4=Critical, 6=Normal, 7=High, 8=Full
};

bool pad_battery_charging(const PadBattery& b);
int pad_battery_low_stage(const PadBattery& b, int low_level);
const char* pad_battery_level_name(const PadBattery& b);
std::string pad_battery_charge(const PadBattery& b);
std::string pad_battery_text(const PadBattery& b);
double pad_battery_fraction(const PadBattery& b);

struct PadInfo {
    std::string path;          // e.g. "xinput-0"
    int xinput_slot = 0;
    std::string name;          // "Xbox Wireless Controller"
    std::string nickname;      // empty if none
    PadConnection connection = PadConnection::Usb;
    std::string model;         // e.g. "1914"
    std::string firmware;      // e.g. "5.23"
    std::string firmware_update;
    std::string driver;        // e.g. "XInput"
    std::string event_node;    // e.g. "XInput slot 1"
    std::string identity_id;   // stable ID for nickname hashing
    bool rumble = true;
    uint32_t checklist = 0;
    int64_t since = 0;
    PadBattery battery;
    PadState state;
    bool finding = false;
    bool playing_rumble = false;
};

const char* pad_connection_name(PadConnection c);
std::string pad_display_name(const PadInfo& pad);
std::string pad_clean_nickname(const std::string& text);
std::string pad_nickname_key(const std::string& identity_id, const std::string& salt);
std::string pad_model_for_pid(uint16_t pid);

class PadManager {
public:
    explicit PadManager(bool dry_run);
    ~PadManager();

    PadManager(const PadManager&) = delete;
    PadManager& operator=(const PadManager&) = delete;

    void set_notify_hwnd(HWND hwnd);
    void set_wanted(bool list_wanted, bool live_input_wanted);
    void request_refresh();

    std::vector<PadInfo> list() const;
    bool any_connected() const;

    void apply_nicknames(const PadPrefs& prefs);
    bool set_nickname(PadPrefs& prefs, const std::string& pad_path, const std::string& raw_nickname);

    void rumble_async(const std::string& pad_path, int strong_pct, int weak_pct, int length_ms);
    void toggle_find_controller(const std::string& pad_path);

private:
    void worker_loop();
    void poll_pads(bool read_live);
    void stop_rumble_thread();

    bool dry_run_ = false;
    mutable std::mutex mtx_;
    HWND notify_hwnd_ = nullptr;
    bool list_wanted_ = false;
    bool live_wanted_ = false;
    bool force_refresh_ = false;
    bool stop_ = false;

    std::vector<PadInfo> pads_;
    PadPrefs cached_prefs_;
    std::thread worker_;
    std::thread rumble_thread_;
    HANDLE rumble_cancel_event_ = nullptr;
    HANDLE wake_ = nullptr;
};

} // namespace lares
