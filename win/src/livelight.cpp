// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "livelight.h"

#include "hid_win.h"

#include <cstring>
#include <string>
#include <vector>

namespace lares {

LiveStep livelight_on_event(HidppLive state, bool held, const HidppEvent& ev)
{
    LiveStep s;
    s.state = state;
    s.held = held;
    switch (ev.kind) {
    case HidppEventKind::Link:
        /* Asleep or gone, it has no lights to hold; back, it's given them again. */
        s.held = false;
        s.stop_dark = true;
        if (ev.linked) {
            s.state = HidppLive::Take;
            s.again = true;
        }
        break;
    case HidppEventKind::Idle:
        /* Said again until answered; once dark, it stays dark until a key. */
        if (!held) break;
        if (state != HidppLive::Dark) s.state = HidppLive::Idle;
        s.again = true;
        break;
    case HidppEventKind::Active:
        if (!held) break;
        s.stop_dark = true;
        s.state = HidppLive::Active;
        s.again = true;
        break;
    default:
        break;
    }
    return s;
}

LiveStep livelight_on_conf(HidppLive state, bool held)
{
    LiveStep s;
    s.held = held;
    s.again = true;
    if (!held) s.state = HidppLive::Take;
    else if (state == HidppLive::Dark) s.state = HidppLive::Idle;   // a new pick shows, dimmed, until its time
    else s.state = state;
    return s;
}

namespace {

constexpr DWORD kPluggedMs = 1000;   // a new receiver's collections settle a moment after it appears
constexpr uint8_t kIndex = 1;        // the keyboard's slot on its receiver

struct Node {
    std::wstring s_path;
    std::wstring l_path;
    HidPort s;   // listened to while an effect is wanted
    HidPort l;
    HidppEffects fx{};
    bool known = false;
    bool held = false;
    HidppLive state = HidppLive::Take;
};

class Runner {
public:
    explicit Runner(HANDLE dark) : dark_(dark) {}

    bool listening() const { return node_.s.is_open(); }
    HidPort& short_port() { return node_.s; }
    HidPort& long_port() { return node_.l; }
    bool has_node() const { return !node_.s_path.empty(); }
    bool held() const { return node_.held; }
    HidppLive state() const { return node_.state; }
    void set_state(HidppLive s) { node_.state = s; }
    void set_held(bool h) { node_.held = h; }

    void stop_dark()
    {
        CancelWaitableTimer(dark_);
    }

    void forget_node()
    {
        stop_listening();
        stop_dark();
        node_.s_path.clear();
        node_.l_path.clear();
        node_.known = false;
        node_.held = false;
    }

    // Plays what the settings ask, or gives the lights back.
    void sync(bool wanted, const HidppLighting& light)
    {
        if (wanted) {
            if (!find_node()) return;
            listen_to();
            request(false, light);
        } else if (has_node() && node_.held) {
            stop_dark();
            request(true, light);
            stop_listening();
        } else {
            stop_listening();
        }
    }

    // The keyboard's own reports; FALSE when the receiver is gone.
    bool take_events(std::vector<HidppEvent>& out)
    {
        HidppKeyboard idx;
        idx.status_index = node_.fx.status;
        idx.rgb_index = node_.known ? node_.fx.feature : 0;
        for (HidPort* p : { &node_.s, &node_.l }) {
            uint8_t r[64];
            DWORD got = 0;
            if (!p->take(r, got)) return false;
            HidppEvent ev;
            if (got && hidpp_event(r, got, kIndex, &idx, &ev)) out.push_back(ev);
            if (!p->arm()) return false;
        }
        return true;
    }

    void release_now()
    {
        if (!has_node() || !node_.held) return;
        HidppLighting none;
        request(true, none);
    }

private:
    bool find_node()
    {
        if (has_node()) return true;
        HidFound f = hid_find(hid_collections());
        if (f.hidpp_short.empty() || f.hidpp_long.empty()) return false;
        node_.s_path = f.hidpp_short;
        node_.l_path = f.hidpp_long;
        node_.known = false;
        node_.held = false;
        return true;
    }

    void listen_to()
    {
        if (node_.s.is_open()) return;
        if (!node_.s.open(node_.s_path, false) || !node_.l.open(node_.l_path, false) ||
            !node_.s.arm() || !node_.l.arm()) {
            stop_listening();
        }
    }

    void stop_listening()
    {
        node_.s.close();
        node_.l.close();
    }

    void request(bool release, const HidppLighting& light)
    {
        HidppResult r;
        int dark_after = -1;
        {
            ScopedWinMutex lock(kHidppLock);
            HidppLinkWin link;
            link.sw = kHidppSwHelper;
            link.index = kIndex;
            if (!link.open(node_.s_path, node_.l_path)) {
                r = { false, HidFail::Gone, "Its receiver is gone." };
            } else {
                r.ok = true;
                if (!node_.known) {
                    r = hidpp_live_effects_win(link, node_.fx);
                    node_.known = r.ok;
                }
                if (r.ok) {
                    r = release ? hidpp_live_release_win(link, node_.fx)
                                : hidpp_live_play_win(link, node_.fx, light, node_.state);
                }
                /* Read as it goes idle, so a change from the window counts at once. */
                int dim = 0, off = 0;
                if (r.ok && !release && node_.state == HidppLive::Idle && hidpp_live_timers_win(link, dim, off).ok) {
                    dark_after = hidpp_dark_after(dim, off);
                }
            }
        }
        bool asleep = r.fail == HidFail::Asleep || r.fail == HidFail::NoAnswer;
        node_.held = r.ok ? !release : (asleep ? false : node_.held);
        if (dark_after >= 0 && node_.held && node_.state == HidppLive::Idle) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(dark_after) * 10000000LL;
            SetWaitableTimer(dark_, &due, 0, nullptr, nullptr, FALSE);
        }
        if (r.fail == HidFail::Gone) forget_node();
    }

    HANDLE dark_;
    Node node_;
};

} // namespace

LiveLight::LiveLight(bool dry_run)
    : dry_(dry_run)
{
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&LiveLight::run, this);
}

LiveLight::~LiveLight()
{
    stop_.store(true);
    if (wake_) SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    if (wake_) CloseHandle(wake_);
}

void LiveLight::set_wanted(KeyLight light, const uint8_t rgb[3])
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        bool same = light == light_ && (!rgb || std::memcmp(rgb, rgb_, 3) == 0);
        light_ = light;
        if (rgb) std::memcpy(rgb_, rgb, 3);
        if (!same) conf_due_ = true;
    }
    if (wake_) SetEvent(wake_);
}

void LiveLight::devices_changed()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        plug_due_ = true;
    }
    if (wake_) SetEvent(wake_);
}

void LiveLight::run()
{
    /* Auto-reset: a timer that stayed signalled once it fired would win
     * every wait after it, and the keyboard's events would go unheard. */
    HANDLE dark = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    Runner run(dark);
    ULONGLONG plug_at = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        conf_due_ = true;   // the first settings count as a change
    }
    while (!stop_.load()) {
        HidppLighting light;
        bool conf = false, plug = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            light.effect = light_;
            std::memcpy(light.rgb, rgb_, 3);
            conf = conf_due_;
            plug = plug_due_;
            conf_due_ = plug_due_ = false;
        }
        bool wanted = !dry_ && key_light_live(light.effect);
        bool again = false;

        if (conf) {
            LiveStep st = livelight_on_conf(run.state(), run.held());
            run.set_state(st.state);
            again = true;
        }
        if (plug) {
            /* A receiver that came back is found once it settles; one that
             * went is noticed by its reads failing. */
            if (wanted && !run.has_node()) plug_at = GetTickCount64() + kPluggedMs;
        }
        if (plug_at && GetTickCount64() >= plug_at) {
            plug_at = 0;
            run.set_state(HidppLive::Take);
            again = true;
        }
        if (again) run.sync(wanted, light);

        HANDLE waits[4] = { wake_, dark };
        DWORD n = 2;
        if (run.listening()) {
            waits[n++] = run.short_port().read_event();
            waits[n++] = run.long_port().read_event();
        }
        DWORD timeout = INFINITE;
        if (plug_at) {
            ULONGLONG now = GetTickCount64();
            timeout = plug_at > now ? static_cast<DWORD>(plug_at - now) : 0;
        }
        DWORD w = WaitForMultipleObjects(n, waits, FALSE, timeout);
        if (stop_.load()) break;
        if (w == WAIT_OBJECT_0 + 1) {
            /* The profile's lights-off time came while it was idle. */
            if (run.held() && run.state() == HidppLive::Idle) {
                run.set_state(HidppLive::Dark);
                run.sync(wanted, light);
            }
        } else if (w >= WAIT_OBJECT_0 + 2 && w < WAIT_OBJECT_0 + n) {
            std::vector<HidppEvent> events;
            if (!run.take_events(events)) {
                run.forget_node();   // unplugged: a device change says when it's back
                continue;
            }
            bool sync = false;
            for (const auto& ev : events) {
                LiveStep st = livelight_on_event(run.state(), run.held(), ev);
                run.set_state(st.state);
                run.set_held(st.held);
                if (st.stop_dark) run.stop_dark();
                sync |= st.again;
            }
            if (sync) run.sync(wanted, light);
        }
    }
    /* The lights go back to the keyboard before the helper ends, so it plays
     * what its profile keeps. */
    run.release_now();
    run.forget_node();
    CloseHandle(dark);
}

} // namespace lares
