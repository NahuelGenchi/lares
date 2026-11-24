// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The adapter's one-time driver switch (issue #109): Device Manager's
 * "Let me pick from a list of drivers" done by SetupAPI, on the in-box
 * winusb.inf. */
#include "driver_setup.h"

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
    SP_DEVINSTALL_PARAMS_W p{ sizeof(SP_DEVINSTALL_PARAMS_W) };
    if (!SetupDiGetDeviceInstallParamsW(a.set, &a.dev, &p)) return false;
    p.Flags |= DI_ENUMSINGLEINF;
    p.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    wcsncpy(p.DriverPath, inf.c_str(), MAX_PATH - 1);
    return SetupDiSetDeviceInstallParamsW(a.set, &a.dev, &p) != FALSE;
}

std::string list_drivers(Adapter& a, DWORD type)
{
    std::string out;
    SP_DRVINFO_DATA_W drv{ sizeof(SP_DRVINFO_DATA_W) };
    for (DWORD i = 0; SetupDiEnumDriverInfoW(a.set, &a.dev, type, i, &drv); i++) {
        out += "  " + narrow(drv.Description) + " | " + narrow(drv.MfgName) + " | " + narrow(drv.ProviderName);
        DWORD need = 0;
        SetupDiGetDriverInfoDetailW(a.set, &a.dev, &drv, nullptr, 0, &need);
        std::vector<BYTE> buf(need ? need : 1);
        auto* detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        if (need && SetupDiGetDriverInfoDetailW(a.set, &a.dev, &drv, detail, need, nullptr)) {
            const wchar_t* slash = wcsrchr(detail->InfFileName, L'\\');
            out += " | " + narrow(slash ? slash + 1 : detail->InfFileName);
        }
        out += "\n";
    }
    if (out.empty()) out = "  (none)\n";
    return out;
}

/* The "WinUsb Device" model of winusb.inf, selected for the adapter. */
bool select_winusb(Adapter& a, SP_DRVINFO_DATA_W& chosen)
{
    SP_DRVINFO_DATA_W drv{ sizeof(SP_DRVINFO_DATA_W) };
    for (DWORD type : { (DWORD)SPDIT_CLASSDRIVER, (DWORD)SPDIT_COMPATDRIVER }) {
        if (!SetupDiBuildDriverInfoList(a.set, &a.dev, type)) continue;
        for (DWORD i = 0; SetupDiEnumDriverInfoW(a.set, &a.dev, type, i, &drv); i++) {
            if (_wcsicmp(drv.Description, L"WinUsb Device") != 0) continue;
            if (!SetupDiSetSelectedDriverW(a.set, &a.dev, &drv)) return false;
            chosen = drv;
            return true;
        }
    }
    return false;
}

} // namespace

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

namespace {

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

} // namespace

bool adapter_use_winusb(std::string& err)
{
    Adapter a;
    if (!a.open(nullptr)) {
        err = "The Wi-Fi adapter isn't plugged in.";
        return false;
    }
    std::wstring inf = winusb_inf();
    SP_DRVINFO_DATA_W drv{ sizeof(SP_DRVINFO_DATA_W) };
    GUID was = a.dev.ClassGuid;
    bool moved = false;
    bool offered = !inf.empty() && use_single_inf(a, inf) && select_winusb(a, drv);
    if (!offered) {
        moved = set_class(a, GUID_DEVCLASS_USBDEVICE);
        if (!moved) {
            err = error_text("Couldn't move the adapter to Windows' USB devices");
            return false;
        }
        /* A set opened afresh sees the device in its new class. */
        Adapter b;
        offered = b.open(nullptr) && use_single_inf(b, inf) && select_winusb(b, drv);
        if (offered) {
            BOOL reboot = FALSE;
            if (DiInstallDevice(nullptr, b.set, &b.dev, &drv, DIIRFLAG_FORCE_INF, &reboot)) return true;
            err = error_text("Couldn't give the adapter the WinUSB driver");
        } else {
            err = error_text("Windows didn't offer its WinUSB driver for the adapter");
        }
        set_class(a, was);
        return false;
    }
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, a.set, &a.dev, &drv, DIIRFLAG_FORCE_INF, &reboot)) {
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
    /* The best match for its hardware ID, as Windows would pick on a first
     * plug-in: Microsoft's netrtwlanu.inf on this PC. */
    if (!SetupDiBuildDriverInfoList(a.set, &a.dev, SPDIT_COMPATDRIVER) ||
        !SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, a.set, &a.dev)) {
        err = error_text("Windows has no driver of its own for the adapter");
        return false;
    }
    SP_DRVINFO_DATA_W drv{ sizeof(SP_DRVINFO_DATA_W) };
    if (!SetupDiGetSelectedDriverW(a.set, &a.dev, &drv)) {
        err = error_text("Windows has no driver of its own for the adapter");
        return false;
    }
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, a.set, &a.dev, &drv, 0, &reboot)) {
        err = error_text("Couldn't give the adapter back to Windows' driver");
        return false;
    }
    return true;
}

} // namespace lares
