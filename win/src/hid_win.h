// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Win32 side of talking to the HyperX and Logitech receivers: finding
 * their HID collections, a handle that keeps one read pending so no report
 * is lost between waits, and the exchanges src/hyperx.c and src/hidpp.c make
 * on Linux, on top of the shared codecs in devices.cpp. Windows splits a
 * Logitech receiver's HID++ interface into two collections, one for short
 * (7-byte) and one for long (20-byte) reports, so a HID++ link is a pair. */
#pragma once
#ifdef _WIN32
#include "devices.h"

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace lares {

struct HidCollection {
    std::wstring path;
    std::wstring parent;   // the device instance ID of the devnode above the collection's
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint16_t usage_page = 0;
    uint16_t usage = 0;
};

inline constexpr uint16_t kUsagePageVendor = 0xff00;
inline constexpr uint16_t kUsagePageLighting = 0x0059;   // HID Lighting and Illumination
inline constexpr uint16_t kUsageLampArray = 0x0001;

// Every present HID collection of the vendors Lares knows (HyperX, Logitech).
std::vector<HidCollection> hid_collections();

// What a scan of those collections found.
struct HidFound {
    std::wstring hyperx_receiver;   // vendor collection of the 2.4 GHz receiver
    std::wstring hyperx_cable;      // vendor collection of the mouse on its cable
    std::wstring hyperx_lamps_receiver;
    std::wstring hyperx_lamps_cable;
    std::wstring hidpp_short;       // the G715 receiver's HID++ collections
    std::wstring hidpp_long;
    std::wstring keyboard_lamps;    // a LampArray Windows paints the G715 through
};
HidFound hid_find(const std::vector<HidCollection>& all);

// Windows Dynamic Lighting: whether it paints the LampArray at `lamp_path`.
// Off for the whole PC, or for that device, gives the lights back to it.
std::wstring lighting_key_name(const std::wstring& lamp_path);
bool windows_lighting_on(const std::wstring& lamp_path);

class HidPort {
public:
    HidPort() = default;
    ~HidPort();
    HidPort(const HidPort&) = delete;
    HidPort& operator=(const HidPort&) = delete;

    bool open(const std::wstring& path, bool write);
    void close();
    bool is_open() const { return h_ != INVALID_HANDLE_VALUE; }
    HANDLE read_event() const { return read_ev_; }

    // Keeps a read pending; FALSE when the device is gone.
    bool arm();
    // The report a finished read brought, if any; FALSE when the device is gone.
    bool take(uint8_t* out, DWORD& got);
    // Throws away what came before a request.
    bool drain();
    bool write(const uint8_t* data, DWORD len, DWORD timeout_ms);

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE read_ev_ = nullptr;
    OVERLAPPED read_ov_ = {};
    bool pending_ = false;
    DWORD in_len_ = 64;
    uint8_t buf_[64] = {};
};

// Serialises one receiver's exchanges across Lares' processes.
struct ScopedWinMutex {
    HANDLE h = nullptr;
    bool locked = false;
    explicit ScopedWinMutex(const wchar_t* name, DWORD timeout_ms = 5000);
    ~ScopedWinMutex();
    ScopedWinMutex(const ScopedWinMutex&) = delete;
    ScopedWinMutex& operator=(const ScopedWinMutex&) = delete;
};

inline constexpr wchar_t kHyperxLock[] = L"Local\\LaresHyperxLock";
inline constexpr wchar_t kHidppLock[] = L"Local\\LaresHidppLock";

// ------------------------------------------------------------ HyperX
enum class HidFail { None, Asleep, NoAnswer, Refused, Gone };

struct HyperxResult {
    bool ok = false;
    HidFail fail = HidFail::None;
    std::string message;
};

bool hyperx_battery_win(HidPort& p, int& level, bool& charging);
HyperxResult hyperx_read_win(HidPort& p, const HyperxModel* model, HyperxState& state);
HyperxResult hyperx_write_win(HidPort& p, const HyperxModel* model, const MouseSettings& base,
                              const MouseSettings& want, HyperxState& state);

// ------------------------------------------------------------ HID++ 2.0
struct HidppLinkWin {
    HidPort s;
    HidPort l;
    uint8_t index = 1;
    uint8_t sw = kHidppSwId;
    uint8_t refused = 0;
    bool open(const std::wstring& short_path, const std::wstring& long_path);
};

struct HidppResult {
    bool ok = false;
    HidFail fail = HidFail::None;
    std::string message;
};

HidppResult hidpp_read_all_win(HidppLinkWin& l, HidppKeyboard& out);
HidppResult hidpp_change_win(HidppLinkWin& l, const KeyChange& c, HidppKeyboard& out);
HidppResult hidpp_live_effects_win(HidppLinkWin& l, HidppEffects& out);
HidppResult hidpp_live_play_win(HidppLinkWin& l, const HidppEffects& fx, const HidppLighting& light, HidppLive state);
HidppResult hidpp_live_release_win(HidppLinkWin& l, const HidppEffects& fx);
HidppResult hidpp_live_timers_win(HidppLinkWin& l, int& dim_s, int& off_s);

} // namespace lares
#endif
