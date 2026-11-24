// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Bluetooth LE advertisement that wakes AirDrop receivers, sent by
 * Windows' own Bluetooth while Lares looks for devices to send to; the rest
 * of AirDrop runs in lares-share.exe (docs/WINDOWS.md). It's the
 * advertisement src/airdrop/ble.c registers with BlueZ on Linux. */
#pragma once
#include <cstdint>

namespace lares {

enum class BleWakeState {
    Off,
    Starting,
    On,
    Failed,   // no radio, the radio is off, or it can't advertise
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
