// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The shell's drawing layer (issue #141). See draw.h and docs/SHELL.md. */
#include "draw.h"
#include "dwrite_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

static ULONG_PTR g_gdip_token = 0;

/* IID_IDWriteFactory = {b859ee5a-d838-4b5b-a2e8-1adc7d93db48} */
static const GUID kIidDWriteFactory =
    { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

/* Surfaces that are alive, so draw_text can find the bitmap behind a GpGraphics. */
#define MAX_ACTIVE_SURFACES 24
static LayerSurface *g_active_surfs[MAX_ACTIVE_SURFACES];

static IDWriteFactory *g_dw_factory = NULL;
static wchar_t g_dw_ui_face[64] = L"Segoe UI";
static bool    g_dw_is_inter = false;
static wchar_t g_edit_ui_face[64] = L"Segoe UI";
static wchar_t g_locale[LOCALE_NAME_MAX_LENGTH] = L"en-US";

/* Gamma 1.8 tables for blending ClearType coverage. */
static float g_srgb_to_lin[256];
static BYTE  g_lin_to_srgb[4096];

/* The coverage of one glyph run, reused (freed in render_shutdown). */
static BYTE  *g_dw_alpha_buf = NULL;
static size_t g_dw_alpha_cap = 0;

#define MAX_DW_FMT_CACHE 64
typedef struct {
    wchar_t face[64];
    int     size_tenths;
    int     style;
    int     halign;
    int     valign;
    bool    ellipsis;
    bool    wrap;
    IDWriteTextFormat   *fmt;
    IDWriteInlineObject *trim_sign;
    uint32_t tick;
} CachedDwFmt;

static CachedDwFmt g_dw_fmt_cache[MAX_DW_FMT_CACHE];
static int         g_dw_fmt_count = 0;
static uint32_t    g_dw_fmt_tick = 1;

typedef struct {
    BYTE *bits;
    int   width;
    int   height;
    bool  is_opaque;
    int   clip_x0, clip_y0, clip_x1, clip_y1;
    int   fg_r, fg_g, fg_b, fg_a;
    float fg_lr, fg_lg, fg_lb;
} DwDrawCtx;

static void init_gamma_luts(void) {
    const float gam = 1.8f;
    const float inv_gam = 1.0f / gam;
    for (int i = 0; i < 256; i++) {
        g_srgb_to_lin[i] = powf((float)i / 255.0f, gam);
    }
    for (int i = 0; i < 4096; i++) {
        int v = (int)(powf((float)i / 4095.0f, inv_gam) * 255.0f + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        g_lin_to_srgb[i] = (BYTE)v;
    }
}

static inline BYTE lin_to_srgb_fast(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    int idx = (int)(v * 4095.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 4095) idx = 4095;
    return g_lin_to_srgb[idx];
}

static void register_surface(LayerSurface *s) {
    if (!s) return;
    for (int i = 0; i < MAX_ACTIVE_SURFACES; i++) {
        if (g_active_surfs[i] == s) return;
    }
    for (int i = 0; i < MAX_ACTIVE_SURFACES; i++) {
        if (!g_active_surfs[i]) { g_active_surfs[i] = s; return; }
    }
}

static void unregister_surface(LayerSurface *s) {
    if (!s) return;
    for (int i = 0; i < MAX_ACTIVE_SURFACES; i++) {
        if (g_active_surfs[i] == s) g_active_surfs[i] = NULL;
    }
}

static LayerSurface *find_surface(GpGraphics *g) {
    if (!g) return NULL;
    for (int i = 0; i < MAX_ACTIVE_SURFACES; i++) {
        if (g_active_surfs[i] && g_active_surfs[i]->g == g) return g_active_surfs[i];
    }
    return NULL;
}

/* A DirectWrite text renderer in C: a static table of functions, no allocation. */
static HRESULT STDMETHODCALLTYPE dw_rend_qi(IDWriteTextRenderer *This, REFIID riid, void **ppv) {
    (void)This; (void)riid;
    if (ppv) *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dw_rend_addref(IDWriteTextRenderer *This) { (void)This; return 1; }
static ULONG STDMETHODCALLTYPE dw_rend_release(IDWriteTextRenderer *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE dw_rend_snap(IDWriteTextRenderer *This, void *ctx, BOOL *isDisabled) {
    (void)This; (void)ctx;
    if (isDisabled) *isDisabled = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dw_rend_xform(IDWriteTextRenderer *This, void *ctx, DW_MATRIX *m) {
    (void)This; (void)ctx;
    if (m) { m->m11 = 1.0f; m->m12 = 0.0f; m->m21 = 0.0f; m->m22 = 1.0f; m->dx = 0.0f; m->dy = 0.0f; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dw_rend_ppd(IDWriteTextRenderer *This, void *ctx, FLOAT *ppd) {
    (void)This; (void)ctx;
    if (ppd) *ppd = 1.0f;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dw_rend_underline(IDWriteTextRenderer *This, void *ctx,
                                                   FLOAT bx, FLOAT by, const void *u, IUnknown *eff) {
    (void)This; (void)ctx; (void)bx; (void)by; (void)u; (void)eff;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dw_rend_strike(IDWriteTextRenderer *This, void *ctx,
                                                FLOAT bx, FLOAT by, const void *st, IUnknown *eff) {
    (void)This; (void)ctx; (void)bx; (void)by; (void)st; (void)eff;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dw_rend_inline(IDWriteTextRenderer *This, void *ctx,
                                                FLOAT ox, FLOAT oy, IDWriteInlineObject *obj,
                                                BOOL sideways, BOOL rtl, IUnknown *eff) {
    if (obj && obj->lpVtbl && obj->lpVtbl->Draw) {
        return obj->lpVtbl->Draw(obj, ctx, This, ox, oy, sideways, rtl, eff);
    }
    return S_OK;
}

/* Turns one glyph run into ClearType coverage and blends it into the bitmap,
 * gamma-correct: on an opaque bitmap per colour channel, on a see-through
 * one against the bitmap's own colour, keeping its premultiplied alpha. */
static HRESULT STDMETHODCALLTYPE dw_rend_glyph_run(IDWriteTextRenderer *This, void *vctx,
                                                   FLOAT baselineX, FLOAT baselineY,
                                                   DW_MEASURING_MODE measuringMode,
                                                   const void *glyphRun, const void *glyphRunDesc,
                                                   IUnknown *eff) {
    (void)This; (void)glyphRunDesc; (void)eff;
    const DwDrawCtx *ctx = (const DwDrawCtx *)vctx;
    if (!ctx || !ctx->bits || !g_dw_factory || !glyphRun) return S_OK;

    IDWriteGlyphRunAnalysis *an = NULL;
    HRESULT hr = g_dw_factory->lpVtbl->CreateGlyphRunAnalysis(
        g_dw_factory, glyphRun, 1.0f, NULL,
        DW_RENDER_NATURAL_SYMMETRIC, measuringMode,
        baselineX, baselineY, &an);
    if (FAILED(hr) || !an) return S_OK;

    RECT b = { 0, 0, 0, 0 };
    hr = an->lpVtbl->GetAlphaTextureBounds(an, DW_TEX_CLEARTYPE_3x1, &b);
    if (SUCCEEDED(hr) && b.right > b.left && b.bottom > b.top) {
        int bw = b.right - b.left;
        int bh = b.bottom - b.top;
        size_t need = (size_t)bw * (size_t)bh * 3;
        if (need > g_dw_alpha_cap) {
            size_t cap = max(need, (size_t)65536);
            BYTE *nb = (BYTE *)(g_dw_alpha_buf
                ? HeapReAlloc(GetProcessHeap(), 0, g_dw_alpha_buf, cap)
                : HeapAlloc(GetProcessHeap(), 0, cap));
            if (nb) {
                g_dw_alpha_buf = nb;
                g_dw_alpha_cap = cap;
            }
        }
        if (g_dw_alpha_buf && need <= g_dw_alpha_cap) {
            hr = an->lpVtbl->CreateAlphaTexture(an, DW_TEX_CLEARTYPE_3x1, &b, g_dw_alpha_buf, (UINT32)need);
            if (SUCCEEDED(hr)) {
                int y0 = max(b.top, ctx->clip_y0);
                int y1 = min(b.bottom, ctx->clip_y1);
                int x0 = max(b.left, ctx->clip_x0);
                int x1 = min(b.right, ctx->clip_x1);
                float fa = (float)ctx->fg_a * (1.0f / 255.0f);
                float inv255 = 1.0f / 255.0f;

                if (ctx->is_opaque) {
                    for (int dy = y0; dy < y1; dy++) {
                        BYTE *drow = ctx->bits + (size_t)dy * (size_t)ctx->width * 4;
                        const BYTE *arow = g_dw_alpha_buf + (size_t)(dy - b.top) * (size_t)bw * 3;
                        for (int dx = x0; dx < x1; dx++) {
                            const BYTE *ap = arow + (size_t)(dx - b.left) * 3;
                            if ((ap[0] | ap[1] | ap[2]) == 0) continue;
                            float ar = (float)ap[0] * inv255 * fa;
                            float ag = (float)ap[1] * inv255 * fa;
                            float ab = (float)ap[2] * inv255 * fa;
                            BYTE *p = drow + (size_t)dx * 4;
                            float lb = g_srgb_to_lin[p[0]];
                            float lg = g_srgb_to_lin[p[1]];
                            float lr = g_srgb_to_lin[p[2]];
                            p[0] = lin_to_srgb_fast(lb + (ctx->fg_lb - lb) * ab);
                            p[1] = lin_to_srgb_fast(lg + (ctx->fg_lg - lg) * ag);
                            p[2] = lin_to_srgb_fast(lr + (ctx->fg_lr - lr) * ar);
                            p[3] = 255;
                        }
                    }
                } else {
                    for (int dy = y0; dy < y1; dy++) {
                        BYTE *drow = ctx->bits + (size_t)dy * (size_t)ctx->width * 4;
                        const BYTE *arow = g_dw_alpha_buf + (size_t)(dy - b.top) * (size_t)bw * 3;
                        for (int dx = x0; dx < x1; dx++) {
                            const BYTE *ap = arow + (size_t)(dx - b.left) * 3;
                            int m = max(ap[0], max(ap[1], ap[2]));
                            if (m == 0) continue;
                            float ar = (float)ap[0] * inv255 * fa;
                            float ag = (float)ap[1] * inv255 * fa;
                            float ab = (float)ap[2] * inv255 * fa;
                            BYTE *p = drow + (size_t)dx * 4;
                            int a_bg = p[3];
                            int b_bg, g_bg, r_bg;
                            if (a_bg > 0) {
                                b_bg = min(255, (p[0] * 255 + a_bg / 2) / a_bg);
                                g_bg = min(255, (p[1] * 255 + a_bg / 2) / a_bg);
                                r_bg = min(255, (p[2] * 255 + a_bg / 2) / a_bg);
                            } else {
                                b_bg = ctx->fg_b;
                                g_bg = ctx->fg_g;
                                r_bg = ctx->fg_r;
                            }
                            float lb = g_srgb_to_lin[b_bg];
                            float lg = g_srgb_to_lin[g_bg];
                            float lr = g_srgb_to_lin[r_bg];
                            int b_out = lin_to_srgb_fast(lb + (ctx->fg_lb - lb) * ab);
                            int g_out = lin_to_srgb_fast(lg + (ctx->fg_lg - lg) * ag);
                            int r_out = lin_to_srgb_fast(lr + (ctx->fg_lr - lr) * ar);
                            int cov = (m * ctx->fg_a + 127) / 255;
                            int a_out = a_bg + ((255 - a_bg) * cov + 127) / 255;
                            p[0] = (BYTE)((b_out * a_out + 127) / 255);
                            p[1] = (BYTE)((g_out * a_out + 127) / 255);
                            p[2] = (BYTE)((r_out * a_out + 127) / 255);
                            p[3] = (BYTE)a_out;
                        }
                    }
                }
            }
        }
    }
    an->lpVtbl->Release(an);
    return S_OK;
}

static const IDWriteTextRendererVtbl g_dw_rend_vtbl = {
    dw_rend_qi,
    dw_rend_addref,
    dw_rend_release,
    dw_rend_snap,
    dw_rend_xform,
    dw_rend_ppd,
    dw_rend_glyph_run,
    dw_rend_underline,
    dw_rend_strike,
    dw_rend_inline
};
static IDWriteTextRenderer g_dw_renderer = { &g_dw_rend_vtbl };

static bool dw_family_exists(IDWriteFontCollection *col, const wchar_t *name) {
    if (!col || !name) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(col->lpVtbl->FindFamilyName(col, name, &idx, &exists)) && exists;
}

/* The shell's face: Inter when it is installed, else Segoe UI Variable Text,
 * else Segoe UI. Windows' own edit controls get Segoe UI Variable Text. */
static void detect_best_fonts(void) {
    wcsncpy_s(g_dw_ui_face, 64, L"Segoe UI", _TRUNCATE);
    wcsncpy_s(g_edit_ui_face, 64, L"Segoe UI", _TRUNCATE);
    g_dw_is_inter = false;
    if (!g_dw_factory) return;

    IDWriteFontCollection *col = NULL;
    if (SUCCEEDED(g_dw_factory->lpVtbl->GetSystemFontCollection(g_dw_factory, &col, FALSE)) && col) {
        if (dw_family_exists(col, L"Inter")) {
            wcsncpy_s(g_dw_ui_face, 64, L"Inter", _TRUNCATE);
            g_dw_is_inter = true;
        } else if (dw_family_exists(col, L"Segoe UI Variable Text")) {
            wcsncpy_s(g_dw_ui_face, 64, L"Segoe UI Variable Text", _TRUNCATE);
        }
        if (dw_family_exists(col, L"Segoe UI Variable Text")) {
            wcsncpy_s(g_edit_ui_face, 64, L"Segoe UI Variable Text", _TRUNCATE);
        }
        col->lpVtbl->Release(col);
    }
}

const wchar_t *render_ui_face(void) {
    return g_dw_ui_face;
}

static void clear_dw_fmt_cache(void) {
    for (int i = 0; i < g_dw_fmt_count; i++) {
        if (g_dw_fmt_cache[i].trim_sign) {
            g_dw_fmt_cache[i].trim_sign->lpVtbl->Release(g_dw_fmt_cache[i].trim_sign);
            g_dw_fmt_cache[i].trim_sign = NULL;
        }
        if (g_dw_fmt_cache[i].fmt) {
            g_dw_fmt_cache[i].fmt->lpVtbl->Release(g_dw_fmt_cache[i].fmt);
            g_dw_fmt_cache[i].fmt = NULL;
        }
    }
    g_dw_fmt_count = 0;
}

static IDWriteTextFormat *get_cached_dw_format_ex(const wchar_t *font_name, float size, int style,
                                                  int halign, int valign, bool ellipsis, bool wrap) {
    if (!g_dw_factory) return NULL;
    const wchar_t *face = font_name;
    bool use_default = (!face || !face[0] || _wcsicmp(face, L"Segoe UI") == 0);
    if (use_default) face = g_dw_ui_face;

    /* Inter stands taller than Segoe UI at the same size; 0.94 evens them out. */
    float eff_size = (use_default && g_dw_is_inter) ? (size * 0.94f) : size;
    if (eff_size < 8.0f) eff_size = 8.0f;
    if (eff_size > 120.0f) eff_size = 120.0f;
    int size_tenths = (int)(eff_size * 10.0f + 0.5f);

    for (int i = 0; i < g_dw_fmt_count; i++) {
        CachedDwFmt *c = &g_dw_fmt_cache[i];
        if (c->size_tenths == size_tenths && c->style == style &&
            c->halign == halign && c->valign == valign && c->ellipsis == ellipsis &&
            c->wrap == wrap && _wcsicmp(c->face, face) == 0) {
            c->tick = ++g_dw_fmt_tick;
            return c->fmt;
        }
    }

    DW_FONT_WEIGHT w = (style & GP_FONT_BOLD) ? DW_WEIGHT_SEMIBOLD : DW_WEIGHT_REGULAR;
    DW_FONT_STYLE  s = (style & GP_FONT_ITALIC) ? DW_STYLE_ITALIC : DW_STYLE_NORMAL;
    IDWriteTextFormat *fmt = NULL;
    if (FAILED(g_dw_factory->lpVtbl->CreateTextFormat(
            g_dw_factory, face, NULL, w, s, DW_STRETCH_NORMAL,
            (float)size_tenths * 0.1f, g_locale, &fmt)) || !fmt) {
        return NULL;
    }

    DW_TEXT_ALIGNMENT ta = (halign == GP_ALIGN_CENTER) ? DW_ALIGN_CENTER
                         : (halign == GP_ALIGN_FAR)    ? DW_ALIGN_TRAILING : DW_ALIGN_LEADING;
    DW_PARAGRAPH_ALIGNMENT pa = (valign == GP_ALIGN_CENTER) ? DW_PALIGN_CENTER
                              : (valign == GP_ALIGN_FAR)    ? DW_PALIGN_FAR : DW_PALIGN_NEAR;
    fmt->lpVtbl->SetTextAlignment(fmt, ta);
    fmt->lpVtbl->SetParagraphAlignment(fmt, pa);
    fmt->lpVtbl->SetWordWrapping(fmt, wrap ? DW_WRAP_WRAP : DW_WRAP_NONE);

    IDWriteInlineObject *sign = NULL;
    if (ellipsis) {
        g_dw_factory->lpVtbl->CreateEllipsisTrimmingSign(g_dw_factory, fmt, &sign);
        DW_TRIMMING tr = { DW_TRIM_CHARACTER, 0, 0 };
        fmt->lpVtbl->SetTrimming(fmt, &tr, sign);
    }

    int slot = g_dw_fmt_count;
    if (slot >= MAX_DW_FMT_CACHE) {
        slot = 0;
        uint32_t oldest = g_dw_fmt_cache[0].tick;
        for (int i = 1; i < MAX_DW_FMT_CACHE; i++) {
            if (g_dw_fmt_cache[i].tick < oldest) { oldest = g_dw_fmt_cache[i].tick; slot = i; }
        }
        if (g_dw_fmt_cache[slot].trim_sign) g_dw_fmt_cache[slot].trim_sign->lpVtbl->Release(g_dw_fmt_cache[slot].trim_sign);
        if (g_dw_fmt_cache[slot].fmt)       g_dw_fmt_cache[slot].fmt->lpVtbl->Release(g_dw_fmt_cache[slot].fmt);
    } else {
        g_dw_fmt_count++;
    }

    wcsncpy_s(g_dw_fmt_cache[slot].face, 64, face, _TRUNCATE);
    g_dw_fmt_cache[slot].size_tenths = size_tenths;
    g_dw_fmt_cache[slot].style = style;
    g_dw_fmt_cache[slot].halign = halign;
    g_dw_fmt_cache[slot].valign = valign;
    g_dw_fmt_cache[slot].ellipsis = ellipsis;
    g_dw_fmt_cache[slot].wrap = wrap;
    g_dw_fmt_cache[slot].fmt = fmt;
    g_dw_fmt_cache[slot].trim_sign = sign;
    g_dw_fmt_cache[slot].tick = ++g_dw_fmt_tick;
    return fmt;
}

static IDWriteTextFormat *get_cached_dw_format(const wchar_t *font_name, float size, int style,
                                               int halign, int valign, bool ellipsis) {
    return get_cached_dw_format_ex(font_name, size, style, halign, valign, ellipsis, false);
}

HFONT render_create_ui_font(int px_height, bool semibold) {
    if (px_height < 8) px_height = 8;
    const wchar_t *face = g_edit_ui_face;
    int weight = semibold ? FW_BOLD : FW_NORMAL;
    if (semibold && _wcsicmp(face, L"Segoe UI Variable Text") == 0) {
        face = L"Segoe UI Variable Text Semibold";
        weight = FW_NORMAL;
    }
    return CreateFontW(-px_height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

bool render_init(void) {
    GdiplusStartupInput in;
    ZeroMemory(&in, sizeof(in));
    in.GdiplusVersion = 1;
    if (GdiplusStartup(&g_gdip_token, &in, NULL) != GP_OK) {
        return false;
    }
    init_gamma_luts();
    /* Text is shaped for the user's own language (fonts for Chinese,
     * Japanese and Korean differ by locale). */
    if (!GetUserDefaultLocaleName(g_locale, LOCALE_NAME_MAX_LENGTH)) {
        wcsncpy_s(g_locale, LOCALE_NAME_MAX_LENGTH, L"en-US", _TRUNCATE);
    }
    DWriteCreateFactory(DW_FACTORY_TYPE_SHARED, &kIidDWriteFactory, (IUnknown **)&g_dw_factory);
    detect_best_fonts();
    theme_reload();
    return true;
}

void render_shutdown(void) {
    clear_dw_fmt_cache();
    if (g_dw_alpha_buf) {
        HeapFree(GetProcessHeap(), 0, g_dw_alpha_buf);
        g_dw_alpha_buf = NULL;
        g_dw_alpha_cap = 0;
    }
    if (g_dw_factory) {
        g_dw_factory->lpVtbl->Release(g_dw_factory);
        g_dw_factory = NULL;
    }
    ZeroMemory(g_active_surfs, sizeof(g_active_surfs));
    if (g_gdip_token) {
        GdiplusShutdown(g_gdip_token);
        g_gdip_token = 0;
    }
}

static bool surface_ensure_ex(LayerSurface *s, int w, int h, bool is_opaque) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (s->hdc && s->width == w && s->height == h && s->g && s->is_opaque == is_opaque) {
        return true;
    }
    surface_destroy(s);

    HDC screen = GetDC(NULL);
    s->hdc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!s->hdc) return false;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s->hbmp = CreateDIBSection(s->hdc, &bmi, DIB_RGB_COLORS, &s->bits, NULL, 0);
    if (!s->hbmp || !s->bits) {
        surface_destroy(s);
        return false;
    }
    s->old_bmp = (HBITMAP)SelectObject(s->hdc, s->hbmp);
    s->width = w;
    s->height = h;
    s->is_opaque = is_opaque;

    if (GdipCreateFromHDC(s->hdc, &s->g) != GP_OK || !s->g) {
        surface_destroy(s);
        return false;
    }
    GdipSetSmoothingMode(s->g, GP_SMOOTH_ANTIALIAS);
    GdipSetTextRenderingHint(s->g, GP_TEXT_ANTIALIAS);
    GdipSetInterpolationMode(s->g, GP_INTERP_HIGHQUALITY_BICUBIC);
    if (!is_opaque) {
        GdipSetPixelOffsetMode(s->g, GP_PIXEL_OFFSET_HALF);
    }
    register_surface(s);
    return true;
}

bool surface_ensure(LayerSurface *s, int w, int h) {
    return surface_ensure_ex(s, w, h, false);
}

bool opaque_frame_begin(LayerSurface *s, int w, int h) {
    return surface_ensure_ex(s, w, h, true);
}

void opaque_frame_end(HDC target_hdc, LayerSurface *s) {
    if (!s) return;
    if (s->g) GdipFlush(s->g, GP_FLUSH_INTENTION_SYNC);
    if (target_hdc && s->hdc) {
        BitBlt(target_hdc, 0, 0, s->width, s->height, s->hdc, 0, 0, SRCCOPY);
    }
    surface_destroy(s);
}

void surface_destroy(LayerSurface *s) {
    if (!s) return;
    unregister_surface(s);
    if (s->g) {
        GdipDeleteGraphics(s->g);
        s->g = NULL;
    }
    if (s->hdc) {
        if (s->old_bmp) {
            SelectObject(s->hdc, s->old_bmp);
            s->old_bmp = NULL;
        }
        if (s->hbmp) {
            DeleteObject(s->hbmp);
            s->hbmp = NULL;
        }
        DeleteDC(s->hdc);
        s->hdc = NULL;
    }
    s->bits = NULL;
    s->width = 0;
    s->height = 0;
    s->is_opaque = false;
}

void surface_present(HWND hwnd, LayerSurface *s, int x, int y, BYTE alpha) {
    if (!hwnd || !s || !s->hdc) return;
    POINT ptDst = { x, y };
    POINT ptSrc = { 0, 0 };
    SIZE  sz    = { s->width, s->height };
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = alpha;
    bf.AlphaFormat = AC_SRC_ALPHA;

    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(hwnd, screen, &ptDst, &sz, s->hdc, &ptSrc, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

int surface_save_png(LayerSurface *s, const wchar_t *path) {
    /* GDI+'s PNG encoder: {557CF406-1A04-11D3-9A73-0000F81EF32E} */
    static const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    if (!s || !s->bits || !path) return 0;
    if (s->g) GdipFlush(s->g, GP_FLUSH_INTENTION_SYNC);
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromScan0(s->width, s->height, s->width * 4,
                                  s->is_opaque ? GP_PIXELFORMAT_32BPP_RGB : GP_PIXELFORMAT_32BPP_PARGB,
                                  (BYTE *)s->bits, &bmp) != GP_OK || !bmp) {
        return 0;
    }
    int ok = GdipSaveImageToFile((GpImage *)bmp, path, &png, NULL) == GP_OK;
    GdipDisposeImage((GpImage *)bmp);
    return ok;
}

static GpPath *create_round_rect_path(float x, float y, float w, float h, float r) {
    GpPath *path = NULL;
    if (GdipCreatePath(GP_FILL_ALTERNATE, &path) != GP_OK) return NULL;
    if (r <= 0.1f) r = 0.1f;
    float max_r = (w < h ? w : h) * 0.5f;
    if (r > max_r) r = max_r;
    float d = r * 2.0f;
    GdipAddPathArc(path, x, y, d, d, 180.0f, 90.0f);
    GdipAddPathArc(path, x + w - d, y, d, d, 270.0f, 90.0f);
    GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    GdipAddPathArc(path, x, y + h - d, d, d, 90.0f, 90.0f);
    GdipClosePathFigure(path);
    return path;
}

void draw_round_rect_fill(GpGraphics *g, float x, float y, float w, float h, float r, ARGB color) {
    if (!g || w <= 0.0f || h <= 0.0f) return;
    GpSolidFill *brush = NULL;
    if (GdipCreateSolidFill(color, &brush) != GP_OK) return;
    if (r <= 0.5f) {
        GdipFillRectangle(g, (GpBrush *)brush, x, y, w, h);
    } else {
        GpPath *path = create_round_rect_path(x, y, w, h, r);
        if (path) {
            GdipFillPath(g, (GpBrush *)brush, path);
            GdipDeletePath(path);
        }
    }
    GdipDeleteBrush((GpBrush *)brush);
}

void draw_round_rect_stroke(GpGraphics *g, float x, float y, float w, float h, float r, ARGB color, float width) {
    if (!g || w <= 0.0f || h <= 0.0f) return;
    GpPen *pen = NULL;
    if (GdipCreatePen1(color, width, GP_UNIT_PIXEL, &pen) != GP_OK) return;
    if (r <= 0.5f) {
        GdipDrawRectangle(g, pen, x, y, w, h);
    } else {
        GpPath *path = create_round_rect_path(x, y, w, h, r);
        if (path) {
            GdipDrawPath(g, pen, path);
            GdipDeletePath(path);
        }
    }
    GdipDeletePen(pen);
}

void draw_circle_fill(GpGraphics *g, float cx, float cy, float r, ARGB color) {
    if (!g || r <= 0.0f) return;
    GpSolidFill *brush = NULL;
    if (GdipCreateSolidFill(color, &brush) != GP_OK) return;
    GdipFillEllipse(g, (GpBrush *)brush, cx - r, cy - r, r * 2.0f, r * 2.0f);
    GdipDeleteBrush((GpBrush *)brush);
}

void draw_line(GpGraphics *g, float x1, float y1, float x2, float y2, ARGB color, float width) {
    if (!g) return;
    GpPen *pen = NULL;
    if (GdipCreatePen1(color, width, GP_UNIT_PIXEL, &pen) != GP_OK) return;
    GdipDrawLine(g, pen, x1, y1, x2, y2);
    GdipDeletePen(pen);
}

/* Draws a laid-out text into the surface behind g, inside its clip. */
static void dw_draw_layout(GpGraphics *g, LayerSurface *surf, IDWriteTextLayout *lay, ARGB color,
                           float x, float y, float w, float h) {
    GdipFlush(g, GP_FLUSH_INTENTION_SYNC);

    DwDrawCtx ctx;
    ctx.bits = (BYTE *)surf->bits;
    ctx.width = surf->width;
    ctx.height = surf->height;
    ctx.is_opaque = surf->is_opaque;
    ctx.clip_x0 = max(0, (int)floorf(x) - 1);
    ctx.clip_y0 = max(0, (int)floorf(y) - 1);
    ctx.clip_x1 = min(surf->width, (int)ceilf(x + w) + 1);
    ctx.clip_y1 = min(surf->height, (int)ceilf(y + h) + 1);

    GpRect clip_rc = { 0, 0, 0, 0 };
    if (GdipGetClipBoundsI(g, &clip_rc) == GP_OK &&
        clip_rc.Width > 0 && clip_rc.Height > 0 &&
        clip_rc.Width < 1000000 && clip_rc.Height < 1000000) {
        ctx.clip_x0 = max(ctx.clip_x0, clip_rc.X);
        ctx.clip_y0 = max(ctx.clip_y0, clip_rc.Y);
        ctx.clip_x1 = min(ctx.clip_x1, clip_rc.X + clip_rc.Width);
        ctx.clip_y1 = min(ctx.clip_y1, clip_rc.Y + clip_rc.Height);
    }

    ctx.fg_a = (int)((color >> 24) & 0xFFu);
    ctx.fg_r = (int)((color >> 16) & 0xFFu);
    ctx.fg_g = (int)((color >> 8) & 0xFFu);
    ctx.fg_b = (int)(color & 0xFFu);
    ctx.fg_lr = g_srgb_to_lin[ctx.fg_r];
    ctx.fg_lg = g_srgb_to_lin[ctx.fg_g];
    ctx.fg_lb = g_srgb_to_lin[ctx.fg_b];

    if (ctx.fg_a > 0 && ctx.clip_x1 > ctx.clip_x0 && ctx.clip_y1 > ctx.clip_y0) {
        lay->lpVtbl->Draw(lay, &ctx, &g_dw_renderer, x, y);
    }
}

float draw_text_wrapped(GpGraphics *g, const wchar_t *text, int len, const wchar_t *font_name, float size,
                        int style, ARGB color, float x, float y, float w, float h, bool draw) {
    if (!text || len <= 0 || w <= 1.0f || !g_dw_factory) return 0.0f;
    IDWriteTextFormat *fmt = get_cached_dw_format_ex(font_name, size, style, GP_ALIGN_NEAR, GP_ALIGN_NEAR, false, true);
    if (!fmt) return 0.0f;

    float out_h = 0.0f;
    IDWriteTextLayout *lay = NULL;
    float lay_h = (h > 1.0f) ? h : 100000.0f;
    if (SUCCEEDED(g_dw_factory->lpVtbl->CreateTextLayout(g_dw_factory, text, (UINT32)len, fmt, w, lay_h, &lay)) && lay) {
        DW_TEXT_METRICS tm;
        ZeroMemory(&tm, sizeof(tm));
        if (SUCCEEDED(lay->lpVtbl->GetMetrics(lay, &tm))) out_h = tm.height;
        if (draw && g) {
            LayerSurface *surf = find_surface(g);
            if (surf && surf->bits) dw_draw_layout(g, surf, lay, color, x, y, w, (h > 1.0f) ? h : out_h + 2.0f);
        }
        lay->lpVtbl->base.Release((IDWriteTextFormat *)lay);
    }
    return out_h;
}

void draw_text(GpGraphics *g, const wchar_t *text, const wchar_t *font_name, float size, int style,
               ARGB color, float x, float y, float w, float h, int halign, int valign, bool ellipsis) {
    if (!g || !text || !text[0] || w <= 0.5f || h <= 0.5f) return;

    LayerSurface *surf = find_surface(g);
    if (surf && surf->bits && g_dw_factory) {
        IDWriteTextFormat *fmt = get_cached_dw_format(font_name, size, style, halign, valign, ellipsis);
        if (fmt) {
            IDWriteTextLayout *lay = NULL;
            UINT32 slen = (UINT32)wcslen(text);
            if (SUCCEEDED(g_dw_factory->lpVtbl->CreateTextLayout(g_dw_factory, text, slen, fmt, w, h, &lay)) && lay) {
                dw_draw_layout(g, surf, lay, color, x, y, w, h);
                lay->lpVtbl->base.Release((IDWriteTextFormat *)lay);
                return;
            }
        }
    }
    /* Outside a surface of this layer: GDI+'s own text. */
    GpFontFamily *fam = NULL;
    if (GdipCreateFontFamilyFromName(font_name ? font_name : L"Segoe UI", NULL, &fam) != GP_OK) return;
    GpFont *font = NULL;
    if (GdipCreateFont(fam, size, style, GP_UNIT_PIXEL, &font) == GP_OK) {
        GpStringFormat *sf = NULL;
        if (GdipCreateStringFormat(0, 0, &sf) == GP_OK) {
            GdipSetStringFormatAlign(sf, halign);
            GdipSetStringFormatLineAlign(sf, valign);
            GdipSetStringFormatFlags(sf, GP_STRFMT_NOWRAP | GP_STRFMT_NOFITBLACK);
            if (ellipsis) GdipSetStringFormatTrimming(sf, GP_TRIM_ELLIPSIS_CHARACTER);
            GpSolidFill *brush = NULL;
            if (GdipCreateSolidFill(color, &brush) == GP_OK) {
                GpRectF rect = { x, y, w, h };
                GdipDrawString(g, text, -1, font, &rect, sf, (GpBrush *)brush);
                GdipDeleteBrush((GpBrush *)brush);
            }
            GdipDeleteStringFormat(sf);
        }
        GdipDeleteFont(font);
    }
    GdipDeleteFontFamily(fam);
}

float measure_text_width(GpGraphics *g, const wchar_t *text, const wchar_t *font_name, float size, int style) {
    (void)g;
    if (!text || !text[0] || !g_dw_factory) return 0.0f;
    IDWriteTextFormat *fmt = get_cached_dw_format(font_name, size, style, GP_ALIGN_NEAR, GP_ALIGN_NEAR, false);
    if (!fmt) return 0.0f;

    float out_w = 0.0f;
    IDWriteTextLayout *lay = NULL;
    if (SUCCEEDED(g_dw_factory->lpVtbl->CreateTextLayout(g_dw_factory, text, (UINT32)wcslen(text),
                                                         fmt, 4000.0f, 200.0f, &lay)) && lay) {
        DW_TEXT_METRICS tm;
        ZeroMemory(&tm, sizeof(tm));
        if (SUCCEEDED(lay->lpVtbl->GetMetrics(lay, &tm))) {
            out_w = tm.widthIncludingTrailingWhitespace + 2.0f;
        }
        lay->lpVtbl->base.Release((IDWriteTextFormat *)lay);
    }
    return out_w;
}

void draw_icon_glyph(GpGraphics *g, wchar_t glyph, float size, ARGB color, float x, float y, float w, float h) {
    if (!g) return;
    wchar_t buf[2] = { glyph, L'\0' };
    GpFontFamily *fam = NULL;
    if (GdipCreateFontFamilyFromName(L"Segoe Fluent Icons", NULL, &fam) != GP_OK) {
        if (GdipCreateFontFamilyFromName(L"Segoe MDL2 Assets", NULL, &fam) != GP_OK) {
            return;
        }
    }
    int prev_hint = GP_TEXT_ANTIALIAS;
    GdipGetTextRenderingHint(g, &prev_hint);
    GdipSetTextRenderingHint(g, GP_TEXT_ANTIALIAS);

    GpFont *font = NULL;
    if (GdipCreateFont(fam, size, GP_FONT_REGULAR, GP_UNIT_PIXEL, &font) == GP_OK) {
        GpStringFormat *fmt = NULL;
        if (GdipCreateStringFormat(0, 0, &fmt) == GP_OK) {
            GdipSetStringFormatAlign(fmt, GP_ALIGN_CENTER);
            GdipSetStringFormatLineAlign(fmt, GP_ALIGN_CENTER);
            GpSolidFill *brush = NULL;
            if (GdipCreateSolidFill(color, &brush) == GP_OK) {
                GpRectF rect = { floorf(x + 0.5f), floorf(y + 0.5f), floorf(w + 0.5f), floorf(h + 0.5f) };
                GdipDrawString(g, buf, 1, font, &rect, fmt, (GpBrush *)brush);
                GdipDeleteBrush((GpBrush *)brush);
            }
            GdipDeleteStringFormat(fmt);
        }
        GdipDeleteFont(font);
    }
    GdipSetTextRenderingHint(g, prev_hint);
    GdipDeleteFontFamily(fam);
}

float anim_ease_out(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void draw_share_icon(GpGraphics *g, float cx, float cy, float r, ARGB color, float stroke) {
    GpPen *pen = NULL;
    if (GdipCreatePen1(color, stroke, GP_UNIT_PIXEL, &pen) != GP_OK || !pen) return;
    GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
    static const float k[3] = { 1.0f, 0.68f, 0.36f };
    for (int i = 0; i < 3; i++) {
        float rr = r * k[i];
        GdipDrawArc(g, pen, cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, 128.0f, 284.0f);
    }
    GdipDeletePen(pen);
    draw_circle_fill(g, cx, cy, r * 0.13f + 0.4f, color);
}
