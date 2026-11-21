// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "devices.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <bluetoothapis.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#endif

namespace lares {

const char* dev_link_text(DevLink link)
{
    switch (link) {
    case DevLink::Usb:       return "USB";
    case DevLink::Receiver:  return "USB receiver";
    case DevLink::Bluetooth: return "Bluetooth";
    default:                 return "Unknown";
    }
}

// ========================================================= HyperX protocol
namespace {

constexpr uint8_t kHxAck        = 0xff;
constexpr uint8_t kHxEvent      = 0xfb;
constexpr uint8_t kHxLink       = 0x52;
constexpr uint8_t kHxBattery    = 0x50;
constexpr uint8_t kHxDpi        = 0x32;
constexpr uint8_t kHxSensor     = 0x36;
constexpr uint8_t kHxBrightness = 0x40;
constexpr uint8_t kHxLighting   = 0x42;
constexpr uint8_t kHxFrame      = 0x44;
constexpr uint8_t kHxSet        = 0x01;
constexpr uint8_t kHxGet        = 0x02;
constexpr uint8_t kHxLightSet   = 0x02;
constexpr uint8_t kHxLightGet   = 0x03;
constexpr int kHxDpiStep        = 50;
constexpr uint8_t kHxDefSpeed   = 50;

const HyperxModel kHyperxModels[] = {
    { 0x03f0, 0x0f98, 2, true,  0x0d97, 200, 26000, 1000 },
    { 0x03f0, 0x0d97, 2, false, 0x0000, 200, 26000, 1000 },
};

const struct { uint8_t code; int hz; } kHyperxRates[] = {
    { 0x40, 125 }, { 0x20, 250 }, { 0x10, 500 }, { 0x08, 1000 },
    { 0x04, 2000 }, { 0x02, 4000 }, { 0x01, 8000 },
};

int hx_rate_hz(uint8_t code)
{
    for (const auto& r : kHyperxRates) {
        if (r.code == code) return r.hz;
    }
    return 0;
}

uint8_t hx_rate_code(int hz)
{
    for (const auto& r : kHyperxRates) {
        if (r.hz == hz) return r.code;
    }
    return 0;
}

int hx_stage_dpi(const uint8_t* stage)
{
    return ((static_cast<int>(stage[0]) | (static_cast<int>(stage[1]) << 8)) + 1) * kHxDpiStep;
}

bool hx_has_colour(LightEffect light)
{
    return light == LightEffect::Solid || light == LightEffect::Breathing;
}

bool hx_same_light(const MouseSettings* a, const MouseSettings* b)
{
    return a->light == b->light && (!hx_has_colour(a->light) || std::memcmp(a->rgb, b->rgb, 3) == 0);
}

bool hx_same_dpi(const MouseSettings* a, const MouseSettings* b)
{
    if (a->rate != b->rate || a->stages != b->stages || a->active != b->active) return false;
    for (int i = 0; i < a->stages; ++i) {
        if (a->dpi[i] != b->dpi[i]) return false;
    }
    return true;
}

uint8_t hx_effect_code(LightEffect l)
{
    switch (l) {
    case LightEffect::Off:       return 0;
    case LightEffect::Solid:     return 1;
    case LightEffect::Breathing: return 2;
    case LightEffect::Cycle:     return 4;
    default:                     return 0;
    }
}

uint8_t* hx_packet(uint8_t out[][kHyperxReport], int* n, uint8_t command, uint8_t sub, uint16_t slot)
{
    uint8_t* p = out[(*n)++];
    std::memset(p, 0, kHyperxReport);
    p[0] = command;
    p[1] = sub;
    p[2] = static_cast<uint8_t>(slot & 0xff);
    p[3] = static_cast<uint8_t>(slot >> 8);
    return p;
}

void hx_answer_from(uint8_t* answer, uint8_t command, uint8_t sub, const uint8_t* p)
{
    std::memset(answer, 0, kHyperxReport);
    answer[0] = static_cast<uint8_t>(command + 1);
    answer[1] = sub;
    std::memcpy(answer + 2, p + 4, kHyperxReport - 4);
}

} // namespace

const HyperxModel* hyperx_model(uint16_t vendor, uint16_t product)
{
    for (const auto& m : kHyperxModels) {
        if (m.vendor == vendor && m.product == product) return &m;
    }
    return nullptr;
}

bool hyperx_on_cable(const HyperxModel* receiver, const HyperxModel* const* plugged, size_t n)
{
    if (!receiver || !receiver->receiver || !receiver->cable) return false;
    for (size_t i = 0; i < n; ++i) {
        if (plugged[i] && plugged[i]->vendor == receiver->vendor && plugged[i]->product == receiver->cable) {
            return true;
        }
    }
    return false;
}

bool hyperx_battery_parse(const uint8_t* report, size_t len, int* level, bool* charging)
{
    if (!report || len < 4 || report[0] != kHxBattery + 1 || report[1] != 0x02 || report[2] > 100) {
        return false;
    }
    if (level) *level = report[2];
    if (charging) *charging = (report[3] == 1);
    return true;
}

bool hyperx_event(const uint8_t* report, size_t len, HyperxEvent* event)
{
    if (!report || !event) return false;
    int level = 0;
    bool charging = false;
    if (hyperx_battery_parse(report, len, &level, &charging)) {
        event->kind = HyperxEventKind::Battery;
        event->level = level;
        event->charging = charging;
        return true;
    }
    if (len < 3 || report[0] != kHxEvent) return false;
    if (report[1] == 0x08 && len >= 5 && report[2] < 8) {
        event->kind = HyperxEventKind::Stage;
        event->stage = report[2];
        event->code = static_cast<int>(report[3]) | (static_cast<int>(report[4]) << 8);
        return true;
    }
    if (report[1] == 0x0a) {
        event->kind = HyperxEventKind::Link;
        event->linked = (report[2] == 1);
        return true;
    }
    return false;
}

bool hyperx_take_stage(HyperxState* s, const HyperxEvent* ev)
{
    if (!s || !ev || ev->kind != HyperxEventKind::Stage || ev->stage < 0 || ev->stage >= 8) {
        return false;
    }
    const uint8_t* stage = s->dpi + 5 + 5 * ev->stage;
    if (!((s->dpi[3] >> ev->stage) & 1) ||
        (static_cast<int>(stage[0]) | (static_cast<int>(stage[1]) << 8)) != ev->code) {
        return false;
    }
    s->dpi[4] = static_cast<uint8_t>(ev->stage);
    return true;
}

void hyperx_decode(const HyperxModel* model, const HyperxState* s, MouseSettings* out)
{
    if (!s || !out) return;
    *out = MouseSettings{};
    out->stages = 0;
    out->active = 0;
    for (int bit = 0; bit < 8 && out->stages < kMouseStages; ++bit) {
        if (!((s->dpi[3] >> bit) & 1)) continue;
        if (bit == s->dpi[4]) out->active = out->stages;
        out->dpi[out->stages++] = hx_stage_dpi(s->dpi + 5 + 5 * bit);
    }
    out->rate = hx_rate_hz(s->dpi[2]);
    out->lift_mm = (s->sensor[5] == 2) ? 2 : 1;
    out->brightness = (static_cast<int>(s->brightness[2]) * 100 + 127) / 255;
    switch (s->light[2]) {
    case 0:  out->light = LightEffect::Off; break;
    case 1:  out->light = LightEffect::Solid; break;
    case 2:  out->light = LightEffect::Breathing; break;
    case 4:  out->light = LightEffect::Cycle; break;
    default: out->light = LightEffect::Other; break;
    }
    std::memset(out->rgb, 0, 3);
    if (hx_has_colour(out->light)) std::memcpy(out->rgb, s->light + 7, 3);
    if (model) {
        out->dpi_min = model->dpi_min;
        out->dpi_max = model->dpi_max;
        out->rate_max = model->rate_max;
    } else {
        out->dpi_min = 0;
        out->dpi_max = 0;
        out->rate_max = 0;
    }
}

void hyperx_merge(const HyperxState* now, const MouseSettings* base, const MouseSettings* want, MouseSettings* out)
{
    hyperx_decode(nullptr, now, out);
    out->dpi_min = want->dpi_min;
    out->dpi_max = want->dpi_max;
    out->rate_max = want->rate_max;
    for (int i = 0; i < kMouseStages; ++i) {
        if (want->dpi[i] != base->dpi[i]) out->dpi[i] = want->dpi[i];
    }
    if (want->active != base->active) out->active = want->active;
    if (want->rate != base->rate) out->rate = want->rate;
    if (want->lift_mm != base->lift_mm) out->lift_mm = want->lift_mm;
    if (want->brightness != base->brightness) out->brightness = want->brightness;
    if (!hx_same_light(want, base)) {
        out->light = want->light;
        std::memcpy(out->rgb, want->rgb, 3);
    }
}

bool hyperx_shows(const MouseSettings* was, const MouseSettings* now)
{
    return now->light == LightEffect::Solid && now->brightness != was->brightness;
}

int hyperx_packets(const HyperxState* now, const MouseSettings* want, uint16_t slot,
                   uint8_t out[kHyperxBlocks][kHyperxReport])
{
    MouseSettings was;
    hyperx_decode(nullptr, now, &was);
    int n = 0;

    if (!hx_same_dpi(want, &was)) {
        uint8_t code = hx_rate_code(want->rate);
        if (!code || (want->rate_max && want->rate > want->rate_max) ||
            want->stages != was.stages || want->active < 0 || want->active >= was.stages) {
            return -1;
        }
        uint8_t* p = hx_packet(out, &n, kHxDpi, kHxSet, slot);
        std::memcpy(p + 4, now->dpi + 2, kHyperxReport - 4);
        p[4] = code;
        for (int bit = 0, i = 0; bit < 8 && i < was.stages; ++bit) {
            if (!((now->dpi[3] >> bit) & 1)) continue;
            int dpi = want->dpi[i];
            int min_dpi = std::max(want->dpi_min, kHxDpiStep);
            if ((dpi % kHxDpiStep) != 0 || dpi < min_dpi || (want->dpi_max && dpi > want->dpi_max)) {
                return -1;
            }
            int stage_code = dpi / kHxDpiStep - 1;
            p[7 + 5 * bit] = static_cast<uint8_t>(stage_code & 0xff);
            p[8 + 5 * bit] = static_cast<uint8_t>(stage_code >> 8);
            if (i == want->active) p[6] = static_cast<uint8_t>(bit);
            ++i;
        }
    }
    if (want->lift_mm != was.lift_mm) {
        if (want->lift_mm != 1 && want->lift_mm != 2) return -1;
        uint8_t* p = hx_packet(out, &n, kHxSensor, kHxSet, slot);
        std::memcpy(p + 4, now->sensor + 2, 5);
        p[7] = (want->lift_mm == 2) ? 2 : 0;
    }
    if (want->brightness != was.brightness) {
        if (want->brightness < 0 || want->brightness > 100) return -1;
        hx_packet(out, &n, kHxBrightness, kHxSet, slot)[4] =
            static_cast<uint8_t>(want->brightness * 255 / 100);
    }
    if (!hx_same_light(want, &was)) {
        if (static_cast<int>(want->light) >= static_cast<int>(LightEffect::Other) ||
            static_cast<int>(want->light) < 0) {
            return -1;
        }
        uint8_t* p = hx_packet(out, &n, kHxLighting, kHxLightSet, slot);
        p[4] = hx_effect_code(want->light);
        if (want->light == LightEffect::Breathing || want->light == LightEffect::Cycle) {
            p[5] = (want->light == was.light && now->light[3]) ? now->light[3] : kHxDefSpeed;
        }
        if (hx_has_colour(want->light)) {
            p[8] = 1;
            std::memcpy(p + 9, want->rgb, 3);
        } else if (want->light == LightEffect::Cycle) {
            p[8] = 0xfe;
        }
    }
    return n;
}

void hyperx_apply(HyperxState* s, const uint8_t* p)
{
    if (p[0] == kHxDpi && p[1] == kHxSet) hx_answer_from(s->dpi, kHxDpi, kHxGet, p);
    else if (p[0] == kHxSensor && p[1] == kHxSet) hx_answer_from(s->sensor, kHxSensor, kHxGet, p);
    else if (p[0] == kHxBrightness && p[1] == kHxSet) hx_answer_from(s->brightness, kHxBrightness, kHxGet, p);
    else if (p[0] == kHxLighting && p[1] == kHxLightSet) hx_answer_from(s->light, kHxLighting, kHxLightGet, p);
}

void hyperx_sample(HyperxState* s)
{
    static const uint8_t dpi[] = {
        kHxDpi + 1, kHxGet, 0x08, 0x1f, 0x02,
        0x07, 0x00, 0xff, 0x00, 0x00,   0x0f, 0x00, 0x00, 0x00, 0xff,   0x1f, 0x00, 0xff, 0xff, 0x00,
        0x3f, 0x00, 0x00, 0xff, 0x00,   0x7f, 0x00, 0xff, 0xff, 0xff,
    };
    std::memset(s, 0, sizeof(*s));
    std::memcpy(s->dpi, dpi, sizeof(dpi));
    s->sensor[0] = kHxSensor + 1;
    s->sensor[1] = kHxGet;
    s->brightness[0] = kHxBrightness + 1;
    s->brightness[1] = kHxGet;
    s->brightness[2] = 0xff;
    s->light[0] = kHxLighting + 1;
    s->light[1] = kHxLightGet;
    s->light[2] = 6;
    s->light[3] = kHxDefSpeed;
}

// ================================================= Logitech HID++ 2.0
namespace {

constexpr uint8_t kRootGetFeature   = 0;
constexpr uint8_t kRootPing         = 1;
constexpr uint8_t kBrightnessInfo   = 0;
constexpr uint8_t kBrightnessGet    = 1;
constexpr uint8_t kBrightnessSet    = 2;
constexpr uint8_t kBrightnessLight  = 3;
constexpr uint8_t kRgbInfo          = 0;
constexpr uint8_t kRgbSetEffect     = 1;
constexpr uint8_t kRgbNvConfig      = 3;
constexpr uint8_t kRgbSwControl     = 5;
constexpr uint8_t kProfilesInfo     = 0;
constexpr uint8_t kProfilesMode     = 2;
constexpr uint8_t kProfilesCurrent  = 4;
constexpr uint8_t kProfilesRead     = 5;
constexpr uint8_t kProfilesBegin    = 6;
constexpr uint8_t kProfilesWrite    = 7;
constexpr uint8_t kProfilesEnd      = 8;
constexpr size_t  kKeysSlot         = 0xd0;
constexpr size_t  kSavingSlot       = 0xe6;
constexpr size_t  kTimersAt         = 0x1c;
constexpr uint8_t kNvGet            = 0x00;
constexpr uint8_t kNvSet            = 0x01;
constexpr uint8_t kNvOn             = 0x01;
constexpr uint8_t kNvOff            = 0x02;

enum : int {
    FX_OFF = 0x00, FX_SOLID = 0x01, FX_BREATHING = 0x0a,
    FX_CYCLE_OLD = 0x03, FX_WAVE_OLD = 0x04, FX_RIPPLE_OLD = 0x0b,
    FX_CYCLE = 0x15, FX_WAVE = 0x16, FX_RIPPLE = 0x17,
    FX_OWN = 0x18, FX_OWN_SAVING = 0x19,
};

constexpr uint16_t kBreathMs     = 5000;
constexpr uint16_t kCycleMs      = 5000;
constexpr uint16_t kWaveMs       = 5000;
constexpr uint16_t kRipplePeriod = 20;
constexpr uint8_t  kFull         = 100;
constexpr uint8_t  kSaturated    = 0xff;
constexpr uint8_t  kNoRamp       = 2;
constexpr uint8_t  kWaveRight    = 1;
constexpr uint8_t  kVolatile     = 1;
constexpr uint8_t  kSwFirmware   = 0;
constexpr uint8_t  kSwSoftware   = 3;
constexpr uint8_t  kSwFlags[4]   = { 6, 3, 5, 3 };
constexpr uint8_t  kConnection   = 0x41;
constexpr uint8_t  kLinkDown     = 0x40;
constexpr uint8_t  kStatusReconn = 1;

const uint8_t kStartupColours[kHidppNvColours]  = { 0xff, 0x8d, 0xaa, 0x8b, 0xe4, 0xe9 };
const uint8_t kShutdownColours[kHidppNvColours] = { 0xfe, 0x83, 0x85, 0xcb, 0xe8, 0xce };

const HidppModel kHidppModels[] = {
    { 0xc547, 0x40a2, 2, 5, { 0, 12, 25, 50, 100, 0, 0, 0 } },
};

bool check_whole_sector(const uint8_t* p, size_t n)
{
    if (!p || n != 6) return false;
    uint16_t sector = static_cast<uint16_t>((p[0] << 8) | p[1]);
    uint16_t offset = static_cast<uint16_t>((p[2] << 8) | p[3]);
    uint16_t length = static_cast<uint16_t>((p[4] << 8) | p[5]);
    return sector == kHidppSector && offset == 0 && length >= 16 && length <= kHidppSectorMax;
}

bool check_one_chunk(const uint8_t*, size_t n) { return n == 16; }
bool check_nothing(const uint8_t*, size_t n) { return n == 0; }

bool check_one_group(const uint8_t* p, size_t n)
{
    return p && n == 13 && p[0] < kHidppClusters && p[1] < kHidppEffects && p[12] == kVolatile;
}

bool check_sw_control(const uint8_t* p, size_t n)
{
    if (!p || n == 0) return false;
    if (p[0] == 0x00) return true;
    if (n < 3 || p[0] != 0x01) return false;
    if (p[1] == kSwFirmware) return p[2] == 0 || p[2] == kSwFlags[static_cast<int>(HidppLive::Take)];
    for (uint8_t flag : kSwFlags) {
        if (p[1] == kSwSoftware && p[2] == flag) return true;
    }
    return false;
}

bool check_nv_config(const uint8_t* p, size_t n)
{
    if (!p || n < 3) return false;
    uint16_t cap = static_cast<uint16_t>((p[1] << 8) | p[2]);
    if (cap != kHidppNvStartup && cap != kHidppNvShutdown) return false;
    if (p[0] == kNvGet) return n == 3;
    return p[0] == kNvSet && n == kHidppNvLen && (p[3] == kNvOn || p[3] == kNvOff);
}

void put16_be(uint8_t* at, uint16_t value)
{
    at[0] = static_cast<uint8_t>((value >> 8) & 0xff);
    at[1] = static_cast<uint8_t>(value & 0xff);
}

uint16_t get16_le(const uint8_t* at)
{
    return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

} // namespace

const HidppModel* hidpp_model(uint16_t receiver, uint16_t product)
{
    for (const auto& m : kHidppModels) {
        if (m.receiver == receiver && m.product == product) return &m;
    }
    return nullptr;
}

bool hidpp_allowed(uint16_t feature, uint8_t function, const uint8_t* params, size_t n)
{
    using CheckFn = bool (*)(const uint8_t*, size_t);
    static const struct { uint16_t feature; uint8_t function; CheckFn check; } kAllowed[] = {
        { HIDPP_ROOT,        kRootGetFeature,  nullptr },
        { HIDPP_ROOT,        kRootPing,        nullptr },
        { HIDPP_BRIGHTNESS,  kBrightnessInfo,  nullptr },
        { HIDPP_BRIGHTNESS,  kBrightnessGet,   nullptr },
        { HIDPP_BRIGHTNESS,  kBrightnessSet,   nullptr },
        { HIDPP_BRIGHTNESS,  kBrightnessLight, nullptr },
        { HIDPP_RGB_EFFECTS, kRgbInfo,         nullptr },
        { HIDPP_RGB_EFFECTS, kRgbSetEffect,    check_one_group },
        { HIDPP_RGB_EFFECTS, kRgbNvConfig,     check_nv_config },
        { HIDPP_RGB_EFFECTS, kRgbSwControl,    check_sw_control },
        { HIDPP_PROFILES,    kProfilesInfo,    nullptr },
        { HIDPP_PROFILES,    kProfilesMode,    nullptr },
        { HIDPP_PROFILES,    kProfilesCurrent, nullptr },
        { HIDPP_PROFILES,    kProfilesRead,    nullptr },
        { HIDPP_PROFILES,    kProfilesBegin,   check_whole_sector },
        { HIDPP_PROFILES,    kProfilesWrite,   check_one_chunk },
        { HIDPP_PROFILES,    kProfilesEnd,     check_nothing },
    };
    for (const auto& a : kAllowed) {
        if (a.feature == feature && a.function == function) {
            return !a.check || a.check(params, n);
        }
    }
    return false;
}

size_t hidpp_packet(uint8_t sw, uint8_t index, uint8_t feature_index, uint8_t function,
                    const uint8_t* params, size_t n, uint8_t out[kHidppLongLen])
{
    if (n > kHidppParams || function > 0x0f) return 0;
    size_t len = (n <= kHidppShortLen - 4) ? kHidppShortLen : kHidppLongLen;
    std::memset(out, 0, kHidppLongLen);
    out[0] = (len == kHidppShortLen) ? kHidppShort : kHidppLong;
    out[1] = index;
    out[2] = feature_index;
    out[3] = static_cast<uint8_t>((function << 4) | (sw & 0x0f));
    if (n && params) std::memcpy(out + 4, params, n);
    return len;
}

HidppMatch hidpp_match(const uint8_t* r, size_t len, const uint8_t* packet)
{
    if (!r || !packet || len < 5 || (r[0] != kHidppShort && r[0] != kHidppLong) || r[1] != packet[1]) {
        return HidppMatch::Other;
    }
    if (r[2] == packet[2] && r[3] == packet[3]) return HidppMatch::Answer;
    if (len >= 6 && r[3] == packet[2] && r[4] == packet[3]) {
        if (r[2] == 0xff) return HidppMatch::Error;
        if (r[2] == 0x8f) return HidppMatch::ReceiverError;
    }
    return HidppMatch::Other;
}

uint16_t hidpp_crc(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i] << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

bool hidpp_sector_valid(const uint8_t* sector, size_t size)
{
    if (!sector || size <= 2) return false;
    uint16_t stored = static_cast<uint16_t>((sector[size - 2] << 8) | sector[size - 1]);
    return hidpp_crc(sector, size - 2) == stored;
}

int hidpp_keys_slot(size_t profile_size)
{
    return (profile_size >= kKeysSlot + kHidppSlot + 2) ? static_cast<int>(kKeysSlot) : -1;
}

bool hidpp_kept(KeyLight effect)
{
    return effect == KeyLight::Wave || effect == KeyLight::Ripple || effect == KeyLight::Own;
}

void hidpp_slot_decode(const uint8_t* slot, HidppLighting* out)
{
    *out = HidppLighting{};
    switch (slot[0]) {
    case FX_OFF:       out->effect = KeyLight::Off; break;
    case FX_SOLID:     out->effect = KeyLight::Solid; break;
    case FX_BREATHING: out->effect = KeyLight::Breathing; break;
    case FX_CYCLE_OLD:
    case FX_CYCLE:     out->effect = KeyLight::Cycle; break;
    case FX_WAVE_OLD:
    case FX_WAVE:      out->effect = KeyLight::Wave; break;
    case FX_RIPPLE_OLD:
    case FX_RIPPLE:    out->effect = KeyLight::Ripple; break;
    case FX_OWN:
    case FX_OWN_SAVING:out->effect = KeyLight::Own; break;
    default:           out->effect = KeyLight::Other; break;
    }
    if (key_light_coloured(out->effect)) std::memcpy(out->rgb, slot + 1, 3);
}

int hidpp_effect(const HidppLighting* l, bool dim, uint8_t p[10])
{
    uint8_t intensity = dim ? (kFull / 2) : kFull;
    std::memset(p, 0, 10);
    if (key_light_coloured(l->effect)) {
        for (int i = 0; i < 3; ++i) p[i] = dim ? (l->rgb[i] / 2) : l->rgb[i];
    }
    switch (l->effect) {
    case KeyLight::Off:
        return FX_OFF;
    case KeyLight::Solid:
        p[3] = kNoRamp;
        return FX_SOLID;
    case KeyLight::Breathing:
        put16_be(p + 3, kBreathMs);
        p[6] = intensity;
        return FX_BREATHING;
    case KeyLight::Cycle:
        p[1] = kSaturated;
        put16_be(p + 6, kCycleMs);
        p[8] = intensity;
        return FX_CYCLE;
    case KeyLight::Wave:
        p[1] = kSaturated;
        put16_be(p + 6, kWaveMs);
        p[8] = intensity;
        p[9] = kWaveRight;
        return FX_WAVE;
    case KeyLight::Ripple:
        p[3] = kSaturated;
        put16_be(p + 6, kRipplePeriod);
        return FX_RIPPLE;
    case KeyLight::Own:
        return dim ? FX_OWN_SAVING : FX_OWN;
    default:
        return -1;
    }
}

bool hidpp_slot_encode(const HidppLighting* l, bool saving, uint8_t slot[kHidppSlot])
{
    if (!hidpp_kept(l->effect)) return false;
    slot[0] = static_cast<uint8_t>(hidpp_effect(l, saving, slot + 1));
    return true;
}

static void put_slot(uint8_t* at, const HidppLighting* l, const uint8_t fresh[kHidppSlot])
{
    if (at[0] != fresh[0]) std::memcpy(at, fresh, kHidppSlot);
    else if (key_light_coloured(l->effect)) std::memcpy(at + 1, fresh + 1, 3);
}

bool hidpp_profile_lit(const uint8_t* sector, size_t size, const HidppLighting* l, uint8_t* out)
{
    uint8_t on[kHidppSlot], saving[kHidppSlot];
    if (size > kHidppSectorMax || size < kSavingSlot + kHidppSlot + 2 ||
        !hidpp_sector_valid(sector, size) ||
        !hidpp_slot_encode(l, false, on) || !hidpp_slot_encode(l, true, saving)) {
        return false;
    }
    std::memmove(out, sector, size);
    put_slot(out + kKeysSlot, l, on);
    put_slot(out + kSavingSlot, l, saving);
    put16_be(out + size - 2, hidpp_crc(out, size - 2));
    return true;
}

bool hidpp_profile_timed(const uint8_t* sector, size_t size, int dim_s, int off_s, uint8_t* out)
{
    if (size > kHidppSectorMax || size < kTimersAt + 4 + 2 || !hidpp_sector_valid(sector, size) ||
        dim_s < 1 || dim_s > kHidppTimerMax || off_s < 1 || off_s > kHidppTimerMax) {
        return false;
    }
    std::memmove(out, sector, size);
    const uint8_t times[4] = {
        static_cast<uint8_t>(dim_s & 0xff), static_cast<uint8_t>((dim_s >> 8) & 0xff),
        static_cast<uint8_t>(off_s & 0xff), static_cast<uint8_t>((off_s >> 8) & 0xff)
    };
    std::memcpy(out + kTimersAt, times, sizeof(times));
    put16_be(out + size - 2, hidpp_crc(out, size - 2));
    return true;
}

int hidpp_dark_after(int dim_s, int off_s)
{
    if (dim_s < 1 || dim_s > kHidppTimerMax || off_s < 1 || off_s > kHidppTimerMax) return -1;
    return std::max(off_s - dim_s, 0);
}

bool hidpp_nv_decode(uint16_t cap, const uint8_t* a, size_t n, HidppNv* out)
{
    *out = HidppNv{};
    if (!a || n < 4 + kHidppNvColours || a[0] != kNvGet ||
        static_cast<uint16_t>((a[1] << 8) | a[2]) != cap ||
        (a[3] != kNvOn && a[3] != kNvOff)) {
        return false;
    }
    out->known = true;
    out->on = (a[3] == kNvOn);
    std::memcpy(out->colours, a + 4, kHidppNvColours);
    return true;
}

void hidpp_nv_encode(uint16_t cap, bool on, const HidppNv* was, uint8_t out[kHidppNvLen])
{
    static const uint8_t none[kHidppNvColours] = {};
    const uint8_t* colours = (was && was->known && std::memcmp(was->colours, none, sizeof(none)) != 0)
        ? was->colours
        : (cap == kHidppNvStartup ? kStartupColours : kShutdownColours);
    out[0] = kNvSet;
    put16_be(out + 1, cap);
    out[3] = on ? kNvOn : kNvOff;
    std::memcpy(out + 4, colours, kHidppNvColours);
}

void hidpp_decode_brightness(const uint8_t* info, const uint8_t* now, HidppKeyboard* out)
{
    out->has_brightness = false;
    if (!info) return;
    out->brightness_max = (info[0] << 8) | info[1];
    out->brightness_steps = info[2] & 0x0f;
    out->brightness_min = (info[4] << 8) | info[5];
    if (out->brightness_max <= out->brightness_min) return;
    out->has_brightness = (now != nullptr);
    if (now) {
        out->brightness = std::clamp((now[0] << 8) | now[1], 0, out->brightness_max);
    }
}

void hidpp_decode_profile(const uint8_t* sector, size_t size, HidppKeyboard* out)
{
    int slot = hidpp_keys_slot(size);
    out->has_profile = (size <= kHidppSectorMax && slot >= 0 && hidpp_sector_valid(sector, size));
    if (!out->has_profile) return;
    std::memcpy(out->profile, sector, size);
    out->profile_size = size;
    hidpp_slot_decode(sector + slot, &out->light);
    out->dim_s = static_cast<int>(get16_le(sector + kTimersAt));
    out->off_s = static_cast<int>(get16_le(sector + kTimersAt + 2));
}

static int percent_of(const HidppKeyboard* k, int value)
{
    int range = k->brightness_max - k->brightness_min;
    if (range <= 0) return 0;
    return std::clamp(static_cast<int>((value - k->brightness_min) * 100.0 / range + 0.5), 0, 100);
}

void hidpp_decode(const HidppKeyboard* k, const HidppModel* model, KeyboardSettings* out)
{
    *out = KeyboardSettings{};
    out->has_brightness = k->has_brightness;
    out->levels = 0;
    if (k->has_brightness) {
        out->brightness = percent_of(k, k->brightness);
        if (model && model->levels > 1) {
            out->levels = model->levels;
            for (int i = 0; i < model->levels; ++i) {
                out->level[i] = percent_of(k, model->level[i]);
            }
        }
    }
    out->has_lighting = k->has_profile;
    out->app_controls = k->app_controls;
    out->light = k->has_profile ? k->light.effect : KeyLight::Own;
    std::memcpy(out->rgb, k->light.rgb, 3);
    out->has_timers = k->has_profile;
    out->dim_s = k->dim_s;
    out->off_s = k->off_s;
    out->has_startup = k->startup.known;
    out->startup = k->startup.on;
    out->has_shutdown = k->shutdown.known;
    out->shutdown = k->shutdown.on;
}

int hidpp_brightness_value(const HidppKeyboard* k, int percent)
{
    int range = k->brightness_max - k->brightness_min;
    return k->brightness_min + static_cast<int>(std::clamp(percent, 0, 100) * range / 100.0 + 0.5);
}

bool hidpp_event(const uint8_t* r, size_t len, uint8_t index, const HidppKeyboard* k, HidppEvent* out)
{
    if (!r || !k || !out || len < kHidppShortLen ||
        (r[0] != kHidppShort && r[0] != kHidppLong) || r[1] != index) {
        return false;
    }
    if (r[0] == kHidppShort && r[2] == kConnection) {
        out->kind = HidppEventKind::Link;
        out->linked = !(r[4] & kLinkDown);
        return true;
    }
    if (r[2] == 0 || r[2] == 0xff || r[2] == 0x8f || (r[3] & 0x0f) != 0) return false;
    if (k->status_index && r[2] == k->status_index) {
        if (r[4] != kStatusReconn) return false;
        out->kind = HidppEventKind::Link;
        out->linked = true;
        return true;
    }
    if (k->brightness_index && r[2] == k->brightness_index && r[3] == 0x00) {
        out->kind = HidppEventKind::Brightness;
        out->brightness = (r[4] << 8) | r[5];
        return true;
    }
    if ((k->brightness_index && r[2] == k->brightness_index) ||
        (k->profiles_index && r[2] == k->profiles_index)) {
        out->kind = HidppEventKind::Changed;
        return true;
    }
    if (k->rgb_index && r[2] == k->rgb_index && r[3] == 0x10 && r[4] <= 1) {
        out->kind = r[4] ? HidppEventKind::Active : HidppEventKind::Idle;
        return true;
    }
    return false;
}

void hidpp_sample(HidppKeyboard* out)
{
    static const uint8_t info[] = { 0x00, 0x64, 0x05, 0x03, 0x00, 0x00 };
    static const uint8_t now[]  = { 0x00, 0x0c };
    uint8_t prof[255];
    std::memset(prof, 0xff, sizeof(prof));
    static const uint8_t head[48] = {
        0x01, 0x01, 0x00, 0x90, 0x01, 0x20, 0x03, 0x40, 0x06, 0x80, 0x0c, 0x00, 0x00, 0xff, 0xff, 0xff,
        0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3c, 0x00, 0x2c, 0x01,
        0x80, 0x02, 0x00, 0x3a, 0x80, 0x02, 0x00, 0x3b, 0x80, 0x02, 0x00, 0x3c, 0x80, 0x02, 0x00, 0x3d,
    };
    static const uint8_t mid[32] = {
        0x80, 0x02, 0x00, 0x3e, 0x80, 0x02, 0x00, 0x3f, 0x80, 0x02, 0x00, 0x40, 0x80, 0x02, 0x00, 0x41,
        0x80, 0x02, 0x00, 0x42, 0x80, 0x02, 0x00, 0x43, 0x80, 0x02, 0x00, 0x44, 0x80, 0x02, 0x00, 0x45,
    };
    static const uint8_t tail[47] = {
        0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0xff, 0x00, 0x11,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x0a, 0x00, 0x00, 0xff, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xa9, 0xf7,
    };
    std::memcpy(prof, head, sizeof(head));
    std::memcpy(prof + 48, mid, sizeof(mid));
    std::memcpy(prof + 0xd0, tail, sizeof(tail));

    *out = HidppKeyboard{};
    hidpp_decode_brightness(info, now, out);
    out->sector = kHidppSector;
    hidpp_decode_profile(prof, sizeof(prof), out);
    out->startup.known = true;
    out->startup.on = true;
    std::memcpy(out->startup.colours, kStartupColours, kHidppNvColours);
    out->shutdown.known = true;
    out->shutdown.on = false;
    out->status_index = 0x04;
    out->rgb_index = 0x08;
    out->brightness_index = 0x0f;
    out->profiles_index = 0x10;
}

#ifdef _WIN32
// ================================================ Win32 HID & Device Manager
namespace {

std::string wide_to_utf8_dev(const wchar_t* ws)
{
    if (!ws || !*ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool hid_write_overlapped(HANDLE h, const uint8_t* data, DWORD len, DWORD timeout_ms)
{
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, len, &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, timeout_ms) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(h, &ov, &written, FALSE);
        } else {
            CancelIo(h);
        }
    }
    CloseHandle(ev);
    return ok && written == len;
}

bool hid_read_overlapped(HANDLE h, uint8_t* out, DWORD len, DWORD timeout_ms, DWORD& got)
{
    got = 0;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    BOOL ok = ReadFile(h, out, len, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, timeout_ms) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(h, &ov, &got, FALSE);
        } else {
            CancelIo(h);
        }
    }
    CloseHandle(ev);
    return ok && got > 0;
}

bool hyperx_ask_win(HANDLE h, const uint8_t req[kHyperxReport], uint8_t ans[kHyperxReport], DWORD timeout_ms)
{
    Sleep(10);
    if (!hid_write_overlapped(h, req, kHyperxReport, timeout_ms)) return false;
    ULONGLONG end = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < end) {
        DWORD left = static_cast<DWORD>(end - GetTickCount64());
        uint8_t r[kHyperxReport] = {};
        DWORD got = 0;
        if (!hid_read_overlapped(h, r, kHyperxReport, left, got)) return false;
        if (got >= 2 && r[0] == req[0] + 1 && r[1] == req[1]) {
            std::memcpy(ans, r, kHyperxReport);
            return true;
        }
    }
    return false;
}

bool hyperx_send_ack_win(HANDLE h, const uint8_t req[kHyperxReport], DWORD timeout_ms)
{
    Sleep(10);
    if (!hid_write_overlapped(h, req, kHyperxReport, timeout_ms)) return false;
    ULONGLONG end = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < end) {
        DWORD left = static_cast<DWORD>(end - GetTickCount64());
        uint8_t r[kHyperxReport] = {};
        DWORD got = 0;
        if (!hid_read_overlapped(h, r, kHyperxReport, left, got)) return false;
        if (got > 16 && r[0] == kHxAck && r[1] == 0x01 && r[14] == req[0] && r[15] == req[1] && r[16] == 0) {
            return true;
        }
    }
    return false;
}

void read_hyperx_device_win(DeviceEntry& dev, HyperxState& state, bool read_full_settings)
{
    HANDLE h = CreateFileW(dev.hid_path_primary.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    const HyperxModel* model = hyperx_model(dev.usb_vid, dev.usb_pid);
    bool linked = true;
    if (model && model->receiver) {
        uint8_t q[kHyperxReport] = { kHxLink, 0x01 };
        uint8_t a[kHyperxReport] = {};
        linked = hyperx_ask_win(h, q, a, 250) && (a[2] == 1);
    }
    dev.asleep = !linked;
    dev.connected = linked;

    if (linked) {
        uint8_t qb[kHyperxReport] = { kHxBattery, 0x02 };
        uint8_t ab[kHyperxReport] = {};
        if (hyperx_ask_win(h, qb, ab, 400)) {
            int lvl = -1;
            bool chg = false;
            if (hyperx_battery_parse(ab, kHyperxReport, &lvl, &chg)) {
                dev.battery = lvl;
                dev.charging = chg;
            }
        }
        if (read_full_settings) {
            uint8_t q_dpi[kHyperxReport] = { kHxDpi, kHxGet, 0 };
            uint8_t q_sen[kHyperxReport] = { kHxSensor, kHxGet, 0 };
            uint8_t q_bri[kHyperxReport] = { kHxBrightness, kHxGet, 0 };
            uint8_t q_lit[kHyperxReport] = { kHxLighting, kHxLightGet, 0 };
            if (hyperx_ask_win(h, q_dpi, state.dpi, 500) &&
                hyperx_ask_win(h, q_sen, state.sensor, 500) &&
                hyperx_ask_win(h, q_bri, state.brightness, 500) &&
                hyperx_ask_win(h, q_lit, state.light, 500)) {
                hyperx_decode(model, &state, &dev.mouse);
            }
        }
    }
    CloseHandle(h);
}

} // namespace

DevicesManager::DevicesManager(bool dry_run)
    : dry_run_(dry_run)
{
    hyperx_sample(&hyperx_state_);
    hidpp_sample(&hidpp_state_);
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker_ = std::thread(&DevicesManager::worker_loop, this);
}

DevicesManager::~DevicesManager()
{
    stop_.store(true);
    if (wake_event_) SetEvent(wake_event_);
    if (worker_.joinable()) worker_.join();
    if (write_thread_.joinable()) write_thread_.join();
    if (wake_event_) CloseHandle(wake_event_);
}

void DevicesManager::set_wanted(bool want_list, bool want_mouse_settings, bool want_keyboard_settings)
{
    bool prev_l = want_list_.exchange(want_list);
    bool prev_m = want_mouse_.exchange(want_mouse_settings);
    bool prev_k = want_kbd_.exchange(want_keyboard_settings);
    if ((want_list && !prev_l) || (want_mouse_settings && !prev_m) || (want_keyboard_settings && !prev_k)) {
        if (wake_event_) SetEvent(wake_event_);
    }
}

void DevicesManager::request_refresh()
{
    if (wake_event_) SetEvent(wake_event_);
}

std::vector<DeviceEntry> DevicesManager::list() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return devices_;
}

bool DevicesManager::any_of(DevKind kind) const
{
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& d : devices_) {
        if (d.kind == kind) return true;
    }
    return false;
}

void DevicesManager::scan_devices(bool read_mouse_cfg, bool read_kbd_cfg)
{
    if (dry_run_) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!dry_seeded_) {
            dry_seeded_ = true;
            devices_.clear();

            DeviceEntry hp;
            hp.id = "bt-airpods";
            hp.name = "AirPods Pro";
            hp.vendor_name = "Apple";
            hp.kind = DevKind::Audio;
            hp.link = DevLink::Bluetooth;
            hp.bluetooth = true;
            hp.connected = true;
            hp.battery = 90;
            devices_.push_back(hp);

            DeviceEntry kbd;
            kbd.id = "usb-g715";
            kbd.name = "G715 Wireless Gaming Keyboard";
            kbd.vendor_name = "Logitech";
            kbd.kind = DevKind::Keyboard;
            kbd.link = DevLink::Receiver;
            kbd.connected = true;
            kbd.has_settings = true;
            hidpp_decode(hidpp_model(0xc547, 0x40a2) ? &hidpp_state_ : &hidpp_state_,
                         hidpp_model(0xc547, 0x40a2), &kbd.keyboard);
            devices_.push_back(kbd);

            DeviceEntry mse;
            mse.id = "usb-hyperx";
            mse.name = "HyperX Pulsefire Haste 2 Wireless";
            mse.vendor_name = "HyperX";
            mse.kind = DevKind::Mouse;
            mse.link = DevLink::Receiver;
            mse.connected = true;
            mse.battery = 82;
            mse.has_settings = true;
            hyperx_decode(hyperx_model(0x03f0, 0x0f98), &hyperx_state_, &mse.mouse);
            devices_.push_back(mse);
        }
        return;
    }

    std::wstring hx_recv_path, hx_cable_path;
    std::wstring g715_short_path, g715_long_path;

    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA did = {};
        did.cbSize = sizeof(did);
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &hid_guid, i, &did); ++i) {
            DWORD req = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &did, nullptr, 0, &req, nullptr);
            if (req == 0) continue;
            std::vector<uint8_t> buf(req);
            auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(info, &did, det, req, &req, nullptr)) {
                HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    HIDD_ATTRIBUTES attr = {};
                    attr.Size = sizeof(attr);
                    if (HidD_GetAttributes(h, &attr)) {
                        PHIDP_PREPARSED_DATA ppd = nullptr;
                        HIDP_CAPS caps = {};
                        if (HidD_GetPreparsedData(h, &ppd) && ppd) {
                            HidP_GetCaps(ppd, &caps);
                            HidD_FreePreparsedData(ppd);
                        }
                        if (attr.VendorID == 0x03f0 && attr.ProductID == 0x0f98 &&
                            caps.UsagePage == 0xff00 && caps.Usage == 0x0001) {
                            hx_recv_path = det->DevicePath;
                        } else if (attr.VendorID == 0x03f0 && attr.ProductID == 0x0d97 &&
                                   caps.UsagePage == 0xff00 && caps.Usage == 0x0001) {
                            hx_cable_path = det->DevicePath;
                        } else if (attr.VendorID == 0x046d && attr.ProductID == 0xc547 &&
                                   caps.UsagePage == 0xff00) {
                            if (caps.Usage == 0x0001) g715_short_path = det->DevicePath;
                            else if (caps.Usage == 0x0002) g715_long_path = det->DevicePath;
                        }
                    }
                    CloseHandle(h);
                }
            }
        }
        SetupDiDestroyDeviceInfoList(info);
    }

    std::vector<DeviceEntry> found;

    // 1. Bluetooth audio headphones (if paired)
    BLUETOOTH_DEVICE_SEARCH_PARAMS bsp = {};
    bsp.dwSize = sizeof(bsp);
    bsp.fReturnAuthenticated = TRUE;
    bsp.fReturnRemembered = TRUE;
    bsp.fReturnConnected = TRUE;
    BLUETOOTH_DEVICE_INFO bdi = {};
    bdi.dwSize = sizeof(bdi);
    HBLUETOOTH_DEVICE_FIND hbf = BluetoothFindFirstDevice(&bsp, &bdi);
    if (hbf) {
        do {
            // Major Device Class 0x04 = Audio/Video
            ULONG major = (bdi.ulClassofDevice >> 8) & 0x1f;
            if (major == 0x04) {
                DeviceEntry d;
                d.id = "bt-audio-" + wide_to_utf8_dev(bdi.szName);
                d.name = wide_to_utf8_dev(bdi.szName);
                if (d.name.empty()) d.name = "Bluetooth Headphones";
                d.kind = DevKind::Audio;
                d.link = DevLink::Bluetooth;
                d.bluetooth = true;
                d.connected = (bdi.fConnected != FALSE);
                found.push_back(d);
            }
        } while (BluetoothFindNextDevice(hbf, &bdi));
        BluetoothFindDeviceClose(hbf);
    }

    // 2. Logitech G715 on its LIGHTSPEED receiver
    if (!g715_short_path.empty() || !g715_long_path.empty()) {
        DeviceEntry kbd;
        kbd.id = "usb-046d-c547";
        kbd.name = "G715 Wireless Gaming Keyboard";
        kbd.vendor_name = "Logitech";
        kbd.kind = DevKind::Keyboard;
        kbd.link = DevLink::Receiver;
        kbd.connected = true;
        kbd.has_settings = true;
        kbd.hid_path_primary = g715_short_path;
        kbd.hid_path_secondary = g715_long_path;
        kbd.usb_vid = 0x046d;
        kbd.usb_pid = 0xc547;
        hidpp_decode(&hidpp_state_, hidpp_model(0xc547, 0x40a2), &kbd.keyboard);
        (void)read_kbd_cfg;
        found.push_back(kbd);
    }

    // 3. HyperX Pulsefire Haste 2 Wireless (cable takes precedence over receiver, as one card)
    if (!hx_cable_path.empty() || !hx_recv_path.empty()) {
        bool on_cable = !hx_cable_path.empty();
        DeviceEntry mse;
        mse.id = "usb-03f0-haste2";
        mse.name = "HyperX Pulsefire Haste 2 Wireless";
        mse.vendor_name = "HyperX";
        mse.kind = DevKind::Mouse;
        mse.link = on_cable ? DevLink::Usb : DevLink::Receiver;
        mse.connected = true;
        mse.has_settings = true;
        mse.hid_path_primary = on_cable ? hx_cable_path : hx_recv_path;
        mse.usb_vid = 0x03f0;
        mse.usb_pid = on_cable ? 0x0d97 : 0x0f98;
        hyperx_decode(hyperx_model(mse.usb_vid, mse.usb_pid), &hyperx_state_, &mse.mouse);
        read_hyperx_device_win(mse, hyperx_state_, read_mouse_cfg);
        found.push_back(mse);
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        devices_ = found;
    }
}

void DevicesManager::apply_mouse_async(const std::string& id, const MouseSettings& want)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& d : devices_) {
            if (d.id == id) {
                uint8_t pkts[kHyperxBlocks][kHyperxReport];
                int n = hyperx_packets(&hyperx_state_, &want, 0, pkts);
                for (int i = 0; i < n; ++i) hyperx_apply(&hyperx_state_, pkts[i]);
                d.mouse = want;
                break;
            }
        }
    }
    HWND hwnd = notify_hwnd_.load();
    if (hwnd && !stop_.load()) PostMessageW(hwnd, kMsgDevicesUpdated, 0, 0);

    if (dry_run_) return;

    if (write_thread_.joinable()) write_thread_.join();
    write_thread_ = std::thread([this, id, want]() {
        std::wstring path;
        HyperxState now_copy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            now_copy = hyperx_state_;
            for (const auto& d : devices_) {
                if (d.id == id) { path = d.hid_path_primary; break; }
            }
        }
        if (path.empty()) return;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        for (uint16_t slot = 0; slot < 2; ++slot) {
            uint8_t pkts[kHyperxBlocks][kHyperxReport];
            int n = hyperx_packets(&now_copy, &want, slot, pkts);
            for (int i = 0; i < n; ++i) {
                if (slot == 0) hyperx_send_ack_win(h, pkts[i], 500);
                else hid_write_overlapped(h, pkts[i], kHyperxReport, 500);
            }
        }
        CloseHandle(h);
    });
}

void DevicesManager::apply_keyboard_async(const std::string& id, const KeyboardSettings& want)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& d : devices_) {
            if (d.id == id) {
                d.keyboard = want;
                break;
            }
        }
    }
    HWND hwnd = notify_hwnd_.load();
    if (hwnd && !stop_.load()) PostMessageW(hwnd, kMsgDevicesUpdated, 0, 0);
}

void DevicesManager::worker_loop()
{
    // Perform one initial fast scan so sidebar tabs appear right at launch.
    scan_devices(false, false);
    if (HWND h = notify_hwnd_.load()) PostMessageW(h, kMsgDevicesUpdated, 0, 0);

    while (!stop_.load()) {
        if (!want_list_.load() && !want_mouse_.load() && !want_kbd_.load()) {
            WaitForSingleObject(wake_event_, INFINITE);
            continue;
        }
        scan_devices(want_mouse_.load(), want_kbd_.load());
        HWND hwnd = notify_hwnd_.load();
        if (hwnd && !stop_.load()) {
            PostMessageW(hwnd, kMsgDevicesUpdated, 0, 0);
        }
        WaitForSingleObject(wake_event_, 5000);
    }
}

#endif // _WIN32

} // namespace lares
