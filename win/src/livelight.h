// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The keyboard lighting lares-pad.exe plays live, ported from
 * src/helper/livelight.c (issues #57 and #68): Off, Solid, Breathing or
 * Colour cycle, which a Logitech keyboard can't keep in its onboard
 * profile, set under software control as Logitech G HUB does. While
 * software holds its lights the keyboard stops dimming itself: it says when
 * it's been left alone and when it's used again, and is answered with the
 * effect at half or full strength; at its profile's lights-off time the
 * lights go off here. A keyboard that reconnects is given the effect again,
 * and when the setting goes or the helper quits the lights go back to the
 * keyboard, which plays what its profile keeps.
 *
 * Nothing polls: the receiver is listened to only while an effect is
 * wanted, its coming back is a device change, and the lights-off time is
 * one timer. Everything runs on one thread, one request at a time. */
#pragma once
#ifdef _WIN32
#include "devices.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <windows.h>

namespace lares {

/* What the keyboard is told next, from what it just said: pure, so the
 * unit tests check it. `again` says a request is due. */
struct LiveStep {
    HidppLive state = HidppLive::Take;
    bool held = false;
    bool again = false;
    bool stop_dark = false;
};
LiveStep livelight_on_event(HidppLive state, bool held, const HidppEvent& ev);
LiveStep livelight_on_conf(HidppLive state, bool held);

class LiveLight {
public:
    explicit LiveLight(bool dry_run);
    ~LiveLight();
    LiveLight(const LiveLight&) = delete;
    LiveLight& operator=(const LiveLight&) = delete;

    // The effect pad.ini asks for; Own (or any kept effect) gives the lights back.
    void set_wanted(KeyLight light, const uint8_t rgb[3]);
    // A HID device came or went.
    void devices_changed();

private:
    void run();

    bool dry_ = false;
    std::mutex mu_;
    KeyLight light_ = KeyLight::Own;
    uint8_t rgb_[3] = { 0xff, 0xff, 0xff };
    bool conf_due_ = false;
    bool plug_due_ = false;
    std::atomic<bool> stop_{ false };
    HANDLE wake_ = nullptr;
    std::thread thread_;
};

} // namespace lares
#endif
