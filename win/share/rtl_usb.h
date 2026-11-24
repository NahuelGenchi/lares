// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* The Wi-Fi adapter over WinUSB (issue #109): finding it, its vendor
 * requests (register reads and writes) and its bulk pipes. What the chip
 * does with them is rtl8188eu.c's. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_USB_VID 0x0bda
#define RTL_USB_PID 0x8179
#define RTL_USB_MAX_OUT 4

struct rtl_usb;

/* Where the adapter is and which driver Windows gives it. */
enum rtl_usb_presence {
    RTL_USB_ABSENT,         /* not plugged in */
    RTL_USB_OTHER_DRIVER,   /* there, but on Windows' own driver (or none) */
    RTL_USB_WINUSB,         /* there, on WinUSB: Lares can open it */
};
enum rtl_usb_presence rtl_usb_find(void);

/* Opens the adapter on WinUSB. NULL with `err` if it isn't there or is
 * busy. */
struct rtl_usb *rtl_usb_open(char *err, size_t errlen);
void rtl_usb_close(struct rtl_usb *u);

/* The bulk endpoints, as the device lists them: one IN, and the OUT ones in
 * address order. */
uint8_t rtl_usb_in_ep(const struct rtl_usb *u);
unsigned rtl_usb_out_count(const struct rtl_usb *u);
uint8_t rtl_usb_out_ep(const struct rtl_usb *u, unsigned i);
/* The bulk IN endpoint's packet size (512 at high speed, 64 at full). */
unsigned rtl_usb_packet_size(const struct rtl_usb *u);

/* Realtek's vendor request 0x05: `len` bytes at register `addr`. 0, or -1
 * when the transfer failed. Synchronous, with a short timeout; safe from
 * any thread. */
int rtl_usb_read(struct rtl_usb *u, uint16_t addr, void *buf, uint16_t len);
int rtl_usb_write(struct rtl_usb *u, uint16_t addr, const void *buf, uint16_t len);

/* Overlapped bulk transfers. A transfer is a buffer and an event that is
 * signalled when it completes; rtl_usb_finish then says how it went. */
struct rtl_xfer {
    uint8_t *buf;
    size_t cap;             /* how much a read may take */
    size_t len;             /* a write's length; a finished read's */
    void *os;               /* the OVERLAPPED, owned by usb.c */
    void *event;            /* its event HANDLE, for WaitForMultipleObjects */
    bool busy;
};
int rtl_xfer_init(struct rtl_xfer *x, size_t cap);
void rtl_xfer_free(struct rtl_xfer *x);
/* Starts a read into x->buf (up to x->cap) or a write of x->len bytes. 0,
 * or -1 when it couldn't start. */
int rtl_usb_read_start(struct rtl_usb *u, struct rtl_xfer *x);
int rtl_usb_write_start(struct rtl_usb *u, uint8_t ep, struct rtl_xfer *x);
/* After x->event: 1 done (a read's length in x->len), 0 not yet, -1 failed
 * (-2: the adapter is gone). */
int rtl_usb_finish(struct rtl_usb *u, struct rtl_xfer *x);
/* Cancels what is pending on every pipe (on the way out). */
void rtl_usb_abort(struct rtl_usb *u);

#ifdef __cplusplus
}
#endif
