// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The exchanges of src/hyperx.c and src/hidpp.c over Win32 HID. Each open
 * HID handle gets its own copy of every input report, so a handle that
 * listens for events never takes an answer meant for another, and the other
 * way round. */
#include "hid_win.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include <cfgmgr32.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}

namespace lares {

using namespace wire;

// ================================================================ finding

std::vector<HidCollection> hid_collections()
{
    std::vector<HidCollection> out;
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA did = {};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &hid_guid, i, &did); ++i) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &did, nullptr, 0, &req, nullptr);
        if (req == 0) continue;
        std::vector<uint8_t> buf(req);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev = {};
        dev.cbSize = sizeof(dev);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &did, det, req, &req, &dev)) continue;
        // No access asked: enough for the attributes, and it never disturbs the device.
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HidCollection c;
        HIDD_ATTRIBUTES attr = {};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr)) {
            c.vid = attr.VendorID;
            c.pid = attr.ProductID;
        }
        PHIDP_PREPARSED_DATA ppd = nullptr;
        if (HidD_GetPreparsedData(h, &ppd) && ppd) {
            HIDP_CAPS caps = {};
            if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                c.usage_page = caps.UsagePage;
                c.usage = caps.Usage;
            }
            HidD_FreePreparsedData(ppd);
        }
        CloseHandle(h);
        bool known = c.vid == 0x03f0 || c.vid == 0x046d;
        bool lamps = c.usage_page == kUsagePageLighting && c.usage == kUsageLampArray;
        if (!known && !lamps) continue;
        DEVINST parent = 0;
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Parent(&parent, dev.DevInst, 0) == CR_SUCCESS &&
            CM_Get_Device_IDW(parent, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
            c.parent = id;
        }
        c.path = det->DevicePath;
        out.push_back(std::move(c));
    }
    SetupDiDestroyDeviceInfoList(info);
    return out;
}

static bool contains_nocase(const std::wstring& hay, const wchar_t* needle)
{
    std::wstring a = hay, b = needle;
    for (auto& ch : a) ch = static_cast<wchar_t>(towupper(ch));
    for (auto& ch : b) ch = static_cast<wchar_t>(towupper(ch));
    return a.find(b) != std::wstring::npos;
}

HidFound hid_find(const std::vector<HidCollection>& all)
{
    HidFound f;
    for (const auto& c : all) {
        bool lamps = c.usage_page == kUsagePageLighting && c.usage == kUsageLampArray;
        if (c.vid == 0x03f0 && c.pid == 0x0f98) {
            if (c.usage_page == kUsagePageVendor && c.usage == 0x0001) f.hyperx_receiver = c.path;
            else if (lamps) f.hyperx_lamps_receiver = c.path;
        } else if (c.vid == 0x03f0 && c.pid == 0x0d97) {
            if (c.usage_page == kUsagePageVendor && c.usage == 0x0001) f.hyperx_cable = c.path;
            else if (lamps) f.hyperx_lamps_cable = c.path;
        } else if (c.vid == 0x046d && c.pid == 0xc547 && c.usage_page == kUsagePageVendor) {
            if (c.usage == 0x0001) f.hidpp_short = c.path;
            else if (c.usage == 0x0002) f.hidpp_long = c.path;
        }
        /* The Logitech LampArray Service, from Windows Update, paints a
         * Lightspeed keyboard through a virtual LampArray under the
         * receiver: its first slot is the keyboard. */
        if (lamps && contains_nocase(c.parent, L"VID_046D&PID_C547&LAMPARRAY") &&
            (f.keyboard_lamps.empty() || contains_nocase(c.parent, L"SLOT00"))) {
            f.keyboard_lamps = c.path;
        }
    }
    return f;
}

std::wstring lighting_key_name(const std::wstring& lamp_path)
{
    static const wchar_t kPrefix[] = L"\\\\?\\";
    std::wstring name = lamp_path;
    if (name.compare(0, 4, kPrefix) == 0) name.erase(0, 4);
    return name;
}

static bool read_dword(const std::wstring& key, DWORD& value)
{
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"AmbientLightingEnabled", RRF_RT_REG_DWORD,
                        nullptr, &value, &size) == ERROR_SUCCESS;
}

bool windows_lighting_on(const std::wstring& lamp_path)
{
    if (lamp_path.empty()) return false;
    static const wchar_t kLighting[] = L"Software\\Microsoft\\Lighting";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLighting, 0, KEY_READ, &root) != ERROR_SUCCESS) return false;
    RegCloseKey(root);
    DWORD all = 1;   // Windows turns it on until someone turns it off
    if (read_dword(kLighting, all) && all == 0) return false;
    DWORD one = 1;
    std::wstring key = std::wstring(kLighting) + L"\\Devices\\" + lighting_key_name(lamp_path);
    if (read_dword(key, one)) return one != 0;
    return true;
}

// ================================================================= a port

HidPort::~HidPort()
{
    close();
}

bool HidPort::open(const std::wstring& path, bool write)
{
    close();
    if (path.empty()) return false;
    h_ = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) return false;
    PHIDP_PREPARSED_DATA ppd = nullptr;
    if (HidD_GetPreparsedData(h_, &ppd) && ppd) {
        HIDP_CAPS caps = {};
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength > 0) {
            in_len_ = std::min<DWORD>(caps.InputReportByteLength, sizeof(buf_));
        }
        HidD_FreePreparsedData(ppd);
    }
    read_ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!read_ev_) {
        close();
        return false;
    }
    return true;
}

void HidPort::close()
{
    if (h_ != INVALID_HANDLE_VALUE) {
        if (pending_) {
            CancelIoEx(h_, &read_ov_);
            DWORD dummy = 0;
            GetOverlappedResult(h_, &read_ov_, &dummy, TRUE);
            pending_ = false;
        }
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
    if (read_ev_) {
        CloseHandle(read_ev_);
        read_ev_ = nullptr;
    }
}

bool HidPort::arm()
{
    if (h_ == INVALID_HANDLE_VALUE) return false;
    if (pending_) return true;
    ResetEvent(read_ev_);
    read_ov_ = {};
    read_ov_.hEvent = read_ev_;
    if (!ReadFile(h_, buf_, in_len_, nullptr, &read_ov_) && GetLastError() != ERROR_IO_PENDING) return false;
    pending_ = true;   // done at once or later, the event says when
    return true;
}

bool HidPort::take(uint8_t* out, DWORD& got)
{
    got = 0;
    if (h_ == INVALID_HANDLE_VALUE) return false;
    if (!pending_ || !HasOverlappedIoCompleted(&read_ov_)) return true;
    pending_ = false;
    DWORD n = 0;
    if (!GetOverlappedResult(h_, &read_ov_, &n, FALSE)) return false;
    std::memset(out, 0, sizeof(buf_));
    std::memcpy(out, buf_, std::min<DWORD>(n, sizeof(buf_)));
    got = n;
    return true;
}

bool HidPort::drain()
{
    uint8_t junk[64];
    DWORD got = 0;
    if (!take(junk, got)) return false;
    HidD_FlushQueue(h_);
    return true;
}

bool HidPort::write(const uint8_t* data, DWORD len, DWORD timeout_ms)
{
    if (h_ == INVALID_HANDLE_VALUE) return false;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    DWORD written = 0;
    BOOL ok = WriteFile(h_, data, len, &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, timeout_ms) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(h_, &ov, &written, FALSE);
        } else {
            CancelIoEx(h_, &ov);
            GetOverlappedResult(h_, &ov, &written, TRUE);
            ok = FALSE;
        }
    }
    CloseHandle(ev);
    return ok && written == len;
}

/* Reads reports from `ports` until `match` takes one, or `ms` pass, skipping
 * everything else: acknowledgements, events, other answers. */
template <class Match>
static bool await_report(HidPort* const* ports, int n, DWORD ms, Match match, uint8_t* out, bool* gone = nullptr)
{
    ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= end) return false;
        HANDLE evs[2] = {};
        for (int i = 0; i < n; ++i) {
            if (!ports[i]->arm()) {
                if (gone) *gone = true;
                return false;
            }
            evs[i] = ports[i]->read_event();
        }
        DWORD w = WaitForMultipleObjects(static_cast<DWORD>(n), evs, FALSE, static_cast<DWORD>(end - now));
        if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + static_cast<DWORD>(n)) return false;
        for (int i = 0; i < n; ++i) {
            uint8_t r[64];
            DWORD got = 0;
            if (!ports[i]->take(r, got)) {
                if (gone) *gone = true;
                return false;
            }
            if (got && match(r, got)) {
                if (out) std::memcpy(out, r, sizeof(r));
                return true;
            }
        }
    }
}

ScopedWinMutex::ScopedWinMutex(const wchar_t* name, DWORD timeout_ms)
{
    h = CreateMutexW(nullptr, FALSE, name);
    if (h) {
        DWORD wr = WaitForSingleObject(h, timeout_ms);
        locked = (wr == WAIT_OBJECT_0 || wr == WAIT_ABANDONED);
    }
}

ScopedWinMutex::~ScopedWinMutex()
{
    if (h) {
        if (locked) ReleaseMutex(h);
        CloseHandle(h);
    }
}

// ================================================================= HyperX

/* Packets go at least 10 ms apart and each waits for its answer, or the
 * mouse drops some over the air (src/hyperx.c). */
namespace {

constexpr DWORD kHxPaceMs = 10;
constexpr DWORD kHxAnswerMs = 500;
constexpr int kHxTries = 3;
constexpr DWORD kHxLinkMs = 1500;
constexpr DWORD kHxLinkAnswerMs = 200;
constexpr DWORD kHxLinkWaitMs = 100;

struct HxPort {
    HidPort& p;
    ULONGLONG sent = 0;
};

bool hx_send(HxPort& x, const uint8_t* packet)
{
    ULONGLONG now = GetTickCount64();
    if (x.sent && now < x.sent + kHxPaceMs) Sleep(static_cast<DWORD>(x.sent + kHxPaceMs - now));
    x.p.drain();
    bool ok = x.p.write(packet, kHyperxReport, kHxAnswerMs);
    x.sent = GetTickCount64();
    return ok;
}

bool hx_wait(HxPort& x, DWORD ms, bool (*match)(const uint8_t*, DWORD, const uint8_t*), const uint8_t* packet,
             uint8_t* out)
{
    HidPort* ports[1] = { &x.p };
    return await_report(ports, 1, ms, [&](const uint8_t* r, DWORD got) { return match(r, got, packet); }, out);
}

bool is_answer(const uint8_t* r, DWORD len, const uint8_t* packet)
{
    return len >= 2 && r[0] == packet[0] + 1 && r[1] == packet[1];
}

bool is_battery(const uint8_t* r, DWORD len, const uint8_t*)
{
    int level = 0;
    bool charging = false;
    return hyperx_battery_parse(r, len, &level, &charging);
}

bool is_ack(const uint8_t* r, DWORD len, const uint8_t* packet)
{
    return len > 16 && r[0] == kHxAck && r[1] == 0x01 && r[14] == packet[0] && r[15] == packet[1] && r[16] == 0;
}

bool hx_ask(HxPort& x, const uint8_t* packet, uint8_t* answer)
{
    return hx_send(x, packet) && hx_wait(x, kHxAnswerMs, is_answer, packet, answer);
}

bool hx_ask_live(HxPort& x, uint8_t command, uint8_t sub, uint8_t* answer)
{
    const uint8_t q[kHyperxReport] = { command, sub, 0 };
    for (int i = 0; i < kHxTries; ++i) {
        if (hx_ask(x, q, answer)) return true;
    }
    return false;
}

/* The receiver answers this itself, asleep or not; as the mouse wakes it
 * may say "not linked" for a moment, so a no is asked again for a second
 * and a half before it counts. */
HyperxResult hx_linked(HxPort& x)
{
    const uint8_t q[kHyperxReport] = { kHxLink, 0x01 };
    uint8_t a[kHyperxReport] = {};
    bool answered = false;
    ULONGLONG end = GetTickCount64() + kHxLinkMs;
    for (;;) {
        if (hx_send(x, q) && hx_wait(x, kHxLinkAnswerMs, is_answer, q, a)) {
            if (a[2] == 1) return { true, HidFail::None, {} };
            answered = true;
        }
        if (GetTickCount64() >= end) break;
        Sleep(kHxLinkWaitMs);
    }
    if (answered) return { false, HidFail::Asleep, "It's asleep. Move it to wake it." };
    return { false, HidFail::NoAnswer, "Its receiver didn't answer." };
}

HyperxResult hx_reachable(HxPort& x, const HyperxModel* model)
{
    if (!model || !model->receiver) return { true, HidFail::None, {} };
    return hx_linked(x);
}

bool hx_read_state(HxPort& x, HyperxState& s)
{
    return hx_ask_live(x, kHxDpi, kHxGet, s.dpi) && hx_ask_live(x, kHxSensor, kHxGet, s.sensor) &&
           hx_ask_live(x, kHxBrightness, kHxGet, s.brightness) && hx_ask_live(x, kHxLighting, kHxLightGet, s.light);
}

/* A change waits for the receiver's acknowledgement, which comes once the
 * mouse has it. */
bool hx_send_change(HxPort& x, const uint8_t* packet)
{
    return hx_send(x, packet) && hx_wait(x, kHxAnswerMs, is_ack, packet, nullptr);
}

bool hx_show(HxPort& x, const uint8_t rgb[3])
{
    const uint8_t frame[kHyperxReport] = { kHxFrame, 0x02, 0, 0, rgb[0], rgb[1], rgb[2] };
    return hx_send_change(x, frame);
}

bool same_light(const MouseSettings& a, const MouseSettings& b)
{
    bool coloured = a.light == LightEffect::Solid || a.light == LightEffect::Breathing;
    return a.light == b.light && (!coloured || std::memcmp(a.rgb, b.rgb, 3) == 0);
}

bool holds(const MouseSettings& got, const MouseSettings& want)
{
    if (got.rate != want.rate || got.stages != want.stages || got.active != want.active) return false;
    for (int i = 0; i < got.stages; ++i) {
        if (got.dpi[i] != want.dpi[i]) return false;
    }
    return got.lift_mm == want.lift_mm && got.brightness == want.brightness && same_light(got, want);
}

} // namespace

bool hyperx_battery_win(HidPort& p, int& level, bool& charging)
{
    HxPort x{ p };
    const uint8_t q[kHyperxReport] = { kHxBattery, 0x02 };
    uint8_t a[kHyperxReport] = {};
    /* The receiver may only wake the mouse on the first ask. */
    for (int round = 0; round < 2; ++round) {
        if (!hx_send(x, q)) return false;
        if (hx_wait(x, round == 0 ? 400 : 800, is_battery, q, a)) {
            return hyperx_battery_parse(a, sizeof(a), &level, &charging);
        }
    }
    return false;
}

HyperxResult hyperx_read_win(HidPort& p, const HyperxModel* model, HyperxState& state)
{
    HxPort x{ p };
    HyperxResult r = hx_reachable(x, model);
    if (!r.ok) return r;
    HyperxState s = state;
    if (!hx_read_state(x, s)) return { false, HidFail::NoAnswer, "It didn't answer; it may have fallen asleep." };
    state = s;
    return { true, HidFail::None, {} };
}

/* Live first, then the profile it reloads when switched on, as NGENUITY
 * saves. A change stored in profile 1 goes unacknowledged, so what follows
 * it is a question, asked again until it's answered. Then it's read back. */
HyperxResult hyperx_write_win(HidPort& p, const HyperxModel* model, const MouseSettings& base,
                              const MouseSettings& want, HyperxState& state)
{
    HxPort x{ p };
    HyperxResult r = hx_reachable(x, model);
    if (!r.ok) return r;
    HyperxState now{};
    if (!hx_read_state(x, now)) return { false, HidFail::NoAnswer, "It didn't answer; it may have fallen asleep." };
    MouseSettings was{}, merged{};
    hyperx_decode(nullptr, &now, &was);
    hyperx_merge(&now, &base, &want, &merged);
    uint8_t packets[kHyperxBlocks][kHyperxReport];
    for (uint16_t slot = 0; slot < 2; ++slot) {
        int n = hyperx_packets(&now, &merged, slot, packets);
        if (n < 0) return { false, HidFail::Refused, "The mouse can't hold that setting." };
        for (int i = 0; i < n; ++i) {
            bool sent = (slot == 0) ? hx_send_change(x, packets[i]) : hx_send(x, packets[i]);
            if (!sent) return { false, HidFail::NoAnswer, "It didn't take the change; it may have fallen asleep." };
            uint8_t answer[kHyperxReport];
            if (slot == 1 && !hx_ask_live(x, kHxDpi, kHxGet, answer)) {
                return { false, HidFail::NoAnswer, "It didn't answer; it may have fallen asleep." };
            }
        }
        /* A Solid colour's new brightness shows now rather than once the
         * mouse takes it up (issue #62). */
        if (slot == 0 && hyperx_shows(&was, &merged)) hx_show(x, merged.rgb);
    }
    HyperxState after{};
    if (!hx_read_state(x, after)) return { false, HidFail::NoAnswer, "It didn't answer; it may have fallen asleep." };
    state = after;
    MouseSettings got{};
    hyperx_decode(nullptr, &after, &got);
    if (!holds(got, merged)) return { false, HidFail::Refused, "It kept its earlier setting." };
    return { true, HidFail::None, {} };
}

// ================================================================== HID++

bool HidppLinkWin::open(const std::wstring& short_path, const std::wstring& long_path)
{
    return s.open(short_path, true) && l.open(long_path, true);
}

namespace {

constexpr DWORD kAnswerMs = 1000;
constexpr DWORD kSavedMs = 3000;   // the end of a profile write, which the keyboard stores before it answers

HidppResult fail_with(HidFail f, const char* message)
{
    return { false, f, message };
}

const HidppResult kOk = { true, HidFail::None, {} };

/* Sends function `fn` of `feature`, at `fi`, and waits up to `ms` for its
 * answer's parameters. */
HidppResult call_for(HidppLinkWin& l, DWORD ms, uint16_t feature, uint8_t fi, uint8_t fn, const uint8_t* params,
                     size_t n, uint8_t out[kHidppParams])
{
    if (!hidpp_allowed(feature, fn, params, n)) return fail_with(HidFail::Refused, "Lares doesn't send that request.");
    uint8_t packet[kHidppLongLen] = {};
    size_t len = hidpp_packet(l.sw, l.index, fi, fn, params, n, packet);
    if (!len) return fail_with(HidFail::Refused, "Lares doesn't send that request.");
    l.s.drain();
    l.l.drain();
    HidPort& port = (packet[0] == kHidppShort) ? l.s : l.l;
    if (!port.write(packet, static_cast<DWORD>(len), ms)) {
        return fail_with(HidFail::Gone, "Its receiver couldn't be asked.");
    }
    HidppMatch kind = HidppMatch::Other;
    uint8_t r[64] = {};
    DWORD got_len = 0;
    HidPort* ports[2] = { &l.s, &l.l };
    bool gone = false;
    bool found = await_report(ports, 2, ms, [&](const uint8_t* rep, DWORD got) {
        HidppMatch m = hidpp_match(rep, got, packet);
        if (m == HidppMatch::Other) return false;
        kind = m;
        got_len = got;
        return true;
    }, r, &gone);
    if (!found) {
        return gone ? fail_with(HidFail::Gone, "Its receiver is gone.")
                    : fail_with(HidFail::NoAnswer, "It didn't answer; it may be asleep.");
    }
    switch (kind) {
    case HidppMatch::Answer:
        std::memset(out, 0, kHidppParams);
        if (got_len > 4) std::memcpy(out, r + 4, std::min<size_t>(got_len - 4, kHidppParams));
        return kOk;
    case HidppMatch::Error: {
        l.refused = r[5];
        HidppResult res = fail_with(HidFail::Refused, "");
        char msg[64];
        std::snprintf(msg, sizeof(msg), "It refused the request (error %d).", r[5]);
        res.message = msg;
        return res;
    }
    default:
        return fail_with(HidFail::Asleep, "It's asleep. Press a key to wake it.");
    }
}

HidppResult call(HidppLinkWin& l, uint16_t feature, uint8_t fi, uint8_t fn, const uint8_t* params, size_t n,
                 uint8_t out[kHidppParams])
{
    return call_for(l, kAnswerMs, feature, fi, fn, params, n, out);
}

HidppResult feature_index(HidppLinkWin& l, uint16_t feature, uint8_t& index)
{
    const uint8_t id[2] = { static_cast<uint8_t>(feature >> 8), static_cast<uint8_t>(feature & 0xff) };
    uint8_t a[kHidppParams] = {};
    HidppResult r = call(l, HIDPP_ROOT, 0, kRootGetFeature, id, sizeof(id), a);
    if (r.ok) index = a[0];
    return r;
}

HidppResult read_profile(HidppLinkWin& l, uint8_t fi, HidppKeyboard& out)
{
    uint8_t info[kHidppParams] = {}, mode[kHidppParams] = {}, current[kHidppParams] = {};
    HidppResult r = call(l, HIDPP_PROFILES, fi, kProfilesInfo, nullptr, 0, info);
    if (r.ok) r = call(l, HIDPP_PROFILES, fi, kProfilesMode, nullptr, 0, mode);
    if (r.ok) r = call(l, HIDPP_PROFILES, fi, kProfilesCurrent, nullptr, 0, current);
    if (!r.ok) return r;
    size_t size = (static_cast<size_t>(info[7]) << 8) | info[8];
    out.sector = static_cast<uint16_t>((current[0] << 8) | current[1]);
    if (mode[0] == 2) out.app_controls = true;
    if (size < 16 || size > kHidppSectorMax) return kOk;
    uint8_t sector[kHidppSectorMax] = {};
    /* Sixteen bytes a time; the last read ends at the sector's end. */
    for (size_t got = 0; got < size;) {
        size_t at = std::min(got, size - 16);
        const uint8_t where[4] = {
            static_cast<uint8_t>(out.sector >> 8), static_cast<uint8_t>(out.sector & 0xff),
            static_cast<uint8_t>(at >> 8), static_cast<uint8_t>(at & 0xff)
        };
        uint8_t chunk[kHidppParams] = {};
        r = call(l, HIDPP_PROFILES, fi, kProfilesRead, where, sizeof(where), chunk);
        if (!r.ok) return r;
        std::memcpy(sector + at, chunk, 16);
        got = at + 16;
    }
    hidpp_decode_profile(sector, size, &out);
    return kOk;
}

/* A startup or shutdown effect. One never set is refused with error 1: it's
 * off. Another refusal means the keyboard hasn't it. */
HidppResult read_nv(HidppLinkWin& l, uint8_t fi, uint16_t cap, HidppNv& out)
{
    const uint8_t get[3] = { kNvGet, static_cast<uint8_t>(cap >> 8), static_cast<uint8_t>(cap & 0xff) };
    uint8_t a[kHidppParams] = {};
    out = HidppNv{};
    l.refused = 0;
    HidppResult r = call(l, HIDPP_RGB_EFFECTS, fi, kRgbNvConfig, get, sizeof(get), a);
    if (r.ok) {
        hidpp_nv_decode(cap, a, sizeof(a), &out);
        return kOk;
    }
    if (r.fail != HidFail::Refused) return r;
    out.known = (l.refused == kNeverSet);
    return kOk;
}

HidppResult read_all(HidppLinkWin& l, HidppKeyboard& out)
{
    HidppKeyboard k;
    uint8_t bi = 0, pi = 0, ri = 0;
    HidppResult r = feature_index(l, HIDPP_BRIGHTNESS, bi);
    if (r.ok) r = feature_index(l, HIDPP_PROFILES, pi);
    if (r.ok) r = feature_index(l, HIDPP_RGB_EFFECTS, ri);
    if (r.ok) r = feature_index(l, HIDPP_STATUS, k.status_index);
    if (!r.ok) return r;
    k.brightness_index = bi;
    k.profiles_index = pi;
    k.rgb_index = ri;
    if (bi) {
        uint8_t info[kHidppParams] = {}, now[kHidppParams] = {};
        r = call(l, HIDPP_BRIGHTNESS, bi, kBrightnessInfo, nullptr, 0, info);
        if (r.ok) r = call(l, HIDPP_BRIGHTNESS, bi, kBrightnessGet, nullptr, 0, now);
        if (!r.ok) return r;
        hidpp_decode_brightness(info, now, &k);
    }
    if (pi) {
        r = read_profile(l, pi, k);
        if (!r.ok) return r;
    }
    if (ri) {
        /* Software holds the lighting, another app's or the helper's: what
         * shows isn't the profile. */
        const uint8_t get[1] = { 0x00 };
        uint8_t a[kHidppParams] = {};
        r = call(l, HIDPP_RGB_EFFECTS, ri, kRgbSwControl, get, sizeof(get), a);
        if (!r.ok) return r;
        if (a[1] != 0) k.app_controls = true;
        r = read_nv(l, ri, kHidppNvStartup, k.startup);
        if (r.ok) r = read_nv(l, ri, kHidppNvShutdown, k.shutdown);
        if (!r.ok) return r;
    }
    out = k;
    return kOk;
}

HidppResult set_brightness(HidppLinkWin& l, const HidppKeyboard& k, int value)
{
    if (!k.brightness_index || !k.has_brightness) return fail_with(HidFail::Refused, "It has no brightness to set.");
    value = std::clamp(value, 0, k.brightness_max);
    const uint8_t v[2] = { static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xff) };
    uint8_t a[kHidppParams] = {};
    return call(l, HIDPP_BRIGHTNESS, k.brightness_index, kBrightnessSet, v, sizeof(v), a);
}

/* The whole sector, 16 bytes a time, the last padded; the keyboard keeps
 * the length the write began with, checks the CRC at its end, and only then
 * stores it. */
HidppResult write_profile(HidppLinkWin& l, uint8_t fi, uint16_t sector, const uint8_t* data, size_t size)
{
    const uint8_t begin[6] = {
        static_cast<uint8_t>(sector >> 8), static_cast<uint8_t>(sector & 0xff), 0x00, 0x00,
        static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size & 0xff)
    };
    uint8_t a[kHidppParams] = {};
    HidppResult r = call(l, HIDPP_PROFILES, fi, kProfilesBegin, begin, sizeof(begin), a);
    if (!r.ok) return r;
    for (size_t at = 0; at < size; at += 16) {
        uint8_t chunk[16] = {};
        std::memcpy(chunk, data + at, std::min<size_t>(16, size - at));
        r = call(l, HIDPP_PROFILES, fi, kProfilesWrite, chunk, sizeof(chunk), a);
        if (!r.ok) return r;
    }
    return call_for(l, kSavedMs, HIDPP_PROFILES, fi, kProfilesEnd, nullptr, 0, a);
}

HidppResult set_nv(HidppLinkWin& l, uint8_t fi, uint16_t cap, const HidppNv& now, int want)
{
    if (want < 0 || (now.known && now.on == (want != 0))) return kOk;
    if (!now.known) return fail_with(HidFail::Refused, "It has no such effect.");
    uint8_t set[kHidppNvLen] = {}, a[kHidppParams] = {};
    hidpp_nv_encode(cap, want != 0, &now, set);
    return call(l, HIDPP_RGB_EFFECTS, fi, kRgbNvConfig, set, sizeof(set), a);
}

HidppResult read_effects(HidppLinkWin& l, HidppEffects& out)
{
    out = HidppEffects{};
    HidppResult r = feature_index(l, HIDPP_RGB_EFFECTS, out.feature);
    if (r.ok) r = feature_index(l, HIDPP_STATUS, out.status);
    if (!r.ok) return r;
    if (!out.feature) return fail_with(HidFail::Refused, "It has no lighting to play.");
    const uint8_t device[3] = { 0xff, 0xff, 0x00 };
    uint8_t a[kHidppParams] = {};
    r = call(l, HIDPP_RGB_EFFECTS, out.feature, kRgbInfo, device, sizeof(device), a);
    if (!r.ok) return r;
    out.clusters = std::min<int>(a[2], kHidppClusters);
    for (int c = 0; c < out.clusters; ++c) {
        const uint8_t group[3] = { static_cast<uint8_t>(c), 0xff, 0x00 };
        r = call(l, HIDPP_RGB_EFFECTS, out.feature, kRgbInfo, group, sizeof(group), a);
        if (!r.ok) return r;
        out.count[c] = std::min<int>(a[4], kHidppEffects);
        for (int i = 0; i < out.count[c]; ++i) {
            const uint8_t which[3] = { static_cast<uint8_t>(c), static_cast<uint8_t>(i), 0x00 };
            r = call(l, HIDPP_RGB_EFFECTS, out.feature, kRgbInfo, which, sizeof(which), a);
            if (!r.ok) return r;
            out.effect[c][i] = static_cast<uint16_t>((a[2] << 8) | a[3]);
        }
    }
    return kOk;
}

HidppResult sw_set(HidppLinkWin& l, const HidppEffects& fx, uint8_t mode, uint8_t flags)
{
    const uint8_t set[3] = { 0x01, mode, flags };
    uint8_t a[kHidppParams] = {};
    return call(l, HIDPP_RGB_EFFECTS, fx.feature, kRgbSwControl, set, sizeof(set), a);
}

uint16_t get16_le(const uint8_t* at)
{
    return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

} // namespace

HidppResult hidpp_read_all_win(HidppLinkWin& l, HidppKeyboard& out)
{
    return read_all(l, out);
}

HidppResult hidpp_change_win(HidppLinkWin& l, const KeyChange& c, HidppKeyboard& out)
{
    HidppKeyboard k;
    HidppResult r = read_all(l, k);
    if (!r.ok) return r;
    if (c.brightness >= 0) {
        r = set_brightness(l, k, c.brightness);
        if (!r.ok) return r;
    }
    bool profile = c.light || c.dim_s >= 0 || c.off_s >= 0;
    uint8_t sector[kHidppSectorMax] = {};
    if (profile) {
        if (!k.profiles_index || !k.has_profile) return fail_with(HidFail::Refused, "Its profile can't be changed.");
        if (k.sector != kHidppSector) return fail_with(HidFail::Refused, "It's using a profile Lares doesn't change.");
        std::memcpy(sector, k.profile, k.profile_size);
        if (c.light && !hidpp_profile_lit(sector, k.profile_size, &c.lit, sector)) {
            return fail_with(HidFail::Refused, "Its profile can't be changed.");
        }
        if ((c.dim_s >= 0 || c.off_s >= 0) &&
            !hidpp_profile_timed(sector, k.profile_size, c.dim_s < 0 ? k.dim_s : c.dim_s,
                                 c.off_s < 0 ? k.off_s : c.off_s, sector)) {
            return fail_with(HidFail::Refused, "Its profile can't be changed.");
        }
        profile = std::memcmp(sector, k.profile, k.profile_size) != 0;
        if (profile) {
            r = write_profile(l, k.profiles_index, k.sector, sector, k.profile_size);
            if (!r.ok) return r;
        }
    }
    r = set_nv(l, k.rgb_index, kHidppNvStartup, k.startup, c.startup);
    if (r.ok) r = set_nv(l, k.rgb_index, kHidppNvShutdown, k.shutdown, c.shutdown);
    if (!r.ok) return r;
    /* An effect the keyboard keeps shows only once software lets its
     * lights go; the helper does too, as the setting changes. */
    if (c.release && k.rgb_index && k.app_controls) {
        HidppEffects fx;
        fx.feature = k.rgb_index;
        r = sw_set(l, fx, kSwFirmware, 0);
        if (!r.ok) return r;
    }
    size_t size = k.profile_size;
    r = read_all(l, k);
    if (!r.ok) return r;
    if (profile && (!k.has_profile || std::memcmp(k.profile, sector, size) != 0)) {
        return fail_with(HidFail::Refused, "It didn't keep the change.");
    }
    if ((c.startup >= 0 && k.startup.on != (c.startup != 0)) ||
        (c.shutdown >= 0 && k.shutdown.on != (c.shutdown != 0))) {
        return fail_with(HidFail::Refused, "It didn't keep the change.");
    }
    out = k;
    return kOk;
}

HidppResult hidpp_live_effects_win(HidppLinkWin& l, HidppEffects& out)
{
    return read_effects(l, out);
}

HidppResult hidpp_live_play_win(HidppLinkWin& l, const HidppEffects& fx, const HidppLighting& light, HidppLive state)
{
    static const HidppLighting dark = { KeyLight::Off, { 0, 0, 0 } };
    uint8_t params[10] = {};
    int id = hidpp_effect(state == HidppLive::Dark ? &dark : &light, state == HidppLive::Idle, params);
    if (id < 0 || hidpp_kept(light.effect)) return fail_with(HidFail::Refused, "Lares doesn't play that live.");
    uint8_t flags = kSwFlags[static_cast<int>(state)];
    HidppResult r = kOk;
    if (state == HidppLive::Take) r = sw_set(l, fx, kSwFirmware, flags);
    if (r.ok) r = sw_set(l, fx, kSwSoftware, flags);
    if (!r.ok) return r;
    bool any = false;
    for (int c = 0; c < fx.clusters; ++c) {
        int at = hidpp_effect_index(&fx, c, static_cast<uint16_t>(id));
        if (at < 0) continue;
        uint8_t set[13] = { static_cast<uint8_t>(c), static_cast<uint8_t>(at) };
        std::memcpy(set + 2, params, 10);
        set[12] = kVolatile;
        uint8_t a[kHidppParams] = {};
        r = call(l, HIDPP_RGB_EFFECTS, fx.feature, kRgbSetEffect, set, sizeof(set), a);
        if (!r.ok) return r;
        any = true;
    }
    return any ? kOk : fail_with(HidFail::Refused, "It has no such effect.");
}

HidppResult hidpp_live_release_win(HidppLinkWin& l, const HidppEffects& fx)
{
    return sw_set(l, fx, kSwFirmware, 0);
}

/* The profile in use's timers, from the 16 bytes that hold them: no sector
 * in use (0) has none. */
HidppResult hidpp_live_timers_win(HidppLinkWin& l, int& dim_s, int& off_s)
{
    dim_s = off_s = 0;
    uint8_t fi = 0;
    HidppResult r = feature_index(l, HIDPP_PROFILES, fi);
    if (!r.ok || !fi) return r;
    uint8_t current[kHidppParams] = {}, chunk[kHidppParams] = {};
    r = call(l, HIDPP_PROFILES, fi, kProfilesCurrent, nullptr, 0, current);
    if (!r.ok) return r;
    if (!current[0] && !current[1]) return kOk;
    const uint8_t where[4] = { current[0], current[1], 0x00, static_cast<uint8_t>(kTimersAt & 0xf0) };
    r = call(l, HIDPP_PROFILES, fi, kProfilesRead, where, sizeof(where), chunk);
    if (!r.ok) return r;
    dim_s = get16_le(chunk + (kTimersAt & 0x0f));
    off_s = get16_le(chunk + (kTimersAt & 0x0f) + 2);
    return kOk;
}

} // namespace lares
