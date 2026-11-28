// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar's AirDrop panel (issue #155) on Lares' own sharing code: a C
 * face for win/src/share_win.cpp's ShareManager, which Lares' Share page
 * uses too, and for the Bluetooth wake-up (ble_wake.cpp). Nothing of it
 * exists until lares_share_open; lares_share_close ends the session, and
 * the adapter powers down. AirDrop is a trademark of Apple Inc.; Lares is
 * not affiliated with Apple. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ShareManager's update message, posted to the window lares_share_open
 * names; that window calls lares_share_updated on it. */
#define LARES_SHARE_UPDATED_MSG (WM_APP + 5)

enum { LARES_SHARE_NO_ADAPTER = 0, LARES_SHARE_NEEDS_SETUP, LARES_SHARE_READY, LARES_SHARE_NO_PROGRAM };
enum { LARES_SHARE_LINK_OFF = 0, LARES_SHARE_LINK_STARTING, LARES_SHARE_LINK_UP, LARES_SHARE_LINK_STOPPING,
       LARES_SHARE_LINK_FAILED };
enum { LARES_SHARE_MODE_OFF = 0, LARES_SHARE_MODE_CONTACTS, LARES_SHARE_MODE_EVERYONE };
enum { LARES_SHARE_IN_ASKING = 0, LARES_SHARE_IN_RECEIVING, LARES_SHARE_IN_DONE, LARES_SHARE_IN_FAILED };
enum { LARES_SHARE_DEV_FOUND = 0, LARES_SHARE_DEV_ASKING, LARES_SHARE_DEV_SENDING, LARES_SHARE_DEV_SENT,
       LARES_SHARE_DEV_DECLINED, LARES_SHARE_DEV_FAILED };
enum { LARES_SHARE_KIND_PHONE = 0, LARES_SHARE_KIND_TABLET, LARES_SHARE_KIND_LAPTOP, LARES_SHARE_KIND_DESKTOP,
       LARES_SHARE_KIND_WATCH, LARES_SHARE_KIND_OTHER };
enum { LARES_SHARE_WAKE_OFF = 0, LARES_SHARE_WAKE_STARTING, LARES_SHARE_WAKE_ON, LARES_SHARE_WAKE_RADIO_OFF,
       LARES_SHARE_WAKE_FAILED };

#define LARES_SHARE_MAX_DEVICES 8

typedef struct {
    char     key[96];
    wchar_t  name[96];
    wchar_t  model[48];   /* "iPhone", "MacBook Air": what kind it is, in words */
    wchar_t  message[96];
    int      kind;
    int      state;
    uint64_t sent, total;
} LaresShareDevice;

typedef struct {
    bool     open;          /* lares_share_open ran */
    int      setup;
    bool     setup_busy;
    bool     running;       /* a session is on */
    int      link;
    int      mode;
    bool     receiving;
    int      left_s;        /* seconds of Everyone for 10 Minutes left, now */
    bool     browsing, sending, wake, quiet;
    int      wake_state;    /* the Bluetooth wake-up */
    int      contact_count; /* contacts saved in Lares, for Contacts Only */
    wchar_t  name[96];      /* how others see this PC */
    wchar_t  error[200];
    bool     has_incoming;
    int      in_state;
    uint64_t in_done, in_total;
    wchar_t  in_sender[96];
    wchar_t  in_what[160];  /* "IMG_0412.HEIC and 1 more" */
    wchar_t  in_saved[MAX_PATH];
    wchar_t  in_message[160];
    int      device_count;
    LaresShareDevice devices[LARES_SHARE_MAX_DEVICES];
} LaresShareState;

/* Whether lares-share.exe came beside this program. */
bool lares_share_available(void);
/* Creates the manager (a dry run with LARES_DRY_RUN), looks at the adapter,
 * and has its updates posted to `notify`. Calling it again does nothing. */
void lares_share_open(HWND notify);
/* Ends the session and the wake-up, and frees the manager. */
void lares_share_close(void);
bool lares_share_is_open(void);
/* On LARES_SHARE_UPDATED_MSG: a session asked for while one ended starts,
 * and the Bluetooth wake-up follows what the session wants. */
void lares_share_updated(void);
void lares_share_get(LaresShareState *out);

void lares_share_set_mode(int mode);
void lares_share_set_browsing(bool on);
void lares_share_answer(bool accept);
void lares_share_dismiss(void);
void lares_share_cancel_send(void);
/* Sends files (full paths) to the device with `key`. */
bool lares_share_send_to(const char *key, const wchar_t *const *paths, int count);

#ifdef __cplusplus
}
#endif
