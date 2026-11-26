// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Locale-independent in-box WinUSB driver matching tests (issue #114). */
#include "share_check.h"

#include "driver_setup.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

#include <windows.h>
#include <setupapi.h>

namespace {

using lares::DriverCandidate;
using lares::WinUsbMatch;

/* Builds a real SP_DRVINFO_DETAIL_DATA_W buffer with the same layout
 * SetupDiGetDriverInfoDetailW fills in. */
std::vector<BYTE> make_detail_buf(const wchar_t* inf_file, const wchar_t* section_name,
                                  const wchar_t* description, const wchar_t* hw_id,
                                  const std::vector<std::wstring>& compat_ids)
{
    std::vector<wchar_t> id_chars;
    if (hw_id && *hw_id) {
        id_chars.insert(id_chars.end(), hw_id, hw_id + std::wcslen(hw_id));
    }
    id_chars.push_back(L'\0');

    DWORD compat_offset = 0;
    DWORD compat_len = 0;
    if (!compat_ids.empty()) {
        compat_offset = static_cast<DWORD>(id_chars.size());
        for (const std::wstring& cid : compat_ids) {
            id_chars.insert(id_chars.end(), cid.begin(), cid.end());
            id_chars.push_back(L'\0');
        }
        id_chars.push_back(L'\0');
        compat_len = static_cast<DWORD>(id_chars.size()) - compat_offset;
    } else {
        id_chars.push_back(L'\0');
    }

    const size_t hdr = offsetof(SP_DRVINFO_DETAIL_DATA_W, HardwareID);
    const size_t total = std::max(sizeof(SP_DRVINFO_DETAIL_DATA_W), hdr + id_chars.size() * sizeof(wchar_t));
    std::vector<BYTE> buf(total, 0);
    auto* d = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buf.data());
    d->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
    d->CompatIDsOffset = compat_offset;
    d->CompatIDsLength = compat_len;
    if (inf_file) std::wcsncpy(d->InfFileName, inf_file, MAX_PATH - 1);
    if (section_name) std::wcsncpy(d->SectionName, section_name, LINE_LEN - 1);
    if (description) std::wcsncpy(d->DrvDescription, description, LINE_LEN - 1);
    std::wmemcpy(d->HardwareID, id_chars.data(), id_chars.size());
    return buf;
}

void test_localized_descriptions()
{
    /* Same in-box winusb.inf model under non-English Windows display languages:
     * matching never reads DrvDescription. */
    static const wchar_t* const descriptions[] = {
        L"WinUsb Device",
        L"Dispositivo WinUsb",
        L"WinUsb-Ger\u00e4t",
        L"P\u00e9riph\u00e9rique WinUsb",
        L"WinUsb \u30c7\u30d0\u30a4\u30b9",
        L"\u0423\u0441\u0442\u0440\u043e\u0439\u0441\u0442\u0432\u043e WinUsb",
        L"",
    };
    for (const wchar_t* desc : descriptions) {
        auto buf = make_detail_buf(L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB", desc, L"USB\\MS_COMP_WINUSB", {});
        DriverCandidate cand;
        CHECK(lares::parse_driver_detail_ids(buf.data(), buf.size(), cand));
        CHECK(cand.inf_file == L"C:\\WINDOWS\\INF\\winusb.inf");
        CHECK(cand.section_name == L"WINUSB");
        CHECK(cand.hardware_id == L"USB\\MS_COMP_WINUSB");
        CHECK(cand.compat_ids.empty());
        CHECK(lares::match_winusb_driver(cand) == WinUsbMatch::IdAndSection);
    }
}

void test_paths_sections_and_compat_ids()
{
    /* DriverStore path, uppercase INF name, and decorated NT section names. */
    static const wchar_t* const sections[] = {
        L"WINUSB",
        L"winusb",
        L"WINUSB.NT",
        L"WINUSB.NTamd64",
        L"WINUSB.NTarm64",
        L"WINUSB.NTx86",
        L"WINUSB.NTamd64.10.0",
        L"WINUSB.NT.10.0",
    };
    for (const wchar_t* sec : sections) {
        DriverCandidate c;
        c.inf_file = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\winusb.inf_amd64_d8874b9a834f5618\\WINUSB.INF";
        c.section_name = sec;
        c.hardware_id = L"usb\\ms_comp_winusb";
        CHECK(lares::match_winusb_driver(c) == WinUsbMatch::IdAndSection);
    }

    /* Forward slashes and bare INF name. */
    {
        DriverCandidate c;
        c.inf_file = L"C:/Windows/INF/winusb.inf";
        c.section_name = L"WINUSB.NT";
        c.hardware_id = L"USB\\MS_COMP_WINUSB";
        CHECK(lares::match_winusb_driver(c) == WinUsbMatch::IdAndSection);

        c.inf_file = L"winusb.inf";
        CHECK(lares::match_winusb_driver(c) == WinUsbMatch::IdAndSection);
    }

    /* USB\MS_COMP_WINUSB supplied in CompatIDs (with or without a HardwareID). */
    {
        auto buf = make_detail_buf(L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB.NT", L"Dispositivo WinUsb",
                                   L"USB\\VID_0BDA&PID_8179", { L"USB\\Class_FF&SubClass_FF", L"USB\\MS_COMP_WINUSB" });
        DriverCandidate cand;
        CHECK(lares::parse_driver_detail_ids(buf.data(), buf.size(), cand));
        CHECK(cand.hardware_id == L"USB\\VID_0BDA&PID_8179");
        CHECK(cand.compat_ids.size() == 2);
        CHECK(lares::match_winusb_driver(cand) == WinUsbMatch::IdAndSection);
    }
    {
        auto buf = make_detail_buf(L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB", L"Dispositivo WinUsb",
                                   L"", { L"USB\\MS_COMP_WINUSB" });
        DriverCandidate cand;
        CHECK(lares::parse_driver_detail_ids(buf.data(), buf.size(), cand));
        CHECK(cand.hardware_id.empty());
        CHECK(cand.compat_ids.size() == 1 && cand.compat_ids[0] == L"USB\\MS_COMP_WINUSB");
        CHECK(lares::match_winusb_driver(cand) == WinUsbMatch::IdAndSection);
    }

    /* Section-only and ID-only tiers within winusb.inf. */
    {
        DriverCandidate sec_only{ L"C:\\Windows\\INF\\winusb.inf", L"WINUSB.NT", L"", {} };
        CHECK(lares::match_winusb_driver(sec_only) == WinUsbMatch::SectionOnly);

        DriverCandidate id_only{ L"C:\\Windows\\INF\\winusb.inf", L"WINUSB_GENERIC.NT", L"USB\\MS_COMP_WINUSB", {} };
        CHECK(lares::match_winusb_driver(id_only) == WinUsbMatch::IdOnly);
    }
}

void test_rejects_other_models_and_infs()
{
    /* Other models inside Microsoft's winusb.inf must never match. */
    {
        DriverCandidate billboard{ L"C:\\WINDOWS\\INF\\winusb.inf", L"BILLBOARD",
                                   L"USB\\Class_11&SubClass_00&Prot_00", {} };
        CHECK(lares::match_winusb_driver(billboard) == WinUsbMatch::None);

        DriverCandidate adb{ L"C:\\WINDOWS\\INF\\winusb.inf", L"ADB",
                             L"USB\\Class_ff&SubClass_42&Prot_01", {} };
        CHECK(lares::match_winusb_driver(adb) == WinUsbMatch::None);

        DriverCandidate filter{ L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB_LowerFilter", L"", {} };
        CHECK(lares::match_winusb_driver(filter) == WinUsbMatch::None);

        DriverCandidate filter_nt{ L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB_LowerFilter.NT",
                                   L"USB\\MS_COMP_WINUSB", {} };
        CHECK(lares::match_winusb_driver(filter_nt) == WinUsbMatch::None);

        DriverCandidate services{ L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB.NT.Services", L"", {} };
        CHECK(lares::match_winusb_driver(services) == WinUsbMatch::None);

        DriverCandidate hw{ L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB.NT.HW", L"", {} };
        CHECK(lares::match_winusb_driver(hw) == WinUsbMatch::None);
    }

    /* Non-inbox INFs must never match even with WINUSB section and ID. */
    static const wchar_t* const other_infs[] = {
        L"",
        L"C:\\WINDOWS\\INF\\oem12.inf",
        L"C:\\WINDOWS\\INF\\netrtwlanu.inf",
        L"C:\\WINDOWS\\INF\\mywinusb.inf",
        L"C:\\WINDOWS\\INF\\winusb.inf\\fake.inf",
    };
    for (const wchar_t* inf : other_infs) {
        DriverCandidate c{ inf, L"WINUSB", L"USB\\MS_COMP_WINUSB", {} };
        CHECK(lares::match_winusb_driver(c) == WinUsbMatch::None);
    }
}

void test_candidate_selection_order()
{
    /* Realistic winusb.inf list with WINUSB_LowerFilter, Billboard and ADB ahead
     * of the real WINUSB entry, all under non-English descriptions. */
    std::vector<DriverCandidate> list = {
        { L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB_LowerFilter", L"", {} },
        { L"C:\\WINDOWS\\INF\\winusb.inf", L"BILLBOARD", L"USB\\Class_11&SubClass_00&Prot_00", {} },
        { L"C:\\WINDOWS\\INF\\winusb.inf", L"ADB", L"USB\\Class_ff&SubClass_42&Prot_01", {} },
        { L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB", L"USB\\MS_COMP_WINUSB", {} },
    };
    CHECK(lares::pick_winusb_candidate(list) == 3);

    /* Ranking prefers IdAndSection > IdOnly > SectionOnly. */
    std::vector<DriverCandidate> ranked = {
        { L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB.NT", L"", {} },
        { L"C:\\WINDOWS\\INF\\winusb.inf", L"OTHER.NT", L"USB\\MS_COMP_WINUSB", {} },
        { L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB.NT", L"USB\\MS_COMP_WINUSB", {} },
    };
    CHECK(lares::pick_winusb_candidate(ranked) == 2);

    ranked.pop_back();
    CHECK(lares::pick_winusb_candidate(ranked) == 1);

    ranked.pop_back();
    CHECK(lares::pick_winusb_candidate(ranked) == 0);

    /* Empty or non-matching list returns -1. */
    CHECK(lares::pick_winusb_candidate({}) == -1);
    std::vector<DriverCandidate> none = {
        { L"C:\\WINDOWS\\INF\\netrtwlanu.inf", L"RTL8188eu.ndi", L"USB\\VID_0BDA&PID_8179", {} },
    };
    CHECK(lares::pick_winusb_candidate(none) == -1);
}

void test_malformed_detail_buffers()
{
    DriverCandidate cand;
    CHECK(!lares::parse_driver_detail_ids(nullptr, 0, cand));

    std::vector<BYTE> too_small(sizeof(SP_DRVINFO_DETAIL_DATA_W) - 1, 0);
    CHECK(!lares::parse_driver_detail_ids(too_small.data(), too_small.size(), cand));

    auto bad_cb = make_detail_buf(L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB", L"WinUsb", L"USB\\MS_COMP_WINUSB", {});
    reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(bad_cb.data())->cbSize = 4;
    CHECK(!lares::parse_driver_detail_ids(bad_cb.data(), bad_cb.size(), cand));

    /* Out-of-bounds CompatIDsOffset and overflowing CompatIDsLength stay safe. */
    auto oob = make_detail_buf(L"C:\\WINDOWS\\INF\\winusb.inf", L"WINUSB", L"WinUsb", L"USB\\MS_COMP_WINUSB", {});
    auto* d = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(oob.data());
    d->CompatIDsOffset = 999999;
    d->CompatIDsLength = 100;
    CHECK(lares::parse_driver_detail_ids(oob.data(), oob.size(), cand));
    CHECK(cand.hardware_id == L"USB\\MS_COMP_WINUSB");
    CHECK(cand.compat_ids.empty());

    d->CompatIDsOffset = 0;
    d->CompatIDsLength = (std::numeric_limits<DWORD>::max)();
    CHECK(lares::parse_driver_detail_ids(oob.data(), oob.size(), cand));
    CHECK(cand.compat_ids.size() == 1 && cand.compat_ids[0] == L"USB\\MS_COMP_WINUSB");
}

} // namespace

void run_driver_tests()
{
    test_localized_descriptions();
    test_paths_sections_and_compat_ids();
    test_rejects_other_models_and_infs();
    test_candidate_selection_order();
    test_malformed_detail_buffers();
}
