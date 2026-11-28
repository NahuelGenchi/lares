// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar's device indicators (issue #157): a controller, Bluetooth
 * headphones, a keyboard or mouse Lares can light or set up, and Remote
 * mode, each an icon that opens Lares on that page. What is there is read
 * by win/src/indicators.cpp, Lares' own detection, on a worker thread:
 * once when the switch comes on, then when Windows says devices came or
 * went (a moment after, so a burst of arrivals is one look) and when the
 * screen changes, which Remote mode does. Nothing polls. */
#include "shell.h"
#include <dbt.h>

#define TIMER_INDICATORS 204
#define INDICATORS_SETTLE_MS 1500

static bool             g_on = false;
static CRITICAL_SECTION g_cs;
static bool             g_cs_ready = false;
static LaresIndicators  g_now;
static HDEVNOTIFY       g_notify = NULL;
static HANDLE           g_thread = NULL;
static bool             g_again = false;
static bool             g_stuck = false; /* a look outlived stop(): its lock must stay */
static bool             g_has_test = false;
static LaresIndicators  g_test;

static DWORD WINAPI scan_thread(LPVOID unused) {
    (void)unused;
    LaresIndicators r;
    lares_indicators_scan(&r);
    EnterCriticalSection(&g_cs);
    g_now = r;
    LeaveCriticalSection(&g_cs);
    if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_INDICATORS_UPDATED, 0, 0);
    return 0;
}

static void rescan(void) {
    if (!g_on) return;
    if (g_thread) {
        g_again = true; /* one more look once this one ends */
        return;
    }
    g_thread = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
}

static void start(void) {
    if (g_on || g_stuck) return;
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
    }
    ZeroMemory(&g_now, sizeof(g_now));
    g_now.controller_battery = LARES_PAD_BATTERY_UNKNOWN;
    g_on = true;
    DEV_BROADCAST_DEVICEINTERFACE_W filter;
    ZeroMemory(&filter, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    g_notify = RegisterDeviceNotificationW(g_hHostWnd, &filter,
                                           DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
    rescan();
}

static void stop(void) {
    if (!g_on) return;
    g_on = false;
    g_again = false;
    if (g_notify) {
        UnregisterDeviceNotification(g_notify);
        g_notify = NULL;
    }
    if (g_hHostWnd) KillTimer(g_hHostWnd, TIMER_INDICATORS);
    if (g_thread) {
        if (WaitForSingleObject(g_thread, 5000) == WAIT_TIMEOUT) g_stuck = true;
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stuck) return;
    EnterCriticalSection(&g_cs);
    ZeroMemory(&g_now, sizeof(g_now));
    LeaveCriticalSection(&g_cs);
}

void indicators_apply_settings(void) {
    bool want = g_cfg.indicators_enabled && g_cfg.topbar_enabled && !g_has_test;
    if (want) start();
    else stop();
}

void indicators_shutdown(void) {
    stop();
    if (g_cs_ready && !g_stuck) {
        DeleteCriticalSection(&g_cs);
        g_cs_ready = false;
    }
}

/* WM_DEVICECHANGE and WM_DISPLAYCHANGE: look again once things settle. */
void indicators_on_change(void) {
    if (g_on && g_hHostWnd) SetTimer(g_hHostWnd, TIMER_INDICATORS, INDICATORS_SETTLE_MS, NULL);
}

bool indicators_on_timer(UINT_PTR id) {
    if (id != TIMER_INDICATORS) return false;
    KillTimer(g_hHostWnd, TIMER_INDICATORS);
    rescan();
    return true;
}

/* WM_LARES_INDICATORS_UPDATED: the look ended. */
void indicators_on_updated(void) {
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE); /* it has posted: it is ending */
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_again) {
        g_again = false;
        rescan();
    }
    topbar_repaint();
}

void indicators_get(LaresIndicators *out) {
    if (!out) return;
    if (g_has_test) {
        *out = g_test;
        return;
    }
    ZeroMemory(out, sizeof(*out));
    out->controller_battery = LARES_PAD_BATTERY_UNKNOWN;
    if (!g_on || !g_cs_ready) return;
    EnterCriticalSection(&g_cs);
    *out = g_now;
    LeaveCriticalSection(&g_cs);
}

/* Test copies draw the indicators from made-up state. */
void indicators_set_test_state(const LaresIndicators *s) {
    if (s) {
        g_test = *s;
        g_has_test = true;
    } else {
        g_has_test = false;
    }
}
