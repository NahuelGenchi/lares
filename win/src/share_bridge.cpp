// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar's AirDrop panel on Lares' own sharing code (issue #155):
 * share_bridge.h. */
#include "share_bridge.h"

#include <memory>
#include <string>
#include <vector>

#include "ble_wake.h"
#include "share_win.h"
#include "shell_prefs.h"

using namespace lares;

namespace {

std::unique_ptr<ShareManager> g_mgr;
std::unique_ptr<BleWake> g_ble;
bool g_dry = false;

std::wstring wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void put(wchar_t* out, size_t len, const std::string& s)
{
    wcsncpy_s(out, len, wide(s).c_str(), _TRUNCATE);
}

std::wstring share_program()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return dir.substr(0, slash) + L"\\lares-share.exe";
}

int link_of(ShareLink l)
{
    switch (l) {
    case ShareLink::Starting: return LARES_SHARE_LINK_STARTING;
    case ShareLink::Up: return LARES_SHARE_LINK_UP;
    case ShareLink::Stopping: return LARES_SHARE_LINK_STOPPING;
    case ShareLink::Failed: return LARES_SHARE_LINK_FAILED;
    default: return LARES_SHARE_LINK_OFF;
    }
}

int device_state_of(ShareDeviceState s)
{
    switch (s) {
    case ShareDeviceState::Asking: return LARES_SHARE_DEV_ASKING;
    case ShareDeviceState::Sending: return LARES_SHARE_DEV_SENDING;
    case ShareDeviceState::Sent: return LARES_SHARE_DEV_SENT;
    case ShareDeviceState::Declined: return LARES_SHARE_DEV_DECLINED;
    case ShareDeviceState::Failed: return LARES_SHARE_DEV_FAILED;
    default: return LARES_SHARE_DEV_FOUND;
    }
}

int kind_of(ShareDeviceKind k)
{
    switch (k) {
    case ShareDeviceKind::Phone: return LARES_SHARE_KIND_PHONE;
    case ShareDeviceKind::Tablet: return LARES_SHARE_KIND_TABLET;
    case ShareDeviceKind::Laptop: return LARES_SHARE_KIND_LAPTOP;
    case ShareDeviceKind::Desktop: return LARES_SHARE_KIND_DESKTOP;
    case ShareDeviceKind::Watch: return LARES_SHARE_KIND_WATCH;
    default: return LARES_SHARE_KIND_OTHER;
    }
}

int wake_of(BleWakeState s)
{
    switch (s) {
    case BleWakeState::Starting: return LARES_SHARE_WAKE_STARTING;
    case BleWakeState::On: return LARES_SHARE_WAKE_ON;
    case BleWakeState::RadioOff: return LARES_SHARE_WAKE_RADIO_OFF;
    case BleWakeState::Failed: return LARES_SHARE_WAKE_FAILED;
    default: return LARES_SHARE_WAKE_OFF;
    }
}

} // namespace

extern "C" {

bool lares_share_available(void)
{
    std::wstring exe = share_program();
    return !exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void lares_share_open(HWND notify)
{
    if (g_mgr) return;
    g_dry = lares_shell_dry_run();
    g_mgr = std::make_unique<ShareManager>(g_dry);
    g_mgr->set_notify_hwnd(notify);
    g_mgr->refresh();
}

void lares_share_close(void)
{
    if (g_ble) g_ble->set_active(false);
    g_ble.reset();
    /* The manager's end: the session is told to quit, and the adapter
     * powers down on its way out. */
    g_mgr.reset();
}

bool lares_share_is_open(void) { return g_mgr != nullptr; }

void lares_share_updated(void)
{
    if (!g_mgr) return;
    g_mgr->resume();
    ShareView v = g_mgr->view();
    bool wake = v.running && v.s.wake && !g_dry;
    if (wake && !g_ble) g_ble = std::make_unique<BleWake>();
    if (g_ble) g_ble->set_active(wake);
}

void lares_share_get(LaresShareState* out)
{
    if (!out) return;
    ZeroMemory(out, sizeof(*out));
    if (!g_mgr) {
        out->setup = lares_share_available() ? LARES_SHARE_READY : LARES_SHARE_NO_PROGRAM;
        return;
    }
    ShareView v = g_mgr->view();
    out->open = true;
    if (!g_dry && !lares_share_available()) out->setup = LARES_SHARE_NO_PROGRAM;
    else if (v.setup == ShareSetup::NoAdapter) out->setup = LARES_SHARE_NO_ADAPTER;
    else if (v.setup == ShareSetup::NeedsSetup) out->setup = LARES_SHARE_NEEDS_SETUP;
    else out->setup = LARES_SHARE_READY;
    out->setup_busy = v.setup_busy;
    out->running = v.running;
    out->link = link_of(v.s.link);
    out->mode = v.s.mode == ShareDiscoverMode::ContactsOnly ? LARES_SHARE_MODE_CONTACTS
              : v.s.mode == ShareDiscoverMode::Everyone   ? LARES_SHARE_MODE_EVERYONE
                                                          : LARES_SHARE_MODE_OFF;
    out->receiving = v.s.receiving;
    if (v.s.mode == ShareDiscoverMode::Everyone && v.s.left > 0) {
        long long gone = static_cast<long long>((GetTickCount64() - v.left_at_ms) / 1000);
        long long left = v.s.left - gone;
        out->left_s = left > 0 ? static_cast<int>(left) : 0;
    }
    out->browsing = v.s.browsing;
    out->sending = v.s.sending;
    out->wake = v.s.wake;
    out->quiet = v.s.quiet;
    out->wake_state = g_ble ? wake_of(g_ble->state()) : LARES_SHARE_WAKE_OFF;
    out->contact_count = static_cast<int>(v.contacts.size());
    put(out->name, 96, v.s.name);
    const std::string& err = !v.error.empty() ? v.error : !v.setup_error.empty() ? v.setup_error : v.s.error;
    put(out->error, 200, err);

    out->has_incoming = v.s.has_incoming;
    if (v.s.has_incoming) {
        const ShareIncoming& in = v.s.incoming;
        out->in_state = in.state == ShareIncomingState::Receiving ? LARES_SHARE_IN_RECEIVING
                      : in.state == ShareIncomingState::Done      ? LARES_SHARE_IN_DONE
                      : in.state == ShareIncomingState::Failed    ? LARES_SHARE_IN_FAILED
                                                                  : LARES_SHARE_IN_ASKING;
        out->in_done = in.done;
        out->in_total = in.total;
        const ShareContact* who = share_find_contact(v.contacts, in.sender, in.matched_hash);
        put(out->in_sender, 96, who && !who->name.empty() ? who->name : in.sender);
        put(out->in_what, 160, share_offer_text(in));
        if (!in.saved.empty()) put(out->in_saved, MAX_PATH, in.saved.front());
        put(out->in_message, 160, in.message);
    }

    for (const ShareDevice& d : v.s.devices) {
        if (out->device_count >= LARES_SHARE_MAX_DEVICES) break;
        LaresShareDevice* o = &out->devices[out->device_count++];
        strncpy_s(o->key, sizeof(o->key), d.key.c_str(), _TRUNCATE);
        put(o->name, 96, d.name);
        put(o->model, 48, share_model_label(d.model, d.name));
        put(o->message, 96, d.message);
        o->kind = kind_of(share_device_kind(d.model, d.name));
        o->state = device_state_of(d.state);
        o->sent = d.sent;
        o->total = d.total;
    }
}

void lares_share_set_mode(int mode)
{
    if (!g_mgr) return;
    g_mgr->set_discover_mode(mode == LARES_SHARE_MODE_CONTACTS   ? ShareDiscoverMode::ContactsOnly
                             : mode == LARES_SHARE_MODE_EVERYONE ? ShareDiscoverMode::Everyone
                                                                 : ShareDiscoverMode::Off);
}

void lares_share_set_browsing(bool on)
{
    if (g_mgr) g_mgr->set_browsing(on);
}

void lares_share_answer(bool accept)
{
    if (g_mgr) g_mgr->answer(accept);
}

void lares_share_dismiss(void)
{
    if (g_mgr) g_mgr->dismiss_incoming();
}

void lares_share_cancel_send(void)
{
    if (g_mgr) g_mgr->cancel_send();
}

bool lares_share_send_to(const char* key, const wchar_t* const* paths, int count)
{
    if (!g_mgr || !key || !key[0] || !paths || count <= 0) return false;
    std::vector<std::wstring> list;
    for (int i = 0; i < count; ++i)
        if (paths[i] && paths[i][0]) list.emplace_back(paths[i]);
    if (list.empty()) return false;
    g_mgr->send_files_to(key, list);
    return true;
}

} // extern "C"
