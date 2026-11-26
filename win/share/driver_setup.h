// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* The one-time switch of the Wi-Fi adapter between Windows' own driver and
 * the in-box WinUSB one (issue #109). Both need administrator rights, which
 * Set up asks for once; nothing per session. */
#include <cstddef>
#include <string>
#include <vector>

namespace lares {

/* Locale-independent driver identity fields extracted from
 * SP_DRVINFO_DETAIL_DATA_W (issue #114). */
struct DriverCandidate {
    std::wstring inf_file;
    std::wstring section_name;
    std::wstring hardware_id;
    std::vector<std::wstring> compat_ids;
};

enum class WinUsbMatch {
    None = 0,
    SectionOnly = 1,
    IdOnly = 2,
    IdAndSection = 3,
};

/* Parses INF path, section name, hardware ID and compatible IDs from a raw
 * SP_DRVINFO_DETAIL_DATA_W buffer bounded by detail_bytes. */
bool parse_driver_detail_ids(const void* detail_data, size_t detail_bytes, DriverCandidate& out);

/* Matches Microsoft's in-box winusb.inf device driver without inspecting any
 * localized display string. */
WinUsbMatch match_winusb_driver(const DriverCandidate& candidate);

/* Picks the highest-ranked WinUSB candidate from an enumerated driver list,
 * or returns -1 when none matches. */
int pick_winusb_candidate(const std::vector<DriverCandidate>& candidates);

/* What a driver list for the adapter offers, for --driver-probe: one line
 * a driver. Needs no rights. */
std::string adapter_driver_probe();

/* Gives the adapter Microsoft's in-box WinUSB driver from winusb.inf (in
 * the box and signed: nothing is added to the driver store). */
bool adapter_use_winusb(std::string& err);

/* Gives it back to the best driver Windows has for it (netrtwlanu.inf),
 * restoring GUID_DEVCLASS_NET if the adapter was moved to USBDevice. */
bool adapter_use_windows_driver(std::string& err);

} // namespace lares
