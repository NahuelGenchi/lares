// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// lares-draw-bench: draws the same shell screens with the two engines Lares
// could use for its Windows shell, each in a process of its own, and prints
// what each costs (issue #140).
//
//   gdiplus  shapes and pictures with GDI+, text as DirectWrite glyphs that
//            Lares blends into the picture itself (ClearType, gamma 1.8)
//   d2d      a Direct2D software target bound to a bitmap, the way lares.exe
//            draws its window
//
//   lares-draw-bench compare [--frames N] [--png DIR]
//   lares-draw-bench run <engine> <scene> [--scale S] [--frames N] [--png FILE]
//
// Scenes: bar (1920x32, see-through), dock (640x64, see-through), popup
// (360x480, see-through) and list (960x640, opaque). Nothing is shown on the
// screen: every scene is drawn into a bitmap in memory, which is what both
// engines do before Windows puts the picture on screen; that last step is the
// same for both and is left out.
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

using namespace Gdiplus::DllExports;
using Gdiplus::GpBitmap;
using Gdiplus::GpGraphics;
using Gdiplus::GpPath;
using Gdiplus::GpPen;
using Gdiplus::GpSolidFill;

constexpr int kIcons = 12;
constexpr int kIconPx = 64;

enum class Align { Near, Center, Far };

// ---------------------------------------------------------------------------
// A bitmap in memory, 32 bits a pixel, premultiplied when see-through.

struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint8_t* bits = nullptr;
    int w = 0, h = 0;
    bool opaque = false;

    bool create(int width, int height, bool is_opaque)
    {
        w = width;
        h = height;
        opaque = is_opaque;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* p = nullptr;
        bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &p, nullptr, 0);
        if (!bmp || !p) return false;
        bits = static_cast<uint8_t*>(p);
        old = SelectObject(dc, bmp);
        return true;
    }
};

// Made-up app icons: a rounded square with a gradient, premultiplied.
static void make_icon(int index, uint8_t* px)
{
    const float hue = static_cast<float>(index) / kIcons;
    for (int y = 0; y < kIconPx; ++y) {
        for (int x = 0; x < kIconPx; ++x) {
            float cx = std::fabs(x + 0.5f - kIconPx / 2.0f) - (kIconPx / 2.0f - 14.0f);
            float cy = std::fabs(y + 0.5f - kIconPx / 2.0f) - (kIconPx / 2.0f - 14.0f);
            float d = std::sqrt(std::max(cx, 0.0f) * std::max(cx, 0.0f) + std::max(cy, 0.0f) * std::max(cy, 0.0f));
            float a = std::clamp(14.5f - d, 0.0f, 1.0f);
            float t = static_cast<float>(y) / kIconPx;
            float r = 0.5f + 0.5f * std::sin(6.2832f * (hue + 0.00f)) * (1.0f - 0.4f * t);
            float g = 0.5f + 0.5f * std::sin(6.2832f * (hue + 0.33f)) * (1.0f - 0.4f * t);
            float b = 0.5f + 0.5f * std::sin(6.2832f * (hue + 0.67f)) * (1.0f - 0.4f * t);
            uint8_t* p = px + (static_cast<size_t>(y) * kIconPx + x) * 4;
            p[0] = static_cast<uint8_t>(b * a * 255.0f + 0.5f);
            p[1] = static_cast<uint8_t>(g * a * 255.0f + 0.5f);
            p[2] = static_cast<uint8_t>(r * a * 255.0f + 0.5f);
            p[3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
}

static const wchar_t* pick_face(IDWriteFactory* f)
{
    static const wchar_t* faces[] = { L"Segoe UI Variable Text", L"Segoe UI" };
    IDWriteFontCollection* col = nullptr;
    const wchar_t* chosen = L"Segoe UI";
    if (SUCCEEDED(f->GetSystemFontCollection(&col, FALSE)) && col) {
        for (const wchar_t* face : faces) {
            UINT32 i = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(col->FindFamilyName(face, &i, &exists)) && exists) {
                chosen = face;
                break;
            }
        }
        col->Release();
    }
    return chosen;
}

// One cache of text formats for both engines: a format per face, size,
// weight, alignment and trimming, as the prototype keeps.
class FormatCache {
public:
    void init(IDWriteFactory* f, const wchar_t* face)
    {
        factory_ = f;
        face_ = face;
    }
    void clear()
    {
        for (auto& e : items_) {
            if (e.sign) e.sign->Release();
            if (e.fmt) e.fmt->Release();
        }
        items_.clear();
    }
    IDWriteTextFormat* get(float size, bool bold, Align h, bool ellipsis, bool wrap)
    {
        int tenths = static_cast<int>(size * 10.0f + 0.5f);
        for (auto& e : items_) {
            if (e.tenths == tenths && e.bold == bold && e.h == h && e.ellipsis == ellipsis && e.wrap == wrap) return e.fmt;
        }
        Entry e;
        e.tenths = tenths;
        e.bold = bold;
        e.h = h;
        e.ellipsis = ellipsis;
        e.wrap = wrap;
        if (FAILED(factory_->CreateTextFormat(face_, nullptr,
                bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, tenths * 0.1f, L"en-US", &e.fmt)) || !e.fmt) {
            return nullptr;
        }
        e.fmt->SetTextAlignment(h == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                : h == Align::Far ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                  : DWRITE_TEXT_ALIGNMENT_LEADING);
        e.fmt->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        e.fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (ellipsis && SUCCEEDED(factory_->CreateEllipsisTrimmingSign(e.fmt, &e.sign))) {
            DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            e.fmt->SetTrimming(&tr, e.sign);
        }
        items_.push_back(e);
        return e.fmt;
    }

private:
    struct Entry {
        int tenths = 0;
        bool bold = false;
        Align h = Align::Near;
        bool ellipsis = false;
        bool wrap = false;
        IDWriteTextFormat* fmt = nullptr;
        IDWriteInlineObject* sign = nullptr;
    };
    IDWriteFactory* factory_ = nullptr;
    const wchar_t* face_ = L"Segoe UI";
    std::vector<Entry> items_;
};

// ---------------------------------------------------------------------------
// What a scene asks of an engine.

class Engine {
public:
    virtual ~Engine() = default;
    virtual bool init(Canvas& c) = 0;
    virtual void begin() = 0;
    virtual void end() = 0;
    virtual void fill_round(float x, float y, float w, float h, float r, uint32_t argb) = 0;
    virtual void stroke_round(float x, float y, float w, float h, float r, uint32_t argb, float width) = 0;
    virtual void circle(float cx, float cy, float r, uint32_t argb) = 0;
    virtual void line(float x1, float y1, float x2, float y2, uint32_t argb, float width) = 0;
    virtual void text(const wchar_t* s, float size, bool bold, uint32_t argb, float x, float y, float w, float h,
                      Align align, bool ellipsis) = 0;
    virtual float paragraph(const wchar_t* s, float size, uint32_t argb, float x, float y, float w) = 0;
    virtual void image(int index, float x, float y, float w, float h) = 0;
};

static uint32_t argb(int a, int r, int g, int b)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

// ---------------------------------------------------------------------------
// GDI+ and Lares' own glyph blending (the shell prototype's path).

class GdiplusEngine;

struct GlyphTarget {
    uint8_t* bits;
    int width, height;
    bool opaque;
    int x0, y0, x1, y1;
    int fr, fg, fb, fa;
    float lr, lg, lb;
};

class GlyphRenderer final : public IDWriteTextRenderer {
public:
    GlyphRenderer(IDWriteFactory* f, const float* to_lin, const uint8_t* to_srgb)
        : factory_(f), to_lin_(to_lin), to_srgb_(to_srgb) {}
    ~GlyphRenderer() { std::free(alpha_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* off) override
    {
        *off = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override
    {
        *m = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override
    {
        *p = 1.0f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* ctx, FLOAT x, FLOAT y, IDWriteInlineObject* obj, BOOL sideways,
                                               BOOL rtl, IUnknown* effect) override
    {
        return obj ? obj->Draw(ctx, this, x, y, sideways, rtl, effect) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void* vctx, FLOAT bx, FLOAT by, DWRITE_MEASURING_MODE mode,
                                           const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) override
    {
        const GlyphTarget* t = static_cast<const GlyphTarget*>(vctx);
        IDWriteGlyphRunAnalysis* an = nullptr;
        if (FAILED(factory_->CreateGlyphRunAnalysis(run, 1.0f, nullptr,
                DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC, mode, bx, by, &an)) || !an) {
            return S_OK;
        }
        RECT b = {};
        if (SUCCEEDED(an->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &b)) && b.right > b.left &&
            b.bottom > b.top) {
            int bw = b.right - b.left;
            int bh = b.bottom - b.top;
            size_t need = static_cast<size_t>(bw) * bh * 3;
            if (need > cap_) {
                std::free(alpha_);
                cap_ = std::max<size_t>(need, 65536);
                alpha_ = static_cast<uint8_t*>(std::malloc(cap_));
                if (!alpha_) cap_ = 0;
            }
            if (alpha_ && SUCCEEDED(an->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &b, alpha_,
                                                           static_cast<UINT32>(need)))) {
                blend(*t, b, bw);
            }
        }
        an->Release();
        return S_OK;
    }

private:
    uint8_t srgb(float v) const
    {
        if (v <= 0.0f) return 0;
        if (v >= 1.0f) return 255;
        return to_srgb_[static_cast<int>(v * 4095.0f + 0.5f)];
    }

    void blend(const GlyphTarget& t, const RECT& b, int bw) const
    {
        int y0 = std::max<int>(b.top, t.y0), y1 = std::min<int>(b.bottom, t.y1);
        int x0 = std::max<int>(b.left, t.x0), x1 = std::min<int>(b.right, t.x1);
        float fa = t.fa / 255.0f;
        const float k = 1.0f / 255.0f;
        for (int dy = y0; dy < y1; ++dy) {
            uint8_t* row = t.bits + static_cast<size_t>(dy) * t.width * 4;
            const uint8_t* arow = alpha_ + static_cast<size_t>(dy - b.top) * bw * 3;
            for (int dx = x0; dx < x1; ++dx) {
                const uint8_t* ap = arow + static_cast<size_t>(dx - b.left) * 3;
                int m = std::max<int>(ap[0], std::max<int>(ap[1], ap[2]));
                if (m == 0) continue;
                float ar = ap[0] * k * fa, ag = ap[1] * k * fa, ab = ap[2] * k * fa;
                uint8_t* p = row + static_cast<size_t>(dx) * 4;
                if (t.opaque) {
                    float lb = to_lin_[p[0]], lg = to_lin_[p[1]], lr = to_lin_[p[2]];
                    p[0] = srgb(lb + (t.lb - lb) * ab);
                    p[1] = srgb(lg + (t.lg - lg) * ag);
                    p[2] = srgb(lr + (t.lr - lr) * ar);
                    p[3] = 255;
                } else {
                    int a_bg = p[3];
                    int bb = t.fb, bg = t.fg, br = t.fr;
                    if (a_bg > 0) {
                        bb = std::min(255, (p[0] * 255 + a_bg / 2) / a_bg);
                        bg = std::min(255, (p[1] * 255 + a_bg / 2) / a_bg);
                        br = std::min(255, (p[2] * 255 + a_bg / 2) / a_bg);
                    }
                    float lb = to_lin_[bb], lg = to_lin_[bg], lr = to_lin_[br];
                    int ob = srgb(lb + (t.lb - lb) * ab);
                    int og = srgb(lg + (t.lg - lg) * ag);
                    int orr = srgb(lr + (t.lr - lr) * ar);
                    int cov = (m * t.fa + 127) / 255;
                    int a = a_bg + ((255 - a_bg) * cov + 127) / 255;
                    p[0] = static_cast<uint8_t>((ob * a + 127) / 255);
                    p[1] = static_cast<uint8_t>((og * a + 127) / 255);
                    p[2] = static_cast<uint8_t>((orr * a + 127) / 255);
                    p[3] = static_cast<uint8_t>(a);
                }
            }
        }
    }

    IDWriteFactory* factory_;
    const float* to_lin_;
    const uint8_t* to_srgb_;
    uint8_t* alpha_ = nullptr;
    size_t cap_ = 0;
};

class GdiplusEngine final : public Engine {
public:
    ~GdiplusEngine() override
    {
        for (GpBitmap*& b : icons_) {
            if (b) GdipDisposeImage(reinterpret_cast<Gdiplus::GpImage*>(b));
        }
        if (g_) GdipDeleteGraphics(g_);
        delete renderer_;
        formats_.clear();
        if (dw_) dw_->Release();
        if (token_) Gdiplus::GdiplusShutdown(token_);
    }

    bool init(Canvas& c) override
    {
        c_ = &c;
        Gdiplus::GdiplusStartupInput in;
        if (Gdiplus::GdiplusStartup(&token_, &in, nullptr) != Gdiplus::Ok) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&dw_)))) {
            return false;
        }
        for (int i = 0; i < 256; ++i) to_lin_[i] = std::pow(i / 255.0f, 1.8f);
        for (int i = 0; i < 4096; ++i) {
            to_srgb_[i] = static_cast<uint8_t>(std::clamp(std::pow(i / 4095.0f, 1.0f / 1.8f) * 255.0f + 0.5f, 0.0f, 255.0f));
        }
        formats_.init(dw_, pick_face(dw_));
        renderer_ = new GlyphRenderer(dw_, to_lin_, to_srgb_);
        if (GdipCreateFromHDC(c.dc, &g_) != Gdiplus::Ok) return false;
        GdipSetSmoothingMode(g_, Gdiplus::SmoothingModeAntiAlias);
        GdipSetInterpolationMode(g_, Gdiplus::InterpolationModeHighQualityBicubic);
        if (!c.opaque) GdipSetPixelOffsetMode(g_, Gdiplus::PixelOffsetModeHalf);
        std::vector<uint8_t> px(static_cast<size_t>(kIconPx) * kIconPx * 4);
        for (int i = 0; i < kIcons; ++i) {
            make_icon(i, px.data());
            if (GdipCreateBitmapFromScan0(kIconPx, kIconPx, 0, PixelFormat32bppPARGB, nullptr, &icons_[i]) !=
                Gdiplus::Ok) {
                return false;
            }
            Gdiplus::GpRect rc(0, 0, kIconPx, kIconPx);
            Gdiplus::BitmapData data = {};
            if (GdipBitmapLockBits(icons_[i], &rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &data) ==
                Gdiplus::Ok) {
                for (int y = 0; y < kIconPx; ++y) {
                    std::memcpy(static_cast<uint8_t*>(data.Scan0) + static_cast<size_t>(y) * data.Stride,
                                px.data() + static_cast<size_t>(y) * kIconPx * 4, kIconPx * 4);
                }
                GdipBitmapUnlockBits(icons_[i], &data);
            }
        }
        return true;
    }

    void begin() override { std::memset(c_->bits, 0, static_cast<size_t>(c_->w) * c_->h * 4); }
    void end() override { GdipFlush(g_, Gdiplus::FlushIntentionSync); }

    void fill_round(float x, float y, float w, float h, float r, uint32_t color) override
    {
        GpSolidFill* brush = nullptr;
        if (GdipCreateSolidFill(color, &brush) != Gdiplus::Ok) return;
        if (r <= 0.5f) {
            GdipFillRectangle(g_, brush, x, y, w, h);
        } else if (GpPath* path = round_path(x, y, w, h, r)) {
            GdipFillPath(g_, brush, path);
            GdipDeletePath(path);
        }
        GdipDeleteBrush(brush);
    }

    void stroke_round(float x, float y, float w, float h, float r, uint32_t color, float width) override
    {
        GpPen* pen = nullptr;
        if (GdipCreatePen1(color, width, Gdiplus::UnitPixel, &pen) != Gdiplus::Ok) return;
        if (GpPath* path = round_path(x, y, w, h, r)) {
            GdipDrawPath(g_, pen, path);
            GdipDeletePath(path);
        }
        GdipDeletePen(pen);
    }

    void circle(float cx, float cy, float r, uint32_t color) override
    {
        GpSolidFill* brush = nullptr;
        if (GdipCreateSolidFill(color, &brush) != Gdiplus::Ok) return;
        GdipFillEllipse(g_, brush, cx - r, cy - r, r * 2.0f, r * 2.0f);
        GdipDeleteBrush(brush);
    }

    void line(float x1, float y1, float x2, float y2, uint32_t color, float width) override
    {
        GpPen* pen = nullptr;
        if (GdipCreatePen1(color, width, Gdiplus::UnitPixel, &pen) != Gdiplus::Ok) return;
        GdipDrawLine(g_, pen, x1, y1, x2, y2);
        GdipDeletePen(pen);
    }

    void text(const wchar_t* s, float size, bool bold, uint32_t color, float x, float y, float w, float h, Align align,
              bool ellipsis) override
    {
        IDWriteTextFormat* fmt = formats_.get(size, bold, align, ellipsis, false);
        IDWriteTextLayout* layout = nullptr;
        if (!fmt || FAILED(dw_->CreateTextLayout(s, static_cast<UINT32>(wcslen(s)), fmt, w, h, &layout))) return;
        draw_layout(layout, color, x, y, w, h);
        layout->Release();
    }

    float paragraph(const wchar_t* s, float size, uint32_t color, float x, float y, float w) override
    {
        IDWriteTextFormat* fmt = formats_.get(size, false, Align::Near, false, true);
        IDWriteTextLayout* layout = nullptr;
        if (!fmt || FAILED(dw_->CreateTextLayout(s, static_cast<UINT32>(wcslen(s)), fmt, w, 100000.0f, &layout))) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        draw_layout(layout, color, x, y, w, m.height + 2.0f);
        layout->Release();
        return m.height;
    }

    void image(int index, float x, float y, float w, float h) override
    {
        GdipDrawImageRect(g_, reinterpret_cast<Gdiplus::GpImage*>(icons_[index % kIcons]), x, y, w, h);
    }

private:
    static GpPath* round_path(float x, float y, float w, float h, float r)
    {
        GpPath* path = nullptr;
        if (GdipCreatePath(Gdiplus::FillModeAlternate, &path) != Gdiplus::Ok) return nullptr;
        r = std::min(std::max(r, 0.1f), std::min(w, h) * 0.5f);
        float d = r * 2.0f;
        GdipAddPathArc(path, x, y, d, d, 180.0f, 90.0f);
        GdipAddPathArc(path, x + w - d, y, d, d, 270.0f, 90.0f);
        GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        GdipAddPathArc(path, x, y + h - d, d, d, 90.0f, 90.0f);
        GdipClosePathFigure(path);
        return path;
    }

    void draw_layout(IDWriteTextLayout* layout, uint32_t color, float x, float y, float w, float h)
    {
        GdipFlush(g_, Gdiplus::FlushIntentionSync);
        GlyphTarget t;
        t.bits = c_->bits;
        t.width = c_->w;
        t.height = c_->h;
        t.opaque = c_->opaque;
        t.x0 = std::max(0, static_cast<int>(std::floor(x)) - 1);
        t.y0 = std::max(0, static_cast<int>(std::floor(y)) - 1);
        t.x1 = std::min(c_->w, static_cast<int>(std::ceil(x + w)) + 1);
        t.y1 = std::min(c_->h, static_cast<int>(std::ceil(y + h)) + 1);
        t.fa = static_cast<int>((color >> 24) & 0xff);
        t.fr = static_cast<int>((color >> 16) & 0xff);
        t.fg = static_cast<int>((color >> 8) & 0xff);
        t.fb = static_cast<int>(color & 0xff);
        t.lr = to_lin_[t.fr];
        t.lg = to_lin_[t.fg];
        t.lb = to_lin_[t.fb];
        if (t.fa > 0 && t.x1 > t.x0 && t.y1 > t.y0) layout->Draw(&t, renderer_, x, y);
    }

    Canvas* c_ = nullptr;
    ULONG_PTR token_ = 0;
    IDWriteFactory* dw_ = nullptr;
    GpGraphics* g_ = nullptr;
    GlyphRenderer* renderer_ = nullptr;
    FormatCache formats_;
    GpBitmap* icons_[kIcons] = {};
    float to_lin_[256] = {};
    uint8_t to_srgb_[4096] = {};
};

// ---------------------------------------------------------------------------
// Direct2D on a software target bound to the bitmap (lares.exe's path).

class D2dEngine final : public Engine {
public:
    ~D2dEngine() override
    {
        for (ID2D1Bitmap*& b : icons_) {
            if (b) b->Release();
        }
        if (brush_) brush_->Release();
        if (rt_) rt_->Release();
        if (factory_) factory_->Release();
        formats_.clear();
        if (dw_) dw_->Release();
    }

    bool init(Canvas& c) override
    {
        c_ = &c;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&dw_)))) {
            return false;
        }
        formats_.init(dw_, pick_face(dw_));
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              c.opaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(factory_->CreateDCRenderTarget(&props, &rt_))) return false;
        RECT rc = { 0, 0, c.w, c.h };
        if (FAILED(rt_->BindDC(c.dc, &rc))) return false;
        // ClearType needs an opaque target; on a see-through one Direct2D
        // draws text in grey levels.
        rt_->SetTextAntialiasMode(c.opaque ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_))) return false;
        std::vector<uint8_t> px(static_cast<size_t>(kIconPx) * kIconPx * 4);
        D2D1_BITMAP_PROPERTIES bp =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        for (int i = 0; i < kIcons; ++i) {
            make_icon(i, px.data());
            if (FAILED(rt_->CreateBitmap(D2D1::SizeU(kIconPx, kIconPx), px.data(), kIconPx * 4, &bp, &icons_[i]))) {
                return false;
            }
        }
        return true;
    }

    void begin() override
    {
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    }
    void end() override { rt_->EndDraw(); }

    void fill_round(float x, float y, float w, float h, float r, uint32_t color) override
    {
        set(color);
        D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
        if (r <= 0.5f) rt_->FillRectangle(rc, brush_);
        else rt_->FillRoundedRectangle(D2D1::RoundedRect(rc, r, r), brush_);
    }

    void stroke_round(float x, float y, float w, float h, float r, uint32_t color, float width) override
    {
        set(color);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush_, width);
    }

    void circle(float cx, float cy, float r, uint32_t color) override
    {
        set(color);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_);
    }

    void line(float x1, float y1, float x2, float y2, uint32_t color, float width) override
    {
        set(color);
        rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_, width);
    }

    void text(const wchar_t* s, float size, bool bold, uint32_t color, float x, float y, float w, float h, Align align,
              bool ellipsis) override
    {
        IDWriteTextFormat* fmt = formats_.get(size, bold, align, ellipsis, false);
        IDWriteTextLayout* layout = nullptr;
        if (!fmt || FAILED(dw_->CreateTextLayout(s, static_cast<UINT32>(wcslen(s)), fmt, w, h, &layout))) return;
        set(color);
        rt_->DrawTextLayout(D2D1::Point2F(x, y), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        layout->Release();
    }

    float paragraph(const wchar_t* s, float size, uint32_t color, float x, float y, float w) override
    {
        IDWriteTextFormat* fmt = formats_.get(size, false, Align::Near, false, true);
        IDWriteTextLayout* layout = nullptr;
        if (!fmt || FAILED(dw_->CreateTextLayout(s, static_cast<UINT32>(wcslen(s)), fmt, w, 100000.0f, &layout))) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        set(color);
        rt_->DrawTextLayout(D2D1::Point2F(x, y), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_NONE);
        layout->Release();
        return m.height;
    }

    void image(int index, float x, float y, float w, float h) override
    {
        rt_->DrawBitmap(icons_[index % kIcons], D2D1::RectF(x, y, x + w, y + h), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

private:
    void set(uint32_t color)
    {
        brush_->SetColor(D2D1::ColorF(((color >> 16) & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f,
                                      (color & 0xff) / 255.0f, ((color >> 24) & 0xff) / 255.0f));
    }

    Canvas* c_ = nullptr;
    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1Bitmap* icons_[kIcons] = {};
    FormatCache formats_;
};

// ---------------------------------------------------------------------------
// The four screens, in the shell's dark colours. Every size is multiplied by
// the scale, as a shell drawn for a 150% screen would.

struct Scene {
    const char* name;
    int w, h;
    bool opaque;
};
static const Scene kScenes[] = {
    { "bar", 1920, 32, false },
    { "dock", 640, 64, false },
    { "popup", 360, 480, false },
    { "list", 960, 640, true },
};

static const uint32_t kText = argb(255, 232, 232, 236);
static const uint32_t kMuted = argb(255, 140, 140, 150);
static const uint32_t kAccent = argb(255, 96, 180, 255);

static void draw_bar(Engine& e, float s)
{
    e.fill_round(0, 0, 1920 * s, 32 * s, 0, argb(232, 26, 26, 28));
    e.fill_round(8 * s, 4 * s, 24 * s, 24 * s, 6 * s, argb(235, 38, 38, 42));
    e.image(3, 10 * s, 6 * s, 20 * s, 20 * s);
    e.text(L"Files", 13 * s, true, kText, 38 * s, 0, 200 * s, 32 * s, Align::Near, true);
    e.text(L"Fri 27 Nov  14:32", 13 * s, true, kText, 860 * s, 0, 200 * s, 32 * s, Align::Center, false);
    static const wchar_t* right[] = { L"ENG", L"CPU 3%", L"GPU 0%", L"RAM 41%", L"Wi-Fi", L"62%", L"Share" };
    float x = 1910 * s;
    for (const wchar_t* item : right) {
        float w = (static_cast<float>(wcslen(item)) * 8.0f + 20.0f) * s;
        x -= w + 6 * s;
        e.fill_round(x, 5 * s, w, 22 * s, 7 * s, argb(235, 38, 38, 42));
        e.text(item, 12 * s, false, kText, x, 0, w, 32 * s, Align::Center, false);
    }
    e.circle(1250 * s, 16 * s, 4 * s, kAccent);
}

static void draw_dock(Engine& e, float s)
{
    e.fill_round(0, 0, 640 * s, 64 * s, 16 * s, argb(208, 26, 26, 28));
    e.stroke_round(0.5f * s, 0.5f * s, 639 * s, 63 * s, 16 * s, argb(60, 255, 255, 255), 1.0f * s);
    for (int i = 0; i < kIcons; ++i) {
        float x = (12 + i * 51) * s;
        if (i == 4) e.fill_round(x - 4 * s, 6 * s, 48 * s, 48 * s, 12 * s, argb(52, 96, 180, 255));
        e.image(i, x, 10 * s, 40 * s, 40 * s);
        if (i % 3 == 0) e.circle(x + 20 * s, 57 * s, 2 * s, argb(255, 168, 168, 176));
        if (i == 7) {
            e.circle(x + 36 * s, 14 * s, 8 * s, argb(255, 118, 118, 126));
            e.text(L"3", 10 * s, true, argb(255, 24, 24, 28), x + 28 * s, 6 * s, 16 * s, 16 * s, Align::Center, false);
        }
    }
    e.line(330 * s, 14 * s, 330 * s, 50 * s, argb(110, 160, 160, 170), 1.0f * s);
}

static void draw_popup(Engine& e, float s)
{
    e.fill_round(0, 0, 360 * s, 480 * s, 14 * s, argb(244, 28, 28, 32));
    e.stroke_round(0.5f * s, 0.5f * s, 359 * s, 479 * s, 14 * s, argb(95, 255, 255, 255), 1.0f * s);
    e.text(L"Network", 15 * s, true, kText, 18 * s, 14 * s, 240 * s, 24 * s, Align::Near, false);
    static const wchar_t* rows[] = { L"Wi-Fi", L"Ethernet", L"Tailscale", L"Exit node", L"Airplane mode",
                                     L"Mobile hotspot" };
    float y = 50 * s;
    for (int i = 0; i < 6; ++i) {
        e.fill_round(12 * s, y, 336 * s, 44 * s, 10 * s, argb(255, 40, 40, 46));
        e.text(rows[i], 13 * s, true, kText, 26 * s, y + 4 * s, 200 * s, 20 * s, Align::Near, true);
        e.text(i % 2 ? L"Off" : L"Connected", 12 * s, false, kMuted, 26 * s, y + 22 * s, 200 * s, 18 * s, Align::Near,
               true);
        bool on = (i % 2) == 0;
        e.fill_round(292 * s, y + 12 * s, 40 * s, 20 * s, 10 * s, on ? kAccent : argb(255, 70, 70, 78));
        e.circle((on ? 322 : 302) * s, y + 22 * s, 7 * s, argb(255, 255, 255, 255));
        y += 52 * s;
    }
    y += 4 * s;
    e.line(18 * s, y, 342 * s, y, argb(110, 160, 160, 170), 1.0f * s);
    y += 10 * s;
    y += e.paragraph(L"Tailscale keeps this PC reachable from your own devices. Turning it off stops the service "
                     L"and frees its memory until you turn it on again.",
                     12 * s, kMuted, 18 * s, y, 324 * s);
    e.fill_round(18 * s, y + 10 * s, 324 * s, 30 * s, 8 * s, kAccent);
    e.text(L"Network settings", 13 * s, true, argb(255, 18, 18, 20), 18 * s, y + 10 * s, 324 * s, 30 * s,
           Align::Center, false);
}

static void draw_list(Engine& e, float s)
{
    e.fill_round(0, 0, 960 * s, 640 * s, 0, argb(255, 30, 27, 36));
    e.fill_round(0, 0, 200 * s, 640 * s, 0, argb(255, 34, 29, 40));
    e.fill_round(200 * s, 0, 760 * s, 44 * s, 0, argb(255, 43, 39, 49));
    e.fill_round(260 * s, 8 * s, 520 * s, 28 * s, 8 * s, argb(255, 56, 52, 60));
    e.text(L"Documents  ›  Notes  ›  2026", 13 * s, false, kText, 272 * s, 8 * s, 500 * s, 28 * s,
           Align::Near, true);
    static const wchar_t* side[] = { L"Home", L"Desktop", L"Documents", L"Downloads", L"Pictures", L"Music",
                                     L"Videos", L"Projects", L"Recycle Bin", L"Local Disk", L"Backup", L"USB drive" };
    for (int i = 0; i < 12; ++i) {
        float y = (14 + i * 30) * s;
        if (i == 2) e.fill_round(8 * s, y, 184 * s, 26 * s, 6 * s, argb(255, 59, 87, 134));
        e.image(i, 16 * s, y + 5 * s, 16 * s, 16 * s);
        e.text(side[i], 13 * s, false, kText, 40 * s, y, 150 * s, 26 * s, Align::Near, true);
    }
    wchar_t name[64];
    for (int i = 0; i < 26; ++i) {
        float y = (56 + i * 22) * s;
        if (i % 2) e.fill_round(200 * s, y, 760 * s, 22 * s, 0, argb(255, 38, 34, 45));
        if (i == 5) e.fill_round(204 * s, y, 752 * s, 22 * s, 4 * s, argb(255, 59, 87, 134));
        e.image(i, 210 * s, y + 3 * s, 16 * s, 16 * s);
        swprintf(name, 64, L"Meeting notes %02d, project plan and budget.md", i + 1);
        e.text(name, 13 * s, false, kText, 234 * s, y, 380 * s, 22 * s, Align::Near, true);
        e.text(L"Today 14:32", 12 * s, false, kMuted, 620 * s, y, 130 * s, 22 * s, Align::Near, false);
        swprintf(name, 64, L"%d KB", 3 + i * 7);
        e.text(name, 12 * s, false, kMuted, 750 * s, y, 70 * s, 22 * s, Align::Far, false);
        e.text(L"Markdown file", 12 * s, false, kMuted, 836 * s, y, 120 * s, 22 * s, Align::Near, true);
    }
}

static void draw_scene(Engine& e, const Scene& sc, float s)
{
    if (!strcmp(sc.name, "bar")) draw_bar(e, s);
    else if (!strcmp(sc.name, "dock")) draw_dock(e, s);
    else if (!strcmp(sc.name, "popup")) draw_popup(e, s);
    else draw_list(e, s);
}

// ---------------------------------------------------------------------------
// Readings.

static double private_mb()
{
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage / (1024.0 * 1024.0);
}

static double cpu_ms()
{
    FILETIME c, x, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
    auto v = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(k) + v(u)) / 10000.0;
}

static double now_ms()
{
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq.QuadPart;
}

static int thread_count()
{
    DWORD me = GetCurrentProcessId();
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID == me) ++n;
    }
    CloseHandle(snap);
    return n;
}

// The libraries of interest that are loaded: the drawing ones, and anything
// of Direct3D or a graphics driver.
static std::string modules_of_interest()
{
    HMODULE mods[1024];
    DWORD need = 0;
    std::string out;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return out;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
        wchar_t name[MAX_PATH];
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
        _wcslwr_s(name);
        bool want = wcsstr(name, L"d3d") || wcsstr(name, L"dxgi") || wcsstr(name, L"warp") ||
                    wcsstr(name, L"gdiplus") || wcsstr(name, L"d2d1") || wcsstr(name, L"dwrite") ||
                    wcsstr(name, L"amdxx") || wcsstr(name, L"atiu") || wcsstr(name, L"nvwgf") ||
                    wcsstr(name, L"nvd3d") || wcsstr(name, L"igd") || wcsstr(name, L"dcomp");
        if (!want) continue;
        char narrow[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, MAX_PATH, nullptr, nullptr);
        if (!out.empty()) out += ",";
        out += narrow;
    }
    return out;
}

// Writes the picture as a PNG, a see-through one laid over a plain backdrop
// so it reads the way it would on a desktop.
static bool write_png(const Canvas& c, const wchar_t* path)
{
    std::vector<uint8_t> px(static_cast<size_t>(c.w) * c.h * 4);
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            const uint8_t* s = c.bits + (static_cast<size_t>(y) * c.w + x) * 4;
            uint8_t* d = px.data() + (static_cast<size_t>(y) * c.w + x) * 4;
            int a = c.opaque ? 255 : s[3];
            const int bg[3] = { 150, 120, 96 }; // b, g, r
            for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>(std::min(255, s[k] + bg[k] * (255 - a) / 255));
            d[3] = 255;
        }
    }
    bool ok = false;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(c.w, c.h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
             SUCCEEDED(frame->WritePixels(c.h, c.w * 4, static_cast<UINT>(px.size()), px.data())) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    wic->Release();
    return ok;
}

static const Scene* find_scene(const char* name)
{
    for (const Scene& sc : kScenes) {
        if (!strcmp(sc.name, name)) return &sc;
    }
    return nullptr;
}

// One engine, one scene, in this process. Prints one line of readings.
static int run_one(const char* engine, const char* scene, float scale, int min_frames, const wchar_t* png)
{
    const Scene* sc = find_scene(scene);
    if (!sc || (strcmp(engine, "gdiplus") && strcmp(engine, "d2d"))) return 2;
    Canvas canvas;
    if (!canvas.create(static_cast<int>(sc->w * scale), static_cast<int>(sc->h * scale), sc->opaque)) return 3;

    std::string mods_start = modules_of_interest();
    double before = private_mb();
    double t0 = now_ms();
    Engine* e = strcmp(engine, "d2d") ? static_cast<Engine*>(new GdiplusEngine) : new D2dEngine;
    if (!e->init(canvas)) {
        std::printf("engine=%s scene=%s error=init\n", engine, scene);
        return 4;
    }
    double t1 = now_ms();
    e->begin();
    draw_scene(*e, *sc, scale);
    e->end();
    double first_ms = now_ms() - t1;
    double after_first = private_mb();

    // Enough frames for the clock's 15.6 ms steps to stop mattering.
    int frames = 0;
    double c0 = cpu_ms(), w0 = now_ms();
    while (frames < min_frames || now_ms() - w0 < 1500.0) {
        e->begin();
        draw_scene(*e, *sc, scale);
        e->end();
        ++frames;
    }
    double wall = now_ms() - w0, cpu = cpu_ms() - c0;
    double after = private_mb();
    std::string mods = modules_of_interest();
    int threads = thread_count();

    std::printf("engine=%s scene=%s scale=%.2f frames=%d init_ms=%.1f first_frame_ms=%.2f "
                "frame_wall_ms=%.3f frame_cpu_ms=%.3f private_before_mb=%.2f private_first_mb=%.2f "
                "private_after_mb=%.2f threads=%d modules=%s modules_at_start=%s\n",
                engine, scene, scale, frames, t1 - t0, first_ms, wall / frames, cpu / frames, before, after_first,
                after, threads, mods.c_str(), mods_start.empty() ? "none" : mods_start.c_str());
    std::fflush(stdout);
    if (png && *png) {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            write_png(canvas, png);
            CoUninitialize();
        }
    }
    delete e;
    return 0;
}

// Runs one engine and scene in a new process and returns its line.
static std::string run_child(const char* engine, const char* scene, float scale, int frames, const std::wstring& png)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t cmd[2048];
    swprintf(cmd, 2048, L"\"%ls\" run %hs %hs --scale %.2f --frames %d%ls%ls%ls", self, engine, scene, scale, frames,
             png.empty() ? L"" : L" --png \"", png.c_str(), png.empty() ? L"" : L"\"");
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::string out;
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static std::string field(const std::string& line, const char* key)
{
    std::string k = std::string(key) + "=";
    size_t at = line.find(" " + k);
    if (at == std::string::npos) {
        if (line.compare(0, k.size(), k) != 0) return "?";
        at = 0;
    } else {
        at += 1;
    }
    size_t end = line.find(' ', at);
    return line.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
}

static int compare(int frames, const wchar_t* png_dir)
{
    std::printf("| Scene | Scale | Engine | Own memory (MB) | Processor per frame (ms) | Wall per frame (ms) | "
                "Threads | Libraries |\n");
    std::printf("| :--- | ---: | :--- | ---: | ---: | ---: | ---: | :--- |\n");
    static const float scales[] = { 1.0f, 1.5f };
    static const char* engines[] = { "gdiplus", "d2d" };
    int failed = 0;
    for (float scale : scales) {
        for (const Scene& sc : kScenes) {
            for (const char* engine : engines) {
                std::wstring png;
                if (png_dir && *png_dir) {
                    wchar_t p[MAX_PATH];
                    swprintf(p, MAX_PATH, L"%ls\\%hs-%hs-%d.png", png_dir, sc.name, engine,
                             static_cast<int>(scale * 100.0f + 0.5f));
                    png = p;
                }
                std::string line = run_child(engine, sc.name, scale, frames, png);
                if (line.find("frame_cpu_ms=") == std::string::npos) {
                    std::printf("| %s | %.0f%% | %s | failed: %s | | | | |\n", sc.name, scale * 100.0f, engine,
                                line.c_str());
                    ++failed;
                    continue;
                }
                double own = std::atof(field(line, "private_after_mb").c_str()) -
                             std::atof(field(line, "private_before_mb").c_str());
                std::printf("| %s | %.0f%% | %s | %.2f | %s | %s | %s | %s |\n", sc.name, scale * 100.0f, engine, own,
                            field(line, "frame_cpu_ms").c_str(), field(line, "frame_wall_ms").c_str(),
                            field(line, "threads").c_str(), field(line, "modules").c_str());
            }
        }
    }
    std::fflush(stdout);
    return failed ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: lares-draw-bench compare [--frames N] [--png DIR]\n"
                     "       lares-draw-bench run <gdiplus|d2d> <bar|dock|popup|list> [--scale S] [--frames N] "
                     "[--png FILE]\n");
        return 2;
    }
    int frames = 60;
    float scale = 1.0f;
    std::wstring png;
    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--frames") && i + 1 < argc) frames = std::max(1, _wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--scale") && i + 1 < argc) scale = static_cast<float>(_wtof(argv[++i]));
        else if (!wcscmp(argv[i], L"--png") && i + 1 < argc) png = argv[++i];
    }
    if (!wcscmp(argv[1], L"compare")) return compare(frames, png.c_str());
    if (!wcscmp(argv[1], L"run") && argc >= 4) {
        char engine[16], scene[16];
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, engine, sizeof(engine), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, scene, sizeof(scene), nullptr, nullptr);
        return run_one(engine, scene, std::clamp(scale, 0.5f, 3.0f), frames, png.c_str());
    }
    return 2;
}
