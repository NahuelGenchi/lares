// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar's popups (issue #147): calendar, keyboard layout, Bluetooth,
 * network and Tailscale, media and volume, notifications, power, and the
 * screenshot menu. One see-through window, drawn when it opens and when
 * something it shows changes; its memory goes back when it closes. */
#include "shell.h"
#include "programs.h"
#include <shellapi.h>
#include <powrprof.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wlanapi.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <bluetoothapis.h>
#pragma warning(pop)
#include <winsvc.h>
#include <stdio.h>

#define POPUP_CLASS L"LaresPopupWnd"

/* TailscalePeer layout must match telemetry.c */
typedef struct {
    wchar_t name[96];
    wchar_t ip[64];
    bool    online;
    bool    can_exit_node;
    bool    is_active_exit_node;
} PopupTsPeer;

extern int telemetry_get_ts_peers(void *out_buf, int max_peers);

/* Cached Bluetooth device entry */
typedef struct {
    wchar_t name[128];
    bool    connected;
    bool    remembered;
} BtDevItem;

/* Audio render endpoint entry */
typedef struct {
    wchar_t name[128];
    bool    is_default;
} AudioDevItem;

typedef struct {
    int  action_id;
    int  param;
    RECT rc;
    wchar_t str_param[128];
} PopupClickRect;

#define MAX_POPUP_RECTS 64

static const GUID LARES_CLSID_MMDevEnum2 =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID LARES_IID_IMMDevEnum2 =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const PROPERTYKEY LARES_PKEY_DevFriendlyName =
    { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };

static HWND         g_hPopupWnd = NULL;
static LayerSurface g_pop_surf = {0};
static PopupKind    g_pop_kind = POPUP_NONE;
static int          g_pop_x = 0, g_pop_y = 0, g_pop_w = 300, g_pop_h = 300;
static int          g_anchor_x = 0, g_anchor_y = 38;
static HWND         g_last_fg_wnd = NULL;

static PopupClickRect g_rects[MAX_POPUP_RECTS];
static int            g_rect_count = 0;
static int            g_hover_rect = -1;
static bool           g_dragging_vol = false;
static RECT           g_vol_track_rc = {0};
static DWORD          g_copied_ip_tick = 0;

/* Open and close transition: a short fade with a few pixels of travel. */
#define TIMER_POP_ANIM 301
#define POP_ANIM_IN_MS  140
#define POP_ANIM_OUT_MS 90
static float g_pop_vis = 1.0f, g_pop_vis_from = 1.0f, g_pop_vis_to = 1.0f;
static DWORD g_pop_anim_start = 0;
static bool  g_pop_closing = false;


/* Calendar view state */
static int g_cal_year = 2026;
static int g_cal_month = 11;

/* Keyboard layouts */
static HKL g_hkls[16];
static int g_hkl_count = 0;

/* Bluetooth devices (cached; manual scan runs on worker thread) */
static CRITICAL_SECTION g_bt_cs;
static BtDevItem g_bt_devs[24];
static int       g_bt_dev_count = 0;
static volatile LONG g_bt_scanning = 0;

/* Audio output devices */
static AudioDevItem g_audio_devs[8];
static int          g_audio_dev_count = 0;

/* Tailscale peer snapshot */
static PopupTsPeer g_ts_peers[32];
static int         g_ts_peer_count = 0;
/* Pending "Manual startup" preference (-1 = follow the real service start type).
 * It is applied together with the next Start/Stop so UAC is only requested for those. */
static int         g_ts_manual_pref = -1;

/* Action IDs */
enum {
    ACT_CAL_PREV = 1,
    ACT_CAL_NEXT,
    ACT_CAL_TODAY,
    ACT_KBD_SELECT,
    ACT_BT_SCAN,
    ACT_BT_SETTINGS,
    ACT_NET_SETTINGS,
    ACT_TS_TOGGLE,
    ACT_TS_COPY_IP,
    ACT_TS_REFRESH,
    ACT_TS_EXIT_NODE,
    ACT_TS_SERVICE,
    ACT_TS_MANUAL,
    ACT_MEDIA_PREV,
    ACT_MEDIA_PLAY,
    ACT_MEDIA_NEXT,
    ACT_VOL_MUTE,
    ACT_VOL_SLIDER,
    ACT_AUDIO_SETTINGS,
    ACT_NOTIF_SETTINGS,
    ACT_NOTIF_WIN_PANEL,
    ACT_PWR_LOCK,
    ACT_PWR_SLEEP,
    ACT_PWR_SIGNOUT,
    ACT_PWR_RESTART,
    ACT_PWR_SHUTDOWN,
    ACT_PWR_FILES,
    ACT_PWR_QUIT,
    ACT_SS_REGION,
    ACT_SS_WINDOW,
    ACT_SS_FULL
};

static void add_click(int action, int param, int x, int y, int w, int h, const wchar_t *sp) {
    if (g_rect_count >= MAX_POPUP_RECTS) return;
    PopupClickRect *r = &g_rects[g_rect_count++];
    r->action_id = action;
    r->param = param;
    r->rc.left = x;
    r->rc.top = y;
    r->rc.right = x + w;
    r->rc.bottom = y + h;
    if (sp) wcsncpy_s(r->str_param, 128, sp, _TRUNCATE);
    else r->str_param[0] = L'\0';
}

/* Enumerate paired/connected Bluetooth devices. active_scan MUST be FALSE on open! */
static void load_bt_devices(bool active_scan) {
    BtDevItem tmp[24];
    int cnt = 0;

    BLUETOOTH_DEVICE_SEARCH_PARAMS sp;
    ZeroMemory(&sp, sizeof(sp));
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = active_scan ? TRUE : FALSE;
    sp.fIssueInquiry = active_scan ? TRUE : FALSE;
    sp.cTimeoutMultiplier = active_scan ? 3 : 0;

    BLUETOOTH_DEVICE_INFO_STRUCT di;
    ZeroMemory(&di, sizeof(di));
    di.dwSize = sizeof(di);
    HBLUETOOTH_DEVICE_FIND hf = BluetoothFindFirstDevice(&sp, &di);
    if (hf) {
        do {
            if (cnt < 24 && di.szName[0]) {
                wcsncpy_s(tmp[cnt].name, 128, di.szName, _TRUNCATE);
                tmp[cnt].connected  = (di.fConnected != FALSE);
                tmp[cnt].remembered = (di.fRemembered != FALSE);
                cnt++;
            }
        } while (BluetoothFindNextDevice(hf, &di));
        BluetoothFindDeviceClose(hf);
    }

    /* Sort connected devices first */
    for (int i = 0; i < cnt; i++) {
        for (int j = i + 1; j < cnt; j++) {
            if (!tmp[i].connected && tmp[j].connected) {
                BtDevItem sw = tmp[i];
                tmp[i] = tmp[j];
                tmp[j] = sw;
            }
        }
    }

    EnterCriticalSection(&g_bt_cs);
    g_bt_dev_count = cnt;
    if (cnt > 0) memcpy(g_bt_devs, tmp, (size_t)cnt * sizeof(BtDevItem));
    LeaveCriticalSection(&g_bt_cs);
}

static DWORD WINAPI bt_scan_thread(LPVOID lp) {
    (void)lp;
    load_bt_devices(true);
    InterlockedExchange(&g_bt_scanning, 0);
    if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_BT_SCAN_DONE, 0, 0);
    return 0;
}

static void load_audio_endpoints(void) {
    g_audio_dev_count = 0;
    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(&LARES_CLSID_MMDevEnum2, NULL, CLSCTX_ALL, &LARES_IID_IMMDevEnum2, (void **)&en)) || !en) {
        return;
    }

    LPWSTR def_id = NULL;
    IMMDevice *def_dev = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &def_dev)) && def_dev) {
        IMMDevice_GetId(def_dev, &def_id);
        IMMDevice_Release(def_dev);
    }

    IMMDeviceCollection *col = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT count = 0;
        IMMDeviceCollection_GetCount(col, &count);
        for (UINT i = 0; i < count && g_audio_dev_count < 8; i++) {
            IMMDevice *dev = NULL;
            if (SUCCEEDED(IMMDeviceCollection_Item(col, i, &dev)) && dev) {
                LPWSTR id = NULL;
                IMMDevice_GetId(dev, &id);
                IPropertyStore *ps = NULL;
                if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(IPropertyStore_GetValue(ps, &LARES_PKEY_DevFriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                        AudioDevItem *ad = &g_audio_devs[g_audio_dev_count++];
                        wcsncpy_s(ad->name, 128, pv.pwszVal, _TRUNCATE);
                        ad->is_default = (def_id && id && wcscmp(def_id, id) == 0);
                    }
                    PropVariantClear(&pv);
                    IPropertyStore_Release(ps);
                }
                if (id) CoTaskMemFree(id);
                IMMDevice_Release(dev);
            }
        }
        IMMDeviceCollection_Release(col);
    }

    if (def_id) CoTaskMemFree(def_id);
    IMMDeviceEnumerator_Release(en);
}

static int days_in_month(int y, int m) {
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) return 29;
    return dim[(m - 1) % 12];
}

static int day_of_week(int y, int m, int d) {
    /* Sakamoto's algorithm: 0=Sun..6=Sat */
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void draw_row_hover(GpGraphics *g, int rect_idx, float r) {
    if (g_hover_rect == rect_idx && rect_idx >= 0 && rect_idx < g_rect_count) {
        const RECT *rc = &g_rects[rect_idx].rc;
        draw_round_rect_fill(g, (float)rc->left, (float)rc->top,
                             (float)(rc->right - rc->left), (float)(rc->bottom - rc->top),
                             r, g_theme.accent_overlay_hover);
    }
}

static void popup_present(void) {
    float v = g_pop_vis;
    int a = (int)(v * 255.0f + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    int dy = (int)((1.0f - v) * 8.0f + 0.5f);
    surface_present(g_hPopupWnd, &g_pop_surf, g_pop_x, g_pop_y - dy, (BYTE)a);
}

static void compute_popup_size(int *out_w, int *out_h) {
    switch (g_pop_kind) {
    case POPUP_CALENDAR:
        *out_w = 312; *out_h = 342; break;
    case POPUP_KEYBOARD:
        *out_w = 276;
        *out_h = 64 + max(1, g_hkl_count) * 40 + 10;
        break;
    case POPUP_BLUETOOTH: {
        EnterCriticalSection(&g_bt_cs);
        int cnt = g_bt_dev_count;
        LeaveCriticalSection(&g_bt_cs);
        int rows = min(cnt, 7);
        if (rows < 1) rows = 1;
        *out_w = 320;
        *out_h = 104 + rows * 38 + 44;
        break;
    }
    case POPUP_NETWORK: {
        int exits = 0;
        for (int i = 0; i < g_ts_peer_count; i++) {
            if (g_ts_peers[i].can_exit_node) exits++;
        }
        if (exits > 4) exits = 4;
        *out_w = 340;
        *out_h = 260 + (g_telemetry.ts_available ? (exits > 0 ? (34 + (exits + 1) * 30) : 0) : 0)
                     + (g_telemetry.ts_svc_state != 0 ? 74 : 0);
        break;
    }
    case POPUP_MEDIA:
        *out_w = 324;
        *out_h = 216 + min(g_audio_dev_count, 4) * 28 + 18;
        break;
    case POPUP_NOTIFICATIONS:
        *out_w = 304; *out_h = 150; break;
    case POPUP_POWER:
        *out_w = 248; *out_h = 314; break;
    case POPUP_SCREENSHOT:
        *out_w = 272; *out_h = 166; break;
    default:
        *out_w = 280; *out_h = 200; break;
    }
}

static void popup_paint(void) {
    if (!g_hPopupWnd || g_pop_kind == POPUP_NONE) return;

    int pw = 300, ph = 300;
    compute_popup_size(&pw, &ph);
    int pad_sh = 10;
    g_pop_w = pw + pad_sh * 2;
    g_pop_h = ph + pad_sh * 2;

    HMONITOR mon = MonitorFromPoint((POINT){ g_anchor_x, g_anchor_y }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    g_pop_x = g_anchor_x - g_pop_w / 2;
    g_pop_y = g_anchor_y - pad_sh + 2;
    if (g_pop_x < mi.rcMonitor.left + 4) g_pop_x = mi.rcMonitor.left + 4;
    if (g_pop_x + g_pop_w > mi.rcMonitor.right - 4) g_pop_x = mi.rcMonitor.right - 4 - g_pop_w;
    if (g_pop_y + g_pop_h > mi.rcMonitor.bottom - 8) g_pop_y = mi.rcMonitor.bottom - 8 - g_pop_h;

    if (!surface_ensure(&g_pop_surf, g_pop_w, g_pop_h)) return;
    GpGraphics *g = g_pop_surf.g;
    GdipGraphicsClear(g, 0);

    int ox = pad_sh, oy = pad_sh;
    /* Soft drop shadow + main rounded popover card */
    draw_round_rect_fill(g, (float)(ox + 2), (float)(oy + 4), (float)pw, (float)ph, 12.0f, MAKE_ARGB(56, 0, 0, 0));
    draw_round_rect_fill(g, (float)ox, (float)oy, (float)pw, (float)ph, 12.0f, g_theme.popup_bg);
    draw_round_rect_stroke(g, (float)ox + 0.5f, (float)oy + 0.5f, (float)pw - 1.0f, (float)ph - 1.0f,
                           12.0f, g_theme.popup_border, 1.0f);

    g_rect_count = 0;

    /* ===================== 1. CALENDAR POPUP ===================== */
    if (g_pop_kind == POPUP_CALENDAR) {
        static const wchar_t *mnames[12] = {
            L"January", L"February", L"March", L"April", L"May", L"June",
            L"July", L"August", L"September", L"October", L"November", L"December"
        };
        static const wchar_t *dnames[7] = {
            L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"
        };
        static const wchar_t *wdays[7] = { L"Su", L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa" };

        SYSTEMTIME now;
        GetLocalTime(&now);
        int hr12 = now.wHour % 12;
        if (hr12 == 0) hr12 = 12;

        wchar_t time_s[32], date_s[64];
        _snwprintf_s(time_s, 32, _TRUNCATE, L"%02d:%02u:%02u %ls", hr12, now.wMinute, now.wSecond,
                     now.wHour >= 12 ? L"PM" : L"AM");
        _snwprintf_s(date_s, 64, _TRUNCATE, L"%ls, %ls %u, %u",
                     dnames[now.wDayOfWeek % 7], mnames[(now.wMonth - 1) % 12], now.wDay, now.wYear);

        draw_text(g, time_s, L"Segoe UI", 24.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 16), (float)(oy + 12), (float)(pw - 32), 32.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        draw_text(g, date_s, L"Segoe UI", 12.5f, GP_FONT_REGULAR, g_theme.fg_secondary,
                  (float)(ox + 16), (float)(oy + 44), (float)(pw - 32), 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        draw_line(g, (float)(ox + 14), (float)(oy + 70), (float)(ox + pw - 14), (float)(oy + 70), g_theme.separator, 1.0f);

        /* Month bar */
        int my = oy + 80;
        wchar_t hdr[48];
        _snwprintf_s(hdr, 48, _TRUNCATE, L"%ls %d", mnames[(g_cal_month - 1) % 12], g_cal_year);
        draw_text(g, hdr, L"Segoe UI", 13.5f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 16), (float)my, 160.0f, 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        int idx_today = g_rect_count;
        add_click(ACT_CAL_TODAY, 0, ox + pw - 118, my, 46, 26, NULL);
        draw_row_hover(g, idx_today, 6.0f);
        draw_text(g, L"Today", L"Segoe UI", 11.5f, GP_FONT_BOLD, g_theme.accent,
                  (float)(ox + pw - 118), (float)my, 46.0f, 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

        int idx_prev = g_rect_count;
        add_click(ACT_CAL_PREV, 0, ox + pw - 66, my, 26, 26, NULL);
        draw_row_hover(g, idx_prev, 6.0f);
        draw_icon_glyph(g, 0xE76B, 11.0f, g_theme.fg_primary, (float)(ox + pw - 66), (float)my, 26.0f, 26.0f);

        int idx_next = g_rect_count;
        add_click(ACT_CAL_NEXT, 0, ox + pw - 38, my, 26, 26, NULL);
        draw_row_hover(g, idx_next, 6.0f);
        draw_icon_glyph(g, 0xE76C, 11.0f, g_theme.fg_primary, (float)(ox + pw - 38), (float)my, 26.0f, 26.0f);

        /* Weekday headers */
        int gx = ox + 16;
        int gy = oy + 114;
        int cell_w = (pw - 32) / 7;
        int cell_h = 30;
        /* The week starts on Sunday or Monday, as shell.ini says. */
        int sow = (g_cfg.start_of_week == 1) ? 1 : 0;
        for (int c = 0; c < 7; c++) {
            draw_text(g, wdays[(c + sow) % 7], L"Segoe UI", 11.5f, GP_FONT_BOLD, g_theme.fg_muted,
                      (float)(gx + c * cell_w), (float)gy, (float)cell_w, 22.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        }

        /* 6x7 day cells */
        int first_dow = (day_of_week(g_cal_year, g_cal_month, 1) - sow + 7) % 7;
        int dim = days_in_month(g_cal_year, g_cal_month);
        int prev_m = (g_cal_month == 1) ? 12 : (g_cal_month - 1);
        int prev_y = (g_cal_month == 1) ? (g_cal_year - 1) : g_cal_year;
        int prev_dim = days_in_month(prev_y, prev_m);

        int start_y = gy + 26;
        for (int cell = 0; cell < 42; cell++) {
            int row = cell / 7;
            int col = cell % 7;
            int cx = gx + col * cell_w;
            int cy = start_y + row * cell_h;

            int day_num = 0;
            bool in_cur = false;
            bool is_today = false;
            if (cell < first_dow) {
                day_num = prev_dim - first_dow + cell + 1;
            } else if (cell < first_dow + dim) {
                day_num = cell - first_dow + 1;
                in_cur = true;
                is_today = (g_cal_year == now.wYear && g_cal_month == now.wMonth && day_num == now.wDay);
            } else {
                day_num = cell - (first_dow + dim) + 1;
            }

            if (is_today) {
                draw_circle_fill(g, (float)cx + cell_w * 0.5f, (float)cy + cell_h * 0.5f, 13.0f, g_theme.accent);
            }
            wchar_t dn[8];
            _snwprintf_s(dn, 8, _TRUNCATE, L"%d", day_num);
            ARGB col_fg = is_today ? g_theme.fg_on_accent : (in_cur ? g_theme.fg_primary : g_theme.fg_muted);
            draw_text(g, dn, L"Segoe UI", 12.0f, is_today ? GP_FONT_BOLD : GP_FONT_REGULAR, col_fg,
                      (float)cx, (float)cy, (float)cell_w, (float)cell_h, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        }
    }

    /* ===================== 2. KEYBOARD SELECTOR ===================== */
    else if (g_pop_kind == POPUP_KEYBOARD) {
        draw_text(g, L"Keyboard Layout", L"Segoe UI", 13.5f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 16), (float)(oy + 14), (float)(pw - 32), 24.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        draw_line(g, (float)(ox + 12), (float)(oy + 44), (float)(ox + pw - 12), (float)(oy + 44), g_theme.separator, 1.0f);

        int ry = oy + 52;
        for (int i = 0; i < g_hkl_count; i++) {
            LANGID lid = LOWORD((UINT_PTR)g_hkls[i]);
            LCID lcid = MAKELCID(lid, SORT_DEFAULT);
            wchar_t iso[16] = L"EN", full[128] = L"Keyboard";
            if (GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, iso, 16) > 0) {
                for (int k = 0; iso[k]; k++) iso[k] = (wchar_t)towupper(iso[k]);
            }
            GetLocaleInfoW(lcid, LOCALE_SLOCALIZEDDISPLAYNAME, full, 128);

            bool active = (g_hkls[i] == g_telemetry.kbd_hkl);
            int idx = g_rect_count;
            add_click(ACT_KBD_SELECT, i, ox + 10, ry, pw - 20, 36, NULL);
            if (active) {
                draw_round_rect_fill(g, (float)(ox + 10), (float)ry, (float)(pw - 20), 36.0f, 8.0f, g_theme.accent_overlay_selected);
            }
            draw_row_hover(g, idx, 8.0f);

            draw_round_rect_fill(g, (float)(ox + 18), (float)(ry + 6), 34.0f, 24.0f, 5.0f,
                                 active ? g_theme.accent : g_theme.popup_surface);
            draw_text(g, iso, L"Segoe UI", 11.0f, GP_FONT_BOLD,
                      active ? g_theme.fg_on_accent : g_theme.fg_primary,
                      (float)(ox + 18), (float)(ry + 6), 34.0f, 24.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
            draw_text(g, full, L"Segoe UI", 12.5f, active ? GP_FONT_BOLD : GP_FONT_REGULAR, g_theme.fg_primary,
                      (float)(ox + 60), (float)ry, (float)(pw - 96), 36.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
            if (active) {
                draw_icon_glyph(g, 0xE73E, 12.0f, g_theme.accent, (float)(ox + pw - 36), (float)ry, 22.0f, 36.0f);
            }
            ry += 40;
        }
    }

    /* ===================== 3. BLUETOOTH (NO SCAN ON OPEN!) ===================== */
    else if (g_pop_kind == POPUP_BLUETOOTH) {
        draw_icon_glyph(g, 0xE702, 16.0f, g_theme.accent, (float)(ox + 14), (float)(oy + 12), 24.0f, 26.0f);
        draw_text(g, L"Bluetooth", L"Segoe UI", 14.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 42), (float)(oy + 12), 140.0f, 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        bool scanning = InterlockedCompareExchange(&g_bt_scanning, 0, 0) != 0;
        int idx_scan = g_rect_count;
        add_click(ACT_BT_SCAN, 0, ox + pw - 96, oy + 12, 82, 26, NULL);
        draw_round_rect_fill(g, (float)(ox + pw - 96), (float)(oy + 12), 82.0f, 26.0f, 6.0f,
                             scanning ? g_theme.accent_overlay_selected : g_theme.popup_surface);
        draw_row_hover(g, idx_scan, 6.0f);
        draw_text(g, scanning ? L"Scanning..." : L"Scan", L"Segoe UI", 11.5f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + pw - 96), (float)(oy + 12), 82.0f, 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

        draw_text(g, L"Cached paired devices (audio-safe, no auto-scan)", L"Segoe UI", 11.0f, GP_FONT_REGULAR,
                  g_theme.fg_muted, (float)(ox + 16), (float)(oy + 42), (float)(pw - 32), 18.0f,
                  GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        draw_line(g, (float)(ox + 12), (float)(oy + 66), (float)(ox + pw - 12), (float)(oy + 66), g_theme.separator, 1.0f);

        EnterCriticalSection(&g_bt_cs);
        int cnt = g_bt_dev_count;
        int ry = oy + 74;
        if (cnt == 0) {
            draw_text(g, g_telemetry.bt_present ? L"No paired Bluetooth devices found" : L"Bluetooth adapter unavailable",
                      L"Segoe UI", 12.0f, GP_FONT_REGULAR, g_theme.fg_muted,
                      (float)(ox + 16), (float)ry, (float)(pw - 32), 34.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
            ry += 38;
        } else {
            int show_n = min(cnt, 7);
            for (int i = 0; i < show_n; i++) {
                draw_round_rect_fill(g, (float)(ox + 12), (float)ry, (float)(pw - 24), 34.0f, 8.0f, g_theme.popup_surface);
                draw_icon_glyph(g, 0xE7F6, 14.0f, g_bt_devs[i].connected ? g_theme.accent : g_theme.fg_muted,
                                (float)(ox + 18), (float)ry, 24.0f, 34.0f);
                draw_text(g, g_bt_devs[i].name, L"Segoe UI", 12.5f,
                          g_bt_devs[i].connected ? GP_FONT_BOLD : GP_FONT_REGULAR, g_theme.fg_primary,
                          (float)(ox + 46), (float)ry, (float)(pw - 134), 34.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                draw_text(g, g_bt_devs[i].connected ? L"Connected" : L"Paired", L"Segoe UI", 11.0f, GP_FONT_BOLD,
                          g_bt_devs[i].connected ? g_theme.success : g_theme.fg_muted,
                          (float)(ox + pw - 90), (float)ry, 70.0f, 34.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
                ry += 38;
            }
        }
        LeaveCriticalSection(&g_bt_cs);

        int foot_y = oy + ph - 38;
        int idx_set = g_rect_count;
        add_click(ACT_BT_SETTINGS, 0, ox + 12, foot_y, pw - 24, 28, NULL);
        draw_row_hover(g, idx_set, 6.0f);
        draw_text(g, L"Open Bluetooth Settings...", L"Segoe UI", 12.0f, GP_FONT_REGULAR, g_theme.accent,
                  (float)(ox + 16), (float)foot_y, (float)(pw - 32), 28.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
    }

    /* ===================== 4. NETWORK + TAILSCALE ===================== */
    else if (g_pop_kind == POPUP_NETWORK) {
        draw_text(g, L"Network & Tailscale", L"Segoe UI", 14.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 16), (float)(oy + 12), (float)(pw - 32), 24.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        /* Primary Adapter Card */
        int y = oy + 44;
        draw_round_rect_fill(g, (float)(ox + 12), (float)y, (float)(pw - 24), 56.0f, 10.0f, g_theme.popup_surface);
        wchar_t ng = !g_telemetry.net_connected ? 0xF384 : (g_telemetry.net_is_wifi ? 0xE701 : 0xE839);
        draw_icon_glyph(g, ng, 20.0f, g_telemetry.net_connected ? g_theme.accent : g_theme.fg_muted,
                        (float)(ox + 20), (float)y, 32.0f, 56.0f);
        const wchar_t *nname = g_telemetry.net_name[0] ? g_telemetry.net_name : L"Disconnected";
        draw_text(g, nname, L"Segoe UI", 13.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 58), (float)(y + 8), (float)(pw - 86), 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        wchar_t sub[96];
        if (g_telemetry.net_connected) {
            _snwprintf_s(sub, 96, _TRUNCATE, L"Connected  |  IPv4 %ls",
                         g_telemetry.net_ipv4[0] ? g_telemetry.net_ipv4 : L"-");
        } else {
            wcsncpy_s(sub, 96, L"No active network", _TRUNCATE);
        }
        draw_text(g, sub, L"Segoe UI", 11.5f, GP_FONT_REGULAR, g_theme.fg_secondary,
                  (float)(ox + 58), (float)(y + 28), (float)(pw - 86), 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

        /* Tailscale Card (directly controls official tailscaled via tailscale.exe) */
        y += 66;
        bool svc_known   = (g_telemetry.ts_svc_state != 0);
        bool svc_running = (!svc_known || g_telemetry.ts_svc_state == SERVICE_RUNNING);
        bool svc_busy    = g_telemetry.ts_svc_busy ||
                           g_telemetry.ts_svc_state == SERVICE_START_PENDING ||
                           g_telemetry.ts_svc_state == SERVICE_STOP_PENDING;
        int ts_card_h = 104 + (svc_known ? 74 : 0);
        draw_round_rect_fill(g, (float)(ox + 12), (float)y, (float)(pw - 24), (float)ts_card_h, 10.0f, g_theme.popup_surface);
        draw_circle_fill(g, (float)(ox + 28), (float)(y + 22), 5.0f,
                         g_telemetry.ts_running ? g_theme.success : g_theme.fg_muted);
        draw_text(g, L"Tailscale", L"Segoe UI", 13.5f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 40), (float)(y + 10), 110.0f, 24.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        /* Refresh icon */
        int idx_ref = g_rect_count;
        add_click(ACT_TS_REFRESH, 0, ox + pw - 122, y + 10, 26, 26, NULL);
        draw_row_hover(g, idx_ref, 6.0f);
        draw_icon_glyph(g, 0xE72C, 12.0f, g_theme.fg_secondary, (float)(ox + pw - 122), (float)(y + 10), 26.0f, 26.0f);

        if (svc_running) {
            /* Connect / Disconnect button (only meaningful while tailscaled is running) */
            int idx_tog = g_rect_count;
            add_click(ACT_TS_TOGGLE, g_telemetry.ts_running ? 0 : 1, ox + pw - 92, y + 10, 74, 26, NULL);
            draw_round_rect_fill(g, (float)(ox + pw - 92), (float)(y + 10), 74.0f, 26.0f, 6.0f,
                                 g_telemetry.ts_running ? g_theme.accent_overlay_selected : g_theme.accent);
            draw_row_hover(g, idx_tog, 6.0f);
            draw_text(g, g_telemetry.ts_running ? L"Disconnect" : L"Connect", L"Segoe UI", 11.0f, GP_FONT_BOLD,
                      g_telemetry.ts_running ? g_theme.fg_primary : g_theme.fg_on_accent,
                      (float)(ox + pw - 92), (float)(y + 10), 74.0f, 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        } else {
            draw_text(g, L"VPN off", L"Segoe UI", 11.0f, GP_FONT_BOLD, g_theme.fg_muted,
                      (float)(ox + pw - 92), (float)(y + 10), 74.0f, 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        }

        wchar_t ts_line1[160];
        if (svc_running) {
            _snwprintf_s(ts_line1, 160, _TRUNCATE, L"%ls  |  %ls  (%d peers)",
                         g_telemetry.ts_state[0] ? g_telemetry.ts_state : L"Checking...",
                         g_telemetry.ts_hostname[0] ? g_telemetry.ts_hostname : L"-",
                         g_telemetry.ts_peer_count);
        } else {
            wcsncpy_s(ts_line1, 160, L"Background service is stopped (no RAM used)", _TRUNCATE);
        }
        draw_text(g, ts_line1, L"Segoe UI", 11.5f, GP_FONT_REGULAR, g_theme.fg_secondary,
                  (float)(ox + 22), (float)(y + 42), (float)(pw - 44), 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

        if (svc_running) {
            /* Clickable Tailscale IP row (copies to clipboard) */
            int idx_ip = g_rect_count;
            add_click(ACT_TS_COPY_IP, 0, ox + 18, y + 66, pw - 36, 28, NULL);
            draw_row_hover(g, idx_ip, 6.0f);
            bool recently_copied = (g_copied_ip_tick != 0 && (GetTickCount() - g_copied_ip_tick) < 1800);
            wchar_t ip_lbl[96];
            if (recently_copied) {
                wcsncpy_s(ip_lbl, 96, L"Copied Tailscale IP to clipboard!", _TRUNCATE);
            } else {
                _snwprintf_s(ip_lbl, 96, _TRUNCATE, L"IP: %ls   (click to copy)",
                             g_telemetry.ts_ip[0] ? g_telemetry.ts_ip : L"N/A");
            }
            draw_text(g, ip_lbl, L"Segoe UI", 11.5f, GP_FONT_BOLD,
                      recently_copied ? g_theme.success : g_theme.accent,
                      (float)(ox + 24), (float)(y + 66), (float)(pw - 48), 28.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        } else {
            draw_text(g, L"Start the service below to connect", L"Segoe UI", 11.5f, GP_FONT_REGULAR, g_theme.fg_muted,
                      (float)(ox + 24), (float)(y + 66), (float)(pw - 48), 28.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        }

        if (svc_known) {
            /* On-demand background service control (UAC only when Start/Stop needs it) */
            int sy = y + 102;
            draw_line(g, (float)(ox + 22), (float)sy, (float)(ox + pw - 22), (float)sy, g_theme.separator, 1.0f);
            sy += 8;

            bool is_running = (g_telemetry.ts_svc_state == SERVICE_RUNNING);
            bool actual_manual = (g_telemetry.ts_svc_start_type == SERVICE_DEMAND_START);
            bool want_manual = (g_ts_manual_pref >= 0) ? (g_ts_manual_pref != 0) : actual_manual;
            const wchar_t *start_lbl =
                g_telemetry.ts_svc_start_type == SERVICE_AUTO_START   ? L"Auto start" :
                g_telemetry.ts_svc_start_type == SERVICE_DEMAND_START ? L"Manual start" :
                g_telemetry.ts_svc_start_type == SERVICE_DISABLED     ? L"Disabled" : L"Unknown";

            wchar_t svc_line[128];
            bool show_msg = g_telemetry.ts_svc_msg[0] && g_telemetry.ts_svc_msg_tick &&
                            (GetTickCount() - g_telemetry.ts_svc_msg_tick) < 8000;
            if (show_msg) {
                wcsncpy_s(svc_line, 128, g_telemetry.ts_svc_msg, _TRUNCATE);
            } else if (svc_busy) {
                wcsncpy_s(svc_line, 128, (g_telemetry.ts_svc_state == SERVICE_RUNNING ||
                                          g_telemetry.ts_svc_state == SERVICE_STOP_PENDING)
                                         ? L"Stopping service..." : L"Starting service...", _TRUNCATE);
            } else if (is_running) {
                if (g_telemetry.ts_svc_mem_mb > 0) {
                    _snwprintf_s(svc_line, 128, _TRUNCATE, L"Service on | %ls | %d MB",
                                 start_lbl, g_telemetry.ts_svc_mem_mb);
                } else {
                    _snwprintf_s(svc_line, 128, _TRUNCATE, L"Service on | %ls", start_lbl);
                }
            } else {
                _snwprintf_s(svc_line, 128, _TRUNCATE, L"Service off | %ls", start_lbl);
            }
            draw_icon_glyph(g, 0xE713, 12.0f, g_theme.fg_secondary, (float)(ox + 20), (float)sy, 20.0f, 26.0f);
            draw_text(g, svc_line, L"Segoe UI", 11.5f, show_msg ? GP_FONT_BOLD : GP_FONT_REGULAR,
                      show_msg ? g_theme.danger : g_theme.fg_primary,
                      (float)(ox + 44), (float)sy, (float)(pw - 150), 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

            int idx_svc = g_rect_count;
            if (!svc_busy) {
                add_click(ACT_TS_SERVICE, is_running ? 0 : 1, ox + pw - 92, sy, 74, 26, NULL);
            }
            draw_round_rect_fill(g, (float)(ox + pw - 92), (float)sy, 74.0f, 26.0f, 6.0f,
                                 svc_busy ? g_theme.popup_border
                                          : (is_running ? g_theme.accent_overlay_selected : g_theme.accent));
            if (!svc_busy) draw_row_hover(g, idx_svc, 6.0f);
            draw_text(g, svc_busy ? L"Working" : (is_running ? L"Stop" : L"Start"), L"Segoe UI", 11.0f, GP_FONT_BOLD,
                      svc_busy ? g_theme.fg_muted : (is_running ? g_theme.fg_primary : g_theme.fg_on_accent),
                      (float)(ox + pw - 92), (float)sy, 74.0f, 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
            sy += 32;

            /* A switch: Manual startup (applied with the next Start/Stop) */
            int idx_man = g_rect_count;
            add_click(ACT_TS_MANUAL, want_manual ? 0 : 1, ox + 18, sy, pw - 36, 26, NULL);
            draw_row_hover(g, idx_man, 6.0f);
            float swx = (float)(ox + 22), swy = (float)(sy + 6);
            draw_round_rect_fill(g, swx, swy, 26.0f, 14.0f, 7.0f, want_manual ? g_theme.success : g_theme.popup_border);
            draw_circle_fill(g, want_manual ? swx + 19.0f : swx + 7.0f, swy + 7.0f, 5.5f, MAKE_ARGB(255, 255, 255, 255));
            wchar_t man_line[128];
            if (want_manual != actual_manual) {
                _snwprintf_s(man_line, 128, _TRUNCATE, L"%ls startup (applies on next %ls)",
                             want_manual ? L"Manual" : L"Automatic", is_running ? L"Stop" : L"Start");
            } else {
                wcsncpy_s(man_line, 128, L"Manual startup: frees its RAM while VPN is off", _TRUNCATE);
            }
            draw_text(g, man_line, L"Segoe UI", 11.0f, GP_FONT_REGULAR, g_theme.fg_secondary,
                      (float)(ox + 56), (float)sy, (float)(pw - 78), 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        }

        y += ts_card_h + 10;

        /* Exit Nodes quick picker (if any peer advertises ExitNodeOption) */
        int exit_indices[4];
        int exit_cnt = 0;
        for (int i = 0; i < g_ts_peer_count && exit_cnt < 4; i++) {
            if (g_ts_peers[i].can_exit_node) exit_indices[exit_cnt++] = i;
        }
        if (exit_cnt > 0) {
            draw_text(g, L"Exit Node", L"Segoe UI", 11.5f, GP_FONT_BOLD, g_theme.fg_muted,
                      (float)(ox + 16), (float)y, (float)(pw - 32), 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
            y += 22;

            bool none_active = (g_telemetry.ts_exit_node[0] == L'\0');
            int idx_none = g_rect_count;
            add_click(ACT_TS_EXIT_NODE, 0, ox + 12, y, pw - 24, 28, L"");
            if (none_active) draw_round_rect_fill(g, (float)(ox + 12), (float)y, (float)(pw - 24), 28.0f, 6.0f, g_theme.accent_overlay_selected);
            draw_row_hover(g, idx_none, 6.0f);
            draw_text(g, L"None (Direct connection)", L"Segoe UI", 12.0f, none_active ? GP_FONT_BOLD : GP_FONT_REGULAR,
                      g_theme.fg_primary, (float)(ox + 20), (float)y, (float)(pw - 60), 28.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
            if (none_active) draw_icon_glyph(g, 0xE73E, 11.0f, g_theme.accent, (float)(ox + pw - 36), (float)y, 20.0f, 28.0f);
            y += 30;

            for (int k = 0; k < exit_cnt; k++) {
                const PopupTsPeer *tp = &g_ts_peers[exit_indices[k]];
                int idx_en = g_rect_count;
                add_click(ACT_TS_EXIT_NODE, k + 1, ox + 12, y, pw - 24, 28, tp->ip);
                if (tp->is_active_exit_node) draw_round_rect_fill(g, (float)(ox + 12), (float)y, (float)(pw - 24), 28.0f, 6.0f, g_theme.accent_overlay_selected);
                draw_row_hover(g, idx_en, 6.0f);
                draw_text(g, tp->name[0] ? tp->name : tp->ip, L"Segoe UI", 12.0f,
                          tp->is_active_exit_node ? GP_FONT_BOLD : GP_FONT_REGULAR, g_theme.fg_primary,
                          (float)(ox + 20), (float)y, (float)(pw - 60), 28.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                if (tp->is_active_exit_node) draw_icon_glyph(g, 0xE73E, 11.0f, g_theme.accent, (float)(ox + pw - 36), (float)y, 20.0f, 28.0f);
                y += 30;
            }
        }

        int foot_y = oy + ph - 36;
        int idx_ns = g_rect_count;
        add_click(ACT_NET_SETTINGS, 0, ox + 12, foot_y, pw - 24, 28, NULL);
        draw_row_hover(g, idx_ns, 6.0f);
        draw_text(g, L"Open Network Settings...", L"Segoe UI", 12.0f, GP_FONT_REGULAR, g_theme.accent,
                  (float)(ox + 16), (float)foot_y, (float)(pw - 32), 28.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
    }

    /* ===================== 5. MEDIA & VOLUME ===================== */
    else if (g_pop_kind == POPUP_MEDIA) {
        MediaState ms;
        media_get_state(&ms);

        /* Now playing card */
        int y = oy + 14;
        draw_round_rect_fill(g, (float)(ox + 12), (float)y, (float)(pw - 24), 84.0f, 10.0f, g_theme.popup_surface);
        GpBitmap *thumb = media_lock_thumbnail();
        if (thumb) {
            GdipDrawImageRectI(g, (GpImage *)thumb, ox + 22, y + 12, 60, 60);
        } else {
            draw_round_rect_fill(g, (float)(ox + 22), (float)(y + 12), 60.0f, 60.0f, 8.0f, g_theme.accent_overlay_selected);
            draw_icon_glyph(g, 0xE8D6, 22.0f, g_theme.fg_secondary, (float)(ox + 22), (float)(y + 12), 60.0f, 60.0f);
        }
        media_unlock_thumbnail();

        draw_text(g, (ms.has_session && ms.title[0]) ? ms.title : L"No active media", L"Segoe UI", 13.0f, GP_FONT_BOLD,
                  g_theme.fg_primary, (float)(ox + 92), (float)(y + 10), (float)(pw - 114), 20.0f,
                  GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        draw_text(g, (ms.has_session && ms.artist[0]) ? ms.artist : L"Ready to play", L"Segoe UI", 11.5f, GP_FONT_REGULAR,
                  g_theme.fg_secondary, (float)(ox + 92), (float)(y + 30), (float)(pw - 114), 18.0f,
                  GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

        int bx = ox + 112;
        int by = y + 52;
        int idx_p = g_rect_count; add_click(ACT_MEDIA_PREV, 0, bx, by, 30, 26, NULL);
        draw_row_hover(g, idx_p, 6.0f);
        draw_icon_glyph(g, 0xE892, 13.0f, g_theme.fg_primary, (float)bx, (float)by, 30.0f, 26.0f);

        int idx_pl = g_rect_count; add_click(ACT_MEDIA_PLAY, 0, bx + 42, by, 34, 26, NULL);
        draw_round_rect_fill(g, (float)(bx + 42), (float)by, 34.0f, 26.0f, 13.0f, g_theme.accent);
        draw_icon_glyph(g, ms.is_playing ? 0xE769 : 0xE768, 13.0f, g_theme.fg_on_accent,
                        (float)(bx + 42), (float)by, 34.0f, 26.0f);
        (void)idx_pl;

        int idx_n = g_rect_count; add_click(ACT_MEDIA_NEXT, 0, bx + 88, by, 30, 26, NULL);
        draw_row_hover(g, idx_n, 6.0f);
        draw_icon_glyph(g, 0xE893, 13.0f, g_theme.fg_primary, (float)(bx + 88), (float)by, 30.0f, 26.0f);

        /* Master Volume Slider */
        y += 96;
        draw_text(g, L"Volume", L"Segoe UI", 12.0f, GP_FONT_BOLD, g_theme.fg_secondary,
                  (float)(ox + 16), (float)y, 120.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        wchar_t vs[16];
        _snwprintf_s(vs, 16, _TRUNCATE, L"%d%%", g_telemetry.volume_muted ? 0 : g_telemetry.volume_percent);
        draw_text(g, vs, L"Segoe UI", 12.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + pw - 60), (float)y, 44.0f, 20.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, false);

        y += 24;
        int idx_m = g_rect_count;
        add_click(ACT_VOL_MUTE, 0, ox + 12, y, 30, 30, NULL);
        draw_row_hover(g, idx_m, 6.0f);
        wchar_t vg = (g_telemetry.volume_muted || g_telemetry.volume_percent == 0) ? 0xE74F : 0xE767;
        draw_icon_glyph(g, vg, 15.0f, g_telemetry.volume_muted ? g_theme.danger : g_theme.fg_primary,
                        (float)(ox + 12), (float)y, 30.0f, 30.0f);

        int tx = ox + 48;
        int tw = pw - 66;
        int ty = y + 12;
        g_vol_track_rc.left = tx; g_vol_track_rc.top = y; g_vol_track_rc.right = tx + tw; g_vol_track_rc.bottom = y + 30;
        add_click(ACT_VOL_SLIDER, 0, tx, y, tw, 30, NULL);

        draw_round_rect_fill(g, (float)tx, (float)ty, (float)tw, 6.0f, 3.0f, g_theme.popup_surface);
        float fill_w = (float)tw * (float)(g_telemetry.volume_muted ? 0 : g_telemetry.volume_percent) / 100.0f;
        if (fill_w > 0.0f) {
            draw_round_rect_fill(g, (float)tx, (float)ty, fill_w, 6.0f, 3.0f, g_theme.accent);
        }
        draw_circle_fill(g, (float)tx + fill_w, (float)ty + 3.0f, 7.5f, g_theme.accent);
        draw_circle_fill(g, (float)tx + fill_w, (float)ty + 3.0f, 3.5f, g_theme.fg_on_accent);

        /* Output Devices */
        y += 42;
        draw_line(g, (float)(ox + 12), (float)y, (float)(ox + pw - 12), (float)y, g_theme.separator, 1.0f);
        y += 8;
        draw_text(g, L"Output Devices", L"Segoe UI", 11.5f, GP_FONT_BOLD, g_theme.fg_muted,
                  (float)(ox + 16), (float)y, 160.0f, 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        y += 22;
        int max_ad = min(g_audio_dev_count, 4);
        for (int i = 0; i < max_ad; i++) {
            int idx_a = g_rect_count;
            add_click(ACT_AUDIO_SETTINGS, i, ox + 12, y, pw - 24, 26, NULL);
            if (g_audio_devs[i].is_default) {
                draw_round_rect_fill(g, (float)(ox + 12), (float)y, (float)(pw - 24), 26.0f, 6.0f, g_theme.accent_overlay_selected);
            }
            draw_row_hover(g, idx_a, 6.0f);
            draw_text(g, g_audio_devs[i].name, L"Segoe UI", 11.5f,
                      g_audio_devs[i].is_default ? GP_FONT_BOLD : GP_FONT_REGULAR, g_theme.fg_primary,
                      (float)(ox + 20), (float)y, (float)(pw - 56), 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
            if (g_audio_devs[i].is_default) {
                draw_icon_glyph(g, 0xE73E, 11.0f, g_theme.accent, (float)(ox + pw - 34), (float)y, 18.0f, 26.0f);
            }
            y += 28;
        }
    }

    /* ===================== 6. NOTIFICATIONS / DND ===================== */
    else if (g_pop_kind == POPUP_NOTIFICATIONS) {
        draw_text(g, L"Notifications", L"Segoe UI", 14.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 16), (float)(oy + 14), (float)(pw - 32), 24.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        /* Do not disturb is Windows' own setting, with no public way to
         * switch it: this row opens it where Windows keeps it. */
        int y = oy + 48;
        int idx_dnd = g_rect_count;
        add_click(ACT_NOTIF_SETTINGS, 0, ox + 12, y, pw - 24, 46, NULL);
        draw_round_rect_fill(g, (float)(ox + 12), (float)y, (float)(pw - 24), 46.0f, 10.0f, g_theme.popup_surface);
        draw_row_hover(g, idx_dnd, 10.0f);
        draw_icon_glyph(g, 0xE708, 16.0f, g_theme.fg_secondary, (float)(ox + 20), (float)y, 26.0f, 46.0f);
        draw_text(g, L"Do Not Disturb", L"Segoe UI", 13.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 52), (float)(y + 6), 200.0f, 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        draw_text(g, L"In Windows' notification settings", L"Segoe UI", 11.0f, GP_FONT_REGULAR,
                  g_theme.fg_muted, (float)(ox + 52), (float)(y + 24), 200.0f, 16.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        draw_icon_glyph(g, 0xE76C, 12.0f, g_theme.fg_muted, (float)(ox + pw - 44), (float)y, 24.0f, 46.0f);

        int idx_wn = g_rect_count;
        add_click(ACT_NOTIF_WIN_PANEL, 0, ox + 12, oy + ph - 38, pw - 24, 28, NULL);
        draw_row_hover(g, idx_wn, 6.0f);
        draw_text(g, L"Open Windows Notification Center", L"Segoe UI", 11.5f, GP_FONT_REGULAR, g_theme.accent,
                  (float)(ox + 16), (float)(oy + ph - 38), (float)(pw - 32), 28.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
    }

    /* ===================== 7. POWER MENU ===================== */
    else if (g_pop_kind == POPUP_POWER) {
        typedef struct { int act; wchar_t glyph; const wchar_t *label; bool danger; bool sep_before; } PwrRow;
        static const PwrRow rows[] = {
            { ACT_PWR_LOCK,     0xE72E, L"Lock",               false, false },
            { ACT_PWR_SLEEP,    0xE708, L"Sleep",              false, false },
            { ACT_PWR_SIGNOUT,  0xF3B1, L"Sign Out",           false, false },
            { ACT_PWR_RESTART,  0xE777, L"Restart",            false, false },
            { ACT_PWR_SHUTDOWN, 0xE7E8, L"Shut Down",          true,  false },
            { ACT_PWR_FILES,    0xE8B7, L"File Explorer",      false, true  },
            { ACT_PWR_QUIT,     0xE711, L"Close the bar and dock", false, false }
        };
        int y = oy + 12;
        for (int i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++) {
            if (rows[i].sep_before) {
                y += 4;
                draw_line(g, (float)(ox + 14), (float)y, (float)(ox + pw - 14), (float)y, g_theme.separator, 1.0f);
                y += 8;
            }
            int idx = g_rect_count;
            add_click(rows[i].act, 0, ox + 10, y, pw - 20, 36, NULL);
            draw_row_hover(g, idx, 8.0f);
            ARGB c = rows[i].danger ? g_theme.danger : g_theme.fg_primary;
            draw_icon_glyph(g, rows[i].glyph, 15.0f, c, (float)(ox + 18), (float)y, 26.0f, 36.0f);
            /* Lares' Files when its switch is on, else File Explorer. */
            const wchar_t *label = (rows[i].act == ACT_PWR_FILES && g_cfg.files_enabled) ? L"Files" : rows[i].label;
            draw_text(g, label, L"Segoe UI", 13.0f, GP_FONT_REGULAR, c,
                      (float)(ox + 50), (float)y, (float)(pw - 66), 36.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
            y += 40;
        }
    }

    /* ===================== 8. SCREENSHOT MENU ===================== */
    else if (g_pop_kind == POPUP_SCREENSHOT) {
        draw_text(g, L"Screenshot", L"Segoe UI", 14.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)(ox + 16), (float)(oy + 12), (float)(pw - 32), 24.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        typedef struct { int act; wchar_t glyph; const wchar_t *label; const wchar_t *hotkey; } SsRow;
        static const SsRow rows[] = {
            { ACT_SS_REGION, 0xE7A8, L"Region Snip",   L"Alt+Shift+S" },
            { ACT_SS_WINDOW, 0xE737, L"Active Window", L"Alt+Shift+W" },
            { ACT_SS_FULL,   0xE7F4, L"Full Screen",   L"Alt+Shift+F" }
        };
        int y = oy + 42;
        for (int i = 0; i < 3; i++) {
            int idx = g_rect_count;
            add_click(rows[i].act, 0, ox + 10, y, pw - 20, 36, NULL);
            draw_row_hover(g, idx, 8.0f);
            draw_icon_glyph(g, rows[i].glyph, 15.0f, g_theme.accent, (float)(ox + 16), (float)y, 26.0f, 36.0f);
            draw_text(g, rows[i].label, L"Segoe UI", 12.5f, GP_FONT_REGULAR, g_theme.fg_primary,
                      (float)(ox + 46), (float)y, 120.0f, 36.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
            /* The shortcuts exist only while Lares' screenshot tool is on. */
            if (g_cfg.screenshot_enabled)
            draw_text(g, rows[i].hotkey, L"Segoe UI", 11.0f, GP_FONT_REGULAR, g_theme.fg_muted,
                      (float)(ox + pw - 98), (float)y, 82.0f, 36.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
            y += 38;
        }
    }

    popup_present();
}

static void copy_text_to_clipboard(const wchar_t *text) {
    if (!text || !text[0]) return;
    if (!OpenClipboard(g_hPopupWnd)) return;
    EmptyClipboard();
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void *p = GlobalLock(hMem);
        if (p) {
            memcpy(p, text, bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
}

static void apply_vol_from_x(int mx) {
    int tw = g_vol_track_rc.right - g_vol_track_rc.left;
    if (tw <= 0) return;
    int pct = ((mx - g_vol_track_rc.left) * 100) / tw;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    telemetry_set_volume(pct);
    popup_paint();
}

static void handle_popup_action(const PopupClickRect *r) {
    switch (r->action_id) {
    case ACT_CAL_PREV:
        if (--g_cal_month < 1) { g_cal_month = 12; g_cal_year--; }
        popup_paint();
        break;
    case ACT_CAL_NEXT:
        if (++g_cal_month > 12) { g_cal_month = 1; g_cal_year++; }
        popup_paint();
        break;
    case ACT_CAL_TODAY: {
        SYSTEMTIME st; GetLocalTime(&st);
        g_cal_year = st.wYear; g_cal_month = st.wMonth;
        popup_paint();
        break;
    }
    case ACT_KBD_SELECT:
        if (r->param >= 0 && r->param < g_hkl_count) {
            HKL hkl = g_hkls[r->param];
            HWND target = (g_last_fg_wnd && IsWindow(g_last_fg_wnd)) ? g_last_fg_wnd : GetForegroundWindow();
            if (target) PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)hkl);
            ActivateKeyboardLayout(hkl, KLF_SETFORPROCESS);
            g_telemetry.kbd_hkl = hkl;
            telemetry_update(false);
            topbar_repaint();
            popups_close();
        }
        break;
    case ACT_BT_SCAN:
        if (InterlockedCompareExchange(&g_bt_scanning, 1, 0) == 0) {
            popup_paint();
            HANDLE h = CreateThread(NULL, 0, bt_scan_thread, NULL, 0, NULL);
            if (h) CloseHandle(h);
            else InterlockedExchange(&g_bt_scanning, 0);
        }
        break;
    case ACT_BT_SETTINGS:
        popups_close();
        ShellExecuteW(NULL, L"open", L"ms-settings:bluetooth", NULL, NULL, SW_SHOWNORMAL);
        break;
    case ACT_NET_SETTINGS:
        popups_close();
        ShellExecuteW(NULL, L"open", L"ms-settings:network", NULL, NULL, SW_SHOWNORMAL);
        break;
    case ACT_NOTIF_SETTINGS:
        popups_close();
        ShellExecuteW(NULL, L"open", L"ms-settings:notifications", NULL, NULL, SW_SHOWNORMAL);
        break;
    case ACT_TS_REFRESH:
        telemetry_refresh_tailscale_async();
        break;
    case ACT_TS_TOGGLE:
        telemetry_tailscale_toggle_async(r->param != 0);
        break;
    case ACT_TS_COPY_IP:
        if (g_telemetry.ts_ip[0]) {
            copy_text_to_clipboard(g_telemetry.ts_ip);
            g_copied_ip_tick = GetTickCount();
            popup_paint();
        }
        break;
    case ACT_TS_EXIT_NODE:
        telemetry_tailscale_set_exit_node_async(r->str_param);
        break;
    case ACT_TS_SERVICE: {
        /* Bundle a pending startup-mode change into this one Start/Stop (single UAC prompt at most) */
        bool start = (r->param != 0);
        bool actual_manual = (g_telemetry.ts_svc_start_type == SERVICE_DEMAND_START);
        bool want_manual = (g_ts_manual_pref >= 0) ? (g_ts_manual_pref != 0) : actual_manual;
        int startup = 0;
        int desired = want_manual ? SERVICE_DEMAND_START : SERVICE_AUTO_START;
        if (g_telemetry.ts_svc_start_type != desired &&
            (want_manual != actual_manual || g_telemetry.ts_svc_start_type == SERVICE_DISABLED)) {
            startup = desired;
        }
        g_ts_manual_pref = -1;
        telemetry_tailscale_service_async(start, startup);
        popup_paint();
        break;
    }
    case ACT_TS_MANUAL: {
        bool actual_manual = (g_telemetry.ts_svc_start_type == SERVICE_DEMAND_START);
        bool want = (r->param != 0);
        g_ts_manual_pref = (want == actual_manual) ? -1 : (want ? 1 : 0);
        popup_paint();
        break;
    }
    case ACT_MEDIA_PREV:
        media_prev();
        break;
    case ACT_MEDIA_PLAY:
        media_play_pause();
        break;
    case ACT_MEDIA_NEXT:
        media_next();
        break;
    case ACT_VOL_MUTE:
        telemetry_toggle_mute();
        popup_paint();
        break;
    case ACT_AUDIO_SETTINGS:
        popups_close();
        ShellExecuteW(NULL, L"open", L"ms-settings:sound", NULL, NULL, SW_SHOWNORMAL);
        break;

    case ACT_NOTIF_WIN_PANEL: {
        popups_close();
        INPUT inp[4];
        ZeroMemory(inp, sizeof(inp));
        inp[0].type = INPUT_KEYBOARD; inp[0].ki.wVk = VK_LWIN;
        inp[1].type = INPUT_KEYBOARD; inp[1].ki.wVk = 'N';
        inp[2].type = INPUT_KEYBOARD; inp[2].ki.wVk = 'N'; inp[2].ki.dwFlags = KEYEVENTF_KEYUP;
        inp[3].type = INPUT_KEYBOARD; inp[3].ki.wVk = VK_LWIN; inp[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, inp, sizeof(INPUT));
        break;
    }
    case ACT_PWR_LOCK:
        popups_close();
        LockWorkStation();
        break;
    case ACT_PWR_SLEEP:
        popups_close();
        SetSuspendState(FALSE, FALSE, FALSE);
        break;
    /* At the end of the session the guard's client gives the taskbar back
     * (guard_client.c, WM_ENDSESSION): nothing to do here first. */
    case ACT_PWR_SIGNOUT:
        popups_close();
        ExitWindowsEx(EWX_LOGOFF, 0);
        break;
    case ACT_PWR_RESTART: {
        popups_close();
        wchar_t shutdown_exe[MAX_PATH];
        if (lares_windows_program(L"shutdown.exe", shutdown_exe, MAX_PATH))
            ShellExecuteW(NULL, L"open", shutdown_exe, L"/r /t 0", NULL, SW_HIDE);
        break;
    }
    case ACT_PWR_SHUTDOWN: {
        popups_close();
        wchar_t shutdown_exe[MAX_PATH];
        if (lares_windows_program(L"shutdown.exe", shutdown_exe, MAX_PATH))
            ShellExecuteW(NULL, L"open", shutdown_exe, L"/s /t 0", NULL, SW_HIDE);
        break;
    }
    case ACT_PWR_FILES:
        popups_close();
        action_open_files(NULL);
        break;
    case ACT_PWR_QUIT:
        /* Until the next sign-in: the guard gives the taskbar back. */
        popups_close();
        if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_QUIT_CLEAN, 0, 0);
        break;
    case ACT_SS_REGION:
        popups_close();
        action_screenshot(0);
        break;
    case ACT_SS_WINDOW:
        popups_close();
        action_screenshot(1);
        break;
    case ACT_SS_FULL:
        popups_close();
        action_screenshot(2);
        break;
    default:
        break;
    }
}

static int hit_test_popup(int mx, int my) {
    if (g_pop_kind == POPUP_NONE) return -1; /* fading out: nothing is clickable */
    for (int i = 0; i < g_rect_count; i++) {
        if (mx >= g_rects[i].rc.left && mx <= g_rects[i].rc.right &&
            my >= g_rects[i].rc.top && my <= g_rects[i].rc.bottom) {
            return i;
        }
    }
    return -1;
}

static LRESULT CALLBACK popup_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && g_pop_kind != POPUP_NONE) {
            popups_close();
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_POP_ANIM) {
            DWORD span = g_pop_closing ? POP_ANIM_OUT_MS : POP_ANIM_IN_MS;
            float t = (float)(GetTickCount() - g_pop_anim_start) / (float)span;
            if (t >= 1.0f) {
                KillTimer(hwnd, TIMER_POP_ANIM);
                g_pop_vis = g_pop_vis_to;
                if (g_pop_closing) {
                    g_pop_closing = false;
                    ShowWindow(hwnd, SW_HIDE);
                } else {
                    popup_present();
                }
            } else {
                g_pop_vis = g_pop_vis_from + (g_pop_vis_to - g_pop_vis_from) * anim_ease_out(t);
                popup_present();
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            popups_close();
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        if (g_dragging_vol && g_pop_kind == POPUP_MEDIA) {
            apply_vol_from_x(mx);
            return 0;
        }
        int hr = hit_test_popup(mx, my);
        if (hr != g_hover_rect) {
            g_hover_rect = hr;
            popup_paint();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        int hr = hit_test_popup(mx, my);
        if (hr >= 0 && g_rects[hr].action_id == ACT_VOL_SLIDER) {
            g_dragging_vol = true;
            SetCapture(hwnd);
            apply_vol_from_x(mx);
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        if (g_dragging_vol) {
            g_dragging_vol = false;
            ReleaseCapture();
            return 0;
        }
        int hr = hit_test_popup(mx, my);
        if (hr >= 0) {
            PopupClickRect copy = g_rects[hr];
            handle_popup_action(&copy);
        }
        return 0;
    }

    case WM_DESTROY:
        surface_destroy(&g_pop_surf);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void popups_init(HINSTANCE hInst) {
    InitializeCriticalSection(&g_bt_cs);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = popup_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = POPUP_CLASS;
    RegisterClassExW(&wc);

    g_hPopupWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        POPUP_CLASS, L"Lares popup",
        WS_POPUP,
        0, 0, 300, 300,
        NULL, NULL, hInst, NULL);
}

void popups_toggle(PopupKind kind, int anchor_x, int anchor_y) {
    if (!g_hPopupWnd) return;
    if (g_pop_kind == kind && IsWindowVisible(g_hPopupWnd)) {
        popups_close();
        return;
    }
    /* Switching panels, or reopening while the last one fades out: drop that transition. */
    KillTimer(g_hPopupWnd, TIMER_POP_ANIM);
    g_pop_closing = false;

    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hPopupWnd && fg != g_hTopBarWnd && fg != g_hDockWnd) {
        g_last_fg_wnd = fg;
    }

    g_pop_kind = kind;
    g_anchor_x = anchor_x;
    g_anchor_y = anchor_y;
    g_hover_rect = -1;
    g_dragging_vol = false;

    if (kind == POPUP_CALENDAR) {
        SYSTEMTIME st; GetLocalTime(&st);
        g_cal_year = st.wYear;
        g_cal_month = st.wMonth;
    } else if (kind == POPUP_KEYBOARD) {
        g_hkl_count = GetKeyboardLayoutList(16, g_hkls);
    } else if (kind == POPUP_BLUETOOTH) {
        load_bt_devices(false); /* what Windows already knows: no scan */
    } else if (kind == POPUP_NETWORK) {
        g_ts_peer_count = telemetry_get_ts_peers(g_ts_peers, 32);
        telemetry_refresh_tailscale_async();
    } else if (kind == POPUP_MEDIA) {
        load_audio_endpoints();
        media_poll();
    }

    /* A test copy draws the popup and shows nothing, so it never takes the
     * focus from the desktop it runs on. */
    if (g_cfg.isolated) {
        g_pop_vis = g_pop_vis_from = g_pop_vis_to = 1.0f;
        popup_paint();
        return;
    }
    bool animate = g_cfg.animations;
    g_pop_vis = animate ? 0.0f : 1.0f;
    g_pop_vis_from = g_pop_vis;
    g_pop_vis_to = 1.0f;
    popup_paint();
    ShowWindow(g_hPopupWnd, SW_SHOW);
    SetWindowPos(g_hPopupWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_hPopupWnd);
    if (animate) {
        g_pop_anim_start = GetTickCount();
        SetTimer(g_hPopupWnd, TIMER_POP_ANIM, 12, NULL);
    }
}

void popups_close(void) {
    PopupKind was = g_pop_kind;
    g_pop_kind = POPUP_NONE;
    g_hover_rect = -1;
    g_dragging_vol = false;
    if (g_hPopupWnd && IsWindowVisible(g_hPopupWnd) && !g_pop_closing) {
        if (g_cfg.animations && was != POPUP_NONE && g_pop_surf.hdc) {
            /* Fade the picture that is already there, then hide */
            g_pop_closing = true;
            g_pop_vis_from = g_pop_vis;
            g_pop_vis_to = 0.0f;
            g_pop_anim_start = GetTickCount();
            SetTimer(g_hPopupWnd, TIMER_POP_ANIM, 12, NULL);
        } else {
            KillTimer(g_hPopupWnd, TIMER_POP_ANIM);
            ShowWindow(g_hPopupWnd, SW_HIDE);
        }
    }
}

bool popups_is_open(void) {
    return (g_pop_kind != POPUP_NONE && g_hPopupWnd && IsWindowVisible(g_hPopupWnd));
}

HWND popups_get_last_fg_window(void) {
    return g_last_fg_wnd;
}

void popups_refresh_if_open(void) {
    if (popups_is_open()) popup_paint();
}

void popups_on_tailscale_updated(void) {
    g_ts_peer_count = telemetry_get_ts_peers(g_ts_peers, 32);
    if (popups_is_open() && g_pop_kind == POPUP_NETWORK) {
        popup_paint();
    }
}

void popups_on_bt_scan_done(void) {
    if (popups_is_open() && g_pop_kind == POPUP_BLUETOOTH) {
        popup_paint();
    }
}

void popups_shutdown(void) {
    if (g_hPopupWnd) {
        DestroyWindow(g_hPopupWnd);
        g_hPopupWnd = NULL;
    }
    DeleteCriticalSection(&g_bt_cs);
}

int popups_snapshot(const wchar_t *png_path) {
    return g_pop_surf.hdc && surface_save_png(&g_pop_surf, png_path);
}

bool popups_is_kind_open(PopupKind kind) {
    /* True as soon as the popup is requested (before it is shown), so async refreshes can tailor work */
    return g_pop_kind == kind;
}
