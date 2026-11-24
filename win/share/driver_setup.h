// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* The one-time switch of the Wi-Fi adapter between Windows' own driver and
 * the in-box WinUSB one (issue #109). Both need administrator rights, which
 * Set up asks for once; nothing per session. */
#include <string>

namespace lares {

/* What a driver list for the adapter offers, for --driver-probe: one line
 * a driver. Needs no rights. */
std::string adapter_driver_probe();

/* Gives the adapter Microsoft's "WinUsb Device" driver from winusb.inf (in
 * the box and signed: nothing is added to the driver store). */
bool adapter_use_winusb(std::string& err);

/* Gives it back to the best driver Windows has for it (netrtwlanu.inf). */
bool adapter_use_windows_driver(std::string& err);

} // namespace lares
