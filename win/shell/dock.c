// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The dock (issue #145): the Start button, pinned and open apps, media
 * controls. Always shown (it reserves its strip of the screen), hidden
 * automatically, or hidden when a window covers it. Pinned apps can be
 * dragged to a new place. */
#include "shell.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>

#define DOCK_CLASS        L"LaresDockWnd"
#define DOCK_SENSOR_CLASS L"LaresDockSensorWnd"
#define TIMER_DOCK_HIDE   101
#define TIMER_DOCK_SHOW   102
#define TIMER_DOCK_ANIM   103
#define TIMER_DOCK_BOUNCE 104
#define DOCK_ANIM_MS      180
#define DOCK_BOUNCE_MS    560
#define WM_LARES_DOCK_APPBAR_CB (WM_APP + 51)

#define MAX_RENDER_ITEMS  80

typedef enum {
    RENDER_APP = 0,
    RENDER_SEP,
    RENDER_MEDIA,
    RENDER_START
} RenderItemKind;

typedef struct {
    RenderItemKind kind;
    int            cfg_idx; /* >= 0 if from g_dock_cfg, -1 if dynamic unpinned running app */
    wchar_t        name[128];
    wchar_t        umid[256];
    wchar_t        exe_path[MAX_PATH];
    wchar_t        command[MAX_PATH];
    wchar_t        args[256];
    wchar_t        workdir[MAX_PATH];
    wchar_t        icon_rel[MAX_PATH];
    bool           is_square;
    bool           pinned;
    HWND           windows[16];
    int            win_count;
    bool           is_focused;
    RECT           rc;      /* tile rect inside dock surface */
} DockRenderItem;

static LayerSurface g_dock_surf = {0};
static HWND g_hSensorWnd = NULL;
static DockRenderItem g_items[MAX_RENDER_ITEMS];
static int g_item_count = 0;
static int g_hover_idx = -1;
static int g_hover_media_btn = 0; /* 0=none, 1=prev, 2=play, 3=next */
static int g_pressed_idx = -1;
static bool g_tracking_dock = false;
static bool g_dock_hidden = false;
static bool g_mouse_in_dock = false;
static bool g_menu_open = false;
static bool g_dock_appbar = false;

/* Slide state: visible fraction of the dock (1 = fully on screen). */
static float g_dock_vis = 1.0f, g_dock_vis_from = 1.0f, g_dock_vis_to = 1.0f;
static DWORD g_dock_anim_start = 0;

/* Launch feedback: the clicked icon hops twice while its app starts. */
static wchar_t g_bounce_name[128];
static DWORD   g_bounce_start = 0;

/* Drag to reorder pinned apps. */
static bool g_drag_armed = false, g_drag_active = false;
static int  g_drag_cfg = -1, g_drag_press_x = 0;

static int g_dock_x = 0, g_dock_y = 0, g_dock_w = 400, g_dock_h = 84;
static RECT g_dock_pill_screen = {0};
static RECT g_media_btn_rc[3];

static bool match_win_to_cfg(const TrackedWindow *w, const DockItemConfig *c) {
    if (c->type != DOCK_ITEM_APP) return false;
    if (w->umid[0]) {
        if (c->umid[0] && _wcsicmp(w->umid, c->umid) == 0) return true;
        /* Fallback match by executable filename when one side lacks UMID */
        if (!c->umid[0] && w->exe_path[0]) {
            if (c->command[0] && _wcsicmp(w->exe_path, c->command) == 0) return true;
            if (c->path[0] && _wcsicmp(w->exe_path, c->path) == 0) return true;
        }
        return false;
    }
    if (w->exe_path[0]) {
        if (c->command[0] && _wcsicmp(w->exe_path, c->command) == 0) return true;
        if (c->path[0] && _wcsicmp(w->exe_path, c->path) == 0) return true;
        if (c->command[0] && _wcsicmp(PathFindFileNameW(w->exe_path), PathFindFileNameW(c->command)) == 0) {
            return true;
        }
    }
    return false;
}

static bool match_two_wins_same_group(const TrackedWindow *a, const TrackedWindow *b) {
    if (a->umid[0] && b->umid[0]) return _wcsicmp(a->umid, b->umid) == 0;
    if (a->exe_path[0] && b->exe_path[0]) return _wcsicmp(a->exe_path, b->exe_path) == 0;
    return a->pid == b->pid;
}

static void rebuild_dock_items(void) {
    TrackedWindow wins[LARES_MAX_WINDOWS];
    int win_n = win_tracker_get_windows(wins, LARES_MAX_WINDOWS);
    bool win_claimed[LARES_MAX_WINDOWS];
    ZeroMemory(win_claimed, sizeof(win_claimed));

    g_item_count = 0;

    /* The Start button, far left; Win+1..0 count from the first app after it. */
    {
        DockRenderItem *st = &g_items[g_item_count++];
        ZeroMemory(st, sizeof(*st));
        st->kind = RENDER_START;
        st->cfg_idx = -1;
        wcsncpy_s(st->name, 128, L"Start", _TRUNCATE);

        DockRenderItem *sep = &g_items[g_item_count++];
        ZeroMemory(sep, sizeof(*sep));
        sep->kind = RENDER_SEP;
        sep->cfg_idx = -1;
    }

    DockGroup groups[3] = { DOCK_GROUP_LEFT, DOCK_GROUP_CENTER, DOCK_GROUP_RIGHT };
    for (int gi = 0; gi < 3; gi++) {
        int group_start = g_item_count;

        for (int i = 0; i < g_dock_cfg_count && g_item_count < MAX_RENDER_ITEMS - 2; i++) {
            const DockItemConfig *c = &g_dock_cfg[i];
            if (c->group != groups[gi]) continue;

            if (c->type == DOCK_ITEM_MEDIA) {
                if (!g_cfg.dock_show_media_widget) continue;
                DockRenderItem *ri = &g_items[g_item_count++];
                ZeroMemory(ri, sizeof(*ri));
                ri->kind = RENDER_MEDIA;
                ri->cfg_idx = i;
                wcsncpy_s(ri->name, 128, L"Media", _TRUNCATE);
                continue;
            }

            DockRenderItem tmp;
            ZeroMemory(&tmp, sizeof(tmp));
            tmp.kind = RENDER_APP;
            tmp.cfg_idx = i;
            wcsncpy_s(tmp.name, 128, c->name, _TRUNCATE);
            wcsncpy_s(tmp.umid, 256, c->umid, _TRUNCATE);
            wcsncpy_s(tmp.exe_path, MAX_PATH, c->path, _TRUNCATE);
            wcsncpy_s(tmp.command, MAX_PATH, c->command[0] ? c->command : c->path, _TRUNCATE);
            wcsncpy_s(tmp.args, 256, c->args, _TRUNCATE);
            wcsncpy_s(tmp.workdir, MAX_PATH, c->workdir, _TRUNCATE);
            wcsncpy_s(tmp.icon_rel, MAX_PATH, c->icon_rel, _TRUNCATE);
            tmp.is_square = c->is_square_icon;
            tmp.pinned = c->pinned;

            for (int w = 0; w < win_n; w++) {
                if (!win_claimed[w] && match_win_to_cfg(&wins[w], c)) {
                    win_claimed[w] = true;
                    if (tmp.win_count < 16) tmp.windows[tmp.win_count++] = wins[w].hwnd;
                    if (wins[w].is_focused) tmp.is_focused = true;
                }
            }

            if (tmp.pinned || tmp.win_count > 0) {
                g_items[g_item_count++] = tmp;
            }
        }

        /* Append dynamic unpinned running windows into CENTER group */
        if (groups[gi] == DOCK_GROUP_CENTER) {
            for (int w = 0; w < win_n && g_item_count < MAX_RENDER_ITEMS - 2; w++) {
                if (win_claimed[w]) continue;
                win_claimed[w] = true;

                DockRenderItem *ri = &g_items[g_item_count++];
                ZeroMemory(ri, sizeof(*ri));
                ri->kind = RENDER_APP;
                ri->cfg_idx = -1;
                wcsncpy_s(ri->name, 128, wins[w].app_name, _TRUNCATE);
                wcsncpy_s(ri->umid, 256, wins[w].umid, _TRUNCATE);
                wcsncpy_s(ri->exe_path, MAX_PATH, wins[w].exe_path, _TRUNCATE);
                wcsncpy_s(ri->command, MAX_PATH, wins[w].exe_path, _TRUNCATE);
                ri->pinned = false;
                ri->windows[ri->win_count++] = wins[w].hwnd;
                if (wins[w].is_focused) ri->is_focused = true;

                for (int w2 = w + 1; w2 < win_n; w2++) {
                    if (!win_claimed[w2] && match_two_wins_same_group(&wins[w], &wins[w2])) {
                        win_claimed[w2] = true;
                        if (ri->win_count < 16) ri->windows[ri->win_count++] = wins[w2].hwnd;
                        if (wins[w2].is_focused) ri->is_focused = true;
                    }
                }
            }
        }

        /* Insert separator between non-empty groups */
        if (gi < 2 && g_item_count > group_start && g_item_count < MAX_RENDER_ITEMS - 1) {
            DockRenderItem *sep = &g_items[g_item_count++];
            ZeroMemory(sep, sizeof(*sep));
            sep->kind = RENDER_SEP;
            sep->cfg_idx = -1;
        }
    }

    /* Trim trailing separator if right group was empty */
    if (g_item_count > 0 && g_items[g_item_count - 1].kind == RENDER_SEP) {
        g_item_count--;
    }
}

static void layout_dock_geometry(void) {
    MONITORINFO mi;
    shell_screen_rect(&mi.rcMonitor);

    int item_sz = g_cfg.dock_item_size;
    int pad     = g_cfg.dock_padding;
    int spacing = g_cfg.dock_spacing;
    int margin  = g_cfg.dock_margin;
    int media_w = item_sz * 3 + spacing * 2; /* three tiles wide */
    int sep_w   = 9;

    int content_w = 0;
    for (int i = 0; i < g_item_count; i++) {
        if (i > 0) content_w += spacing;
        if (g_items[i].kind == RENDER_MEDIA) content_w += media_w;
        else if (g_items[i].kind == RENDER_SEP) content_w += sep_w;
        else content_w += item_sz;
    }
    if (content_w < item_sz) content_w = item_sz;

    int pill_w = content_w + pad * 2;
    int pill_h = item_sz + pad * 2;
    int max_w = (mi.rcMonitor.right - mi.rcMonitor.left) - margin * 2;
    if (pill_w > max_w) pill_w = max_w;

    /* 26 px above the dock for the name of the app under the pointer. */
    int tooltip_space = 26;
    g_dock_w = pill_w + 16; /* 8px shadow breathing room each side */
    g_dock_h = tooltip_space + pill_h + margin;
    g_dock_x = mi.rcMonitor.left + ((mi.rcMonitor.right - mi.rcMonitor.left) - g_dock_w) / 2;
    g_dock_y = mi.rcMonitor.bottom - g_dock_h;

    int pill_x = 8;
    int pill_y = tooltip_space;

    g_dock_pill_screen.left   = g_dock_x + pill_x;
    g_dock_pill_screen.top    = g_dock_y + pill_y;
    g_dock_pill_screen.right  = g_dock_pill_screen.left + pill_w;
    g_dock_pill_screen.bottom = mi.rcMonitor.bottom;

    int cx = pill_x + pad;
    int cy = pill_y + pad;
    for (int i = 0; i < g_item_count; i++) {
        int w = (g_items[i].kind == RENDER_MEDIA) ? media_w
              : (g_items[i].kind == RENDER_SEP) ? sep_w : item_sz;
        g_items[i].rc.left   = cx;
        g_items[i].rc.top    = cy;
        g_items[i].rc.right  = cx + w;
        g_items[i].rc.bottom = cy + item_sz;
        cx += w + spacing;
    }

    if (g_hSensorWnd) {
        SetWindowPos(g_hSensorWnd, HWND_TOPMOST,
                     mi.rcMonitor.left, mi.rcMonitor.bottom - 2,
                     mi.rcMonitor.right - mi.rcMonitor.left, 2,
                     SWP_NOACTIVATE);
    }
}

static void fill_xp_pane(GpGraphics *g, float ox, float oy, float sz,
                         float x0, float y0, float tc1x, float tc1y, float tc2x, float tc2y, float x1, float y1,
                         float x2, float y2, float bc1x, float bc1y, float bc2x, float bc2y, float x3, float y3,
                         ARGB col_left, ARGB col_right, ARGB rim_col) {
    GpPath *path = NULL;
    if (GdipCreatePath(GP_FILL_ALTERNATE, &path) != GP_OK || !path) return;

    GdipAddPathBezier(path,
                      ox + x0 * sz,   oy + y0 * sz,
                      ox + tc1x * sz, oy + tc1y * sz,
                      ox + tc2x * sz, oy + tc2y * sz,
                      ox + x1 * sz,   oy + y1 * sz);
    GdipAddPathLine(path, ox + x1 * sz, oy + y1 * sz, ox + x2 * sz, oy + y2 * sz);
    GdipAddPathBezier(path,
                      ox + x2 * sz,   oy + y2 * sz,
                      ox + bc1x * sz, oy + bc1y * sz,
                      ox + bc2x * sz, oy + bc2y * sz,
                      ox + x3 * sz,   oy + y3 * sz);
    GdipClosePathFigure(path);

    GpPointF p1 = { ox + x0 * sz, oy + (y0 + y3) * 0.5f * sz };
    GpPointF p2 = { ox + x1 * sz, oy + (y1 + y2) * 0.5f * sz };
    GpLineGradient *grad = NULL;
    if (GdipCreateLineBrush(&p1, &p2, col_left, col_right, 1 /* WrapModeTileFlipX */, &grad) == GP_OK && grad) {
        GdipFillPath(g, (GpBrush *)grad, path);
        GdipDeleteBrush((GpBrush *)grad);
    }

    GpPen *pen = NULL;
    if (GdipCreatePen1(rim_col, 0.75f, GP_UNIT_PIXEL, &pen) == GP_OK && pen) {
        GdipDrawPath(g, pen, path);
        GdipDeletePen(pen);
    }
    GdipDeletePath(path);
}

/* The Start button's picture: a waving four-pane flag after Microsoft's
 * Windows XP logo (artwork credit: Microsoft; see README). */
static void draw_winxp_logo(GpGraphics *g, int tile_x, int tile_y, int tile_sz) {
    float pad = (float)tile_sz * 0.195f;
    float sz = (float)tile_sz - pad * 2.0f;
    /* The curves span x 0.010 to 0.960 (middle 0.485) and y 0.089 to 0.993 (middle 0.541). */
    float ox = (float)tile_x + pad + (0.500f - 0.485f) * sz;
    float oy = (float)tile_y + pad + (0.500f - 0.541f) * sz;

    GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);

    /* Top left: red-orange, lighter at the outer edge */
    fill_xp_pane(g, ox, oy, sz,
                 0.16f, 0.14f,  0.29f, 0.05f,  0.42f, 0.07f,  0.52f, 0.15f,
                 0.44f, 0.49f,  0.34f, 0.42f,  0.22f, 0.41f,  0.09f, 0.49f,
                 MAKE_ARGB(255, 255, 118, 56), MAKE_ARGB(255, 214, 38, 14),
                 MAKE_ARGB(90, 255, 165, 110));

    /* Top right: green, lighter at the outer edge */
    fill_xp_pane(g, ox, oy, sz,
                 0.58f, 0.19f,  0.71f, 0.27f,  0.84f, 0.26f,  0.96f, 0.18f,
                 0.88f, 0.53f,  0.76f, 0.61f,  0.63f, 0.61f,  0.50f, 0.53f,
                 MAKE_ARGB(255, 76, 152, 18), MAKE_ARGB(255, 156, 232, 48),
                 MAKE_ARGB(90, 190, 245, 110));

    /* Bottom left: blue, lighter at the outer edge */
    fill_xp_pane(g, ox, oy, sz,
                 0.08f, 0.56f,  0.21f, 0.47f,  0.34f, 0.48f,  0.43f, 0.56f,
                 0.35f, 0.90f,  0.26f, 0.83f,  0.14f, 0.82f,  0.01f, 0.90f,
                 MAKE_ARGB(255, 102, 194, 255), MAKE_ARGB(255, 26, 94, 214),
                 MAKE_ARGB(95, 145, 214, 255));

    /* Bottom right: yellow, lighter at the outer edge */
    fill_xp_pane(g, ox, oy, sz,
                 0.49f, 0.60f,  0.62f, 0.68f,  0.75f, 0.68f,  0.87f, 0.60f,
                 0.79f, 0.94f,  0.67f, 1.01f,  0.54f, 1.01f,  0.41f, 0.94f,
                 MAKE_ARGB(255, 194, 136, 10), MAKE_ARGB(255, 255, 224, 42),
                 MAKE_ARGB(95, 255, 238, 120));
}

static void draw_media_tile(GpGraphics *g, const DockRenderItem *ri) {
    int x = ri->rc.left, y = ri->rc.top;
    int w = ri->rc.right - ri->rc.left;
    int h = ri->rc.bottom - ri->rc.top;
    float r = (float)h * 0.25f;

    draw_round_rect_fill(g, (float)x, (float)y, (float)w, (float)h, r, g_theme.tile_bg);

    MediaState ms;
    media_get_state(&ms);

    /* Left square album art / music glyph */
    GpBitmap *thumb = media_lock_thumbnail();
    if (thumb) {
        GdipDrawImageRectI(g, (GpImage *)thumb, x + 2, y + 2, h - 4, h - 4);
    } else {
        draw_round_rect_fill(g, (float)(x + 2), (float)(y + 2), (float)(h - 4), (float)(h - 4),
                             r - 2.0f, g_theme.accent_overlay_selected);
        draw_icon_glyph(g, 0xE8D6, 17.0f, g_theme.fg_secondary,
                        (float)x, (float)y, (float)h, (float)h);
    }
    media_unlock_thumbnail();

    int ix = x + h + 4;
    int iw = w - h - 8;
    const wchar_t *label = (ms.has_session && ms.title[0]) ? ms.title : L"Not playing";
    draw_text(g, label, L"Segoe UI", 11.0f, GP_FONT_BOLD,
              ms.has_session ? g_theme.fg_primary : g_theme.fg_muted,
              (float)ix, (float)(y + 3), (float)iw, 16.0f,
              GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);

    /* Prev / Play-Pause / Next buttons */
    int bw = iw / 3;
    int by = y + 20;
    int bh = 18;
    wchar_t glyphs[3] = { 0xE892, (ms.has_session && ms.is_playing) ? 0xE769 : 0xE768, 0xE893 };
    for (int b = 0; b < 3; b++) {
        int bx = ix + b * bw;
        g_media_btn_rc[b].left   = bx;
        g_media_btn_rc[b].top    = by;
        g_media_btn_rc[b].right  = bx + bw;
        g_media_btn_rc[b].bottom = by + bh;
        if (g_hover_media_btn == b + 1) {
            draw_round_rect_fill(g, (float)bx, (float)by, (float)bw, (float)bh, 4.0f, g_theme.accent_overlay_hover);
        }
        draw_icon_glyph(g, glyphs[b], 11.0f, ms.has_session ? g_theme.fg_primary : g_theme.fg_muted,
                        (float)bx, (float)by, (float)bw, (float)bh);
    }
}

static void dock_present(void) {
    if (!g_hDockWnd || !g_dock_surf.hdc) return;
    float v = g_dock_vis;
    int off = (int)((1.0f - v) * (float)(g_dock_h - 20) + 0.5f);
    int a = (int)(v * 255.0f + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    surface_present(g_hDockWnd, &g_dock_surf, g_dock_x, g_dock_y + off, (BYTE)a);
}

void dock_repaint(void) {
    if (!g_hDockWnd || !g_cfg.dock_enabled) return;
    rebuild_dock_items();
    layout_dock_geometry();
    /* A dock that is fully off screen keeps its item list and position current but draws nothing. */
    if (g_dock_hidden && g_dock_vis <= 0.0f) return;
    if (!surface_ensure(&g_dock_surf, g_dock_w, g_dock_h)) return;

    GpGraphics *g = g_dock_surf.g;
    GdipGraphicsClear(g, 0);

    int pill_x = 8;
    int pill_y = 26;
    int pill_w = g_dock_w - 16;
    int pill_h = g_cfg.dock_item_size + g_cfg.dock_padding * 2;

    /* A soft shadow under the dock's rounded body */
    draw_round_rect_fill(g, (float)(pill_x + 1), (float)(pill_y + 2), (float)pill_w, (float)pill_h,
                         16.0f, MAKE_ARGB(42, 0, 0, 0));
    draw_round_rect_fill(g, (float)pill_x, (float)pill_y, (float)pill_w, (float)pill_h,
                         16.0f, g_theme.dock_bg);
    draw_round_rect_stroke(g, (float)pill_x + 0.5f, (float)pill_y + 0.5f, (float)pill_w - 1.0f, (float)pill_h - 1.0f,
                           16.0f, g_theme.popup_border, 1.0f);

    float tile_r = (float)g_cfg.dock_item_size * 0.25f;
    int inner_pad = (int)((float)g_cfg.dock_item_size * 0.15f);

    for (int i = 0; i < g_item_count; i++) {
        const DockRenderItem *ri = &g_items[i];
        if (ri->kind == RENDER_SEP) {
            float sx = (float)(ri->rc.left + ri->rc.right) * 0.5f;
            draw_line(g, sx, (float)(ri->rc.top + 4), sx, (float)(ri->rc.bottom - 4), g_theme.separator, 1.0f);
            continue;
        }
        if (ri->kind == RENDER_MEDIA) {
            draw_media_tile(g, ri);
            continue;
        }
        if (ri->kind == RENDER_START) {
            int sx = ri->rc.left;
            int sy = ri->rc.top + ((g_pressed_idx == i) ? 2 : 0);
            int ssz = ri->rc.right - ri->rc.left;
            ARGB sbg = (g_hover_idx == i) ? g_theme.tile_bg_hover : g_theme.tile_bg;
            draw_round_rect_fill(g, (float)sx, (float)sy, (float)ssz, (float)ssz, tile_r, sbg);
            draw_winxp_logo(g, sx, sy, ssz);
            continue;
        }

        int x = ri->rc.left;
        int y = ri->rc.top + ((g_pressed_idx == i) ? 2 : 0);
        int sz = ri->rc.right - ri->rc.left;
        bool dragged = g_drag_active && ri->cfg_idx >= 0 && ri->cfg_idx == g_drag_cfg;
        if (dragged) y = ri->rc.top - 5;
        if (g_bounce_start && g_bounce_name[0] && wcscmp(ri->name, g_bounce_name) == 0) {
            /* Two hops, the second half as high, each a simple parabola */
            float t = (float)(GetTickCount() - g_bounce_start) / (float)DOCK_BOUNCE_MS;
            if (t >= 0.0f && t < 1.0f) {
                float ph = t * 2.0f;
                float amp = (ph < 1.0f) ? 9.0f : 4.5f;
                float f = (ph < 1.0f) ? ph : ph - 1.0f;
                y -= (int)(amp * 4.0f * f * (1.0f - f) + 0.5f);
            }
        }

        ARGB tbg = (g_hover_idx == i || dragged) ? g_theme.tile_bg_hover : g_theme.tile_bg;
        draw_round_rect_fill(g, (float)x, (float)y, (float)sz, (float)sz, tile_r, tbg);

        bool sq_from_cache = false;
        GpBitmap *ico = icon_cache_get(ri->umid, ri->exe_path, ri->icon_rel, &sq_from_cache);
        bool is_sq = ri->is_square || sq_from_cache;
        if (ico) {
            int p = is_sq ? 1 : inner_pad;
            GdipDrawImageRectI(g, (GpImage *)ico, x + p, y + p, sz - p * 2, sz - p * 2);
        } else {
            /* Clean fallback monogram if icon missing */
            wchar_t ch[2] = { ri->name[0] ? ri->name[0] : L'?', L'\0' };
            draw_text(g, ch, L"Segoe UI", 16.0f, GP_FONT_BOLD, g_theme.fg_primary,
                      (float)x, (float)y, (float)sz, (float)sz, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        }

        /* Under an open app: a dot, or a short bar for the one in front */
        if (ri->win_count > 0) {
            float dot_y = (float)(ri->rc.bottom + g_cfg.dock_padding / 2) - 1.5f;
            float cx = (float)(ri->rc.left + ri->rc.right) * 0.5f;
            if (ri->is_focused) {
                float pw = (float)sz * 0.50f;
                draw_round_rect_fill(g, cx - pw * 0.5f, dot_y, pw, 3.0f, 1.5f, g_theme.accent);
            } else {
                draw_circle_fill(g, cx, dot_y + 1.5f, 2.0f, g_theme.dot_open);
            }
        }

        /* How many windows the app has open, from two */
        if (g_cfg.dock_show_instance_counter && ri->win_count > 1) {
            float bx = (float)(ri->rc.right - 13);
            float by = (float)(ri->rc.bottom - 13);
            draw_circle_fill(g, bx + 7.5f, by + 7.5f, 7.5f, g_theme.badge_bg);
            wchar_t cnt[8];
            _snwprintf_s(cnt, 8, _TRUNCATE, L"%d", ri->win_count);
            draw_text(g, cnt, L"Segoe UI", 10.0f, GP_FONT_BOLD, g_theme.badge_fg,
                      bx, by, 15.0f, 15.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        }
    }

    /* Hover tooltip pill above hovered item */
    if (g_hover_idx >= 0 && g_hover_idx < g_item_count &&
        (g_items[g_hover_idx].kind == RENDER_APP || g_items[g_hover_idx].kind == RENDER_START)) {
        const DockRenderItem *hi = &g_items[g_hover_idx];
        float tw = measure_text_width(g, hi->name, L"Segoe UI", 11.5f, GP_FONT_BOLD);
        int bw = (int)tw + 16;
        int bx = (hi->rc.left + hi->rc.right - bw) / 2;
        if (bx < 4) bx = 4;
        if (bx + bw > g_dock_w - 4) bx = g_dock_w - 4 - bw;
        draw_round_rect_fill(g, (float)bx, 2.0f, (float)bw, 20.0f, 6.0f, g_theme.popup_bg);
        draw_round_rect_stroke(g, (float)bx + 0.5f, 2.5f, (float)bw - 1.0f, 19.0f, 6.0f, g_theme.popup_border, 1.0f);
        draw_text(g, hi->name, L"Segoe UI", 11.5f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)bx, 2.0f, (float)bw, 20.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
    }

    if (!g_dock_hidden || g_dock_vis > 0.0f) dock_present();
}

/* The 2-pixel strip at the bottom edge is the only way back to a hidden
 * dock, so it is up whenever the dock is fully hidden and nothing is full
 * screen (issue #163). Checked on every window change, not only when the
 * dock hides: a dock that hid while a full-screen window was in front (a
 * video, a game, Task View, the lock screen) would otherwise never come
 * back. Nothing is done when the strip is already as it should be. */
static void dock_sync_sensor(void) {
    if (!g_hSensorWnd) return;
    bool want = g_cfg.dock_enabled && g_dock_hidden && g_dock_vis <= 0.0f &&
                !win_tracker_is_fullscreen_active();
    bool up = IsWindowVisible(g_hSensorWnd) != FALSE;
    if (want == up) return;
    if (want) {
        ShowWindow(g_hSensorWnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hSensorWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(g_hSensorWnd, SW_HIDE);
    }
}

static void dock_anim_done(void) {
    KillTimer(g_hDockWnd, TIMER_DOCK_ANIM);
    g_dock_vis = g_dock_vis_to;
    if (g_dock_hidden) {
        ShowWindow(g_hDockWnd, SW_HIDE);
        dock_sync_sensor();
    } else {
        dock_present();
    }
}

static void dock_anim_to(float to) {
    g_dock_vis_from = g_dock_vis;
    g_dock_vis_to = to;
    if (!g_cfg.animations || g_dock_vis == to) {
        dock_anim_done();
        return;
    }
    g_dock_anim_start = GetTickCount();
    SetTimer(g_hDockWnd, TIMER_DOCK_ANIM, 12, NULL);
}

static void show_dock_now(void) {
    if (!g_hDockWnd) return;
    KillTimer(g_hDockWnd, TIMER_DOCK_HIDE);
    KillTimer(g_hDockWnd, TIMER_DOCK_SHOW);
    if (g_dock_hidden) {
        g_dock_hidden = false;
        if (g_hSensorWnd) ShowWindow(g_hSensorWnd, SW_HIDE);
        SetWindowPos(g_hDockWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        dock_repaint();
        dock_anim_to(1.0f);
    }
}

static void hide_dock_now(void) {
    if (!g_hDockWnd || g_menu_open || g_mouse_in_dock || g_drag_active) return;
    KillTimer(g_hDockWnd, TIMER_DOCK_HIDE);
    if (!g_dock_hidden) {
        g_dock_hidden = true;
        g_hover_idx = -1;
        g_hover_media_btn = 0;
        dock_anim_to(0.0f);
    }
}

/* A dock that is always shown reserves its strip at the bottom of the screen, so maximized
 * windows stop above it. The other modes reserve nothing. */
static void dock_update_reserve(void) {
    if (!g_hDockWnd) return;
    /* A test copy reserves nothing on the desktop it runs on. */
    bool want = g_cfg.dock_enabled && g_cfg.dock_hide_mode == HIDE_NEVER && !g_cfg.isolated;
    APPBARDATA abd;
    ZeroMemory(&abd, sizeof(abd));
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_hDockWnd;
    if (!want) {
        if (g_dock_appbar) {
            SHAppBarMessage(ABM_REMOVE, &abd);
            g_dock_appbar = false;
        }
        return;
    }
    abd.uCallbackMessage = WM_LARES_DOCK_APPBAR_CB;
    if (!g_dock_appbar) {
        if (!SHAppBarMessage(ABM_NEW, &abd)) return;
        g_dock_appbar = true;
    }
    MONITORINFO mi;
    shell_screen_rect(&mi.rcMonitor);
    int strip = g_cfg.dock_item_size + g_cfg.dock_padding * 2 + g_cfg.dock_margin * 2;
    abd.uEdge = ABE_BOTTOM;
    abd.rc.left   = mi.rcMonitor.left;
    abd.rc.right  = mi.rcMonitor.right;
    abd.rc.bottom = mi.rcMonitor.bottom;
    abd.rc.top    = mi.rcMonitor.bottom - strip;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.top = abd.rc.bottom - strip;
    SHAppBarMessage(ABM_SETPOS, &abd);
}

static void dock_set_hide_mode(DockHideMode mode) {
    if (mode == g_cfg.dock_hide_mode) return;
    g_cfg.dock_hide_mode = mode;
    config_save_behaviour();
    dock_update_reserve();
}

static void append_dock_mode_items(HMENU hMenu) {
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 30, L"Always show the dock");
    AppendMenuW(hMenu, MF_STRING, 31, L"Hide the dock automatically");
    AppendMenuW(hMenu, MF_STRING, 32, L"Hide the dock when a window covers it");
    CheckMenuRadioItem(hMenu, 30, 32,
                       g_cfg.dock_hide_mode == HIDE_NEVER ? 30U : (g_cfg.dock_hide_mode == HIDE_ALWAYS ? 31U : 32U),
                       MF_BYCOMMAND);
    AppendMenuW(hMenu, MF_STRING | (g_cfg.animations ? MF_CHECKED : 0), 33, L"Animations");
}

static void evaluate_autohide(void) {
    if (!g_hDockWnd || !g_cfg.dock_enabled) return;

    bool should_hide = false;
    if (win_tracker_is_fullscreen_active()) {
        should_hide = true;
    } else if (g_cfg.dock_hide_mode == HIDE_NEVER) {
        should_hide = false;
    } else if (g_cfg.dock_hide_mode == HIDE_ALWAYS) {
        should_hide = !g_mouse_in_dock && !g_menu_open;
    } else {
        /* HIDE_ON_OVERLAP */
        bool overlapped = win_tracker_is_rect_overlapped(&g_dock_pill_screen, g_hDockWnd, g_hTopBarWnd);
        should_hide = overlapped && !g_mouse_in_dock && !g_menu_open;
    }

    if (should_hide) {
        KillTimer(g_hDockWnd, TIMER_DOCK_SHOW);
        if (!g_dock_hidden) {
            UINT delay = (UINT)(g_cfg.dock_delay_hide_ms > 0 ? g_cfg.dock_delay_hide_ms : 50);
            SetTimer(g_hDockWnd, TIMER_DOCK_HIDE, delay, NULL);
        }
    } else {
        KillTimer(g_hDockWnd, TIMER_DOCK_HIDE);
        if (g_dock_hidden) {
            if (g_cfg.dock_delay_show_ms <= 0) show_dock_now();
            else SetTimer(g_hDockWnd, TIMER_DOCK_SHOW, (UINT)g_cfg.dock_delay_show_ms, NULL);
        }
    }
    dock_sync_sensor();
}

static void activate_render_item(const DockRenderItem *ri, bool force_new) {
    if (!ri || ri->kind != RENDER_APP) return;

    if (force_new || ri->win_count == 0) {
        if (g_cfg.animations && g_hDockWnd) {
            wcsncpy_s(g_bounce_name, 128, ri->name, _TRUNCATE);
            g_bounce_start = GetTickCount();
            SetTimer(g_hDockWnd, TIMER_DOCK_BOUNCE, 15, NULL);
        }
        shell_launch_app(ri->command, ri->args, ri->workdir, ri->umid);
        return;
    }

    HWND fg = GetForegroundWindow();
    if (ri->win_count == 1) {
        HWND target = ri->windows[0];
        if (target == fg && !IsIconic(target)) {
            ShowWindowAsync(target, SW_MINIMIZE);
        } else {
            if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
            SetForegroundWindow(target);
        }
    } else {
        /* Cycle through multiple windows of the same app */
        int cur = -1;
        for (int i = 0; i < ri->win_count; i++) {
            if (ri->windows[i] == fg) { cur = i; break; }
        }
        HWND target = ri->windows[(cur + 1) % ri->win_count];
        if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
        SetForegroundWindow(target);
    }
}

void dock_activate_index(int index) {
    if (index < 0) return;
    int app_idx = 0;
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].kind != RENDER_APP) continue;
        if (app_idx == index) {
            activate_render_item(&g_items[i], false);
            return;
        }
        app_idx++;
    }
}

void dock_reveal_temporarily(void) {
    show_dock_now();
    evaluate_autohide();
}

static void show_item_context_menu(HWND hwnd, int idx, int screen_x, int screen_y) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    if (idx >= 0 && idx < g_item_count && g_items[idx].kind == RENDER_START) {
        AppendMenuW(hMenu, MF_STRING, 10, g_cfg.files_enabled ? L"Files" : L"File Explorer");
        AppendMenuW(hMenu, MF_STRING, 21, L"Processes");
        AppendMenuW(hMenu, MF_STRING, 23, L"Terminal");
        AppendMenuW(hMenu, MF_STRING, 24, L"Windows Settings");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        append_dock_mode_items(hMenu);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, 11, L"Lares Settings");
    } else {
        if (idx >= 0 && idx < g_item_count && g_items[idx].kind == RENDER_APP) {
            const DockRenderItem *ri = &g_items[idx];
            AppendMenuW(hMenu, MF_STRING, 1, L"Open New Window");
            /* Only for real program files: Store apps cannot be started elevated this way */
            if (ri->command[0] && ri->command[0] != L'@' && PathFileExistsW(ri->command)) {
                AppendMenuW(hMenu, MF_STRING, 5, L"Run as Administrator");
            }
            if (ri->pinned) AppendMenuW(hMenu, MF_STRING, 2, L"Unpin from Dock");
            else            AppendMenuW(hMenu, MF_STRING, 3, L"Pin to Dock");
            if (ri->win_count > 0) {
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu, MF_STRING, 4, ri->win_count > 1 ? L"Close All Windows" : L"Close Window");
            }
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        }

        AppendMenuW(hMenu, MF_STRING, 10, g_cfg.files_enabled ? L"Files" : L"File Explorer");
        append_dock_mode_items(hMenu);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, 11, L"Lares Settings");
    }

    g_menu_open = true;
    SetForegroundWindow(hwnd);
    UINT cmd = (UINT)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_x, screen_y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
    g_menu_open = false;
    {
        POINT cp;
        if (GetCursorPos(&cp)) {
            g_mouse_in_dock = PtInRect(&g_dock_pill_screen, cp) != 0;
        }
    }

    if (cmd == 5 && idx >= 0 && idx < g_item_count) {
        /* Windows shows its own approval prompt; saying no there simply starts nothing */
        const DockRenderItem *ri = &g_items[idx];
        SHELLEXECUTEINFOW sei;
        ZeroMemory(&sei, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = ri->command;
        sei.lpParameters = ri->args[0] ? ri->args : NULL;
        sei.lpDirectory = ri->workdir[0] ? ri->workdir : NULL;
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    } else if (cmd == 1 && idx >= 0 && idx < g_item_count) {
        activate_render_item(&g_items[idx], true);
    } else if (cmd == 2 && idx >= 0 && idx < g_item_count && g_items[idx].cfg_idx >= 0) {
        int ci = g_items[idx].cfg_idx;
        if (ci >= 0 && ci < g_dock_cfg_count) {
            memmove(&g_dock_cfg[ci], &g_dock_cfg[ci + 1],
                    (size_t)(g_dock_cfg_count - ci - 1) * sizeof(DockItemConfig));
            g_dock_cfg_count--;
            config_save_dock();
            dock_repaint();
        }
    } else if (cmd == 3 && idx >= 0 && idx < g_item_count && g_dock_cfg_count < LARES_MAX_DOCK_ITEMS) {
        const DockRenderItem *ri = &g_items[idx];
        DockItemConfig *nc = &g_dock_cfg[g_dock_cfg_count++];
        ZeroMemory(nc, sizeof(*nc));
        nc->group = DOCK_GROUP_CENTER;
        nc->type = DOCK_ITEM_APP;
        wcsncpy_s(nc->name, 128, ri->name, _TRUNCATE);
        wcsncpy_s(nc->umid, 256, ri->umid, _TRUNCATE);
        wcsncpy_s(nc->path, MAX_PATH, ri->exe_path, _TRUNCATE);
        wcsncpy_s(nc->command, MAX_PATH, ri->command, _TRUNCATE);
        nc->pinned = true;
        config_save_dock();
        dock_repaint();
    } else if (cmd == 4 && idx >= 0 && idx < g_item_count) {
        for (int w = 0; w < g_items[idx].win_count; w++) {
            PostMessageW(g_items[idx].windows[w], WM_CLOSE, 0, 0);
        }
    } else if (cmd == 10) {
        action_open_files(NULL);
    } else if (cmd == 11) {
        action_open_lares(L"settings");
    } else if (cmd == 21) {
        action_open_processes();
    } else if (cmd == 23) {
        action_open_terminal();
    } else if (cmd == 24) {
        action_open_settings();
    } else if (cmd == 30) {
        dock_set_hide_mode(HIDE_NEVER);
    } else if (cmd == 31) {
        dock_set_hide_mode(HIDE_ALWAYS);
    } else if (cmd == 32) {
        dock_set_hide_mode(HIDE_ON_OVERLAP);
    } else if (cmd == 33) {
        g_cfg.animations = !g_cfg.animations;
        config_save_behaviour();
    }
    evaluate_autohide();
}

static int dock_hit_test(int x, int y, int *out_media_btn) {
    if (out_media_btn) *out_media_btn = 0;
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].kind == RENDER_SEP) continue;
        if (x >= g_items[i].rc.left && x <= g_items[i].rc.right &&
            y >= g_items[i].rc.top && y <= g_items[i].rc.bottom) {
            if (g_items[i].kind == RENDER_MEDIA && out_media_btn) {
                for (int b = 0; b < 3; b++) {
                    if (x >= g_media_btn_rc[b].left && x <= g_media_btn_rc[b].right &&
                        y >= g_media_btn_rc[b].top && y <= g_media_btn_rc[b].bottom) {
                        *out_media_btn = b + 1;
                        break;
                    }
                }
            }
            return i;
        }
    }
    return -1;
}

static LRESULT CALLBACK sensor_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSEMOVE) {
        if (!win_tracker_is_fullscreen_active()) show_dock_now();
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Moves a pinned app to the slot of another pinned app while it is being dragged. */
static void drag_reorder_at(int mx) {
    for (int i = 0; i < g_item_count; i++) {
        const DockRenderItem *ri = &g_items[i];
        if (ri->kind != RENDER_APP || ri->cfg_idx < 0 || ri->cfg_idx == g_drag_cfg) continue;
        if (mx < ri->rc.left || mx > ri->rc.right) continue;
        int from = g_drag_cfg, to = ri->cfg_idx;
        if (from < 0 || from >= g_dock_cfg_count || to < 0 || to >= g_dock_cfg_count) return;
        DockItemConfig moved = g_dock_cfg[from];
        moved.group = g_dock_cfg[to].group;
        if (from < to) {
            memmove(&g_dock_cfg[from], &g_dock_cfg[from + 1], (size_t)(to - from) * sizeof(DockItemConfig));
        } else {
            memmove(&g_dock_cfg[to + 1], &g_dock_cfg[to], (size_t)(from - to) * sizeof(DockItemConfig));
        }
        g_dock_cfg[to] = moved;
        g_drag_cfg = to;
        dock_repaint();
        return;
    }
}

static void drag_end(HWND hwnd, bool save) {
    bool was_active = g_drag_active;
    g_drag_armed = false;
    g_drag_active = false;
    g_drag_cfg = -1;
    if (GetCapture() == hwnd) ReleaseCapture();
    if (was_active) {
        if (save) config_save_dock();
        dock_repaint();
    }
}

static LRESULT CALLBACK dock_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LARES_DOCK_APPBAR_CB:
        if (wParam == ABN_POSCHANGED) dock_update_reserve();
        else if (wParam == ABN_FULLSCREENAPP) evaluate_autohide();
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_DOCK_HIDE) hide_dock_now();
        else if (wParam == TIMER_DOCK_SHOW) show_dock_now();
        else if (wParam == TIMER_DOCK_ANIM) {
            float t = (float)(GetTickCount() - g_dock_anim_start) / (float)DOCK_ANIM_MS;
            if (t >= 1.0f) {
                dock_anim_done();
            } else {
                g_dock_vis = g_dock_vis_from + (g_dock_vis_to - g_dock_vis_from) * anim_ease_out(t);
                dock_present();
            }
        } else if (wParam == TIMER_DOCK_BOUNCE) {
            if (GetTickCount() - g_bounce_start >= DOCK_BOUNCE_MS) {
                KillTimer(hwnd, TIMER_DOCK_BOUNCE);
                g_bounce_start = 0;
                g_bounce_name[0] = L'\0';
            }
            dock_repaint();
        }
        return 0;

    case WM_MOUSEMOVE: {
        g_mouse_in_dock = true;
        KillTimer(hwnd, TIMER_DOCK_HIDE);
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        if (g_drag_armed) {
            if (!g_drag_active && abs(mx - g_drag_press_x) > 6) {
                g_drag_active = true;
                g_pressed_idx = -1;
                g_hover_idx = -1;
                dock_repaint();
            }
            if (g_drag_active) {
                drag_reorder_at(mx);
                return 0;
            }
        }
        if (!g_tracking_dock) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking_dock = true;
        }
        int mb = 0;
        int idx = dock_hit_test(mx, my, &mb);
        if (idx != g_hover_idx || mb != g_hover_media_btn) {
            g_hover_idx = idx;
            g_hover_media_btn = mb;
            dock_repaint();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking_dock = false;
        g_mouse_in_dock = false;
        g_pressed_idx = -1;
        if (g_hover_idx != -1 || g_hover_media_btn != 0) {
            g_hover_idx = -1;
            g_hover_media_btn = 0;
            dock_repaint();
        }
        evaluate_autohide();
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        int mb = 0;
        int idx = dock_hit_test(mx, my, &mb);
        if (idx >= 0 && (g_items[idx].kind == RENDER_APP || g_items[idx].kind == RENDER_START)) {
            g_pressed_idx = idx;
            if (g_items[idx].kind == RENDER_APP && g_items[idx].cfg_idx >= 0) {
                /* A pinned app can be dragged sideways to a new place */
                g_drag_armed = true;
                g_drag_active = false;
                g_drag_cfg = g_items[idx].cfg_idx;
                g_drag_press_x = mx;
                SetCapture(hwnd);
            }
            dock_repaint();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        g_pressed_idx = -1;
        if (g_drag_active) {
            drag_end(hwnd, true);
            evaluate_autohide();
            return 0;
        }
        drag_end(hwnd, false);
        int mb = 0;
        int idx = dock_hit_test(mx, my, &mb);
        dock_repaint();
        if (idx >= 0) {
            if (g_items[idx].kind == RENDER_START) {
                action_open_start();
            } else if (g_items[idx].kind == RENDER_MEDIA) {
                if (mb == 1) media_prev();
                else if (mb == 2) media_play_pause();
                else if (mb == 3) media_next();
            } else if (g_items[idx].kind == RENDER_APP) {
                activate_render_item(&g_items[idx], false);
            }
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if ((HWND)lParam != hwnd && (g_drag_armed || g_drag_active)) {
            bool was_active = g_drag_active;
            g_drag_armed = false;
            g_drag_active = false;
            g_drag_cfg = -1;
            if (was_active) {
                config_save_dock();
                dock_repaint();
            }
            POINT cp;
            if (GetCursorPos(&cp)) g_mouse_in_dock = PtInRect(&g_dock_pill_screen, cp) != 0;
            evaluate_autohide();
        }
        return 0;

    case WM_MBUTTONUP: {
        if (!g_cfg.dock_middle_click_new) return 0;
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        int idx = dock_hit_test(mx, my, NULL);
        if (idx >= 0 && g_items[idx].kind == RENDER_APP) {
            activate_render_item(&g_items[idx], true);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        int idx = dock_hit_test(mx, my, NULL);
        POINT pt = { mx, my };
        ClientToScreen(hwnd, &pt);
        show_item_context_menu(hwnd, idx, pt.x, pt.y);
        return 0;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_DOCK_ANIM);
        KillTimer(hwnd, TIMER_DOCK_BOUNCE);
        if (g_dock_appbar) {
            APPBARDATA abd;
            ZeroMemory(&abd, sizeof(abd));
            abd.cbSize = sizeof(abd);
            abd.hWnd = hwnd;
            SHAppBarMessage(ABM_REMOVE, &abd);
            g_dock_appbar = false;
        }
        surface_destroy(&g_dock_surf);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool dock_create(HINSTANCE hInst) {
    if (!g_cfg.dock_enabled) return true;
    g_dock_hidden = false;
    g_dock_vis = g_dock_vis_from = g_dock_vis_to = 1.0f;
    g_dock_appbar = false;
    g_hover_idx = -1;
    g_pressed_idx = -1;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dock_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = DOCK_CLASS;
    RegisterClassExW(&wc);

    WNDCLASSEXW sc;
    ZeroMemory(&sc, sizeof(sc));
    sc.cbSize = sizeof(sc);
    sc.lpfnWndProc = sensor_wnd_proc;
    sc.hInstance = hInst;
    sc.lpszClassName = DOCK_SENSOR_CLASS;
    RegisterClassExW(&sc);

    /* A 2 px strip at the bottom edge brings a hidden dock back. It is nearly
     * invisible (alpha 1) and only receives mouse moves: no polling. */
    g_hSensorWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        DOCK_SENSOR_CLASS, L"",
        WS_POPUP,
        0, 0, 100, 2,
        NULL, NULL, hInst, NULL);
    if (g_hSensorWnd) {
        SetLayeredWindowAttributes(g_hSensorWnd, 0, 1, LWA_ALPHA);
    }

    g_hDockWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        DOCK_CLASS, L"Lares dock",
        WS_POPUP,
        0, 0, 1, 1,
        NULL, NULL, hInst, NULL);
    if (!g_hDockWnd) return false;

    ShowWindow(g_hDockWnd, SW_SHOWNOACTIVATE);
    dock_update_reserve();
    dock_repaint();
    evaluate_autohide();
    return true;
}

void dock_on_windows_changed(void) {
    dock_repaint();
    evaluate_autohide();
    /* Put the dock back above normal windows only if something really pushed it down. */
    if (g_hDockWnd && !g_dock_hidden && !win_tracker_is_fullscreen_active() &&
        (!IsWindowVisible(g_hDockWnd) || !(GetWindowLongPtrW(g_hDockWnd, GWL_EXSTYLE) & WS_EX_TOPMOST))) {
        SetWindowPos(g_hDockWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void dock_on_display_change(void) {
    dock_update_reserve();
    dock_repaint();
    evaluate_autohide();
}

void dock_reassert_topmost(void) {
    if (g_hDockWnd && !g_dock_hidden) {
        SetWindowPos(g_hDockWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (g_hSensorWnd && g_dock_hidden) {
        SetWindowPos(g_hSensorWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void dock_destroy(void) {
    if (g_hSensorWnd) {
        DestroyWindow(g_hSensorWnd);
        g_hSensorWnd = NULL;
    }
    if (g_hDockWnd) {
        DestroyWindow(g_hDockWnd);
        g_hDockWnd = NULL;
    }
}

/* Settings changed while the dock is up: its mode, animations, the media
 * controls, the window counts. */
void dock_apply_settings(void) {
    if (!g_hDockWnd) return;
    dock_update_reserve();
    dock_repaint();
    evaluate_autohide();
}

int dock_snapshot(const wchar_t *png_path) {
    return g_dock_surf.hdc && surface_save_png(&g_dock_surf, png_path);
}

/* Explorer restarted: its list of reserved screen strips is gone, so reserve again if needed. */
void dock_on_explorer_restart(void) {
    g_dock_appbar = false;
    dock_update_reserve();
    dock_reassert_topmost();
}
