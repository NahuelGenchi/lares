// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lares {

enum class DevKind : int {
    Controller = 0,
    Audio = 1,
    Keyboard = 2,
    Mouse = 3,
};

enum class DevLink : int {
    Unknown = 0,
    Usb = 1,
    Receiver = 2,
    Bluetooth = 3,
};

enum class LightEffect : int {
    Off = 0,
    Solid = 1,
    Breathing = 2,
    Cycle = 3,
    Other = 4,
};

enum class KeyLight : int {
    Off = 0,
    Solid = 1,
    Breathing = 2,
    Cycle = 3,
    Wave = 4,
    Ripple = 5,
    Own = 6,
    Other = 7,
};

inline bool key_light_coloured(KeyLight l)
{
    return l == KeyLight::Solid || l == KeyLight::Breathing || l == KeyLight::Ripple;
}

inline bool key_light_live(KeyLight l)
{
    return l == KeyLight::Off || l == KeyLight::Solid ||
           l == KeyLight::Breathing || l == KeyLight::Cycle;
}

inline KeyLight key_light_from_name(const std::string& name)
{
    if (name == "off") return KeyLight::Off;
    if (name == "solid") return KeyLight::Solid;
    if (name == "breathing") return KeyLight::Breathing;
    if (name == "cycle") return KeyLight::Cycle;
    return KeyLight::Own;
}

inline constexpr int kMouseStages = 5;
inline constexpr int kKeyLevels = 8;

struct MouseSettings {
    int stages = 5;
    int active = 2;
    int dpi[kMouseStages] = { 400, 800, 1600, 3200, 6400 };
    int dpi_min = 200;
    int dpi_max = 26000;
    int rate = 1000;
    int rate_max = 1000;
    int lift_mm = 1;
    int brightness = 100;
    LightEffect light = LightEffect::Cycle;
    uint8_t rgb[3] = { 0xff, 0x00, 0x00 };
};

struct KeyboardSettings {
    bool has_brightness = true;
    int brightness = 25;
    int levels = 5;
    int level[kKeyLevels] = { 0, 12, 25, 50, 100, 0, 0, 0 };
    bool has_lighting = true;
    bool app_controls = false;
    KeyLight light = KeyLight::Own;
    uint8_t rgb[3] = { 0xff, 0x00, 0x00 };
    bool has_timers = true;
    int dim_s = 60;
    int off_s = 300;
    bool has_startup = true;
    bool has_shutdown = true;
    bool startup = true;
    bool shutdown = false;
};

// --------------------------------------------------------- HyperX protocol
inline constexpr size_t kHyperxReport = 64;
inline constexpr int kHyperxBlocks = 4;

/* The commands, functions and flags on the wire, shared by the codecs in
 * devices.cpp and the Win32 exchanges in hid_win.cpp. */
namespace wire {
inline constexpr uint8_t kHxAck        = 0xff;
inline constexpr uint8_t kHxEvent      = 0xfb;
inline constexpr uint8_t kHxLink       = 0x52;
inline constexpr uint8_t kHxBattery    = 0x50;
inline constexpr uint8_t kHxDpi        = 0x32;
inline constexpr uint8_t kHxSensor     = 0x36;
inline constexpr uint8_t kHxBrightness = 0x40;
inline constexpr uint8_t kHxLighting   = 0x42;
inline constexpr uint8_t kHxFrame      = 0x44;
inline constexpr uint8_t kHxSet        = 0x01;
inline constexpr uint8_t kHxGet        = 0x02;
inline constexpr uint8_t kHxLightSet   = 0x02;
inline constexpr uint8_t kHxLightGet   = 0x03;
inline constexpr int kHxDpiStep        = 50;
inline constexpr uint8_t kHxDefSpeed   = 50;

inline constexpr uint8_t kRootGetFeature   = 0;
inline constexpr uint8_t kRootPing         = 1;
inline constexpr uint8_t kBrightnessInfo   = 0;
inline constexpr uint8_t kBrightnessGet    = 1;
inline constexpr uint8_t kBrightnessSet    = 2;
inline constexpr uint8_t kBrightnessLight  = 3;
inline constexpr uint8_t kRgbInfo          = 0;
inline constexpr uint8_t kRgbSetEffect     = 1;
inline constexpr uint8_t kRgbNvConfig      = 3;
inline constexpr uint8_t kRgbSwControl     = 5;
inline constexpr uint8_t kProfilesInfo     = 0;
inline constexpr uint8_t kProfilesMode     = 2;
inline constexpr uint8_t kProfilesCurrent  = 4;
inline constexpr uint8_t kProfilesRead     = 5;
inline constexpr uint8_t kProfilesBegin    = 6;
inline constexpr uint8_t kProfilesWrite    = 7;
inline constexpr uint8_t kProfilesEnd      = 8;
inline constexpr size_t  kKeysSlot         = 0xd0;
inline constexpr size_t  kSavingSlot       = 0xe6;
inline constexpr size_t  kTimersAt         = 0x1c;
inline constexpr uint8_t kNvGet            = 0x00;
inline constexpr uint8_t kNvSet            = 0x01;
inline constexpr uint8_t kNvOn             = 0x01;
inline constexpr uint8_t kNvOff            = 0x02;
inline constexpr uint8_t kNeverSet         = 1;   // HID++ "unknown": a startup or shutdown effect never set

/* Taking the lights and answering the keyboard's idle and active events, as
 * G HUB does: the firmware's mode with the take's flags first, then software
 * (issue #68); flags by HidppLive. */
inline constexpr uint8_t kSwFirmware = 0;
inline constexpr uint8_t kSwSoftware = 3;
inline constexpr uint8_t kSwFlags[4] = { 6, 3, 5, 3 };
inline constexpr uint8_t kVolatile   = 1;
} // namespace wire

struct HyperxModel {
    uint16_t vendor;
    uint16_t product;
    int interface_num;
    bool receiver;
    uint16_t cable;
    int dpi_min;
    int dpi_max;
    int rate_max;
};

struct HyperxState {
    uint8_t dpi[kHyperxReport] = {};
    uint8_t sensor[kHyperxReport] = {};
    uint8_t brightness[kHyperxReport] = {};
    uint8_t light[kHyperxReport] = {};
};

enum class HyperxEventKind { Stage, Link, Battery };

struct HyperxEvent {
    HyperxEventKind kind = HyperxEventKind::Stage;
    int stage = 0;
    int code = 0;
    bool linked = false;
    int level = 0;
    bool charging = false;
};

const HyperxModel* hyperx_model(uint16_t vendor, uint16_t product);
bool hyperx_on_cable(const HyperxModel* receiver, const HyperxModel* const* plugged, size_t n);
bool hyperx_battery_parse(const uint8_t* report, size_t len, int* level, bool* charging);
bool hyperx_event(const uint8_t* report, size_t len, HyperxEvent* event);
bool hyperx_take_stage(HyperxState* state, const HyperxEvent* event);
void hyperx_decode(const HyperxModel* model, const HyperxState* state, MouseSettings* out);
int hyperx_packets(const HyperxState* now, const MouseSettings* want, uint16_t slot,
                   uint8_t out[kHyperxBlocks][kHyperxReport]);
void hyperx_merge(const HyperxState* now, const MouseSettings* base, const MouseSettings* want, MouseSettings* out);
bool hyperx_shows(const MouseSettings* was, const MouseSettings* now);
void hyperx_apply(HyperxState* state, const uint8_t* packet);
void hyperx_sample(HyperxState* state);

// --------------------------------------------------- Logitech HID++ 2.0
inline constexpr uint8_t kHidppShort = 0x10;
inline constexpr uint8_t kHidppLong = 0x11;
inline constexpr size_t kHidppShortLen = 7;
inline constexpr size_t kHidppLongLen = 20;
inline constexpr size_t kHidppParams = 16;
inline constexpr uint8_t kHidppSwId = 0x0c;
inline constexpr uint8_t kHidppSwHelper = 0x0e;
inline constexpr size_t kHidppSectorMax = 256;
inline constexpr size_t kHidppSlot = 11;
inline constexpr uint16_t kHidppSector = 0x0001;
inline constexpr int kHidppClusters = 2;
inline constexpr int kHidppEffects = 8;
inline constexpr uint16_t kHidppNvStartup = 0x0001;
inline constexpr uint16_t kHidppNvShutdown = 0x0040;
inline constexpr size_t kHidppNvColours = 6;
inline constexpr size_t kHidppNvLen = 10;
inline constexpr int kHidppTimerMax = 3600;

enum HidppFeature : uint16_t {
    HIDPP_ROOT        = 0x0000,
    HIDPP_FEATURE_SET = 0x0001,
    HIDPP_STATUS      = 0x1d4b,
    HIDPP_BRIGHTNESS  = 0x8040,
    HIDPP_RGB_EFFECTS = 0x8071,
    HIDPP_PROFILES    = 0x8100,
};

struct HidppModel {
    uint16_t receiver;
    uint16_t product;
    int interface_num;
    int levels;
    int level[kKeyLevels];
};

struct HidppLighting {
    KeyLight effect = KeyLight::Own;
    uint8_t rgb[3] = {};
};

struct HidppNv {
    bool known = false;
    bool on = false;
    uint8_t colours[kHidppNvColours] = {};
};

struct HidppEffects {
    uint8_t feature = 0;
    uint8_t status = 0;
    int clusters = 0;
    int count[kHidppClusters] = {};
    uint16_t effect[kHidppClusters][kHidppEffects] = {};
};

inline int hidpp_effect_index(const HidppEffects* fx, int cluster, uint16_t id)
{
    if (!fx || cluster < 0 || cluster >= fx->clusters) return -1;
    for (int i = 0; i < fx->count[cluster]; ++i) {
        if (fx->effect[cluster][i] == id) return i;
    }
    return -1;
}

enum class HidppLive { Take = 0, Idle = 1, Active = 2, Dark = 3 };

struct HidppKeyboard {
    bool has_brightness = false;
    int brightness = 0;
    int brightness_min = 0;
    int brightness_max = 100;
    int brightness_steps = 5;
    bool has_profile = false;
    bool app_controls = false;
    HidppLighting light;
    int dim_s = 60;
    int off_s = 300;
    HidppNv startup;
    HidppNv shutdown;
    uint16_t sector = kHidppSector;
    size_t profile_size = 0;
    uint8_t profile[kHidppSectorMax] = {};
    uint8_t status_index = 0;
    uint8_t brightness_index = 0;
    uint8_t profiles_index = 0;
    uint8_t rgb_index = 0;
};

enum class HidppMatch { Other, Answer, Error, ReceiverError };
enum class HidppEventKind { Link, Brightness, Changed, Idle, Active };

struct HidppEvent {
    HidppEventKind kind = HidppEventKind::Link;
    bool linked = false;
    int brightness = 0;
};

/* What a keyboard change asks of the keyboard, from the settings shown
 * before it (`base`) and after it (`want`), as src/devices.c plans it: only
 * an effect the keyboard keeps goes to its profile; live ones are the
 * helper's. -1 leaves a part as it is. */
struct KeyChange {
    int brightness = -1;   // the device's own value
    bool light = false;
    HidppLighting lit;
    int dim_s = -1;
    int off_s = -1;
    int startup = -1;
    int shutdown = -1;
    bool release = false;  // a kept effect takes the lights back from software
};
// FALSE when the keyboard can't hold that change.
bool key_change_plan(const KeyboardSettings& base, const KeyboardSettings& want, const HidppKeyboard& k,
                     KeyChange& out);
bool key_change_empty(const KeyChange& c);

const HidppModel* hidpp_model(uint16_t receiver, uint16_t product);
bool hidpp_allowed(uint16_t feature, uint8_t function, const uint8_t* params, size_t n);
size_t hidpp_packet(uint8_t sw, uint8_t index, uint8_t feature_index, uint8_t function,
                    const uint8_t* params, size_t n, uint8_t out[kHidppLongLen]);
HidppMatch hidpp_match(const uint8_t* report, size_t len, const uint8_t* packet);
uint16_t hidpp_crc(const uint8_t* data, size_t len);
bool hidpp_sector_valid(const uint8_t* sector, size_t size);
int hidpp_keys_slot(size_t profile_size);
bool hidpp_kept(KeyLight effect);
int hidpp_effect(const HidppLighting* l, bool dim, uint8_t params[10]);
void hidpp_slot_decode(const uint8_t* slot, HidppLighting* out);
bool hidpp_slot_encode(const HidppLighting* l, bool saving, uint8_t slot[kHidppSlot]);
bool hidpp_profile_lit(const uint8_t* sector, size_t size, const HidppLighting* l, uint8_t* out);
bool hidpp_profile_timed(const uint8_t* sector, size_t size, int dim_s, int off_s, uint8_t* out);
int hidpp_dark_after(int dim_s, int off_s);
bool hidpp_nv_decode(uint16_t cap, const uint8_t* answer, size_t n, HidppNv* out);
void hidpp_nv_encode(uint16_t cap, bool on, const HidppNv* was, uint8_t out[kHidppNvLen]);
void hidpp_decode_brightness(const uint8_t* info, const uint8_t* now, HidppKeyboard* out);
void hidpp_decode_profile(const uint8_t* sector, size_t size, HidppKeyboard* out);
void hidpp_decode(const HidppKeyboard* k, const HidppModel* model, KeyboardSettings* out);
int hidpp_brightness_value(const HidppKeyboard* k, int percent);
bool hidpp_event(const uint8_t* report, size_t len, uint8_t index, const HidppKeyboard* k, HidppEvent* out);
void hidpp_sample(HidppKeyboard* out);

// -------------------------------------------------------- Device Entry
struct DeviceEntry {
    std::string id;
    std::string name;
    std::string vendor_name;
    DevKind kind = DevKind::Mouse;
    DevLink link = DevLink::Usb;
    bool bluetooth = false;
    bool connected = true;
    bool asleep = false;
    int battery = -1; // 0..100 or -1 when unknown
    bool charging = false;
    bool has_settings = false;
    bool settings_loaded = false;
    MouseSettings mouse;
    KeyboardSettings keyboard;
    std::wstring hid_path_primary;   // HyperX 64B or G715 7B short
    std::wstring hid_path_secondary; // G715 20B long
    std::wstring lamp_path;          // the LampArray Windows Dynamic Lighting paints it through
    bool windows_lighting = false;   // Dynamic Lighting paints it now, over its own effects
    uint16_t usb_vid = 0;
    uint16_t usb_pid = 0;
    std::string status_error;
};

const char* dev_link_text(DevLink link);

#ifdef _WIN32
inline constexpr UINT kMsgDevicesUpdated = WM_APP + 4;

/* The window's devices: found when they come or go, read when their page
 * opens, listened to while it shows, batteries asked every few minutes
 * while one is on screen. Nothing else polls. Changes queue for a writer
 * thread, so a click never waits for the last one; picks that come while
 * one is written go together after it. Live keyboard lighting is
 * lares-pad.exe's to play (livelight.cpp): the window only shows it. */
class DevicesManager {
public:
    explicit DevicesManager(bool dry_run);
    ~DevicesManager();

    DevicesManager(const DevicesManager&) = delete;
    DevicesManager& operator=(const DevicesManager&) = delete;

    void set_notify_hwnd(HWND hwnd) { notify_hwnd_.store(hwnd); }
    void set_wanted(bool want_list, bool want_mouse_settings, bool want_keyboard_settings);
    // Devices came or went.
    void request_refresh();

    std::vector<DeviceEntry> list() const;
    bool any_of(DevKind kind) const;

    // The live lighting the helper plays, which the Keyboard page shows.
    void set_live_key_light(KeyLight light, const uint8_t rgb[3]);
    void apply_mouse_async(const std::string& id, const MouseSettings& want);
    void apply_keyboard_async(const std::string& id, const KeyboardSettings& want);

private:
    struct MouseJob {
        bool due = false;
        std::string id;
        MouseSettings base;
        MouseSettings want;
    };
    struct KeyJob {
        bool due = false;
        std::string id;
        KeyboardSettings base;
        KeyboardSettings want;
    };

    void worker_loop();
    void writer_loop();
    void scan(bool read_mouse, bool read_kbd, bool batteries);
    void read_mouse_now();
    void read_keyboard_now();
    void refresh_windows_lighting();
    void write_mouse(const MouseJob& job);
    void write_keyboard(const KeyJob& job);
    void overlay_live(DeviceEntry& d) const;
    void notify();

    bool dry_run_ = false;
    mutable std::mutex mu_;
    std::vector<DeviceEntry> devices_;
    std::wstring hx_path_;
    uint16_t hx_pid_ = 0;
    std::wstring kb_short_;
    std::wstring kb_long_;
    HyperxState hyperx_state_{};
    bool hyperx_loaded_ = false;
    HidppKeyboard hidpp_state_{};
    bool hidpp_loaded_ = false;
    KeyLight live_key_light_ = KeyLight::Own;
    uint8_t live_key_rgb_[3] = { 0xff, 0x00, 0x00 };

    bool rescan_due_ = false;
    bool mouse_read_due_ = false;
    bool kbd_read_due_ = false;
    MouseJob mouse_job_;
    KeyJob key_job_;
    std::string writing_id_;

    std::atomic<HWND> notify_hwnd_{ nullptr };
    std::atomic<bool> want_list_{ false };
    std::atomic<bool> want_mouse_{ false };
    std::atomic<bool> want_kbd_{ false };
    std::atomic<bool> stop_{ false };
    HANDLE wake_event_ = nullptr;
    HANDLE write_event_ = nullptr;
    std::thread worker_;
    std::thread writer_;
};
#endif

} // namespace lares
