// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The screenshot tool (issue #149), while its switch is on.
 * - A region (Alt+Shift+S), the window in front (Alt+Shift+W) or the screen
 *   (Alt+Shift+F); Win+Shift+S and Print Screen stay Windows' own.
 * - Every capture goes to the clipboard, and a small preview in the corner
 *   can save it as PNG in the user's Pictures\Screenshots folder, which
 *   Windows names.
 * - Its memory (VirtualAlloc) and windows go as soon as a capture is
 *   cancelled, saved or dismissed: nothing is kept at rest.
 */

#define SNIP_CLASS          L"LaresSnipWnd"
#define TOAST_CLASS         L"LaresToastWnd"
#define CTRL_CLASS          L"LaresShotCtrlWnd"

#define TIMER_DELAY_CAPTURE 601
#define TIMER_TOAST_HIDE    602

/* GDI+ built-in PNG encoder CLSID ({557CF406-1A04-11D3-9A73-0000F81EF32E}) */
static const CLSID CLSID_PNG_ENCODER =
    { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };

static HWND           g_hCtrlWnd = NULL;
static HWND           g_hSnipWnd = NULL;
static HWND           g_hToastWnd = NULL;

/* Delayed capture request */
static ScreenshotMode g_pending_mode = SCREENSHOT_REGION;
static HWND           g_pending_target_wnd = NULL;

/* Region snip frozen virtual-desktop buffer (freed immediately on mouse-up or cancel) */
static HBITMAP        g_freeze_hbmp = NULL;
static int            g_vscr_x = 0, g_vscr_y = 0, g_vscr_w = 0, g_vscr_h = 0;
static bool           g_snip_dragging = false;
static POINT          g_snip_p0 = {0, 0};
static POINT          g_snip_p1 = {0, 0};

/* Captured pixels held ONLY while preview toast is open before saving (VirtualAlloc) */
static BYTE          *g_cap_pixels = NULL;
static int            g_cap_w = 0;
static int            g_cap_h = 0;

/* Preview toast state (destroyed immediately on dismiss/timeout) */
static LayerSurface   g_toast_surf = {0};
static GpBitmap      *g_toast_thumb = NULL;
static bool           g_toast_saved = false;
static wchar_t        g_saved_path[MAX_PATH];
static wchar_t        g_saved_name[MAX_PATH];
static int            g_toast_x = 0, g_toast_y = 0, g_toast_w = 364, g_toast_h = 136;
static int            g_toast_hover = 0; /* 0=none, 1=primary btn, 2=secondary btn, 3=close x */
static RECT           g_rc_tbtn1 = {0};
static RECT           g_rc_tbtn2 = {0};
static RECT           g_rc_tclose = {0};
static RECT           g_rc_tthumb = {0};

static bool is_our_shell_window(HWND hwnd) {
    if (!hwnd) return true;
    if (hwnd == g_hTopBarWnd || hwnd == g_hDockWnd || hwnd == g_hHostWnd ||
        hwnd == g_hSnipWnd || hwnd == g_hToastWnd || hwnd == g_hCtrlWnd) {
        return true;
    }
    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"LaresPopup", 10) == 0 ||
        wcsncmp(cls, L"LaresLauncher", 13) == 0 ||
        wcsncmp(cls, L"LaresSnip", 9) == 0 ||
        wcsncmp(cls, L"LaresToast", 10) == 0) {
        return true;
    }
    return false;
}

static void free_freeze_buffer(void) {
    if (g_freeze_hbmp) {
        DeleteObject(g_freeze_hbmp);
        g_freeze_hbmp = NULL;
    }
}

static void free_capture_buffer(void) {
    if (g_cap_pixels) {
        VirtualFree(g_cap_pixels, 0, MEM_RELEASE);
        g_cap_pixels = NULL;
    }
    g_cap_w = 0;
    g_cap_h = 0;
}

static void free_toast_resources(void) {
    if (g_hToastWnd) {
        KillTimer(g_hToastWnd, TIMER_TOAST_HIDE);
        ShowWindow(g_hToastWnd, SW_HIDE);
    }
    if (g_toast_thumb) {
        GdipDisposeImage((GpImage *)g_toast_thumb);
        g_toast_thumb = NULL;
    }
    surface_destroy(&g_toast_surf);
    free_capture_buffer();
    g_toast_saved = false;
    g_toast_hover = 0;
    g_saved_path[0] = L'\0';
    g_saved_name[0] = L'\0';
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
}

static void close_snip_overlay(void) {
    if (g_hSnipWnd && IsWindow(g_hSnipWnd)) {
        DestroyWindow(g_hSnipWnd);
        g_hSnipWnd = NULL;
    }
    g_snip_dragging = false;
    free_freeze_buffer();
}

/* Copy top-down 32bpp BGRA capture buffer to clipboard as bottom-up CF_DIB */
static bool copy_capture_to_clipboard(const BYTE *bgra_topdown, int w, int h) {
    if (!bgra_topdown || w <= 0 || h <= 0) return false;
    SIZE_T img_sz = (SIZE_T)w * (SIZE_T)h * 4;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + img_sz);
    if (!hMem) return false;

    BYTE *dst = (BYTE *)GlobalLock(hMem);
    if (!dst) { GlobalFree(hMem); return false; }

    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)dst;
    ZeroMemory(bih, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = w;
    bih->biHeight = h; /* positive height = standard bottom-up DIB */
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)img_sz;

    BYTE *bits = dst + sizeof(BITMAPINFOHEADER);
    size_t stride = (size_t)w * 4;
    for (int y = 0; y < h; y++) {
        const BYTE *srow = bgra_topdown + (size_t)(h - 1 - y) * stride;
        BYTE *drow = bits + (size_t)y * stride;
        memcpy(drow, srow, stride);
    }
    GlobalUnlock(hMem);

    HWND owner = g_hCtrlWnd ? g_hCtrlWnd : g_hHostWnd;
    if (OpenClipboard(owner)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, hMem)) {
            GlobalFree(hMem);
            CloseClipboard();
            return false;
        }
        CloseClipboard();
        return true;
    }
    GlobalFree(hMem);
    return false;
}

/* Capture an arbitrary virtual-screen rectangle into VirtualAlloc top-down 32bpp BGRA (alpha=0xFF) */
static BYTE *capture_screen_rect(int sx, int sy, int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    HDC hdc_scr = GetDC(NULL);
    if (!hdc_scr) return NULL;

    HDC hdc_mem = CreateCompatibleDC(hdc_scr);
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *dib_bits = NULL;
    HBITMAP hbmp = CreateDIBSection(hdc_scr, &bmi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
    BYTE *out = NULL;

    if (hbmp && dib_bits) {
        HBITMAP old = (HBITMAP)SelectObject(hdc_mem, hbmp);
        BitBlt(hdc_mem, 0, 0, w, h, hdc_scr, sx, sy, SRCCOPY);
        SelectObject(hdc_mem, old);

        SIZE_T bytes = (SIZE_T)w * (SIZE_T)h * 4;
        out = (BYTE *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (out) {
            const uint32_t *src = (const uint32_t *)dib_bits;
            uint32_t *dst = (uint32_t *)out;
            size_t count = (size_t)w * (size_t)h;
            for (size_t i = 0; i < count; i++) {
                dst[i] = src[i] | 0xFF000000u;
            }
        }
    }

    if (hbmp) DeleteObject(hbmp);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_scr);
    return out;
}

/* Crop a sub-rectangle out of g_freeze_hbmp into VirtualAlloc top-down 32bpp BGRA */
static BYTE *crop_from_freeze(int rx, int ry, int w, int h) {
    if (!g_freeze_hbmp || w <= 0 || h <= 0) return NULL;
    HDC hdc_scr = GetDC(NULL);
    if (!hdc_scr) return NULL;

    HDC hdc_src = CreateCompatibleDC(hdc_scr);
    HDC hdc_dst = CreateCompatibleDC(hdc_scr);
    HBITMAP old_src = (HBITMAP)SelectObject(hdc_src, g_freeze_hbmp);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *dib_bits = NULL;
    HBITMAP hdst = CreateDIBSection(hdc_scr, &bmi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
    BYTE *out = NULL;
    if (hdst && dib_bits) {
        HBITMAP old_dst = (HBITMAP)SelectObject(hdc_dst, hdst);
        BitBlt(hdc_dst, 0, 0, w, h, hdc_src, rx, ry, SRCCOPY);
        SelectObject(hdc_dst, old_dst);

        SIZE_T bytes = (SIZE_T)w * (SIZE_T)h * 4;
        out = (BYTE *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (out) {
            const uint32_t *src = (const uint32_t *)dib_bits;
            uint32_t *dst = (uint32_t *)out;
            size_t count = (size_t)w * (size_t)h;
            for (size_t i = 0; i < count; i++) {
                dst[i] = src[i] | 0xFF000000u;
            }
        }
    }

    if (hdst) DeleteObject(hdst);
    SelectObject(hdc_src, old_src);
    DeleteDC(hdc_dst);
    DeleteDC(hdc_src);
    ReleaseDC(NULL, hdc_scr);
    return out;
}

static GpBitmap *build_toast_thumbnail(const BYTE *bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return NULL;
    GpBitmap *src = NULL;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, GP_PIXELFORMAT_32BPP_ARGB, (BYTE *)bgra, &src) != GP_OK || !src) {
        return NULL;
    }

    int tw = 160, th = 120;
    if (w >= h) {
        th = max(1, (h * 160) / w);
    } else {
        tw = max(1, (w * 120) / h);
    }
    if (tw > w && th > h) { tw = w; th = h; }

    GpBitmap *thumb = NULL;
    if (GdipCreateBitmapFromScan0(tw, th, 0, GP_PIXELFORMAT_32BPP_ARGB, NULL, &thumb) == GP_OK && thumb) {
        GpGraphics *g = NULL;
        if (GdipGetImageGraphicsContext((GpImage *)thumb, &g) == GP_OK && g) {
            GdipSetInterpolationMode(g, GP_INTERP_HIGHQUALITY_BICUBIC);
            GdipDrawImageRectI(g, (GpImage *)src, 0, 0, tw, th);
            GdipDeleteGraphics(g);
        }
    }
    GdipDisposeImage((GpImage *)src);
    return thumb;
}

static void toast_paint(void) {
    if (!g_hToastWnd) return;
    const int pw = 344, ph = 116, pad = 10;
    g_toast_w = pw + pad * 2;
    g_toast_h = ph + pad * 2;

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_toast_x = wa.right - g_toast_w - 8;
    g_toast_y = wa.bottom - g_toast_h - 8;

    if (!surface_ensure(&g_toast_surf, g_toast_w, g_toast_h)) return;
    GpGraphics *g = g_toast_surf.g;
    GdipGraphicsClear(g, 0);

    int ox = pad, oy = pad;
    draw_round_rect_fill(g, (float)(ox + 2), (float)(oy + 4), (float)pw, (float)ph, 12.0f, MAKE_ARGB(64, 0, 0, 0));
    draw_round_rect_fill(g, (float)ox, (float)oy, (float)pw, (float)ph, 12.0f, g_theme.popup_bg);
    draw_round_rect_stroke(g, (float)ox + 0.5f, (float)oy + 0.5f, (float)pw - 1.0f, (float)ph - 1.0f,
                           12.0f, g_theme.popup_border, 1.0f);

    /* Thumbnail card on left */
    g_rc_tthumb.left = ox + 12;
    g_rc_tthumb.top = oy + 14;
    g_rc_tthumb.right = ox + 114;
    g_rc_tthumb.bottom = oy + ph - 14;
    int tcw = g_rc_tthumb.right - g_rc_tthumb.left;
    int tch = g_rc_tthumb.bottom - g_rc_tthumb.top;
    draw_round_rect_fill(g, (float)g_rc_tthumb.left, (float)g_rc_tthumb.top, (float)tcw, (float)tch,
                         8.0f, g_theme.popup_surface);
    if (g_toast_thumb) {
        UINT iw = 0, ih = 0;
        GdipGetImageWidth((GpImage *)g_toast_thumb, &iw);
        GdipGetImageHeight((GpImage *)g_toast_thumb, &ih);
        if (iw > 0 && ih > 0) {
            float sx = (float)(tcw - 8) / (float)iw;
            float sy = (float)(tch - 8) / (float)ih;
            float s = (sx < sy) ? sx : sy;
            int dw = max(1, (int)((float)iw * s));
            int dh = max(1, (int)((float)ih * s));
            GdipDrawImageRectI(g, (GpImage *)g_toast_thumb,
                               g_rc_tthumb.left + (tcw - dw) / 2,
                               g_rc_tthumb.top + (tch - dh) / 2, dw, dh);
        }
    }
    draw_round_rect_stroke(g, (float)g_rc_tthumb.left + 0.5f, (float)g_rc_tthumb.top + 0.5f,
                           (float)tcw - 1.0f, (float)tch - 1.0f, 8.0f, g_theme.popup_border, 1.0f);

    /* Close button top-right */
    g_rc_tclose.left = ox + pw - 32;
    g_rc_tclose.top = oy + 10;
    g_rc_tclose.right = ox + pw - 10;
    g_rc_tclose.bottom = oy + 32;
    if (g_toast_hover == 3) {
        draw_round_rect_fill(g, (float)g_rc_tclose.left, (float)g_rc_tclose.top, 22.0f, 22.0f,
                             6.0f, g_theme.accent_overlay_hover);
    }
    draw_icon_glyph(g, 0xE711, 10.0f, g_theme.fg_muted,
                    (float)g_rc_tclose.left, (float)g_rc_tclose.top, 22.0f, 22.0f);

    /* Title & Subtitle */
    int tx = ox + 126;
    int tw = pw - 160;
    if (g_toast_saved) {
        draw_text(g, L"Saved to Pictures\\Screenshots", L"Segoe UI", 12.5f, GP_FONT_BOLD, g_theme.success,
                  (float)tx, (float)(oy + 16), (float)tw, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        draw_text(g, g_saved_name, L"Segoe UI", 11.0f, GP_FONT_REGULAR, g_theme.fg_muted,
                  (float)tx, (float)(oy + 38), (float)(pw - 140), 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
    } else {
        draw_text(g, L"Screenshot copied", L"Segoe UI", 13.0f, GP_FONT_BOLD, g_theme.fg_primary,
                  (float)tx, (float)(oy + 16), (float)tw, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        wchar_t sub[64];
        _snwprintf_s(sub, 64, _TRUNCATE, L"%d x %d px  |  Ready to paste", g_cap_w, g_cap_h);
        draw_text(g, sub, L"Segoe UI", 11.5f, GP_FONT_REGULAR, g_theme.fg_muted,
                  (float)tx, (float)(oy + 38), (float)(pw - 140), 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
    }

    /* Action buttons */
    int by = oy + ph - 40;
    g_rc_tbtn1.left = tx;
    g_rc_tbtn1.top = by;
    g_rc_tbtn1.right = tx + 112;
    g_rc_tbtn1.bottom = by + 28;

    g_rc_tbtn2.left = tx + 120;
    g_rc_tbtn2.top = by;
    g_rc_tbtn2.right = ox + pw - 14;
    g_rc_tbtn2.bottom = by + 28;

    ARGB b1_bg = (g_toast_hover == 1) ? g_theme.accent_hover : g_theme.accent;
    draw_round_rect_fill(g, (float)g_rc_tbtn1.left, (float)g_rc_tbtn1.top,
                         (float)(g_rc_tbtn1.right - g_rc_tbtn1.left), 28.0f, 7.0f, b1_bg);
    draw_text(g, g_toast_saved ? L"Open Image" : L"Save PNG", L"Segoe UI", 11.5f, GP_FONT_BOLD,
              g_theme.fg_on_accent, (float)g_rc_tbtn1.left, (float)g_rc_tbtn1.top,
              (float)(g_rc_tbtn1.right - g_rc_tbtn1.left), 28.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

    ARGB b2_bg = (g_toast_hover == 2) ? g_theme.accent_overlay_hover : g_theme.popup_surface;
    draw_round_rect_fill(g, (float)g_rc_tbtn2.left, (float)g_rc_tbtn2.top,
                         (float)(g_rc_tbtn2.right - g_rc_tbtn2.left), 28.0f, 7.0f, b2_bg);
    draw_round_rect_stroke(g, (float)g_rc_tbtn2.left + 0.5f, (float)g_rc_tbtn2.top + 0.5f,
                           (float)(g_rc_tbtn2.right - g_rc_tbtn2.left) - 1.0f, 27.0f, 7.0f, g_theme.popup_border, 1.0f);
    draw_text(g, L"Dismiss", L"Segoe UI", 11.5f, GP_FONT_REGULAR, g_theme.fg_primary,
              (float)g_rc_tbtn2.left, (float)g_rc_tbtn2.top,
              (float)(g_rc_tbtn2.right - g_rc_tbtn2.left), 28.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

    surface_present(g_hToastWnd, &g_toast_surf, g_toast_x, g_toast_y, 255);
}

static bool toast_save_png(void) {
    if (!g_cap_pixels || g_cap_w <= 0 || g_cap_h <= 0) return false;

    /* The user's Pictures folder as Windows names it, wherever it lives. */
    wchar_t pic_dir[MAX_PATH] = {0};
    if (SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, pic_dir) != S_OK) return false;

    wchar_t ss_dir[MAX_PATH];
    _snwprintf_s(ss_dir, MAX_PATH, _TRUNCATE, L"%ls\\Screenshots", pic_dir);
    SHCreateDirectoryExW(NULL, ss_dir, NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t base[96];
    _snwprintf_s(base, 96, _TRUNCATE, L"Screenshot_%04u-%02u-%02u_%02u-%02u-%02u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    wchar_t out_path[MAX_PATH] = {0};
    wchar_t out_name[MAX_PATH] = {0};
    for (int n = 0; n < 99; n++) {
        if (n == 0) _snwprintf_s(out_name, MAX_PATH, _TRUNCATE, L"%ls.png", base);
        else        _snwprintf_s(out_name, MAX_PATH, _TRUNCATE, L"%ls_%d.png", base, n + 1);
        _snwprintf_s(out_path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", ss_dir, out_name);
        if (GetFileAttributesW(out_path) == INVALID_FILE_ATTRIBUTES) break;
    }

    GpBitmap *bmp = NULL;
    bool saved = false;
    if (GdipCreateBitmapFromScan0(g_cap_w, g_cap_h, g_cap_w * 4, GP_PIXELFORMAT_32BPP_ARGB, g_cap_pixels, &bmp) == GP_OK && bmp) {
        if (GdipSaveImageToFile((GpImage *)bmp, out_path, &CLSID_PNG_ENCODER, NULL) == GP_OK) {
            saved = true;
        }
        GdipDisposeImage((GpImage *)bmp);
    }

    if (saved) {
        /* Free full-resolution capture buffer immediately after PNG write! */
        free_capture_buffer();
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
        g_toast_saved = true;
        wcsncpy_s(g_saved_path, MAX_PATH, out_path, _TRUNCATE);
        wcsncpy_s(g_saved_name, MAX_PATH, out_name, _TRUNCATE);
        if (g_hToastWnd && IsWindowVisible(g_hToastWnd)) {
            toast_paint();
            SetTimer(g_hToastWnd, TIMER_TOAST_HIDE, 5000, NULL);
        }
    }
    return saved;
}

static void finish_capture(BYTE *pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return;

    /* Release any previous toast/capture buffers first */
    free_toast_resources();

    copy_capture_to_clipboard(pixels, w, h);

    g_cap_pixels = pixels;
    g_cap_w = w;
    g_cap_h = h;
    g_toast_thumb = build_toast_thumbnail(pixels, w, h);
    g_toast_saved = false;
    g_toast_hover = 0;

    if (g_hToastWnd) {
        toast_paint();
        SetWindowPos(g_hToastWnd, HWND_TOPMOST, g_toast_x, g_toast_y, g_toast_w, g_toast_h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(g_hToastWnd, TIMER_TOAST_HIDE, 8000, NULL);
    }
}

/* The preview drawn into a picture, for checks: with a made-up capture,
 * never the PC's own screen, and without touching the clipboard. */
int screenshot_snapshot(const wchar_t *png_path) {
    if (!g_hToastWnd) return 0;
    free_toast_resources();
    const int w = 640, h = 400;
    BYTE *px = (BYTE *)VirtualAlloc(NULL, (size_t)w * h * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!px) return 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            BYTE *p = px + ((size_t)y * w + x) * 4;
            p[0] = (BYTE)(120 + x * 100 / w);
            p[1] = (BYTE)(80 + y * 120 / h);
            p[2] = (BYTE)(60 + (x + y) * 60 / (w + h));
            p[3] = 255;
        }
    g_cap_pixels = px;
    g_cap_w = w;
    g_cap_h = h;
    g_toast_thumb = build_toast_thumbnail(px, w, h);
    g_toast_saved = false;
    g_toast_hover = 0;
    toast_paint();
    int ok = surface_save_png(&g_toast_surf, png_path);
    free_toast_resources();
    return ok;
}

/* ==================== SNIP OVERLAY WINDOW ==================== */
static void snip_get_sel_rect(RECT *out) {
    out->left   = min(g_snip_p0.x, g_snip_p1.x);
    out->top    = min(g_snip_p0.y, g_snip_p1.y);
    out->right  = max(g_snip_p0.x, g_snip_p1.x);
    out->bottom = max(g_snip_p0.y, g_snip_p1.y);
}

static void snip_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    int w = g_vscr_w, h = g_vscr_h;

    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    if (opaque_frame_begin(&fb, w, h)) {
        /* Blit frozen screen base */
        if (g_freeze_hbmp) {
            HDC fdc = CreateCompatibleDC(hdc);
            HBITMAP fold = (HBITMAP)SelectObject(fdc, g_freeze_hbmp);
            BitBlt(fb.hdc, 0, 0, w, h, fdc, 0, 0, SRCCOPY);
            SelectObject(fdc, fold);
            DeleteDC(fdc);
        }

        GpGraphics *g = fb.g;
        GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
        GdipSetTextRenderingHint(g, GP_TEXT_ANTIALIAS);

        ARGB veil = MAKE_ARGB(132, 8, 10, 14);
        if (g_snip_dragging) {
            RECT sr;
            snip_get_sel_rect(&sr);
            int sw = sr.right - sr.left;
            int sh = sr.bottom - sr.top;

            /* 4 dark veil bands around the bright un-dimmed selection cutout */
            if (sr.top > 0) draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, (float)sr.top, 0.0f, veil);
            if (sr.bottom < h) draw_round_rect_fill(g, 0.0f, (float)sr.bottom, (float)w, (float)(h - sr.bottom), 0.0f, veil);
            if (sr.left > 0 && sh > 0) draw_round_rect_fill(g, 0.0f, (float)sr.top, (float)sr.left, (float)sh, 0.0f, veil);
            if (sr.right < w && sh > 0) draw_round_rect_fill(g, (float)sr.right, (float)sr.top, (float)(w - sr.right), (float)sh, 0.0f, veil);

            if (sw > 1 && sh > 1) {
                draw_round_rect_stroke(g, (float)sr.left - 0.5f, (float)sr.top - 0.5f,
                                       (float)sw + 1.0f, (float)sh + 1.0f, 0.0f, g_theme.accent, 2.0f);

                /* Live dimension badge */
                wchar_t dim[48];
                _snwprintf_s(dim, 48, _TRUNCATE, L"%d x %d", sw, sh);
                float bw = 88.0f, bh = 24.0f;
                float bx = (float)sr.left;
                float by = (float)sr.top - bh - 6.0f;
                if (by < 8.0f) by = (float)sr.bottom + 6.0f;
                if (bx + bw > (float)w - 8.0f) bx = (float)w - bw - 8.0f;
                draw_round_rect_fill(g, bx, by, bw, bh, 6.0f, MAKE_ARGB(220, 18, 20, 24));
                draw_text(g, dim, L"Segoe UI", 11.5f, GP_FONT_BOLD, MAKE_ARGB(255, 255, 255, 255),
                          bx, by, bw, bh, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
            }
        } else {
            draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, (float)h, 0.0f, veil);
        }

        /* Top instruction banner on primary monitor */
        HMONITOR mon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        int px = (mi.rcMonitor.left - g_vscr_x) + ((mi.rcMonitor.right - mi.rcMonitor.left) - 340) / 2;
        int py = (mi.rcMonitor.top - g_vscr_y) + 22;
        draw_round_rect_fill(g, (float)px, (float)py, 340.0f, 34.0f, 17.0f, MAKE_ARGB(225, 22, 24, 28));
        draw_round_rect_stroke(g, (float)px + 0.5f, (float)py + 0.5f, 339.0f, 33.0f, 17.0f, MAKE_ARGB(80, 255, 255, 255), 1.0f);
        draw_text(g, L"Drag to capture region   |   Esc or Right-click to cancel",
                  L"Segoe UI", 12.0f, GP_FONT_BOLD, MAKE_ARGB(255, 248, 248, 250),
                  (float)px, (float)py, 340.0f, 34.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

        opaque_frame_end(hdc, &fb);
    }
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK snip_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        snip_paint(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            close_snip_overlay();
            SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
            return 0;
        }
        break;
    case WM_RBUTTONUP:
        close_snip_overlay();
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
        return 0;
    case WM_LBUTTONDOWN:
        g_snip_dragging = true;
        g_snip_p0.x = (int)(short)LOWORD(lParam);
        g_snip_p0.y = (int)(short)HIWORD(lParam);
        g_snip_p1 = g_snip_p0;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (g_snip_dragging) {
            g_snip_p1.x = (int)(short)LOWORD(lParam);
            g_snip_p1.y = (int)(short)HIWORD(lParam);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_snip_dragging) {
            g_snip_dragging = false;
            ReleaseCapture();
            g_snip_p1.x = (int)(short)LOWORD(lParam);
            g_snip_p1.y = (int)(short)HIWORD(lParam);
            RECT sr;
            snip_get_sel_rect(&sr);
            int sw = sr.right - sr.left;
            int sh = sr.bottom - sr.top;
            BYTE *cropped = NULL;
            if (sw >= 4 && sh >= 4) {
                cropped = crop_from_freeze(sr.left, sr.top, sw, sh);
            }
            /* Destroy overlay and free full-screen freeze buffer IMMEDIATELY */
            close_snip_overlay();
            if (cropped) {
                finish_capture(cropped, sw, sh);
            } else {
                SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
            }
        }
        return 0;
    case WM_DESTROY:
        g_hSnipWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ==================== PREVIEW TOAST WINDOW ==================== */
static int toast_hit_test(POINT pt) {
    if (PtInRect(&g_rc_tclose, pt)) return 3;
    if (PtInRect(&g_rc_tbtn1, pt))  return 1;
    if (PtInRect(&g_rc_tbtn2, pt))  return 2;
    return 0;
}

static LRESULT CALLBACK toast_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == TIMER_TOAST_HIDE) {
            free_toast_resources();
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        int nh = toast_hit_test(pt);
        if (nh != g_toast_hover) {
            g_toast_hover = nh;
            toast_paint();
        }
        /* Keep toast alive briefly while user hovers it */
        SetTimer(hwnd, TIMER_TOAST_HIDE, 6000, NULL);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        int hit = toast_hit_test(pt);
        if (hit == 1) {
            if (g_toast_saved && g_saved_path[0]) {
                wchar_t open_p[MAX_PATH];
                wcsncpy_s(open_p, MAX_PATH, g_saved_path, _TRUNCATE);
                free_toast_resources();
                if (!action_open_viewer(0, open_p)) ShellExecuteW(NULL, L"open", open_p, NULL, NULL, SW_SHOWNORMAL);
            } else {
                toast_save_png();
            }
        } else if (hit == 2 || hit == 3) {
            free_toast_resources();
        } else if (g_toast_saved && g_saved_path[0] && PtInRect(&g_rc_tthumb, pt)) {
            wchar_t open_p[MAX_PATH];
            wcsncpy_s(open_p, MAX_PATH, g_saved_path, _TRUNCATE);
            free_toast_resources();
            if (!action_open_viewer(0, open_p)) ShellExecuteW(NULL, L"open", open_p, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_USER + 1:
        /* Saves the shown capture, for checks that drive the tool */
        return toast_save_png() ? 1 : 0;
    case WM_CLOSE:
        free_toast_resources();
        return 0;
    case WM_DESTROY:
        surface_destroy(&g_toast_surf);
        g_hToastWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ==================== CAPTURE EXECUTION ==================== */
static void begin_region_snip(void) {
    close_snip_overlay();
    free_toast_resources();

    g_vscr_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vscr_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vscr_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vscr_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_vscr_w <= 0 || g_vscr_h <= 0) return;

    HDC hdc_scr = GetDC(NULL);
    if (!hdc_scr) return;
    HDC hdc_mem = CreateCompatibleDC(hdc_scr);
    g_freeze_hbmp = CreateCompatibleBitmap(hdc_scr, g_vscr_w, g_vscr_h);
    if (g_freeze_hbmp) {
        HBITMAP old = (HBITMAP)SelectObject(hdc_mem, g_freeze_hbmp);
        BitBlt(hdc_mem, 0, 0, g_vscr_w, g_vscr_h, hdc_scr, g_vscr_x, g_vscr_y, SRCCOPY);
        SelectObject(hdc_mem, old);
    }
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_scr);

    if (!g_freeze_hbmp) return;

    g_snip_dragging = false;
    g_hSnipWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        SNIP_CLASS, L"LaresSnip",
        WS_POPUP | WS_VISIBLE,
        g_vscr_x, g_vscr_y, g_vscr_w, g_vscr_h,
        NULL, NULL, g_hInst, NULL
    );
    if (g_hSnipWnd) {
        SetWindowPos(g_hSnipWnd, HWND_TOPMOST, g_vscr_x, g_vscr_y, g_vscr_w, g_vscr_h, SWP_SHOWWINDOW);
        SetForegroundWindow(g_hSnipWnd);
        SetFocus(g_hSnipWnd);
    } else {
        free_freeze_buffer();
    }
}

static void execute_window_capture(void) {
    free_toast_resources();

    HWND target = g_pending_target_wnd;
    if (!target || !IsWindow(target) || !IsWindowVisible(target) || is_our_shell_window(target)) {
        HWND fg = GetForegroundWindow();
        if (fg && !is_our_shell_window(fg)) target = fg;
    }

    RECT rc = {0};
    bool have_rc = false;
    if (target && IsWindow(target) && IsWindowVisible(target) && !IsIconic(target)) {
        if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
            have_rc = true;
        } else if (GetWindowRect(target, &rc)) {
            have_rc = true;
        }
    }

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (!have_rc) {
        /* Graceful fallback to primary monitor if no foreground window exists */
        HMONITOR mon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        rc = mi.rcMonitor;
    } else {
        if (rc.left < vx) rc.left = vx;
        if (rc.top < vy) rc.top = vy;
        if (rc.right > vx + vw) rc.right = vx + vw;
        if (rc.bottom > vy + vh) rc.bottom = vy + vh;
    }

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 4 || h < 4) return;

    BYTE *pix = capture_screen_rect(rc.left, rc.top, w, h);
    if (pix) finish_capture(pix, w, h);
}

static void execute_fullscreen_capture(void) {
    free_toast_resources();
    HMONITOR mon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (w < 4 || h < 4) return;

    BYTE *pix = capture_screen_rect(mi.rcMonitor.left, mi.rcMonitor.top, w, h);
    if (pix) finish_capture(pix, w, h);
}

static LRESULT CALLBACK ctrl_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == TIMER_DELAY_CAPTURE) {
        KillTimer(hwnd, TIMER_DELAY_CAPTURE);
        if (g_pending_mode == SCREENSHOT_REGION)      begin_region_snip();
        else if (g_pending_mode == SCREENSHOT_WINDOW) execute_window_capture();
        else                                          execute_fullscreen_capture();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void screenshot_init(HINSTANCE hInst) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = snip_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
    wc.lpszClassName = SNIP_CLASS;
    RegisterClassExW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = toast_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = TOAST_CLASS;
    RegisterClassExW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ctrl_wnd_proc;
    wc.hInstance = hInst;
    wc.lpszClassName = CTRL_CLASS;
    RegisterClassExW(&wc);

    g_hCtrlWnd = CreateWindowExW(0, CTRL_CLASS, L"", 0,
                                 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    g_hToastWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        TOAST_CLASS, L"Screenshot",
        WS_POPUP,
        0, 0, 364, 136,
        NULL, NULL, hInst, NULL
    );
}

void screenshot_trigger(ScreenshotMode mode) {
    if (!g_hCtrlWnd) return;
    g_pending_mode = mode;

    HWND fg = GetForegroundWindow();
    if (fg && !is_our_shell_window(fg)) {
        g_pending_target_wnd = fg;
    } else {
        g_pending_target_wnd = popups_get_last_fg_window();
    }

    /* Hide any active toast before capturing so it never appears in the screenshot */
    free_toast_resources();

    /* Brief 75ms delay lets top bar popup finish hiding cleanly before BitBlt */
    SetTimer(g_hCtrlWnd, TIMER_DELAY_CAPTURE, 75, NULL);
}

void screenshot_on_theme_changed(void) {
    if (g_hToastWnd && IsWindowVisible(g_hToastWnd)) {
        toast_paint();
    }
    if (g_hSnipWnd && IsWindowVisible(g_hSnipWnd)) {
        InvalidateRect(g_hSnipWnd, NULL, FALSE);
    }
}

bool screenshot_is_active(void) {
    if (g_hSnipWnd && IsWindow(g_hSnipWnd)) return true;
    if (g_hToastWnd && IsWindowVisible(g_hToastWnd)) return true;
    return false;
}

void screenshot_shutdown(void) {
    close_snip_overlay();
    free_toast_resources();
    if (g_hToastWnd) { DestroyWindow(g_hToastWnd); g_hToastWnd = NULL; }
    if (g_hCtrlWnd)  { DestroyWindow(g_hCtrlWnd);  g_hCtrlWnd = NULL; }
}
