// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The adapter's one-time driver switch (issues #109, #114): Device Manager's
 * "Let me pick from a list of drivers" done by SetupAPI, on Microsoft's
 * signed in-box winusb.inf, and restoring Windows' stock Wi-Fi driver. */
#include "driver_setup.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <vector>

#include <windows.h>
#include <devguid.h>
#include <newdev.h>
#include <objbase.h>
#include <setupapi.h>

#include "rtl_usb.h"

namespace lares {

namespace {

wchar_t ascii_lower(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool ascii_iequals(const std::wstring& a, const wchar_t* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i] != L'\0'; ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return i == a.size() && b[i] == L'\0';
}

bool ascii_istarts_with(const std::wstring& s, const wchar_t* prefix)
{
    for (size_t i = 0; prefix[i] != L'\0'; ++i) {
        if (i >= s.size() || ascii_lower(s[i]) != ascii_lower(prefix[i])) return false;
    }
    return true;
}

bool is_ascii_alnum(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
}

bool is_ascii_digit(wchar_t c)
{
    return c >= L'0' && c <= L'9';
}

/* Basename of an INF path, supporting both backslashes and forward slashes. */
std::wstring inf_basename(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool is_winusb_inf_file(const std::wstring& path)
{
    return ascii_iequals(inf_basename(path), L"winusb.inf");
}

/* Matches the function-driver DDInstall section in Microsoft's winusb.inf:
 * WINUSB, WINUSB.NT, or WINUSB.NT<arch>[.<ver>]. Rejects WINUSB_LowerFilter,
 * BILLBOARD, ADB, and sub-sections such as WINUSB.NT.Services. */
bool is_winusb_install_section(const std::wstring& section)
{
    if (ascii_iequals(section, L"WINUSB") || ascii_iequals(section, L"WINUSB.NT")) return true;
    if (!ascii_istarts_with(section, L"WINUSB.NT")) return false;
    size_t i = 9; /* past "WINUSB.NT" */
    if (i < section.size() && section[i] != L'.') {
        while (i < section.size() && is_ascii_alnum(section[i])) ++i;
    }
    if (i == section.size()) return true;
    if (section[i] != L'.') return false;
    /* Optional OS version decorations like .10.0 start with a digit. */
    ++i;
    if (i >= section.size() || !is_ascii_digit(section[i])) return false;
    for (; i < section.size(); ++i) {
        if (!is_ascii_digit(section[i]) && section[i] != L'.') return false;
    }
    return true;
}

bool is_lower_filter_section(const std::wstring& section)
{
    return ascii_istarts_with(section, L"WINUSB_LowerFilter");
}

bool has_winusb_id(const DriverCandidate& c)
{
    if (ascii_iequals(c.hardware_id, L"USB\\MS_COMP_WINUSB")) return true;
    for (const std::wstring& id : c.compat_ids) {
        if (ascii_iequals(id, L"USB\\MS_COMP_WINUSB")) return true;
    }
    return false;
}

std::wstring bounded_wstring(const wchar_t* s, size_t max_chars)
{
    size_t n = 0;
    while (n < max_chars && s[n] != L'\0') ++n;
    return std::wstring(s, n);
}

std::string narrow(const wchar_t* w)
{
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string error_text(const char* what)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s (error 0x%08lx)", what, GetLastError());
    return buf;
}

/* The adapter's entry in a fresh device set of its own. */
struct Adapter {
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dev{ sizeof(SP_DEVINFO_DATA) };
    ~Adapter()
    {
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    }
    bool open(const GUID* cls)
    {
        HDEVINFO all = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (all == INVALID_HANDLE_VALUE) return false;
        wchar_t want[32], instance[256] = L"";
        std::swprintf(want, 32, L"USB\\VID_%04X&PID_%04X\\", RTL_USB_VID, RTL_USB_PID);
        SP_DEVINFO_DATA d{ sizeof(SP_DEVINFO_DATA) };
        bool found = false;
        for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(all, i, &d); i++)
            found = SetupDiGetDeviceInstanceIdW(all, &d, instance, 256, nullptr) &&
                    _wcsnicmp(instance, want, wcslen(want)) == 0;
        SetupDiDestroyDeviceInfoList(all);
        if (!found) return false;
        set = SetupDiCreateDeviceInfoList(cls, nullptr);
        return set != INVALID_HANDLE_VALUE && SetupDiOpenDeviceInfoW(set, instance, nullptr, 0, &dev);
    }
};

std::wstring winusb_inf()
{
    wchar_t dir[MAX_PATH];
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(dir) + L"\\INF\\winusb.inf";
}

/* Points the driver search at winusb.inf alone. */
bool use_single_inf(Adapter& a, const std::wstring& inf)
{
    if (inf.empty() || inf.size() >= MAX_PATH) return false;
    SP_DEVINSTALL_PARAMS_W p{ sizeof(SP_DEVINSTALL_PARAMS_W) };
    if (!SetupDiGetDeviceInstallParamsW(a.set, &a.dev, &p)) return false;
    p.Flags |= DI_ENUMSINGLEINF;
    p.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    std::wmemset(p.DriverPath, 0, MAX_PATH);
    std::wmemcpy(p.DriverPath, inf.c_str(), inf.size());
    return SetupDiSetDeviceInstallParamsW(a.set, &a.dev, &p) != FALSE;
}

const SP_DRVINFO_DETAIL_DATA_W* get_driver_detail(Adapter& a, SP_DRVINFO_DATA_W& drv, std::vector<BYTE>& buf)
{
    DWORD need = 0;
    SetupDiGetDriverInfoDetailW(a.set, &a.dev, &drv, nullptr, 0, &need);
    if (need < sizeof(SP_DRVINFO_DETAIL_DATA_W)) return nullptr;
    buf.assign(need, 0);
    auto* detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buf.data());
    detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
    if (!SetupDiGetDriverInfoDetailW(a.set, &a.dev, &drv, detail, need, nullptr)) return nullptr;
    return detail;
}

std::string list_drivers(Adapter& a, DWORD type)
{
    std::string out;
    SP_DRVINFO_DATA_W drv{ sizeof(SP_DRVINFO_DATA_W) };
    for (DWORD i = 0; SetupDiEnumDriverInfoW(a.set, &a.dev, type, i, &drv); i++) {
        out += "  " + narrow(drv.Description) + " | " + narrow(drv.MfgName) + " | " + narrow(drv.ProviderName);
        std::vector<BYTE> buf;
        DriverCandidate cand;
        if (const auto* detail = get_driver_detail(a, drv, buf)) {
            if (parse_driver_detail_ids(detail, buf.size(), cand)) {
                out += " | " + narrow(inf_basename(cand.inf_file).c_str());
                if (!cand.section_name.empty()) out += " | " + narrow(cand.section_name.c_str());
                if (!cand.hardware_id.empty()) out += " | " + narrow(cand.hardware_id.c_str());
                for (const std::wstring& cid : cand.compat_ids) out += " | " + narrow(cid.c_str());
            }
        }
        out += "\n";
    }
    if (out.empty()) out = "  (none)\n";
    return out;
}

/* Microsoft's in-box WinUSB device model from winusb.inf, matched by INF
 * metadata (USB\MS_COMP_WINUSB and/or WINUSB install section) so Windows UI
 * language never affects selection. */
bool select_winusb(Adapter& a, SP_DRVINFO_DATA_W& chosen)
{
    for (DWORD type : { (DWORD)SPDIT_CLASSDRIVER, (DWORD)SPDIT_COMPATDRIVER }) {
        if (!SetupDiBuildDriverInfoList(a.set, &a.dev, type)) continue;
        std::vector<SP_DRVINFO_DATA_W> infos;
        std::vector<DriverCandidate> candidates;
        SP_DRVINFO_DATA_W drv{ sizeof(SP_DRVINFO_DATA_W) };
        for (DWORD i = 0; SetupDiEnumDriverInfoW(a.set, &a.dev, type, i, &drv); i++) {
            std::vector<BYTE> buf;
            const auto* detail = get_driver_detail(a, drv, buf);
            if (!detail) continue;
            DriverCandidate cand;
            if (!parse_driver_detail_ids(detail, buf.size(), cand)) continue;
            infos.push_back(drv);
            candidates.push_back(std::move(cand));
        }
        int pick = pick_winusb_candidate(candidates);
        if (pick < 0) {
            SetupDiDestroyDriverInfoList(a.set, &a.dev, type);
            continue;
        }
        SP_DRVINFO_DATA_W& best = infos[static_cast<size_t>(pick)];
        if (!SetupDiSetSelectedDriverW(a.set, &a.dev, &best)) return false;
        chosen = best;
        return true;
    }
    return false;
}

/* SetupAPI lists only the drivers of a device's own class, and the adapter
 * is in Net; WinUsb Device is in USBDevice. Device Manager moves a device to
 * the class picked from its list the same way. */
bool set_class(Adapter& a, const GUID& cls)
{
    wchar_t text[40];
    if (!StringFromGUID2(cls, text, 40)) return false;
    return SetupDiSetDeviceRegistryPropertyW(a.set, &a.dev, SPDRP_CLASSGUID, reinterpret_cast<const BYTE*>(text),
                                             static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t))) != FALSE;
}

bool restore_class(Adapter& a, const GUID& cls)
{
    if (IsEqualGUID(cls, GUID_NULL))
        return SetupDiSetDeviceRegistryPropertyW(a.set, &a.dev, SPDRP_CLASSGUID, nullptr, 0) != FALSE;
    return set_class(a, cls);
}

bool is_winusb_drv(Adapter& a, SP_DRVINFO_DATA_W& drv)
{
    std::vector<BYTE> buf;
    const auto* detail = get_driver_detail(a, drv, buf);
    if (!detail) return false;
    DriverCandidate cand;
    return parse_driver_detail_ids(detail, buf.size(), cand) && match_winusb_driver(cand) != WinUsbMatch::None;
}

bool install_windows_driver_in_set(Adapter& target, std::string& err)
{
    /* The best match for its hardware ID, as Windows would pick on a first
     * plug-in: Microsoft's netrtwlanu.inf on this PC. */
    if (!SetupDiBuildDriverInfoList(target.set, &target.dev, SPDIT_COMPATDRIVER)) {
        err = error_text("Windows has no driver of its own for the adapter");
        return false;
    }
    SP_DRVINFO_DATA_W drv{ sizeof(SP_DRVINFO_DATA_W) };
    bool selected = SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, target.set, &target.dev) &&
                    SetupDiGetSelectedDriverW(target.set, &target.dev, &drv);
    if (selected && is_winusb_drv(target, drv)) selected = false;
    if (!selected) {
        SP_DRVINFO_DATA_W item{ sizeof(SP_DRVINFO_DATA_W) };
        for (DWORD i = 0; SetupDiEnumDriverInfoW(target.set, &target.dev, SPDIT_COMPATDRIVER, i, &item); i++) {
            if (is_winusb_drv(target, item)) continue;
            if (SetupDiSetSelectedDriverW(target.set, &target.dev, &item)) {
                drv = item;
                selected = true;
                break;
            }
        }
    }
    if (!selected) {
        err = error_text("Windows has no driver of its own for the adapter");
        return false;
    }
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, target.set, &target.dev, &drv, 0, &reboot)) {
        err = error_text("Couldn't give the adapter back to Windows' driver");
        return false;
    }
    return true;
}

} // namespace

bool parse_driver_detail_ids(const void* detail_data, size_t detail_bytes, DriverCandidate& out)
{
    out = DriverCandidate{};
    if (!detail_data || detail_bytes < sizeof(SP_DRVINFO_DETAIL_DATA_W)) return false;
    const auto* detail = static_cast<const SP_DRVINFO_DETAIL_DATA_W*>(detail_data);
    if (detail->cbSize != sizeof(SP_DRVINFO_DETAIL_DATA_W)) return false;

    out.inf_file = bounded_wstring(detail->InfFileName, MAX_PATH);
    out.section_name = bounded_wstring(detail->SectionName, LINE_LEN);

    const size_t hdr = offsetof(SP_DRVINFO_DETAIL_DATA_W, HardwareID);
    if (detail_bytes <= hdr) return true;
    const size_t hw_chars = (detail_bytes - hdr) / sizeof(wchar_t);
    const wchar_t* ids = detail->HardwareID;

    if (hw_chars > 0 && ids[0] != L'\0') {
        out.hardware_id = bounded_wstring(ids, hw_chars);
    }

    if (detail->CompatIDsLength > 0 && detail->CompatIDsOffset < hw_chars) {
        const size_t start = detail->CompatIDsOffset;
        const size_t avail = hw_chars - start;
        const size_t len = std::min<size_t>(detail->CompatIDsLength, avail);
        const size_t end = start + len;
        size_t i = start;
        while (i < end) {
            if (ids[i] == L'\0') {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < end && ids[j] != L'\0') ++j;
            if (j > i) out.compat_ids.emplace_back(ids + i, j - i);
            i = j + 1;
        }
    }
    return true;
}

WinUsbMatch match_winusb_driver(const DriverCandidate& candidate)
{
    if (!is_winusb_inf_file(candidate.inf_file)) return WinUsbMatch::None;
    if (is_lower_filter_section(candidate.section_name)) return WinUsbMatch::None;

    const bool id_ok = has_winusb_id(candidate);
    const bool sec_ok = is_winusb_install_section(candidate.section_name);
    if (id_ok && sec_ok) return WinUsbMatch::IdAndSection;
    if (id_ok) return WinUsbMatch::IdOnly;
    if (sec_ok) return WinUsbMatch::SectionOnly;
    return WinUsbMatch::None;
}

int pick_winusb_candidate(const std::vector<DriverCandidate>& candidates)
{
    int best_idx = -1;
    WinUsbMatch best_rank = WinUsbMatch::None;
    for (size_t i = 0; i < candidates.size(); ++i) {
        WinUsbMatch rank = match_winusb_driver(candidates[i]);
        if (static_cast<int>(rank) > static_cast<int>(best_rank)) {
            best_rank = rank;
            best_idx = static_cast<int>(i);
            if (best_rank == WinUsbMatch::IdAndSection) break;
        }
    }
    return best_idx;
}

std::string adapter_driver_probe()
{
    std::string out;
    Adapter a;
    if (!a.open(nullptr)) return "adapter: not found\n";
    wchar_t cls[64] = L"";
    SetupDiGetDeviceRegistryPropertyW(a.set, &a.dev, SPDRP_CLASS, nullptr, reinterpret_cast<BYTE*>(cls), sizeof cls - 2, nullptr);
    out += "adapter class: " + narrow(cls) + "\n";
    if (SetupDiBuildDriverInfoList(a.set, &a.dev, SPDIT_COMPATDRIVER))
        out += "compatible drivers:\n" + list_drivers(a, SPDIT_COMPATDRIVER);
    else
        out += error_text("compatible list failed") + "\n";

    std::wstring inf = winusb_inf();
    {
        Adapter b;
        if (b.open(nullptr) && use_single_inf(b, inf) && SetupDiBuildDriverInfoList(b.set, &b.dev, SPDIT_CLASSDRIVER))
            out += "winusb.inf as class drivers, set of no class:\n" + list_drivers(b, SPDIT_CLASSDRIVER);
        else
            out += error_text("winusb.inf class list, set of no class, failed") + "\n";
    }
    {
        Adapter b;
        if (b.open(&GUID_DEVCLASS_NET) && use_single_inf(b, inf) && SetupDiBuildDriverInfoList(b.set, &b.dev, SPDIT_CLASSDRIVER))
            out += "winusb.inf as class drivers, Net set:\n" + list_drivers(b, SPDIT_CLASSDRIVER);
        else
            out += error_text("winusb.inf class list, Net set, failed") + "\n";
    }
    {
        Adapter b;
        if (b.open(nullptr) && use_single_inf(b, inf) && SetupDiBuildDriverInfoList(b.set, &b.dev, SPDIT_COMPATDRIVER))
            out += "winusb.inf as compatible drivers:\n" + list_drivers(b, SPDIT_COMPATDRIVER);
        else
            out += error_text("winusb.inf compatible list failed") + "\n";
    }
    return out;
}

bool adapter_use_winusb(std::string& err)
{
    Adapter a;
    if (!a.open(nullptr)) {
        err = "The Wi-Fi adapter isn't plugged in.";
        return false;
    }
    std::wstring inf = winusb_inf();
    if (inf.empty()) {
        err = error_text("Couldn't find Windows' winusb.inf");
        return false;
    }
    SP_DRVINFO_DATA_W drv{ sizeof(SP_DRVINFO_DATA_W) };
    const GUID was = a.dev.ClassGuid;
    bool offered = use_single_inf(a, inf) && select_winusb(a, drv);
    if (!offered) {
        if (IsEqualGUID(was, GUID_DEVCLASS_USBDEVICE)) {
            err = error_text("Windows didn't offer its WinUSB driver for the adapter");
            return false;
        }
        if (!set_class(a, GUID_DEVCLASS_USBDEVICE)) {
            err = error_text("Couldn't move the adapter to Windows' USB devices");
            return false;
        }
        bool ok = false;
        {
            /* A set opened afresh sees the device in its new class. */
            Adapter b;
            offered = b.open(nullptr) && use_single_inf(b, inf) && select_winusb(b, drv);
            if (offered) {
                BOOL reboot = FALSE;
                if (DiInstallDevice(nullptr, b.set, &b.dev, &drv, 0, &reboot)) {
                    ok = true;
                } else {
                    err = error_text("Couldn't give the adapter the WinUSB driver");
                }
            } else {
                err = error_text("Windows didn't offer its WinUSB driver for the adapter");
            }
        }
        if (ok) return true;
        restore_class(a, was);
        return false;
    }
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, a.set, &a.dev, &drv, 0, &reboot)) {
        err = error_text("Couldn't give the adapter the WinUSB driver");
        return false;
    }
    return true;
}

bool adapter_use_windows_driver(std::string& err)
{
    Adapter a;
    if (!a.open(nullptr)) {
        err = "The Wi-Fi adapter isn't plugged in.";
        return false;
    }
    const GUID was = a.dev.ClassGuid;
    const bool moved = !IsEqualGUID(was, GUID_DEVCLASS_NET);
    if (moved) {
        if (!set_class(a, GUID_DEVCLASS_NET)) {
            err = error_text("Couldn't move the adapter back to Windows' network adapters");
            return false;
        }
        bool ok = false;
        {
            /* A set opened afresh sees the device in its restored class. */
            Adapter b;
            if (!b.open(nullptr)) {
                err = error_text("Couldn't reopen the adapter in Windows' network adapters");
            } else {
                ok = install_windows_driver_in_set(b, err);
            }
        }
        if (ok) return true;
        restore_class(a, was);
        return false;
    }
    return install_windows_driver_in_set(a, err);
}

} // namespace lares
