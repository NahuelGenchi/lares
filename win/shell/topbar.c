// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The top bar (issue #145): the app in front, the clock, and the readings
 * and buttons at the right. Fixed (it reserves its strip of the screen) or
 * hidden automatically, coming back when the pointer touches the top edge. */
#include "shell.h"
#include <shellapi.h>
#include <stdio.h>

#define TOPBAR_CLASS        L"LaresTopBarWnd"
#define TOPBAR_SENSOR_CLASS L"LaresTopSensorWnd"
#define WM_LARES_APPBAR_CB (WM_APP + 50)
#define TIMER_TB_ANIM     201
#define TIMER_TB_HIDECHK  202
#define TB_ANIM_MS        170

typedef enum {
    TB_HIT_NONE = 0,
    TB_HIT_FOCUSED,
    TB_HIT_CLOCK,
    TB_HIT_KEYBOARD,
    TB_HIT_BLUETOOTH,
    TB_HIT_NETWORK,
    TB_HIT_SHARE,
    TB_HIT_MEDIA,
    TB_HIT_CPU,
    TB_HIT_GPU,
    TB_HIT_RAM,
    TB_HIT_SCREENSHOT,
    TB_HIT_NOTIF,
    TB_HIT_POWER,
    TB_HIT_IND_CONTROLLER,
    TB_HIT_IND_HEADPHONES,
    TB_HIT_IND_KEYBOARD,
    TB_HIT_IND_MOUSE,
    TB_HIT_IND_REMOTE
} TopBarHitId;

typedef struct {
    TopBarHitId id;
    RECT rc;
    bool clickable;
} TopBarHit;

static LayerSurface g_tb_surf = {0};
static HWND g_hTbSensor = NULL;
static bool g_tb_appbar_registered = false;
static bool g_tb_fullscreen_hidden = false;
static bool g_tb_menu_open = false;
/* Every button the bar can show, with room: the focused app, the clock, ten
 * on the right and five device indicators (issue #157). */
#define TB_MAX_HITS 24
static TopBarHit g_hits[TB_MAX_HITS];
static int g_hit_count = 0;
static TopBarHitId g_hover_id = TB_HIT_NONE;
static bool g_tracking_mouse = false;
static int g_tb_x = 0, g_tb_y = 0, g_tb_w = 1, g_tb_h = 32;

/* Slide state: g_tb_vis is the visible fraction (1 = fully on screen). */
static bool  g_tb_shown = true;
static float g_tb_vis = 1.0f, g_tb_vis_from = 1.0f, g_tb_vis_to = 1.0f;
static DWORD g_tb_anim_start = 0;
static int   g_tb_idle_checks = 0;

int topbar_get_height(void) {
    if (!g_cfg.topbar_enabled) return 0;
    return g_cfg.topbar_item_size + g_cfg.topbar_padding * 2;
}

static void topbar_unregister_appbar(HWND hwnd) {
    if (!g_tb_appbar_registered || !hwnd) return;
    APPBARDATA abd;
    ZeroMemory(&abd, sizeof(abd));
    abd.cbSize = sizeof(abd);
    abd.hWnd = hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd);
    g_tb_appbar_registered = false;
}

/* Works out where the bar sits. A fixed bar reserves its strip of the screen as an application
 * desktop toolbar, so other windows never sit under it; an auto-hiding bar reserves nothing. */
static void topbar_register_appbar(HWND hwnd) {
    MONITORINFO mi;
    shell_screen_rect(&mi.rcMonitor);

    int margin = g_cfg.topbar_margin;
    int h = topbar_get_height();
    g_tb_x = mi.rcMonitor.left + margin;
    g_tb_y = mi.rcMonitor.top + margin;
    g_tb_w = (mi.rcMonitor.right - mi.rcMonitor.left) - margin * 2;
    g_tb_h = h;

    if (g_hTbSensor) {
        SetWindowPos(g_hTbSensor, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, 2, SWP_NOACTIVATE);
    }

    /* A test copy reserves nothing on the desktop it runs on. */
    if (g_cfg.topbar_autohide || g_cfg.isolated) {
        topbar_unregister_appbar(hwnd);
    } else {
        APPBARDATA abd;
        ZeroMemory(&abd, sizeof(abd));
        abd.cbSize = sizeof(abd);
        abd.hWnd = hwnd;
        abd.uCallbackMessage = WM_LARES_APPBAR_CB;
        if (!g_tb_appbar_registered) {
            if (SHAppBarMessage(ABM_NEW, &abd)) {
                g_tb_appbar_registered = true;
            }
        }

        abd.uEdge = ABE_TOP;
        abd.rc.left   = mi.rcMonitor.left;
        abd.rc.top    = mi.rcMonitor.top;
        abd.rc.right  = mi.rcMonitor.right;
        abd.rc.bottom = mi.rcMonitor.top + h + margin * 2;
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.bottom = abd.rc.top + h + margin * 2;
        SHAppBarMessage(ABM_SETPOS, &abd);

        g_tb_x = abd.rc.left + margin;
        g_tb_y = abd.rc.top + margin;
        g_tb_w = (abd.rc.right - abd.rc.left) - margin * 2;
    }

    if (g_tb_shown && !g_tb_fullscreen_hidden) {
        SetWindowPos(hwnd, HWND_TOPMOST, g_tb_x, g_tb_y, g_tb_w, g_tb_h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

static void topbar_present(void) {
    if (!g_hTopBarWnd || !g_tb_surf.hdc) return;
    float v = g_tb_vis;
    int off = (int)((1.0f - v) * (float)(g_tb_h + g_cfg.topbar_margin) + 0.5f);
    int a = (int)(v * 255.0f + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    surface_present(g_hTopBarWnd, &g_tb_surf, g_tb_x, g_tb_y - off, (BYTE)a);
}

static void topbar_slide_done(void) {
    KillTimer(g_hTopBarWnd, TIMER_TB_ANIM);
    g_tb_vis = g_tb_vis_to;
    if (!g_tb_shown) {
        ShowWindow(g_hTopBarWnd, SW_HIDE);
        if (g_hTbSensor && !g_tb_fullscreen_hidden) {
            ShowWindow(g_hTbSensor, SW_SHOWNOACTIVATE);
            SetWindowPos(g_hTbSensor, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    } else {
        topbar_present();
        if (g_cfg.topbar_autohide) {
            g_tb_idle_checks = 0;
            SetTimer(g_hTopBarWnd, TIMER_TB_HIDECHK, 350, NULL);
        }
    }
}

static void topbar_set_shown(bool show, bool animate) {
    if (!g_hTopBarWnd) return;
    if (show == g_tb_shown && g_tb_vis == (show ? 1.0f : 0.0f)) return;
    g_tb_shown = show;
    g_tb_vis_from = g_tb_vis;
    g_tb_vis_to = show ? 1.0f : 0.0f;
    KillTimer(g_hTopBarWnd, TIMER_TB_HIDECHK);
    if (show) {
        if (g_hTbSensor) ShowWindow(g_hTbSensor, SW_HIDE);
        SetWindowPos(g_hTopBarWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        topbar_repaint();
    }
    if (!animate || !g_cfg.animations) {
        topbar_slide_done();
        return;
    }
    g_tb_anim_start = GetTickCount();
    SetTimer(g_hTopBarWnd, TIMER_TB_ANIM, 12, NULL);
}

static void topbar_apply_mode(void) {
    if (!g_hTopBarWnd) return;
    topbar_register_appbar(g_hTopBarWnd);
    if (g_tb_fullscreen_hidden) return;
    /* Fixed: stay. Auto-hide: show once so the change is visible, the idle check then slides it away. */
    topbar_set_shown(true, true);
    if (g_cfg.topbar_autohide) {
        g_tb_idle_checks = 0;
        SetTimer(g_hTopBarWnd, TIMER_TB_HIDECHK, 350, NULL);
    } else {
        KillTimer(g_hTopBarWnd, TIMER_TB_HIDECHK);
    }
}

static void topbar_show_menu(HWND hwnd, int sx, int sy) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | (g_cfg.topbar_autohide ? 0 : MF_CHECKED), 1, L"Keep the top bar visible");
    AppendMenuW(m, MF_STRING | (g_cfg.topbar_autohide ? MF_CHECKED : 0), 2, L"Hide the top bar automatically");
    CheckMenuRadioItem(m, 1, 2, g_cfg.topbar_autohide ? 2U : 1U, MF_BYCOMMAND);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_cfg.animations ? MF_CHECKED : 0), 3, L"Animations");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 4, L"Lares Settings");

    g_tb_menu_open = true;
    SetForegroundWindow(hwnd);
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, hwnd, NULL);
    DestroyMenu(m);
    g_tb_menu_open = false;

    if (cmd == 1 || cmd == 2) {
        bool want = (cmd == 2);
        if (want != g_cfg.topbar_autohide) {
            g_cfg.topbar_autohide = want;
            config_save_behaviour();
            topbar_apply_mode();
        }
    } else if (cmd == 3) {
        g_cfg.animations = !g_cfg.animations;
        config_save_behaviour();
    } else if (cmd == 4) {
        action_open_lares(L"settings");
    }
}

static void format_clock(wchar_t *buf, size_t len) {
    static const wchar_t *days[7]   = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    static const wchar_t *months[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                         L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    SYSTEMTIME st;
    GetLocalTime(&st);
    int hr12 = st.wHour % 12;
    if (hr12 == 0) hr12 = 12;
    const wchar_t *ampm = (st.wHour >= 12) ? L"PM" : L"AM";
    _snwprintf_s(buf, len, _TRUNCATE, L"%ls %02u %ls, %02d:%02u %ls",
                 days[st.wDayOfWeek % 7], st.wDay,
                 months[(st.wMonth >= 1 && st.wMonth <= 12) ? (st.wMonth - 1) : 0],
                 hr12, st.wMinute, ampm);
}

static void add_hit(TopBarHitId id, int x, int y, int w, int h, bool clickable) {
    if (g_hit_count >= TB_MAX_HITS) return;
    g_hits[g_hit_count].id = id;
    g_hits[g_hit_count].rc.left = x;
    g_hits[g_hit_count].rc.top = y;
    g_hits[g_hit_count].rc.right = x + w;
    g_hits[g_hit_count].rc.bottom = y + h;
    g_hits[g_hit_count].clickable = clickable;
    g_hit_count++;
}

static void draw_hover_bg(GpGraphics *g, TopBarHitId id, int x, int y, int w, int h) {
    if (g_hover_id == id) {
        draw_round_rect_fill(g, (float)x, (float)y, (float)w, (float)h, 5.0f, g_theme.accent_overlay_hover);
    }
}

void topbar_repaint(void) {
    if (!g_hTopBarWnd || !g_cfg.topbar_enabled) return;
    /* Nothing to draw while it is off screen: no work at all for a hidden bar. */
    if (g_tb_fullscreen_hidden || (!g_tb_shown && g_tb_vis <= 0.0f)) return;
    if (!surface_ensure(&g_tb_surf, g_tb_w, g_tb_h)) return;

    GpGraphics *g = g_tb_surf.g;
    GdipGraphicsClear(g, 0);

    float radius = (g_cfg.topbar_margin > 0) ? (float)g_tb_h * 0.3f : 0.0f;
    draw_round_rect_fill(g, 0.0f, 0.0f, (float)g_tb_w, (float)g_tb_h, radius, g_theme.topbar_bg);

    /* Subtle 1px bottom hairline for crisp separation */
    draw_line(g, 0.0f, (float)(g_tb_h - 1), (float)g_tb_w, (float)(g_tb_h - 1),
              g_theme.is_dark ? MAKE_ARGB(45, 255, 255, 255) : MAKE_ARGB(35, 0, 0, 0), 1.0f);

    g_hit_count = 0;
    int pad_x = g_cfg.topbar_padding * 2;
    int item_h = g_tb_h - 6;
    int item_y = 3;
    float font_sz = (float)g_cfg.topbar_item_size * 0.80f;
    if (font_sz < 12.0f) font_sz = 12.0f;
    float icon_sz = (float)g_cfg.topbar_item_size * 0.88f;

    /* === LEFT: Focused App icon + name (clean, no stray dividers) === */
    TrackedWindow focused;
    if (win_tracker_get_focused(&focused) && focused.app_name[0]) {
        bool is_sq = false;
        GpBitmap *ico = icon_cache_get(focused.umid, focused.exe_path, NULL, &is_sq);
        int fx = pad_x;
        int icon_draw = g_cfg.topbar_item_size;
        float text_w = measure_text_width(g, focused.app_name, L"Segoe UI", font_sz, GP_FONT_BOLD);
        if (text_w > 260.0f) text_w = 260.0f;
        int total_w = (ico ? (icon_draw + 8) : 0) + (int)text_w + 12;

        draw_hover_bg(g, TB_HIT_FOCUSED, fx, item_y, total_w, item_h);
        add_hit(TB_HIT_FOCUSED, fx, item_y, total_w, item_h, false);

        int cx = fx + 6;
        if (ico) {
            int iy = (g_tb_h - icon_draw) / 2;
            GdipDrawImageRectI(g, (GpImage *)ico, cx, iy, icon_draw, icon_draw);
            cx += icon_draw + 7;
        }
        draw_text(g, focused.app_name, L"Segoe UI", font_sz, GP_FONT_BOLD,
                  g_theme.fg_primary, (float)cx, 0.0f, text_w + 6.0f, (float)g_tb_h,
                  GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
    }

    /* === CENTER: Clock / Calendar === */
    wchar_t clock_str[64];
    format_clock(clock_str, 64);
    float cw = measure_text_width(g, clock_str, L"Segoe UI", font_sz, GP_FONT_BOLD);
    int pill_w = (int)cw + 18;
    int pill_x = (g_tb_w - pill_w) / 2;
    draw_hover_bg(g, TB_HIT_CLOCK, pill_x, item_y, pill_w, item_h);
    add_hit(TB_HIT_CLOCK, pill_x, item_y, pill_w, item_h, true);
    draw_text(g, clock_str, L"Segoe UI", font_sz, GP_FONT_BOLD,
              g_theme.fg_primary, (float)pill_x, 0.0f, (float)pill_w, (float)g_tb_h,
              GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

    /* === RIGHT, laid out from the right edge inwards; left to right it reads:
     *   [KBD] [BT] [NET+TS] [VOL] [CPU] [GPU] [RAM] [SCREENSHOT] [NOTIF] [POWER]
     */
    int rx = g_tb_w - pad_x;
    const int gap = 4;

    /* 9. Power Menu */
    {
        int w = 26;
        rx -= w;
        draw_hover_bg(g, TB_HIT_POWER, rx, item_y, w, item_h);
        add_hit(TB_HIT_POWER, rx, item_y, w, item_h, true);
        draw_icon_glyph(g, 0xE7E8, icon_sz, g_theme.fg_primary, (float)rx, 0.0f, (float)w, (float)g_tb_h);
        rx -= gap;
    }

    /* 8. Notifications / Focus Assist */
    {
        int w = 26;
        rx -= w;
        draw_hover_bg(g, TB_HIT_NOTIF, rx, item_y, w, item_h);
        add_hit(TB_HIT_NOTIF, rx, item_y, w, item_h, true);
        wchar_t glyph = g_telemetry.dnd_enabled ? 0xE708 : 0xEA8F;
        ARGB col = g_telemetry.dnd_enabled ? g_theme.accent : g_theme.fg_primary;
        draw_icon_glyph(g, glyph, icon_sz, col, (float)rx, 0.0f, (float)w, (float)g_tb_h);
        if (g_telemetry.notif_count > 0) {
            draw_circle_fill(g, (float)(rx + w - 6), 8.0f, 3.5f, g_theme.accent);
        }
        rx -= gap;
    }

    /* 7b. Screenshot Trigger */
    {
        int w = 26;
        rx -= w;
        draw_hover_bg(g, TB_HIT_SCREENSHOT, rx, item_y, w, item_h);
        add_hit(TB_HIT_SCREENSHOT, rx, item_y, w, item_h, true);
        draw_icon_glyph(g, 0xE722, icon_sz, g_theme.fg_primary, (float)rx, 0.0f, (float)w, (float)g_tb_h);
        rx -= gap;
    }

    /* 7. Memory in use, of all, in GB */
    {
        wchar_t ram_s[48];
        int used_gb = (int)((g_telemetry.ram_used_mb + 512) / 1024);
        int tot_gb  = (int)((g_telemetry.ram_total_mb + 512) / 1024);
        _snwprintf_s(ram_s, 48, _TRUNCATE, L"%dGB / %dGB", used_gb, tot_gb);
        float tw = measure_text_width(g, ram_s, L"Segoe UI", font_sz, GP_FONT_REGULAR);
        int w = 20 + (int)tw + 10;
        rx -= w;
        draw_hover_bg(g, TB_HIT_RAM, rx, item_y, w, item_h);
        add_hit(TB_HIT_RAM, rx, item_y, w, item_h, true);
        draw_icon_glyph(g, 0xE964, icon_sz - 1.0f, g_theme.fg_secondary, (float)(rx + 4), 0.0f, 18.0f, (float)g_tb_h);
        draw_text(g, ram_s, L"Segoe UI", font_sz, GP_FONT_REGULAR, g_theme.fg_primary,
                  (float)(rx + 23), 0.0f, tw + 6.0f, (float)g_tb_h, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        rx -= gap;
    }

    /* 6. GPU usage */
    {
        wchar_t gpu_s[24];
        _snwprintf_s(gpu_s, 24, _TRUNCATE, L"%d%%", g_telemetry.gpu_percent);
        float tw = measure_text_width(g, gpu_s, L"Segoe UI", font_sz, GP_FONT_REGULAR);
        int w = 20 + (int)tw + 10;
        rx -= w;
        draw_hover_bg(g, TB_HIT_GPU, rx, item_y, w, item_h);
        add_hit(TB_HIT_GPU, rx, item_y, w, item_h, true);
        draw_icon_glyph(g, 0xE7F4, icon_sz - 1.0f, g_theme.fg_secondary, (float)(rx + 4), 0.0f, 18.0f, (float)g_tb_h);
        draw_text(g, gpu_s, L"Segoe UI", font_sz, GP_FONT_REGULAR, g_theme.fg_primary,
                  (float)(rx + 23), 0.0f, tw + 6.0f, (float)g_tb_h, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        rx -= gap;
    }

    /* 5. CPU usage */
    {
        wchar_t cpu_s[24];
        _snwprintf_s(cpu_s, 24, _TRUNCATE, L"%d%%", g_telemetry.cpu_percent);
        float tw = measure_text_width(g, cpu_s, L"Segoe UI", font_sz, GP_FONT_REGULAR);
        int w = 20 + (int)tw + 10;
        rx -= w;
        draw_hover_bg(g, TB_HIT_CPU, rx, item_y, w, item_h);
        add_hit(TB_HIT_CPU, rx, item_y, w, item_h, true);
        draw_icon_glyph(g, 0xE950, icon_sz - 1.0f, g_theme.fg_secondary, (float)(rx + 4), 0.0f, 18.0f, (float)g_tb_h);
        draw_text(g, cpu_s, L"Segoe UI", font_sz, GP_FONT_REGULAR, g_theme.fg_primary,
                  (float)(rx + 23), 0.0f, tw + 6.0f, (float)g_tb_h, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        rx -= gap;
    }

    /* 4. Media / Volume */
    {
        int w = 28;
        rx -= w;
        draw_hover_bg(g, TB_HIT_MEDIA, rx, item_y, w, item_h);
        add_hit(TB_HIT_MEDIA, rx, item_y, w, item_h, true);
        wchar_t vglyph = 0xE767;
        if (g_telemetry.volume_muted || g_telemetry.volume_percent == 0) vglyph = 0xE74F;
        else if (g_telemetry.volume_percent < 33) vglyph = 0xE993;
        else if (g_telemetry.volume_percent < 66) vglyph = 0xE994;
        else vglyph = 0xE995;
        draw_icon_glyph(g, vglyph, icon_sz, g_theme.fg_primary, (float)rx, 0.0f, (float)w, (float)g_tb_h);
        rx -= gap;
    }

    /* 3b. AirDrop, with its own switch (issue #155): the accent while
     * something is received or sent, a dot while a request waits */
    if (g_cfg.airdrop_enabled) {
        int w = 28;
        rx -= w;
        draw_hover_bg(g, TB_HIT_SHARE, rx, item_y, w, item_h);
        add_hit(TB_HIT_SHARE, rx, item_y, w, item_h, true);
        ARGB sc = airdrop_active() ? g_theme.accent : g_theme.fg_primary;
        draw_share_icon(g, (float)rx + (float)w * 0.5f, (float)g_tb_h * 0.5f + 0.5f, icon_sz * 0.50f, sc, 1.25f);
        if (airdrop_pending()) {
            draw_circle_fill(g, (float)(rx + w - 6), 8.0f, 3.5f, g_theme.danger);
        }
        rx -= gap;
    }

    /* 3. Network + Tailscale dot */
    {
        int w = 28;
        rx -= w;
        draw_hover_bg(g, TB_HIT_NETWORK, rx, item_y, w, item_h);
        add_hit(TB_HIT_NETWORK, rx, item_y, w, item_h, true);
        wchar_t nglyph = !g_telemetry.net_connected ? 0xF384 : (g_telemetry.net_is_wifi ? 0xE701 : 0xE839);
        draw_icon_glyph(g, nglyph, icon_sz, g_theme.fg_primary, (float)rx, 0.0f, (float)w, (float)g_tb_h);
        if (g_telemetry.ts_running) {
            draw_circle_fill(g, (float)(rx + w - 5), (float)(g_tb_h - 7), 3.0f, g_theme.success);
        }
        rx -= gap;
    }

    /* 2. Bluetooth */
    {
        int w = 26;
        rx -= w;
        draw_hover_bg(g, TB_HIT_BLUETOOTH, rx, item_y, w, item_h);
        add_hit(TB_HIT_BLUETOOTH, rx, item_y, w, item_h, true);
        ARGB bt_col = (g_telemetry.bt_connected_count > 0) ? g_theme.accent : g_theme.fg_primary;
        draw_icon_glyph(g, 0xE702, icon_sz, bt_col, (float)rx, 0.0f, (float)w, (float)g_tb_h);
        rx -= gap;
    }

    /* 1. Keyboard layout */
    {
        const wchar_t *kb = g_telemetry.kbd_short[0] ? g_telemetry.kbd_short : L"EN";
        float tw = measure_text_width(g, kb, L"Segoe UI", font_sz, GP_FONT_BOLD);
        int w = (int)tw + 14;
        rx -= w;
        draw_hover_bg(g, TB_HIT_KEYBOARD, rx, item_y, w, item_h);
        add_hit(TB_HIT_KEYBOARD, rx, item_y, w, item_h, true);
        draw_text(g, kb, L"Segoe UI", font_sz, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)rx, 0.0f, (float)w, (float)g_tb_h, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
    }

    /* 0. Device indicators, with their own switch (issue #157): each opens
     * Lares on its page; a controller low on battery in the danger colour */
    if (g_cfg.indicators_enabled) {
        LaresIndicators ind;
        indicators_get(&ind);
        bool pad_low = ind.controller_battery == LARES_PAD_BATTERY_EMPTY ||
                       ind.controller_battery == LARES_PAD_BATTERY_LOW;
        const struct { bool on; wchar_t glyph; TopBarHitId id; ARGB col; } items[] = {
            { ind.remote,     0xE703, TB_HIT_IND_REMOTE,     g_theme.fg_primary },
            { ind.mouse,      0xE962, TB_HIT_IND_MOUSE,      g_theme.fg_primary },
            { ind.keyboard,   0xE765, TB_HIT_IND_KEYBOARD,   g_theme.fg_primary },
            { ind.headphones, 0xE7F6, TB_HIT_IND_HEADPHONES, g_theme.fg_primary },
            { ind.controller, 0xE7FC, TB_HIT_IND_CONTROLLER, pad_low ? g_theme.danger : g_theme.fg_primary },
        };
        bool first = true;
        for (int i = 0; i < (int)(sizeof(items) / sizeof(items[0])); ++i) {
            if (!items[i].on) continue;
            rx -= first ? gap + 4 : 2;
            first = false;
            int w = 26;
            rx -= w;
            draw_hover_bg(g, items[i].id, rx, item_y, w, item_h);
            add_hit(items[i].id, rx, item_y, w, item_h, true);
            draw_icon_glyph(g, items[i].glyph, icon_sz, items[i].col, (float)rx, 0.0f, (float)w, (float)g_tb_h);
        }
    }

    if (g_tb_shown || g_tb_vis > 0.0f) topbar_present();
}

static TopBarHit *hit_test(int x, int y) {
    for (int i = 0; i < g_hit_count; i++) {
        if (x >= g_hits[i].rc.left && x <= g_hits[i].rc.right &&
            y >= g_hits[i].rc.top && y <= g_hits[i].rc.bottom) {
            return &g_hits[i];
        }
    }
    return NULL;
}

static bool cursor_over_bar(void) {
    POINT cp;
    if (!GetCursorPos(&cp)) return false;
    return cp.x >= g_tb_x && cp.x < g_tb_x + g_tb_w &&
           cp.y >= g_tb_y - g_cfg.topbar_margin && cp.y < g_tb_y + g_tb_h + 6;
}

static LRESULT CALLBACK topbar_sensor_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSEMOVE) {
        if (!g_tb_fullscreen_hidden) topbar_set_shown(true, true);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK topbar_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LARES_APPBAR_CB:
        if (wParam == ABN_FULLSCREENAPP) {
            /* The window tracker decides: the desktop itself can be reported as full screen. */
            topbar_on_windows_changed();
        } else if (wParam == ABN_POSCHANGED) {
            topbar_register_appbar(hwnd);
            topbar_repaint();
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_TB_ANIM) {
            float t = (float)(GetTickCount() - g_tb_anim_start) / (float)TB_ANIM_MS;
            if (t >= 1.0f) {
                topbar_slide_done();
            } else {
                g_tb_vis = g_tb_vis_from + (g_tb_vis_to - g_tb_vis_from) * anim_ease_out(t);
                topbar_present();
            }
        } else if (wParam == TIMER_TB_HIDECHK) {
            if (!g_cfg.topbar_autohide || !g_tb_shown) {
                KillTimer(hwnd, TIMER_TB_HIDECHK);
            } else if (cursor_over_bar() || popups_is_open() || g_tb_menu_open) {
                g_tb_idle_checks = 0;
            } else if (++g_tb_idle_checks >= 2) {
                topbar_set_shown(false, true);
            }
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (!g_tracking_mouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking_mouse = true;
        }
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        TopBarHit *h = hit_test(mx, my);
        TopBarHitId new_hover = (h && h->clickable) ? h->id : TB_HIT_NONE;
        if (new_hover != g_hover_id) {
            g_hover_id = new_hover;
            topbar_repaint();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking_mouse = false;
        if (g_hover_id != TB_HIT_NONE) {
            g_hover_id = TB_HIT_NONE;
            topbar_repaint();
        }
        return 0;

    case WM_RBUTTONUP: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        ClientToScreen(hwnd, &pt);
        topbar_show_menu(hwnd, pt.x, pt.y);
        return 0;
    }

    case WM_LBUTTONUP: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        TopBarHit *h = hit_test(mx, my);
        if (!h || !h->clickable) return 0;

        int anchor_x = g_tb_x + (h->rc.left + h->rc.right) / 2;
        int anchor_y = g_tb_y + g_tb_h + 6;

        switch (h->id) {
        case TB_HIT_CLOCK:     popups_toggle(POPUP_CALENDAR, anchor_x, anchor_y); break;
        case TB_HIT_KEYBOARD:  popups_toggle(POPUP_KEYBOARD, anchor_x, anchor_y); break;
        case TB_HIT_BLUETOOTH: popups_toggle(POPUP_BLUETOOTH, anchor_x, anchor_y); break;
        case TB_HIT_NETWORK:   popups_toggle(POPUP_NETWORK, anchor_x, anchor_y); break;
        case TB_HIT_SHARE:     popups_toggle(POPUP_SHARE, anchor_x, anchor_y); break;
        case TB_HIT_IND_CONTROLLER: action_open_lares(L"controller"); break;
        case TB_HIT_IND_HEADPHONES: action_open_lares(L"headphones"); break;
        case TB_HIT_IND_KEYBOARD:   action_open_lares(L"keyboard"); break;
        case TB_HIT_IND_MOUSE:      action_open_lares(L"mouse"); break;
        case TB_HIT_IND_REMOTE:     action_open_lares(L"remote"); break;
        case TB_HIT_MEDIA:     popups_toggle(POPUP_MEDIA, anchor_x, anchor_y); break;
        case TB_HIT_NOTIF:     popups_toggle(POPUP_NOTIFICATIONS, anchor_x, anchor_y); break;
        case TB_HIT_SCREENSHOT:popups_toggle(POPUP_SCREENSHOT, anchor_x, anchor_y); break;
        case TB_HIT_POWER:     popups_toggle(POPUP_POWER, anchor_x, anchor_y); break;
        case TB_HIT_CPU:
        case TB_HIT_GPU:
        case TB_HIT_RAM:
            popups_close();
            if (!g_cfg.isolated) action_open_processes();
            break;
        default: break;
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        TopBarHit *h = hit_test(pt.x, pt.y);
        if (h && h->id == TB_HIT_MEDIA) {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            telemetry_step_volume(delta > 0 ? 4 : -4);
            topbar_repaint();
            return 0;
        }
        break;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TB_ANIM);
        KillTimer(hwnd, TIMER_TB_HIDECHK);
        topbar_unregister_appbar(hwnd);
        surface_destroy(&g_tb_surf);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool topbar_create(HINSTANCE hInst) {
    if (!g_cfg.topbar_enabled) return true;
    /* Made again after being taken away: start from a plain, shown bar. */
    g_tb_shown = true;
    g_tb_vis = g_tb_vis_from = g_tb_vis_to = 1.0f;
    g_tb_fullscreen_hidden = false;
    g_tb_appbar_registered = false;
    g_hover_id = TB_HIT_NONE;
    g_tracking_mouse = false;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = topbar_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = TOPBAR_CLASS;
    RegisterClassExW(&wc);

    WNDCLASSEXW sc;
    ZeroMemory(&sc, sizeof(sc));
    sc.cbSize = sizeof(sc);
    sc.lpfnWndProc = topbar_sensor_proc;
    sc.hInstance = hInst;
    sc.lpszClassName = TOPBAR_SENSOR_CLASS;
    RegisterClassExW(&sc);

    /* A 2 px strip on the top edge brings an auto-hidden bar back: it only
     * receives mouse moves, no polling. */
    g_hTbSensor = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        TOPBAR_SENSOR_CLASS, L"", WS_POPUP, 0, 0, 100, 2, NULL, NULL, hInst, NULL);
    if (g_hTbSensor) SetLayeredWindowAttributes(g_hTbSensor, 0, 1, LWA_ALPHA);

    g_hTopBarWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        TOPBAR_CLASS, L"Lares top bar",
        WS_POPUP,
        0, 0, 1, topbar_get_height(),
        NULL, NULL, hInst, NULL);
    if (!g_hTopBarWnd) return false;

    topbar_register_appbar(g_hTopBarWnd);
    topbar_repaint();
    if (g_cfg.topbar_autohide) {
        g_tb_idle_checks = 0;
        SetTimer(g_hTopBarWnd, TIMER_TB_HIDECHK, 350, NULL);
    }
    return true;
}

void topbar_on_display_change(void) {
    if (g_hTopBarWnd) {
        topbar_register_appbar(g_hTopBarWnd);
        topbar_repaint();
    }
}

/* Explorer restarted: every desktop toolbar registration is gone, so reserve the strip again. */
void topbar_on_explorer_restart(void) {
    if (!g_hTopBarWnd) return;
    g_tb_appbar_registered = false;
    topbar_register_appbar(g_hTopBarWnd);
    topbar_repaint();
    topbar_reassert_topmost();
}

/* Called on every foreground or window change: hide for a real full-screen window, and otherwise
 * make sure the bar is still above normal windows (Show Desktop and some apps push it down). */
void topbar_on_windows_changed(void) {
    if (!g_hTopBarWnd) return;
    bool fs = win_tracker_is_fullscreen_active();
    if (fs != g_tb_fullscreen_hidden) {
        g_tb_fullscreen_hidden = fs;
        if (fs) {
            KillTimer(g_hTopBarWnd, TIMER_TB_ANIM);
            ShowWindow(g_hTopBarWnd, SW_HIDE);
            if (g_hTbSensor) ShowWindow(g_hTbSensor, SW_HIDE);
        } else if (g_tb_shown) {
            g_tb_vis = 1.0f;
            SetWindowPos(g_hTopBarWnd, HWND_TOPMOST, g_tb_x, g_tb_y, g_tb_w, g_tb_h,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            topbar_repaint();
        } else if (g_hTbSensor) {
            ShowWindow(g_hTbSensor, SW_SHOWNOACTIVATE);
        }
        return;
    }
    /* Only touch the window when it really dropped out of the top band or got hidden: an
     * unconditional call here would answer every window event with a new one. */
    if (!fs && g_tb_shown && (!IsWindowVisible(g_hTopBarWnd) ||
                              !(GetWindowLongPtrW(g_hTopBarWnd, GWL_EXSTYLE) & WS_EX_TOPMOST))) {
        topbar_reassert_topmost();
    }
}

void topbar_reassert_topmost(void) {
    if (g_tb_fullscreen_hidden) return;
    if (g_hTopBarWnd && g_tb_shown) {
        SetWindowPos(g_hTopBarWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else if (g_hTbSensor && !g_tb_shown) {
        SetWindowPos(g_hTbSensor, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

/* Settings changed while the bar is up: fixed or auto-hide, animations. */
void topbar_apply_settings(void) {
    if (!g_hTopBarWnd) return;
    topbar_apply_mode();
    topbar_repaint();
}

int topbar_snapshot(const wchar_t *png_path) {
    return g_tb_surf.hdc && surface_save_png(&g_tb_surf, png_path);
}

void topbar_destroy(void) {
    if (g_hTbSensor) {
        DestroyWindow(g_hTbSensor);
        g_hTbSensor = NULL;
    }
    if (g_hTopBarWnd) {
        DestroyWindow(g_hTopBarWnd);
        g_hTopBarWnd = NULL;
    }
}
