// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Bluetooth LE advertisement that wakes AirDrop receivers, sent from
 * Windows while Lares looks for devices to send to: the rest of AirDrop
 * runs in WSL, which has no Bluetooth radio (docs/WINDOWS.md). It's the
 * advertisement src/airdrop/ble.c registers with BlueZ on Linux. */
#pragma once
#include <cstdint>

namespace lares {

/* No peripheral role is needed: the advertisement isn't connectable, and a
 * Bluetooth 4.0 radio Windows denies that role to sends it all the same
 * (issue #107). What stops it is Bluetooth switched off. */
enum class BleWakeState {
    Off,
    Starting,
    On,
    RadioOff, // Bluetooth is switched off in Windows
    Failed,   // no radio, or it can't advertise
};

class BleWake {
public:
    BleWake();
    ~BleWake();
    BleWake(const BleWake&) = delete;
    BleWake& operator=(const BleWake&) = delete;

    /* Starts or stops the advertisement; calling it again with the same
     * value does nothing. Nothing of Bluetooth is loaded before the first
     * `true`. */
    void set_active(bool on);
    BleWakeState state() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace lares
