// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Wi-Fi adapter over WinUSB (issue #109). Windows lists every USB
 * device under GUID_DEVINTERFACE_USB_DEVICE; once Set up has given the
 * adapter the in-box WinUSB driver, that path opens the WinUSB stack, as
 * libusb does. */
#include "rtl_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <usbiodef.h>
#include <winusb.h>

#define CONTROL_TIMEOUT_MS 500
#define VENDOR_REQUEST 0x05     /* Realtek's register access, as rtl8xxxu's */

struct rtl_usb {
    HANDLE file;
    WINUSB_INTERFACE_HANDLE wu;
    uint8_t in_ep;
    uint8_t out_ep[RTL_USB_MAX_OUT];
    unsigned nout;
    unsigned packet;
    SRWLOCK control;            /* one vendor request at a time */
};

/* The instance ID names the device: USB\VID_0BDA&PID_8179\<serial>. */
static bool is_adapter(const wchar_t *instance)
{
    wchar_t want[32];
    swprintf(want, 32, L"USB\\VID_%04X&PID_%04X\\", RTL_USB_VID, RTL_USB_PID);
    return _wcsnicmp(instance, want, wcslen(want)) == 0;
}

/* Walks the present USB devices for the adapter; `path` gets its interface
 * path when asked for, and the result says which driver it has. */
static enum rtl_usb_presence find(wchar_t *path, size_t pathlen)
{
    enum rtl_usb_presence found = RTL_USB_ABSENT;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVICE_INTERFACE_DATA iface = { sizeof iface };
    for (DWORD i = 0; found == RTL_USB_ABSENT && SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_USB_DEVICE, i, &iface); i++) {
        SP_DEVINFO_DATA dev = { sizeof dev };
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
        if (!detail) break;
        detail->cbSize = sizeof *detail;
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, NULL, &dev)) {
            wchar_t instance[256];
            if (SetupDiGetDeviceInstanceIdW(set, &dev, instance, 256, NULL) && is_adapter(instance)) {
                wchar_t service[64] = L"";
                SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_SERVICE, NULL, (BYTE *)service, sizeof service - sizeof(wchar_t), NULL);
                found = _wcsicmp(service, L"WinUSB") == 0 ? RTL_USB_WINUSB : RTL_USB_OTHER_DRIVER;
                if (path && pathlen) {
                    wcsncpy(path, detail->DevicePath, pathlen - 1);
                    path[pathlen - 1] = 0;
                }
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

enum rtl_usb_presence rtl_usb_find(void)
{
    return find(NULL, 0);
}

struct rtl_usb *rtl_usb_open(char *err, size_t errlen)
{
    wchar_t path[512];
    enum rtl_usb_presence p = find(path, 512);
    if (p == RTL_USB_ABSENT) {
        snprintf(err, errlen, "The Wi-Fi adapter isn't plugged in.");
        return NULL;
    }
    if (p != RTL_USB_WINUSB) {
        snprintf(err, errlen, "The Wi-Fi adapter is on Windows' own driver. Set AirDrop up first.");
        return NULL;
    }
    struct rtl_usb *u = (struct rtl_usb *)calloc(1, sizeof *u);
    if (!u) {
        snprintf(err, errlen, "Out of memory.");
        return NULL;
    }
    InitializeSRWLock(&u->control);
    u->file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (u->file == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        snprintf(err, errlen, e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION
                                  ? "The Wi-Fi adapter is in use by another program."
                                  : "Couldn't open the Wi-Fi adapter (error %lu).", e);
        free(u);
        return NULL;
    }
    if (!WinUsb_Initialize(u->file, &u->wu)) {
        snprintf(err, errlen, "Couldn't start WinUSB on the Wi-Fi adapter (error %lu).", GetLastError());
        CloseHandle(u->file);
        free(u);
        return NULL;
    }
    ULONG timeout = CONTROL_TIMEOUT_MS;
    WinUsb_SetPipePolicy(u->wu, 0, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);

    USB_INTERFACE_DESCRIPTOR alt;
    if (WinUsb_QueryInterfaceSettings(u->wu, 0, &alt)) {
        for (UCHAR i = 0; i < alt.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pipe;
            if (!WinUsb_QueryPipe(u->wu, 0, i, &pipe) || pipe.PipeType != UsbdPipeTypeBulk) continue;
            if (pipe.PipeId & 0x80) {
                if (!u->in_ep) {
                    u->in_ep = pipe.PipeId;
                    u->packet = pipe.MaximumPacketSize;
                }
            } else if (u->nout < RTL_USB_MAX_OUT) {
                u->out_ep[u->nout++] = pipe.PipeId;
                /* A transfer that fills whole packets ends with an empty
                 * one, so the chip knows where it stops. */
                UCHAR on = TRUE;
                WinUsb_SetPipePolicy(u->wu, pipe.PipeId, SHORT_PACKET_TERMINATE, sizeof on, &on);
            }
        }
    }
    if (!u->in_ep || !u->nout) {
        snprintf(err, errlen, "The Wi-Fi adapter doesn't show the USB endpoints this chip has.");
        rtl_usb_close(u);
        return NULL;
    }
    /* OUT endpoints in address order, as rtl8xxxu numbers them. */
    for (unsigned a = 0; a < u->nout; a++)
        for (unsigned b = a + 1; b < u->nout; b++)
            if (u->out_ep[b] < u->out_ep[a]) {
                uint8_t t = u->out_ep[a];
                u->out_ep[a] = u->out_ep[b];
                u->out_ep[b] = t;
            }
    return u;
}

void rtl_usb_close(struct rtl_usb *u)
{
    if (!u) return;
    if (u->wu) WinUsb_Free(u->wu);
    if (u->file && u->file != INVALID_HANDLE_VALUE) CloseHandle(u->file);
    free(u);
}

uint8_t rtl_usb_in_ep(const struct rtl_usb *u) { return u->in_ep; }
unsigned rtl_usb_out_count(const struct rtl_usb *u) { return u->nout; }
uint8_t rtl_usb_out_ep(const struct rtl_usb *u, unsigned i) { return i < u->nout ? u->out_ep[i] : 0; }
unsigned rtl_usb_packet_size(const struct rtl_usb *u) { return u->packet; }

static int control(struct rtl_usb *u, bool in, uint16_t addr, void *buf, uint16_t len)
{
    WINUSB_SETUP_PACKET s;
    s.RequestType = in ? 0xc0 : 0x40;
    s.Request = VENDOR_REQUEST;
    s.Value = addr;
    s.Index = 0;
    s.Length = len;
    ULONG done = 0;
    AcquireSRWLockExclusive(&u->control);
    BOOL ok = WinUsb_ControlTransfer(u->wu, s, (PUCHAR)buf, len, &done, NULL);
    ReleaseSRWLockExclusive(&u->control);
    return ok && done == len ? 0 : -1;
}

int rtl_usb_read(struct rtl_usb *u, uint16_t addr, void *buf, uint16_t len)
{
    return control(u, true, addr, buf, len);
}

int rtl_usb_write(struct rtl_usb *u, uint16_t addr, const void *buf, uint16_t len)
{
    return control(u, false, addr, (void *)buf, len);
}

int rtl_xfer_init(struct rtl_xfer *x, size_t cap)
{
    memset(x, 0, sizeof *x);
    x->buf = (uint8_t *)malloc(cap);
    OVERLAPPED *ov = (OVERLAPPED *)calloc(1, sizeof *ov);
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!x->buf || !ov || !ev) {
        free(x->buf);
        free(ov);
        if (ev) CloseHandle(ev);
        memset(x, 0, sizeof *x);
        return -1;
    }
    ov->hEvent = ev;
    x->os = ov;
    x->event = ev;
    x->cap = cap;
    return 0;
}

void rtl_xfer_free(struct rtl_xfer *x)
{
    if (x->event) CloseHandle((HANDLE)x->event);
    free(x->os);
    free(x->buf);
    memset(x, 0, sizeof *x);
}

static int started(struct rtl_xfer *x, BOOL ok)
{
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        x->busy = false;
        return -1;
    }
    x->busy = true;
    return 0;
}

int rtl_usb_read_start(struct rtl_usb *u, struct rtl_xfer *x)
{
    OVERLAPPED *ov = (OVERLAPPED *)x->os;
    HANDLE ev = ov->hEvent;
    memset(ov, 0, sizeof *ov);
    ov->hEvent = ev;
    ResetEvent(ev);
    x->len = 0;
    return started(x, WinUsb_ReadPipe(u->wu, u->in_ep, x->buf, (ULONG)x->cap, NULL, ov));
}

int rtl_usb_write_start(struct rtl_usb *u, uint8_t ep, struct rtl_xfer *x)
{
    OVERLAPPED *ov = (OVERLAPPED *)x->os;
    HANDLE ev = ov->hEvent;
    memset(ov, 0, sizeof *ov);
    ov->hEvent = ev;
    ResetEvent(ev);
    return started(x, WinUsb_WritePipe(u->wu, ep, x->buf, (ULONG)x->len, NULL, ov));
}

int rtl_usb_finish(struct rtl_usb *u, struct rtl_xfer *x)
{
    if (!x->busy) return 1;
    ULONG n = 0;
    if (WinUsb_GetOverlappedResult(u->wu, (OVERLAPPED *)x->os, &n, FALSE)) {
        x->busy = false;
        x->len = n;
        return 1;
    }
    DWORD e = GetLastError();
    if (e == ERROR_IO_INCOMPLETE) return 0;
    x->busy = false;
    x->len = 0;
    return e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_BAD_COMMAND || e == ERROR_FILE_NOT_FOUND ||
                   e == ERROR_NO_SUCH_DEVICE
               ? -2
               : -1;
}

void rtl_usb_abort(struct rtl_usb *u)
{
    WinUsb_AbortPipe(u->wu, u->in_ep);
    for (unsigned i = 0; i < u->nout; i++) WinUsb_AbortPipe(u->wu, u->out_ep[i]);
}
