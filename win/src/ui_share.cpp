// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* macOS Finder-inspired AirDrop Share stage for Windows: concentric radar
 * rings, vector AirDrop emblem, inline "Allow me to be discovered by:"
 * selector (Receiving Off, Contacts Only, Everyone for 10 Minutes), nearby
 * device avatars with profile photos and circular progress rings, floating
 * speech-bubble transfer popovers, OLE drag-and-drop onto device circles,
 * and local Contacts & Avatars management. */
#include "ui.h"
#include "ui_internal.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>

namespace lares {

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct AirDropPalette {
    Rgba stage_bg;
    Rgba stage_border;
    Rgba ring_stroke;
    Rgba blue;
    Rgba blue_hover;
    Rgba blue_tint;
    Rgba green;
    Rgba red;
    Rgba text;
    Rgba sub;
    Rgba muted;
    Rgba pill_bg;
    Rgba pill_hover;
    Rgba pill_border;
    Rgba popover_bg;
    Rgba popover_border;
    Rgba shadow;
    Rgba btn_bg;
    Rgba btn_hover;
    Rgba btn_border;
    Rgba track_bg;
    Rgba field_bg;
    Rgba field_border;
    Rgba backdrop;
    Rgba avatar_ring;
    Rgba cancel_bg;
};

AirDropPalette airdrop_palette(bool dark)
{
    if (dark) {
        return {
            { 0.118f, 0.118f, 0.125f, 1.0f }, // stage_bg (#1E1E20)
            { 1.0f,   1.0f,   1.0f,   0.09f }, // stage_border
            { 1.0f,   1.0f,   1.0f,   0.075f }, // ring_stroke
            { 0.039f, 0.518f, 1.0f,   1.0f }, // blue (#0A84FF)
            { 0.145f, 0.576f, 1.0f,   1.0f }, // blue_hover
            { 0.039f, 0.518f, 1.0f,   0.18f }, // blue_tint
            { 0.188f, 0.820f, 0.345f, 1.0f }, // green (#30D158)
            { 1.0f,   0.271f, 0.227f, 1.0f }, // red (#FF453A)
            { 0.961f, 0.961f, 0.969f, 1.0f }, // text (#F5F5F7)
            { 0.620f, 0.620f, 0.647f, 1.0f }, // sub (#9E9EA5)
            { 0.545f, 0.545f, 0.576f, 1.0f }, // muted
            { 1.0f,   1.0f,   1.0f,   0.07f }, // pill_bg
            { 1.0f,   1.0f,   1.0f,   0.13f }, // pill_hover
            { 1.0f,   1.0f,   1.0f,   0.12f }, // pill_border
            { 0.169f, 0.169f, 0.184f, 1.0f }, // popover_bg (#2B2B2F)
            { 1.0f,   1.0f,   1.0f,   0.15f }, // popover_border
            { 0.0f,   0.0f,   0.0f,   0.38f }, // shadow
            { 1.0f,   1.0f,   1.0f,   0.11f }, // btn_bg
            { 1.0f,   1.0f,   1.0f,   0.18f }, // btn_hover
            { 1.0f,   1.0f,   1.0f,   0.14f }, // btn_border
            { 1.0f,   1.0f,   1.0f,   0.14f }, // track_bg
            { 0.106f, 0.106f, 0.118f, 1.0f }, // field_bg
            { 1.0f,   1.0f,   1.0f,   0.18f }, // field_border
            { 0.0f,   0.0f,   0.0f,   0.48f }, // backdrop
            { 1.0f,   1.0f,   1.0f,   0.16f }, // avatar_ring
            { 0.48f,  0.48f,  0.51f,  1.0f }, // cancel_bg
        };
    }
    return {
        { 1.0f,   1.0f,   1.0f,   1.0f }, // stage_bg (#FFFFFF)
        { 0.0f,   0.0f,   0.0f,   0.085f }, // stage_border
        { 0.0f,   0.0f,   0.0f,   0.068f }, // ring_stroke
        { 0.0f,   0.478f, 1.0f,   1.0f }, // blue (#007AFF)
        { 0.0f,   0.408f, 0.890f, 1.0f }, // blue_hover
        { 0.0f,   0.478f, 1.0f,   0.10f }, // blue_tint
        { 0.204f, 0.780f, 0.349f, 1.0f }, // green (#34C759)
        { 1.0f,   0.231f, 0.188f, 1.0f }, // red (#FF3B30)
        { 0.114f, 0.114f, 0.122f, 1.0f }, // text (#1D1D1F)
        { 0.525f, 0.525f, 0.545f, 1.0f }, // sub (#86868B)
        { 0.431f, 0.431f, 0.451f, 1.0f }, // muted (#6E6E73)
        { 0.0f,   0.0f,   0.0f,   0.042f }, // pill_bg
        { 0.0f,   0.0f,   0.0f,   0.082f }, // pill_hover
        { 0.0f,   0.0f,   0.0f,   0.09f }, // pill_border
        { 0.965f, 0.969f, 0.976f, 1.0f }, // popover_bg (#F6F7F9)
        { 0.0f,   0.0f,   0.0f,   0.13f }, // popover_border
        { 0.0f,   0.0f,   0.0f,   0.13f }, // shadow
        { 0.0f,   0.0f,   0.0f,   0.065f }, // btn_bg
        { 0.0f,   0.0f,   0.0f,   0.12f }, // btn_hover
        { 0.0f,   0.0f,   0.0f,   0.12f }, // btn_border
        { 0.0f,   0.0f,   0.0f,   0.11f }, // track_bg
        { 1.0f,   1.0f,   1.0f,   1.0f }, // field_bg
        { 0.0f,   0.0f,   0.0f,   0.16f }, // field_border
        { 0.0f,   0.0f,   0.0f,   0.22f }, // backdrop
        { 0.0f,   0.0f,   0.0f,   0.10f }, // avatar_ring
        { 0.557f, 0.557f, 0.576f, 1.0f }, // cancel_bg (#8E8E93)
    };
}

double fraction(uint64_t part, uint64_t whole)
{
    return whole ? std::min(1.0, static_cast<double>(part) / static_cast<double>(whole)) : 0.0;
}

bool eq_icase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::string wide_to_utf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
}

const wchar_t* discover_mode_label(ShareDiscoverMode m)
{
    switch (m) {
    case ShareDiscoverMode::Off: return L"Receiving Off";
    case ShareDiscoverMode::ContactsOnly: return L"Contacts Only";
    case ShareDiscoverMode::Everyone: return L"Everyone for 10 Minutes";
    }
    return L"Receiving Off";
}

/* Deterministic macOS-style soft avatar gradient colors from a name. */
std::pair<Rgba, Rgba> avatar_gradient(const std::string& seed)
{
    static const std::pair<Rgba, Rgba> kPairs[] = {
        { { 0.620f, 0.651f, 0.706f, 1.0f }, { 0.486f, 0.518f, 0.576f, 1.0f } }, // macOS Contact Gray
        { { 0.353f, 0.686f, 0.980f, 1.0f }, { 0.180f, 0.506f, 0.902f, 1.0f } }, // Sky Blue
        { { 0.541f, 0.588f, 0.957f, 1.0f }, { 0.369f, 0.412f, 0.824f, 1.0f } }, // Indigo
        { { 0.957f, 0.533f, 0.490f, 1.0f }, { 0.855f, 0.376f, 0.341f, 1.0f } }, // Coral
        { { 0.451f, 0.784f, 0.545f, 1.0f }, { 0.282f, 0.635f, 0.392f, 1.0f } }, // Sage
        { { 0.961f, 0.690f, 0.341f, 1.0f }, { 0.851f, 0.537f, 0.176f, 1.0f } }, // Amber
        { { 0.784f, 0.537f, 0.898f, 1.0f }, { 0.608f, 0.369f, 0.753f, 1.0f } }, // Lavender
    };
    uint32_t h = 2166136261u;
    for (unsigned char c : seed) {
        h ^= c;
        h *= 16777619u;
    }
    return kPairs[h % (sizeof(kPairs) / sizeof(kPairs[0]))];
}

/* Queries exact DirectWrite cap-height / em-size ratio so single-line
 * buttons, inputs, menu rows and circular avatar initials center optically. */
float font_cap_ratio(IDWriteFactory* factory, IDWriteTextFormat* fmt)
{
    if (!factory || !fmt) return 0.70f;
    wchar_t fam[128] = {};
    if (FAILED(fmt->GetFontFamilyName(fam, 128))) return 0.70f;
    IDWriteFontCollection* coll = nullptr;
    if (FAILED(fmt->GetFontCollection(&coll)) || !coll) {
        if (FAILED(factory->GetSystemFontCollection(&coll)) || !coll) return 0.70f;
    }
    UINT32 idx = 0;
    BOOL exists = FALSE;
    float ratio = 0.70f;
    if (SUCCEEDED(coll->FindFamilyName(fam, &idx, &exists)) && exists) {
        IDWriteFontFamily* family = nullptr;
        if (SUCCEEDED(coll->GetFontFamily(idx, &family)) && family) {
            IDWriteFont* font = nullptr;
            if (SUCCEEDED(family->GetFirstMatchingFont(fmt->GetFontWeight(), fmt->GetFontStretch(),
                                                       fmt->GetFontStyle(), &font)) && font) {
                DWRITE_FONT_METRICS fm = {};
                font->GetMetrics(&fm);
                if (fm.designUnitsPerEm > 0 && fm.capHeight > 0) {
                    ratio = static_cast<float>(fm.capHeight) / static_cast<float>(fm.designUnitsPerEm);
                }
                font->Release();
            }
            family->Release();
        }
    }
    coll->Release();
    return ratio;
}

/* Draws single-line text optically centered vertically inside `rc` using real
 * DirectWrite baseline + cap-height metrics (eliminates top/bottom drift). */
void draw_centered_text(ID2D1RenderTarget* rt, IDWriteFactory* factory, ID2D1SolidColorBrush* brush,
                        const wchar_t* text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rc,
                        const Rgba& color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER,
                        bool ellipsis = true)
{
    if (!rt || !factory || !brush || !text || !*text || !fmt) return;
    float bw = rc.right - rc.left;
    float bh = rc.bottom - rc.top;
    if (bw <= 0.0f || bh <= 0.0f) return;
    UINT32 len = static_cast<UINT32>(wcslen(text));
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text, len, fmt, bw, bh, &layout)) || !layout) return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(align);
    if (ellipsis) {
        IDWriteInlineObject* dots = nullptr;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(fmt, &dots)) && dots) {
            DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            layout->SetTrimming(&trim, dots);
            dots->Release();
        }
    }
    DWRITE_LINE_METRICS lm = {};
    UINT32 lines = 0;
    float draw_y = rc.top;
    if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &lines)) && lines >= 1) {
        float cap_h = fmt->GetFontSize() * font_cap_ratio(factory, fmt);
        float mid_y = (rc.top + rc.bottom) * 0.5f;
        draw_y = mid_y - lm.baseline + cap_h * 0.5f;
    }
    brush->SetColor(to_d2d(color));
    rt->DrawTextLayout(D2D1::Point2F(rc.left, draw_y), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

/* Draws a crisp macOS-style rounded-square + diagonal pencil Edit icon. */
void draw_edit_icon(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                    ID2D1StrokeStyle* round_cap, float cx, float cy, float s, const Rgba& color)
{
    if (!factory || !rt || !brush) return;
    brush->SetColor(to_d2d(color));
    const float lw = 1.45f;
    const float cr = s * 0.16f;
    // Open rounded square (left, bottom, partial top & right)
    ID2D1PathGeometry* box = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&box)) && box) {
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(box->Open(&sink)) && sink) {
            float l = cx - s * 0.42f;
            float r = cx + s * 0.36f;
            float t = cy - s * 0.36f;
            float b = cy + s * 0.42f;
            sink->BeginFigure(D2D1::Point2F(cx + s * 0.02f, t), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(l + cr, t));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l, t + cr), D2D1::SizeF(cr, cr), 0.0f,
                                          D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(l, b - cr));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l + cr, b), D2D1::SizeF(cr, cr), 0.0f,
                                          D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(r - cr, b));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r, b - cr), D2D1::SizeF(cr, cr), 0.0f,
                                          D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(r, cy + s * 0.02f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            rt->DrawGeometry(box, brush, lw, round_cap);
        }
        box->Release();
    }

    // Diagonal pencil shaft + triangular nib + top cap
    auto fill_quad = [&](D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_POINT_2F p2, D2D1_POINT_2F p3) {
        ID2D1PathGeometry* g = nullptr;
        if (FAILED(factory->CreatePathGeometry(&g)) || !g) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(g->Open(&sink)) && sink) {
            sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(p1);
            sink->AddLine(p2);
            sink->AddLine(p3);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            rt->FillGeometry(g, brush);
        }
        g->Release();
    };
    const float ux = 0.7071f, uy = -0.7071f; // along pencil axis (bottom-left -> top-right)
    const float nx = 0.7071f, ny = 0.7071f;  // perpendicular half-width
    const float hw = s * 0.105f;
    const float tip_x = cx - s * 0.14f;
    const float tip_y = cy + s * 0.14f;
    auto pt = [&](float along, float perp) {
        return D2D1::Point2F(tip_x + ux * along + nx * perp, tip_y + uy * along + ny * perp);
    };
    // Triangular nib
    ID2D1PathGeometry* nib = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&nib)) && nib) {
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(nib->Open(&sink)) && sink) {
            sink->BeginFigure(pt(0.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(pt(s * 0.19f, -hw));
            sink->AddLine(pt(s * 0.19f, hw));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            rt->FillGeometry(nib, brush);
        }
        nib->Release();
    }
    // Shaft body
    fill_quad(pt(s * 0.24f, -hw), pt(s * 0.65f, -hw), pt(s * 0.65f, hw), pt(s * 0.24f, hw));
    // Eraser cap
    fill_quad(pt(s * 0.70f, -hw), pt(s * 0.81f, -hw), pt(s * 0.81f, hw), pt(s * 0.70f, hw));
}

/* Draws a crisp macOS-style Trash Can icon (handle, lid, rounded bin + vertical ribs). */
void draw_trash_icon(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                     ID2D1StrokeStyle* round_cap, float cx, float cy, float s, const Rgba& color)
{
    if (!factory || !rt || !brush) return;
    brush->SetColor(to_d2d(color));
    const float lw = 1.45f;

    // Lid horizontal bar
    rt->DrawLine(D2D1::Point2F(cx - s * 0.39f, cy - s * 0.24f),
                 D2D1::Point2F(cx + s * 0.39f, cy - s * 0.24f), brush, lw + 0.15f, round_cap);

    // Centered top handle arch
    ID2D1PathGeometry* handle = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&handle)) && handle) {
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(handle->Open(&sink)) && sink) {
            float hr = s * 0.10f;
            float hl = cx - s * 0.16f;
            float hr_x = cx + s * 0.16f;
            float ht = cy - s * 0.43f;
            float hb = cy - s * 0.25f;
            sink->BeginFigure(D2D1::Point2F(hl, hb), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(hl, ht + hr));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(hl + hr, ht), D2D1::SizeF(hr, hr), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(hr_x - hr, ht));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(hr_x, ht + hr), D2D1::SizeF(hr, hr), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(hr_x, hb));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            rt->DrawGeometry(handle, brush, lw, round_cap);
        }
        handle->Release();
    }

    // Rounded bin outline
    ID2D1PathGeometry* bin = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&bin)) && bin) {
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(bin->Open(&sink)) && sink) {
            float br = s * 0.14f;
            float bl = cx - s * 0.29f;
            float br_x = cx + s * 0.29f;
            float bt = cy - s * 0.12f;
            float bb = cy + s * 0.43f;
            sink->BeginFigure(D2D1::Point2F(bl, bt), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(bl + s * 0.03f, bb - br));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(bl + s * 0.03f + br, bb), D2D1::SizeF(br, br), 0.0f,
                                          D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(br_x - s * 0.03f - br, bb));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(br_x - s * 0.03f, bb - br), D2D1::SizeF(br, br), 0.0f,
                                          D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(br_x, bt));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            rt->DrawGeometry(bin, brush, lw, round_cap);
        }
        bin->Release();
    }

    // Two clean inner vertical ribs
    rt->DrawLine(D2D1::Point2F(cx - s * 0.095f, cy - s * 0.01f),
                 D2D1::Point2F(cx - s * 0.085f, cy + s * 0.28f), brush, lw - 0.15f, round_cap);
    rt->DrawLine(D2D1::Point2F(cx + s * 0.095f, cy - s * 0.01f),
                 D2D1::Point2F(cx + s * 0.085f, cy + s * 0.28f), brush, lw - 0.15f, round_cap);
}

/* Draws the official Apple AirDrop vector glyph: center solid dot + 3 open-bottom arcs. */
void draw_airdrop_emblem(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                         ID2D1StrokeStyle* round_cap, float cx, float cy, float scale, const Rgba& color)
{
    if (!factory || !rt || !brush) return;
    brush->SetColor(to_d2d(color));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 4.6f * scale, 4.6f * scale), brush);

    static const float kRadii[3] = { 12.6f, 20.6f, 28.6f };
    float stroke = 3.5f * scale;
    float gap_half = 34.0f * (kPi / 180.0f);
    float a_start = kPi * 0.5f + gap_half; // bottom-left (~124 deg)
    float a_end = kPi * 0.5f - gap_half;   // bottom-right (~56 deg)

    for (float base_r : kRadii) {
        float r = base_r * scale;
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) continue;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(geo->Open(&sink)) && sink) {
            D2D1_POINT_2F p0 = D2D1::Point2F(cx + r * std::cos(a_start), cy + r * std::sin(a_start));
            D2D1_POINT_2F p1 = D2D1::Point2F(cx + r * std::cos(a_end), cy + r * std::sin(a_end));
            sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(r, r), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            rt->DrawGeometry(geo, brush, stroke, round_cap);
        }
        geo->Release();
    }
}

/* Draws a circular progress arc starting at 12 o'clock (-90 deg) clockwise. */
void draw_progress_arc(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                       ID2D1StrokeStyle* round_cap, float cx, float cy, float r, double frac,
                       float stroke_w, const Rgba& color)
{
    if (!factory || !rt || !brush || frac <= 0.001) return;
    brush->SetColor(to_d2d(color));
    if (frac >= 0.999) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, stroke_w);
        return;
    }
    float a0 = -kPi * 0.5f;
    float a1 = a0 + static_cast<float>(frac * 2.0 * kPi);
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(geo->Open(&sink)) && sink) {
        D2D1_POINT_2F p0 = D2D1::Point2F(cx + r * std::cos(a0), cy + r * std::sin(a0));
        D2D1_POINT_2F p1 = D2D1::Point2F(cx + r * std::cos(a1), cy + r * std::sin(a1));
        sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(r, r), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      frac > 0.5 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
        rt->DrawGeometry(geo, brush, stroke_w, round_cap);
    }
    geo->Release();
}

/* Crisp vector chevron pointing down, for the inline macOS discoverability trigger. */
void draw_chevron_down(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* round_cap,
                       float cx, float cy, float size, const Rgba& color)
{
    brush->SetColor(to_d2d(color));
    float hw = size * 0.5f;
    float hh = size * 0.28f;
    D2D1_POINT_2F l = D2D1::Point2F(cx - hw, cy - hh);
    D2D1_POINT_2F m = D2D1::Point2F(cx, cy + hh);
    D2D1_POINT_2F r = D2D1::Point2F(cx + hw, cy - hh);
    rt->DrawLine(l, m, brush, 1.6f, round_cap);
    rt->DrawLine(m, r, brush, 1.6f, round_cap);
}

/* Crisp vector checkmark for the open macOS menu. */
void draw_checkmark(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* round_cap,
                    float x, float cy, const Rgba& color)
{
    brush->SetColor(to_d2d(color));
    D2D1_POINT_2F p0 = D2D1::Point2F(x, cy + 0.5f);
    D2D1_POINT_2F p1 = D2D1::Point2F(x + 3.2f, cy + 3.8f);
    D2D1_POINT_2F p2 = D2D1::Point2F(x + 9.2f, cy - 3.6f);
    rt->DrawLine(p0, p1, brush, 1.7f, round_cap);
    rt->DrawLine(p1, p2, brush, 1.7f, round_cap);
}

/* Draws a crisp minimal vector device glyph inside a small square box. */
void draw_device_glyph(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                       float cx, float cy, float s, ShareDeviceKind kind, const Rgba& color)
{
    brush->SetColor(to_d2d(color));
    float stroke = std::max(1.2f, s * 0.10f);
    switch (kind) {
    case ShareDeviceKind::Phone: {
        float w = s * 0.54f, h = s * 0.88f;
        D2D1_RECT_F r = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, s * 0.12f, s * 0.12f), brush, stroke);
        rt->DrawLine(D2D1::Point2F(cx - w * 0.18f, r.top + h * 0.16f),
                     D2D1::Point2F(cx + w * 0.18f, r.top + h * 0.16f), brush, stroke);
        break;
    }
    case ShareDeviceKind::Tablet: {
        float w = s * 0.68f, h = s * 0.86f;
        D2D1_RECT_F r = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, s * 0.11f, s * 0.11f), brush, stroke);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, r.bottom - h * 0.12f), stroke * 0.65f, stroke * 0.65f), brush);
        break;
    }
    case ShareDeviceKind::Laptop: {
        float w = s * 0.78f, h = s * 0.50f;
        D2D1_RECT_F scr = D2D1::RectF(cx - w * 0.5f, cy - h * 0.58f, cx + w * 0.5f, cy + h * 0.42f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(scr, s * 0.08f, s * 0.08f), brush, stroke);
        float base_y = scr.bottom + stroke * 1.15f;
        rt->DrawLine(D2D1::Point2F(cx - s * 0.50f, base_y), D2D1::Point2F(cx + s * 0.50f, base_y), brush, stroke * 1.2f);
        break;
    }
    case ShareDeviceKind::Desktop: {
        float w = s * 0.82f, h = s * 0.56f;
        D2D1_RECT_F scr = D2D1::RectF(cx - w * 0.5f, cy - h * 0.64f, cx + w * 0.5f, cy + h * 0.36f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(scr, s * 0.08f, s * 0.08f), brush, stroke);
        rt->DrawLine(D2D1::Point2F(cx, scr.bottom), D2D1::Point2F(cx, scr.bottom + s * 0.18f), brush, stroke);
        rt->DrawLine(D2D1::Point2F(cx - s * 0.22f, scr.bottom + s * 0.18f),
                     D2D1::Point2F(cx + s * 0.22f, scr.bottom + s * 0.18f), brush, stroke);
        break;
    }
    case ShareDeviceKind::Watch: {
        float w = s * 0.46f, h = s * 0.56f;
        D2D1_RECT_F face = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(face, s * 0.12f, s * 0.12f), brush, stroke);
        rt->DrawLine(D2D1::Point2F(cx - w * 0.25f, face.top - s * 0.16f),
                     D2D1::Point2F(cx + w * 0.25f, face.top - s * 0.16f), brush, stroke);
        rt->DrawLine(D2D1::Point2F(cx - w * 0.25f, face.bottom + s * 0.16f),
                     D2D1::Point2F(cx + w * 0.25f, face.bottom + s * 0.16f), brush, stroke);
        break;
    }
    case ShareDeviceKind::Other: {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.36f, s * 0.36f), brush, stroke);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.11f, s * 0.11f), brush);
        break;
    }
    }
}

/* Draws a crisp vector file/link/photo thumbnail tile when no bitmap preview is available. */
void draw_file_icon_tile(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* round_cap,
                         const D2D1_RECT_F& rc, bool is_link, bool is_image, bool dark)
{
    Rgba bg = dark ? Rgba{ 0.22f, 0.25f, 0.32f, 1.0f } : Rgba{ 0.90f, 0.93f, 0.98f, 1.0f };
    Rgba fg = dark ? Rgba{ 0.45f, 0.70f, 1.0f, 1.0f } : Rgba{ 0.0f, 0.478f, 1.0f, 1.0f };
    brush->SetColor(to_d2d(bg));
    rt->FillRoundedRectangle(D2D1::RoundedRect(rc, 7.0f, 7.0f), brush);

    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    brush->SetColor(to_d2d(fg));
    if (is_link) {
        D2D1_RECT_F l1 = D2D1::RectF(cx - 11.0f, cy - 4.5f, cx + 2.0f, cy + 4.5f);
        D2D1_RECT_F l2 = D2D1::RectF(cx - 2.0f, cy - 4.5f, cx + 11.0f, cy + 4.5f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(l1, 4.5f, 4.5f), brush, 2.0f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(l2, 4.5f, 4.5f), brush, 2.0f);
    } else if (is_image) {
        D2D1_RECT_F fr = D2D1::RectF(cx - 12.0f, cy - 10.0f, cx + 12.0f, cy + 10.0f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(fr, 3.0f, 3.0f), brush, 1.8f);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 5.5f, cy - 4.0f), 2.3f, 2.3f), brush);
        rt->DrawLine(D2D1::Point2F(fr.left + 2.5f, fr.bottom - 2.5f),
                     D2D1::Point2F(cx - 1.0f, cy + 0.5f), brush, 1.8f, round_cap);
        rt->DrawLine(D2D1::Point2F(cx - 1.0f, cy + 0.5f),
                     D2D1::Point2F(cx + 3.0f, cy + 4.0f), brush, 1.8f, round_cap);
        rt->DrawLine(D2D1::Point2F(cx + 3.0f, cy + 4.0f),
                     D2D1::Point2F(cx + 7.5f, cy - 1.5f), brush, 1.8f, round_cap);
        rt->DrawLine(D2D1::Point2F(cx + 7.5f, cy - 1.5f),
                     D2D1::Point2F(fr.right - 2.5f, fr.bottom - 2.5f), brush, 1.8f, round_cap);
    } else {
        D2D1_RECT_F doc = D2D1::RectF(cx - 9.5f, cy - 12.0f, cx + 9.5f, cy + 12.0f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(doc, 2.5f, 2.5f), brush, 1.8f);
        rt->DrawLine(D2D1::Point2F(doc.left + 4.0f, cy - 4.0f), D2D1::Point2F(doc.right - 4.0f, cy - 4.0f), brush, 1.5f, round_cap);
        rt->DrawLine(D2D1::Point2F(doc.left + 4.0f, cy + 1.0f), D2D1::Point2F(doc.right - 4.0f, cy + 1.0f), brush, 1.5f, round_cap);
        rt->DrawLine(D2D1::Point2F(doc.left + 4.0f, cy + 6.0f), D2D1::Point2F(doc.right - 7.5f, cy + 6.0f), brush, 1.5f, round_cap);
    }
}

bool looks_like_image_name(const std::string& name)
{
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "heic" || ext == "webp" || ext == "gif" || ext == "bmp";
}

/* Draws a rounded speech-bubble popover with a downward pointer triangle aimed at tip_x. */
void draw_speech_popover(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                         const D2D1_RECT_F& box, float tip_x, float radius,
                         const Rgba& bg, const Rgba& border, const Rgba& shadow)
{
    if (!factory || !rt || !brush) return;
    float tw = 9.0f;
    float th = 8.0f;
    float cx = std::clamp(tip_x, box.left + radius + tw + 4.0f, box.right - radius - tw - 4.0f);

    auto make_bubble = [&](float dx, float dy) -> ID2D1PathGeometry* {
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(geo->Open(&sink)) || !sink) {
            geo->Release();
            return nullptr;
        }
        float l = box.left + dx, t = box.top + dy, r = box.right + dx, b = box.bottom + dy;
        float tx = cx + dx;
        D2D1_SIZE_F arc = D2D1::SizeF(radius, radius);
        sink->BeginFigure(D2D1::Point2F(l + radius, t), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(r - radius, t));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r, t + radius), arc, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(r, b - radius));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r - radius, b), arc, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(tx + tw, b));
        sink->AddLine(D2D1::Point2F(tx, b + th));
        sink->AddLine(D2D1::Point2F(tx - tw, b));
        sink->AddLine(D2D1::Point2F(l + radius, b));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l, b - radius), arc, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(l, t + radius));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l + radius, t), arc, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
        return geo;
    };

    // Soft two-step drop shadow
    for (int step = 0; step < 2; ++step) {
        float off = step == 0 ? 4.0f : 2.0f;
        Rgba sh = shadow;
        sh.a *= (step == 0 ? 0.55f : 0.75f);
        if (ID2D1PathGeometry* sgeo = make_bubble(0.0f, off)) {
            brush->SetColor(to_d2d(sh));
            rt->FillGeometry(sgeo, brush);
            sgeo->Release();
        }
    }
    if (ID2D1PathGeometry* geo = make_bubble(0.0f, 0.0f)) {
        brush->SetColor(to_d2d(bg));
        rt->FillGeometry(geo, brush);
        brush->SetColor(to_d2d(border));
        rt->DrawGeometry(geo, brush, 1.0f);
        geo->Release();
    }
}

struct RadarNode {
    std::string key;           // ShareDevice::key, or "__incoming__" if synthetic
    std::string raw_name;      // exact device/sender name advertised over AirDrop
    std::string display_name;
    std::string subtitle;
    ShareDeviceKind kind = ShareDeviceKind::Phone;
    std::wstring photo_path;
    bool has_device = false;
    ShareDevice dev;
    bool has_incoming = false;
    ShareIncoming inc;
    float cx = 0.0f;
    float cy = 0.0f;
};

/* Suggests a clean person name from an Apple device name (e.g. "Nahuel’s iPhone" -> "Nahuel"). */
std::string suggest_person_name(const std::string& raw)
{
    size_t pos = raw.find("\xE2\x80\x99s ");
    if (pos == std::string::npos) pos = raw.find("'s ");
    if (pos != std::string::npos && pos >= 2) return raw.substr(0, pos);
    return raw;
}

int popover_priority(const RadarNode& n)
{
    if (n.has_incoming) return 3;
    if (n.has_device) {
        if (n.dev.state == ShareDeviceState::Sending || n.dev.state == ShareDeviceState::Asking) return 2;
        if (n.dev.state != ShareDeviceState::Found) return 1;
    }
    return 0;
}

bool has_popover(const RadarNode& n)
{
    return popover_priority(n) > 0;
}

} // namespace

// --------------------------------------------------------------- WIC thumbnails

ID2D1Bitmap* MainWindow::load_share_bitmap(const std::wstring& path)
{
    if (path.empty() || !dc_rt_) return nullptr;
    auto it = share_bitmaps_.find(path);
    if (it != share_bitmaps_.end()) return it->second;

    // Cap cache size so memory stays tiny even if many thumbnails arrive.
    if (share_bitmaps_.size() >= 24) {
        for (auto& kv : share_bitmaps_) {
            if (kv.second) kv.second->Release();
        }
        share_bitmaps_.clear();
    }
    share_bitmaps_[path] = nullptr;

    if (!wic_factory_) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&wic_factory_))) || !wic_factory_) {
            return nullptr;
        }
    }

    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(wic_factory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder) {
        return nullptr;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    ID2D1Bitmap* out = nullptr;
    if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
        UINT w = 0, h = 0;
        if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0) {
            constexpr UINT kMaxDim = 144;
            IWICBitmapSource* src = frame;
            IWICBitmapScaler* scaler = nullptr;
            if (w > kMaxDim || h > kMaxDim) {
                double scale = static_cast<double>(kMaxDim) / static_cast<double>(std::max(w, h));
                UINT nw = std::max<UINT>(1, static_cast<UINT>(std::lround(w * scale)));
                UINT nh = std::max<UINT>(1, static_cast<UINT>(std::lround(h * scale)));
                if (SUCCEEDED(wic_factory_->CreateBitmapScaler(&scaler)) && scaler &&
                    SUCCEEDED(scaler->Initialize(frame, nw, nh, WICBitmapInterpolationModeFant))) {
                    src = scaler;
                }
            }
            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(wic_factory_->CreateFormatConverter(&conv)) && conv) {
                if (SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeMedianCut))) {
                    dc_rt_->CreateBitmapFromWicBitmap(conv, nullptr, &out);
                }
                conv->Release();
            }
            if (scaler) scaler->Release();
        }
        frame->Release();
    }
    decoder->Release();
    share_bitmaps_[path] = out;
    return out;
}

void MainWindow::release_share_bitmaps()
{
    for (auto& kv : share_bitmaps_) {
        if (kv.second) kv.second->Release();
    }
    share_bitmaps_.clear();
    if (wic_factory_) {
        wic_factory_->Release();
        wic_factory_ = nullptr;
    }
}

// --------------------------------------------------------------- macOS AirDrop UI

void MainWindow::draw_share_page(ID2D1RenderTarget* rt, const Palette&, float x, float y, float w, float h)
{
    share_hits_.clear();
    share_circles_.clear();
    const ShareView& v = share_view_;
    const ShareSession& s = v.s;
    const AirDropPalette ap = airdrop_palette(dark_);
    const bool overlay_open = share_mode_menu_open_ || share_adapter_open_ || share_contacts_open_ || share_ctx_open_;

    auto hover_bg = [&](const D2D1_RECT_F& rc) {
        return !overlay_open && mouse_in_ && drop_open_.empty() && point_in_rect(mouse_x_, mouse_y_, rc);
    };
    auto hover_top = [&](const D2D1_RECT_F& rc) {
        return mouse_in_ && drop_open_.empty() && point_in_rect(mouse_x_, mouse_y_, rc);
    };

    // Helper for sleek macOS-style buttons inside the AirDrop stage/popovers.
    auto mac_button = [&](const D2D1_RECT_F& rc, const wchar_t* text, bool primary, bool in_overlay,
                          ShareActionKind kind, const std::string& value = {}) {
        bool hov = in_overlay ? hover_top(rc) : hover_bg(rc);
        Rgba bg = primary ? (hov ? ap.blue_hover : ap.blue) : (hov ? ap.btn_hover : ap.btn_bg);
        fill(rt, rc, bg, 7.5f);
        if (!primary) {
            brush_->SetColor(to_d2d(ap.btn_border));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(rc, 7.5f, 7.5f), brush_, 1.0f);
        }
        Rgba fg = primary ? Rgba{ 1.0f, 1.0f, 1.0f, 1.0f } : ap.text;
        D2D1_RECT_F trc = D2D1::RectF(rc.left + 8.0f, rc.top, rc.right - 8.0f, rc.bottom);
        draw_centered_text(rt, fonts_.factory(), brush_, text, fonts_.ui_sub_semibold(), trc, fg,
                           DWRITE_TEXT_ALIGNMENT_CENTER);
        share_hits_.push_back({ rc, kind, value });
    };

    // Full macOS AirDrop stage surface (pure white light / deep Finder dark)
    const float stage_r = 16.0f;
    D2D1_RECT_F stage = D2D1::RectF(x, y, x + w, y + h);
    fill(rt, stage, ap.stage_bg, stage_r);

    // Clip concentric radar rings cleanly to the rounded stage
    const float cx = x + w * 0.5f;
    const float cy = y + std::clamp(h * 0.70f, 290.0f, h - 134.0f);
    const float ring_step = std::clamp(std::min(w * 0.112f, (cy - y) * 0.195f), 56.0f, 76.0f);

    ID2D1RoundedRectangleGeometry* stage_geo = nullptr;
    ID2D1Layer* clip_layer = nullptr;
    bool pushed_layer = false;
    if (d2d_factory_ && SUCCEEDED(d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(stage, stage_r, stage_r), &stage_geo)) &&
        stage_geo && SUCCEEDED(rt->CreateLayer(&clip_layer)) && clip_layer) {
        rt->PushLayer(D2D1::LayerParameters(stage, stage_geo), clip_layer);
        pushed_layer = true;
    }

    // Concentric crisp radar rings centered on the AirDrop emblem
    brush_->SetColor(to_d2d(ap.ring_stroke));
    float max_r = std::hypot(std::max(cx - x, x + w - cx), std::max(cy - y, y + h - cy)) + ring_step;
    for (int k = 1; k * ring_step <= max_r; ++k) {
        float rr = k * ring_step;
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rr, rr), brush_, 1.0f);
    }

    // --------------------------------------------------------- Setup / NoAdapter view
    if (v.setup != ShareSetup::Ready) {
        Rgba logo_c = v.setup == ShareSetup::NoAdapter ? ap.sub : ap.blue;
        draw_airdrop_emblem(d2d_factory_, rt, brush_, share_round_cap_, cx, cy - 36.0f, 1.12f, logo_c);

        const wchar_t* title = v.setup == ShareSetup::NoAdapter
                                   ? L"AirDrop needs a USB Wi-Fi adapter on Windows"
                                   : L"Set up AirDrop for this PC";
        label(rt, title, fonts_.ui_title(), x + 36.0f, cy + 16.0f, w - 72.0f, ap.text, DWRITE_TEXT_ALIGNMENT_CENTER);

        const wchar_t* desc = v.setup == ShareSetup::NoAdapter
                                  ? L"Plug in a Realtek RTL8188EU USB Wi-Fi adapter so Lares can send and receive with nearby iPhones, iPads and Macs."
                                  : L"Lares gives the Realtek USB Wi-Fi adapter Windows' WinUSB driver once (administrator permission required) so it can talk directly to Apple devices.";
        float dw = std::min(460.0f, w - 72.0f);
        IDWriteTextLayout* layout = nullptr;
        UINT32 dlen = static_cast<UINT32>(wcslen(desc));
        if (SUCCEEDED(fonts_.factory()->CreateTextLayout(desc, dlen, fonts_.ui_body(), dw, 160.0f, &layout)) && layout) {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush_->SetColor(to_d2d(ap.sub));
            rt->DrawTextLayout(D2D1::Point2F(std::round(cx - dw * 0.5f), std::round(cy + 48.0f)), layout, brush_);
            layout->Release();
        }

        if (v.setup == ShareSetup::NeedsSetup) {
            const wchar_t* btext = v.setup_busy ? L"Setting up…" : L"Set up AirDrop";
            float bw = std::max(150.0f, text_width(btext, fonts_.ui_semibold()) + 36.0f);
            D2D1_RECT_F brc = D2D1::RectF(cx - bw * 0.5f, cy + 104.0f, cx + bw * 0.5f, cy + 136.0f);
            if (v.setup_busy) {
                fill(rt, brc, ap.pill_bg, 8.0f);
                label(rt, btext, fonts_.ui_semibold(), brc.left, brc.top + 7.0f, bw, ap.sub, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                mac_button(brc, btext, true, false, ShareActionKind::Setup);
            }
        }
        if (!v.setup_error.empty()) {
            std::wstring err = utf8_to_wide(v.setup_error);
            label(rt, err.c_str(), fonts_.ui_sub(), x + 36.0f, cy + 148.0f, w - 72.0f, ap.red, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (pushed_layer) rt->PopLayer();
        if (clip_layer) clip_layer->Release();
        if (stage_geo) stage_geo->Release();
        brush_->SetColor(to_d2d(ap.stage_border));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(stage, stage_r, stage_r), brush_, 1.0f);
        content_height_ = h + kPagePadTop * 2.0f;
        return;
    }

    // --------------------------------------------------------- Ready macOS AirDrop stage

    // Subtle drag-and-drop highlight around the whole canvas when dragging files
    if (share_drag_active_ && share_drag_key_.empty()) {
        Rgba tint = ap.blue;
        tint.a = dark_ ? 0.08f : 0.05f;
        fill(rt, stage, tint, stage_r);
    }

    // Centered official Apple AirDrop vector logo
    draw_airdrop_emblem(d2d_factory_, rt, brush_, share_round_cap_, cx, cy, 1.04f, ap.blue);

    // Tagline directly under the emblem (matches macOS Finder verbatim)
    const wchar_t* tagline = L"AirDrop lets you share instantly with people nearby.";
    label(rt, tagline, fonts_.ui_body(), x + 24.0f, cy + 44.0f, w - 48.0f, ap.text, DWRITE_TEXT_ALIGNMENT_CENTER);

    // Inline blue "Allow me to be discovered by: <Mode> ⌄" dropdown trigger
    std::wstring discover_text = std::wstring(L"Allow me to be discovered by: ") + discover_mode_label(s.mode);
    float dtw = text_width(discover_text.c_str(), fonts_.ui_body());
    float trig_w = dtw + 26.0f;
    float trig_h = 28.0f;
    D2D1_RECT_F trig_rc = D2D1::RectF(cx - trig_w * 0.5f, cy + 72.0f, cx + trig_w * 0.5f, cy + 72.0f + trig_h);
    if (share_mode_menu_open_ || hover_bg(trig_rc)) {
        fill(rt, trig_rc, ap.blue_tint, 7.0f);
    }
    label(rt, discover_text.c_str(), fonts_.ui_body(), trig_rc.left + 8.0f, trig_rc.top + 5.0f, dtw + 2.0f, ap.blue);
    draw_chevron_down(rt, brush_, share_round_cap_, trig_rc.right - 10.5f, trig_rc.top + trig_h * 0.5f + 0.5f, 8.0f, ap.blue);
    share_hits_.push_back({ trig_rc, ShareActionKind::OpenDiscoverMenu, {} });

    // Subtle live status / countdown line underneath the discoverability dropdown
    std::wstring status_line;
    Rgba status_color = ap.sub;
    std::wstring pc_name = utf8_to_wide(s.name.empty() ? std::string("this PC") : s.name);
    if (s.link == ShareLink::Failed) {
        status_line = s.error.empty() ? std::wstring(L"AirDrop stopped.") : utf8_to_wide(s.error);
        status_color = ap.red;
    } else if (!v.error.empty()) {
        status_line = utf8_to_wide(v.error);
        status_color = ap.red;
    } else if (s.mode == ShareDiscoverMode::Everyone) {
        ULONGLONG gone = (GetTickCount64() - v.left_at_ms) / 1000;
        int left = std::max(0, s.left - static_cast<int>(std::min<ULONGLONG>(gone, 24 * 3600)));
        wchar_t tbuf[32];
        swprintf(tbuf, 32, L" · %d:%02d left", left / 60, left % 60);
        status_line = (s.link == ShareLink::Up ? L"Visible as " : L"Starting AirDrop as ") + pc_name + tbuf;
    } else if (s.mode == ShareDiscoverMode::ContactsOnly) {
        if (v.contacts.empty()) {
            status_line = L"Contacts Only · Add a contact to verify nearby Apple devices";
        } else {
            status_line = L"Visible to " + std::to_wstring(v.contacts.size()) +
                          (v.contacts.size() == 1 ? L" contact as " : L" contacts as ") + pc_name;
        }
    } else if (share_drag_active_) {
        status_line = L"Drop files on a nearby device to send immediately";
        status_color = ap.blue;
    } else if (s.devices.empty() && !s.has_incoming) {
        if (s.quiet && s.link == ShareLink::Up) {
            status_line = L"No Apple devices found nearby yet · Open AirDrop on your iPhone, iPad or Mac";
        } else if (s.browsing) {
            status_line = L"Looking for nearby Apple devices…";
        }
    } else {
        status_line = L"Click a person or drop files onto them to share · Right-click to add to contacts or set a photo";
    }
    if (!status_line.empty()) {
        label(rt, status_line.c_str(), fonts_.ui_sub(), x + 24.0f, trig_rc.bottom + 8.0f, w - 48.0f,
              status_color, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // --------------------------------------------------------- Top-right subtle macOS toolbar pills
    float pill_y = y + 14.0f;
    float pill_h = 28.0f;
    const wchar_t* adapter_lbl = L"Adapter";
    float adapter_w = text_width(adapter_lbl, fonts_.ui_sub()) + 22.0f;
    D2D1_RECT_F adapter_rc = D2D1::RectF(x + w - 16.0f - adapter_w, pill_y, x + w - 16.0f, pill_y + pill_h);

    std::wstring contacts_lbl = L"Contacts";
    if (!v.contacts.empty()) contacts_lbl += L" (" + std::to_wstring(v.contacts.size()) + L")";
    float contacts_w = text_width(contacts_lbl.c_str(), fonts_.ui_sub()) + 22.0f;
    D2D1_RECT_F contacts_rc = D2D1::RectF(adapter_rc.left - 8.0f - contacts_w, pill_y, adapter_rc.left - 8.0f, pill_y + pill_h);

    auto toolbar_pill = [&](const D2D1_RECT_F& rc, const wchar_t* txt, bool active, ShareActionKind kind) {
        bool hov = hover_bg(rc);
        Rgba bg = active ? ap.blue_tint : hov ? ap.pill_hover : ap.pill_bg;
        fill(rt, rc, bg, 7.5f);
        brush_->SetColor(to_d2d(active ? ap.blue : ap.pill_border));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(rc, 7.5f, 7.5f), brush_, 1.0f);
        draw_centered_text(rt, fonts_.factory(), brush_, txt, fonts_.ui_sub_semibold(), rc,
                           active ? ap.blue : ap.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        share_hits_.push_back({ rc, kind, {} });
    };
    toolbar_pill(contacts_rc, contacts_lbl.c_str(), share_contacts_open_, ShareActionKind::ToggleContacts);
    toolbar_pill(adapter_rc, adapter_lbl, share_adapter_open_, ShareActionKind::ToggleAdapter);

    // Top-left staged files pill (when files are queued for click-to-send)
    if (s.sending && !v.files.empty()) {
        std::wstring pill_txt = L"Ready to send: " + v.files[0];
        if (v.files.size() > 1) pill_txt += L" +" + std::to_wstring(v.files.size() - 1);
        float max_pw = std::max(140.0f, contacts_rc.left - x - 48.0f);
        float pw = std::min(max_pw, text_width(pill_txt.c_str(), fonts_.ui_sub()) + 40.0f);
        D2D1_RECT_F prc = D2D1::RectF(x + 16.0f, pill_y, x + 16.0f + pw, pill_y + pill_h);
        fill(rt, prc, ap.blue_tint, 7.0f);
        brush_->SetColor(to_d2d(ap.blue));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(prc, 7.0f, 7.0f), brush_, 1.0f);
        label(rt, pill_txt.c_str(), fonts_.ui_sub(), prc.left + 10.0f, prc.top + 6.0f, pw - 34.0f, ap.blue);
        D2D1_RECT_F clr = D2D1::RectF(prc.right - 22.0f, prc.top + 5.0f, prc.right - 4.0f, prc.bottom - 5.0f);
        label(rt, L"×", fonts_.ui_semibold(), clr.left, clr.top - 1.0f, 18.0f, ap.blue, DWRITE_TEXT_ALIGNMENT_CENTER);
        share_hits_.push_back({ clr, ShareActionKind::ClearStagedFiles, {} });
    }

    // --------------------------------------------------------- Unified RadarNode list
    std::vector<RadarNode> nodes;
    bool incoming_attached = false;
    for (const ShareDevice& d : s.devices) {
        RadarNode n;
        n.key = d.key;
        n.has_device = true;
        n.dev = d;
        std::string raw_name = d.name.empty() ? std::string("Apple Device") : d.name;
        n.raw_name = raw_name;
        const ShareContact* contact = share_find_contact(v.contacts, raw_name);
        n.display_name = (contact && !contact->name.empty()) ? contact->name : raw_name;
        n.subtitle = share_model_label(d.model, raw_name);
        n.kind = share_device_kind(d.model, raw_name);
        if (contact && !contact->photo.empty()) n.photo_path = utf8_to_wide(contact->photo);
        if (s.has_incoming && !incoming_attached && !s.incoming.sender.empty() && eq_icase(raw_name, s.incoming.sender)) {
            n.has_incoming = true;
            n.inc = s.incoming;
            if (d.model.empty() && !s.incoming.sender_model.empty()) {
                n.subtitle = share_model_label(s.incoming.sender_model, raw_name);
                n.kind = share_device_kind(s.incoming.sender_model, raw_name);
            }
            if (!contact && !s.incoming.matched_hash.empty()) {
                const ShareContact* hc = share_find_contact(v.contacts, raw_name, s.incoming.matched_hash);
                if (hc) {
                    if (!hc->name.empty()) n.display_name = hc->name;
                    if (!hc->photo.empty()) n.photo_path = utf8_to_wide(hc->photo);
                }
            }
            incoming_attached = true;
        }
        nodes.push_back(std::move(n));
    }

    if (s.has_incoming && !incoming_attached) {
        RadarNode n;
        n.key = "__incoming__";
        n.has_incoming = true;
        n.inc = s.incoming;
        std::string raw_name = s.incoming.sender.empty() ? std::string("Someone nearby") : s.incoming.sender;
        n.raw_name = raw_name;
        const ShareContact* contact = share_find_contact(v.contacts, raw_name, s.incoming.matched_hash);
        n.display_name = (contact && !contact->name.empty()) ? contact->name : raw_name;
        n.subtitle = share_model_label(s.incoming.sender_model, raw_name);
        n.kind = share_device_kind(s.incoming.sender_model, raw_name);
        if (contact && !contact->photo.empty()) n.photo_path = utf8_to_wide(contact->photo);
        nodes.insert(nodes.begin(), std::move(n));
    }

    // Promote highest-priority transferring node (Incoming > Sending/Asking > Sent/Declined/Failed) to index 0 (12 o'clock)
    size_t best_idx = 0;
    int best_prio = nodes.empty() ? 0 : popover_priority(nodes[0]);
    for (size_t i = 1; i < nodes.size(); ++i) {
        int p = popover_priority(nodes[i]);
        if (p > best_prio) {
            best_prio = p;
            best_idx = i;
        }
    }
    if (best_idx > 0) {
        std::rotate(nodes.begin(), nodes.begin() + static_cast<ptrdiff_t>(best_idx),
                    nodes.begin() + static_cast<ptrdiff_t>(best_idx + 1));
    }

    // Compute clean macOS-style polar positions on upper concentric radar arcs
    const float avatar_r = 34.0f;
    const float min_nx = x + 68.0f;
    const float max_nx = x + w - 68.0f;
    const float min_ny = y + 110.0f;
    const float max_ny = cy - 92.0f;
    const float primary_ring = std::clamp(ring_step * 2.75f, 150.0f, std::max(150.0f, (cy - y) * 0.52f));

    auto place = [&](RadarNode& node, float deg, float radius) {
        float rad = deg * (kPi / 180.0f);
        node.cx = std::clamp(cx + radius * std::cos(rad), min_nx, max_nx);
        node.cy = std::clamp(cy + radius * std::sin(rad), min_ny, max_ny);
    };

    const size_t count = nodes.size();
    if (count == 1) {
        place(nodes[0], -90.0f, primary_ring);
    } else if (count == 2) {
        if (has_popover(nodes[0])) {
            place(nodes[0], -90.0f, primary_ring);
            place(nodes[1], -38.0f, primary_ring * 1.04f);
        } else {
            place(nodes[0], -124.0f, primary_ring);
            place(nodes[1], -56.0f, primary_ring);
        }
    } else if (count == 3) {
        place(nodes[0], -90.0f, primary_ring);
        place(nodes[1], -138.0f, primary_ring * 1.04f);
        place(nodes[2], -42.0f, primary_ring * 1.04f);
    } else if (count == 4) {
        if (has_popover(nodes[0])) {
            place(nodes[0], -90.0f, primary_ring);
            place(nodes[1], -144.0f, primary_ring * 1.04f);
            place(nodes[2], -36.0f, primary_ring * 1.04f);
            place(nodes[3], -118.0f, ring_step * 1.75f);
        } else {
            static const float kDeg4[4] = { -145.0f, -108.0f, -72.0f, -35.0f };
            for (size_t i = 0; i < 4; ++i) place(nodes[i], kDeg4[i], primary_ring);
        }
    } else if (count > 4) {
        size_t inner_n = std::min<size_t>(3, count);
        static const float kInnerDeg[3] = { -90.0f, -138.0f, -42.0f };
        for (size_t i = 0; i < inner_n; ++i) place(nodes[i], kInnerDeg[i], ring_step * 2.05f);
        size_t outer_n = count - inner_n;
        for (size_t j = 0; j < outer_n; ++j) {
            float t = outer_n == 1 ? 0.5f : static_cast<float>(j) / static_cast<float>(outer_n - 1);
            float deg = -152.0f + t * 124.0f;
            place(nodes[inner_n + j], deg, std::min(ring_step * 3.35f, (cy - y) - 96.0f));
        }
    }

    // --------------------------------------------------------- Draw device avatars
    for (const RadarNode& n : nodes) {
        D2D1_RECT_F av_hit = D2D1::RectF(n.cx - avatar_r - 18.0f, n.cy - avatar_r - 6.0f,
                                         n.cx + avatar_r + 18.0f, n.cy + avatar_r + 46.0f);
        bool is_drag_target = share_drag_active_ && n.has_device && share_drag_key_ == n.key;
        bool is_hov = n.has_device && hover_bg(av_hit);

        share_circles_.push_back({ n.key, n.raw_name, n.display_name, n.has_device, n.cx, n.cy, avatar_r + 12.0f, av_hit });
        if (n.has_device) {
            share_hits_.push_back({ av_hit, ShareActionKind::DeviceClick, n.key });
        }

        // Hover / Drag halo
        if (is_drag_target) {
            Rgba halo = ap.blue;
            halo.a = dark_ ? 0.28f : 0.16f;
            brush_->SetColor(to_d2d(halo));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), avatar_r + 10.0f, avatar_r + 10.0f), brush_);
            brush_->SetColor(to_d2d(ap.blue));
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), avatar_r + 5.5f, avatar_r + 5.5f), brush_, 2.8f);
        } else if (is_hov) {
            Rgba halo = ap.blue;
            halo.a = dark_ ? 0.22f : 0.13f;
            brush_->SetColor(to_d2d(halo));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), avatar_r + 6.0f, avatar_r + 6.0f), brush_);
        }

        // Subtle avatar drop shadow
        Rgba sh = ap.shadow;
        sh.a *= 0.65f;
        brush_->SetColor(to_d2d(sh));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy + 2.0f), avatar_r, avatar_r), brush_);

        // Avatar interior: real photo or macOS gradient + initials/device icon
        ID2D1Bitmap* photo = !n.photo_path.empty() ? load_share_bitmap(n.photo_path) : nullptr;
        bool drew_device_silhouette = false;
        if (photo) {
            D2D1_SIZE_F sz = photo->GetSize();
            float side = std::max(1.0f, std::min(sz.width, sz.height));
            float sc = (avatar_r * 2.0f) / side;
            float ox = n.cx - (sz.width * sc) * 0.5f;
            float oy = n.cy - (sz.height * sc) * 0.5f;
            ID2D1BitmapBrush* bbrush = nullptr;
            D2D1_BITMAP_BRUSH_PROPERTIES bprops = D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (SUCCEEDED(rt->CreateBitmapBrush(photo, bprops, &bbrush)) && bbrush) {
                bbrush->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(ox, oy));
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), avatar_r, avatar_r), bbrush);
                bbrush->Release();
            }
        } else {
            auto [top_c, bot_c] = avatar_gradient(n.display_name);
            D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, to_d2d(top_c) },
                { 1.0f, to_d2d(bot_c) },
            };
            ID2D1GradientStopCollection* gsc = nullptr;
            ID2D1LinearGradientBrush* gbrush = nullptr;
            if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &gsc)) && gsc) {
                if (SUCCEEDED(rt->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(n.cx, n.cy - avatar_r),
                                                            D2D1::Point2F(n.cx, n.cy + avatar_r)),
                        gsc, &gbrush)) && gbrush) {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), avatar_r, avatar_r), gbrush);
                    gbrush->Release();
                }
                gsc->Release();
            }
            std::string init = share_initials(n.display_name);
            bool generic_name = eq_icase(n.display_name, "iPhone") || eq_icase(n.display_name, "iPad") ||
                                eq_icase(n.display_name, "Mac") || eq_icase(n.display_name, "MacBook") ||
                                eq_icase(n.display_name, "Apple Device");
            if (generic_name || init.empty()) {
                drew_device_silhouette = true;
                draw_device_glyph(rt, brush_, n.cx, n.cy, 30.0f, n.kind, { 1.0f, 1.0f, 1.0f, 0.94f });
            } else {
                std::wstring winit = utf8_to_wide(init);
                D2D1_RECT_F irc = D2D1::RectF(n.cx - avatar_r, n.cy - avatar_r, n.cx + avatar_r, n.cy + avatar_r);
                draw_centered_text(rt, fonts_.factory(), brush_, winit.c_str(), fonts_.ui_initials(), irc,
                                   { 1.0f, 1.0f, 1.0f, 0.96f }, DWRITE_TEXT_ALIGNMENT_CENTER, false);
            }
        }

        // Crisp border ring around avatar
        brush_->SetColor(to_d2d(ap.avatar_ring));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), avatar_r, avatar_r), brush_, 1.2f);

        // Active transfer circular progress arc (matches screenshot blue ring around Susuin's avatar!)
        bool active_progress = false;
        double prog_frac = 0.0;
        bool ring_done = false;
        bool ring_waiting = false;
        if (n.has_incoming) {
            if (n.inc.state == ShareIncomingState::Receiving) {
                active_progress = true;
                prog_frac = n.inc.total ? fraction(n.inc.done, n.inc.total) : 0.35;
            } else if (n.inc.state == ShareIncomingState::Asking) {
                ring_waiting = true;
            } else if (n.inc.state == ShareIncomingState::Done) {
                ring_done = true;
            }
        } else if (n.has_device) {
            if (n.dev.state == ShareDeviceState::Sending) {
                active_progress = true;
                prog_frac = n.dev.total ? fraction(n.dev.sent, n.dev.total) : 0.35;
            } else if (n.dev.state == ShareDeviceState::Asking) {
                ring_waiting = true;
            } else if (n.dev.state == ShareDeviceState::Sent) {
                ring_done = true;
            }
        }
        if (active_progress) {
            float pr = avatar_r + 3.2f;
            Rgba track = ap.blue;
            track.a = dark_ ? 0.24f : 0.16f;
            brush_->SetColor(to_d2d(track));
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), pr, pr), brush_, 2.8f);
            draw_progress_arc(d2d_factory_, rt, brush_, share_round_cap_, n.cx, n.cy, pr,
                              std::max(0.06, prog_frac), 2.8f, ap.blue);
        } else if (ring_waiting) {
            Rgba wait_c = ap.blue;
            wait_c.a = 0.65f;
            brush_->SetColor(to_d2d(wait_c));
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), avatar_r + 3.2f, avatar_r + 3.2f), brush_, 2.2f);
        } else if (ring_done) {
            brush_->SetColor(to_d2d(ap.green));
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(n.cx, n.cy), avatar_r + 3.2f, avatar_r + 3.2f), brush_, 2.4f);
        }

        // Small bottom-right device-type badge (only on photo/monogram avatars, not device silhouettes)
        if (!active_progress && !drew_device_silhouette) {
            float bx = n.cx + avatar_r * 0.68f;
            float by = n.cy + avatar_r * 0.68f;
            float br = 11.5f;
            brush_->SetColor(to_d2d(ap.stage_bg));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx, by), br + 1.8f, br + 1.8f), brush_);
            Rgba bbg = dark_ ? Rgba{ 0.24f, 0.24f, 0.27f, 1.0f } : Rgba{ 0.93f, 0.93f, 0.95f, 1.0f };
            brush_->SetColor(to_d2d(bbg));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx, by), br, br), brush_);
            draw_device_glyph(rt, brush_, bx, by, 13.0f, n.kind, ap.text);
        }

        // Two-line label under avatar: Display Name (SemiBold) + Device Subtitle (muted)
        std::wstring wname = utf8_to_wide(n.display_name);
        std::wstring wsub = utf8_to_wide(n.subtitle);
        Rgba sub_c = ap.sub;
        if (is_drag_target) {
            wsub = L"Drop to send";
            sub_c = ap.blue;
        }
        label(rt, wname.c_str(), fonts_.ui_semibold(), n.cx - 72.0f, n.cy + avatar_r + 7.0f, 144.0f,
              ap.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        label(rt, wsub.c_str(), fonts_.ui_sub(), n.cx - 72.0f, n.cy + avatar_r + 25.0f, 144.0f,
              sub_c, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Release stage clip BEFORE floating popovers/menus so shadows & overlays never clip awkwardly
    if (pushed_layer) rt->PopLayer();
    if (clip_layer) clip_layer->Release();
    if (stage_geo) stage_geo->Release();

    // Stage border stroke
    brush_->SetColor(to_d2d(ap.stage_border));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(stage, stage_r, stage_r), brush_, 1.0f);

    // --------------------------------------------------------- Floating macOS Transfer Popover
    for (const RadarNode& n : nodes) {
        if (!has_popover(n)) continue;
        const bool is_in = n.has_incoming;
        const float pw = std::min(344.0f, w - 24.0f);

        std::wstring title;
        std::wstring detail;
        bool show_bar = false;
        double bar_frac = 0.0;
        bool show_cancel_x = false;
        ShareActionKind x_action = ShareActionKind::CancelSend;
        std::string x_val;
        bool is_link = false;
        bool is_image = false;
        std::wstring thumb_path;

        if (is_in) {
            const ShareIncoming& in = n.inc;
            std::wstring what = utf8_to_wide(share_offer_text(in));
            is_link = in.links;
            if (!in.names.empty()) is_image = looks_like_image_name(in.names[0]);
            if (!in.thumb_path.empty()) thumb_path = utf8_to_wide(in.thumb_path);
            else if (in.state == ShareIncomingState::Done && !in.saved.empty() && is_image) {
                thumb_path = utf8_to_wide(in.saved[0]);
            }
            switch (in.state) {
            case ShareIncomingState::Asking:
                title = utf8_to_wide(n.display_name) + L" would like to share " +
                        (in.links ? L"a link." : L"“" + what + L"”.");
                detail = in.total ? utf8_to_wide(share_format_size(in.total)) + L" · Saved to Downloads"
                                  : L"Saved to Downloads when accepted";
                break;
            case ShareIncomingState::Receiving: {
                title = L"Receiving “" + what + L"”.";
                show_bar = true;
                bar_frac = fraction(in.done, in.total);
                std::string sz = share_format_size(in.done);
                if (in.total) sz += " of " + share_format_size(in.total);
                uint64_t elapsed = v.xfer_start_ms ? (GetTickCount64() - v.xfer_start_ms) : 0;
                std::string eta = share_format_eta(in.done, in.total, elapsed);
                if (!eta.empty()) sz += " - " + eta;
                detail = utf8_to_wide(sz);
                break;
            }
            case ShareIncomingState::Done:
                title = in.links ? L"Received a link." : L"Received “" + what + L"”.";
                detail = in.links ? (in.saved.empty() ? L"" : utf8_to_wide(in.saved[0])) : L"Saved in Downloads";
                break;
            case ShareIncomingState::Failed:
                if (in.message.find("cancel") != std::string::npos || in.message.find("Cancel") != std::string::npos) {
                    title = utf8_to_wide(n.display_name) + L" canceled the copy.";
                    detail = L"“" + what + L"”";
                } else {
                    title = L"Couldn't receive “" + what + L"”.";
                    detail = in.message.empty() ? L"The transfer stopped." : utf8_to_wide(in.message);
                }
                break;
            }
        } else {
            const ShareDevice& d = n.dev;
            std::wstring what = v.files.empty() ? std::wstring(L"files") : v.files[0];
            if (v.files.size() > 1) what += L" and " + std::to_wstring(v.files.size() - 1) + L" more";
            if (!v.file_paths.empty()) {
                is_image = looks_like_image_name(wide_to_utf8(v.files[0]));
                if (is_image) thumb_path = v.file_paths[0];
            }
            switch (d.state) {
            case ShareDeviceState::Asking:
                title = L"Waiting for " + utf8_to_wide(n.display_name) + L"…";
                detail = L"Sending “" + what + L"”";
                show_bar = true;
                bar_frac = 0.14;
                show_cancel_x = true;
                x_action = ShareActionKind::CancelSend;
                break;
            case ShareDeviceState::Sending: {
                title = L"Sending “" + what + L"”.";
                show_bar = true;
                bar_frac = fraction(d.sent, d.total);
                show_cancel_x = true;
                x_action = ShareActionKind::CancelSend;
                std::string sz = share_format_size(d.sent);
                if (d.total) sz += " of " + share_format_size(d.total);
                uint64_t elapsed = v.xfer_start_ms ? (GetTickCount64() - v.xfer_start_ms) : 0;
                std::string eta = share_format_eta(d.sent, d.total, elapsed);
                if (!eta.empty()) sz += " - " + eta;
                detail = utf8_to_wide(sz);
                break;
            }
            case ShareDeviceState::Sent:
                title = L"Sent “" + what + L"”.";
                detail = L"Delivered to " + utf8_to_wide(n.display_name);
                break;
            case ShareDeviceState::Declined:
                title = utf8_to_wide(n.display_name) + L" declined.";
                detail = L"The transfer was declined on the other device.";
                break;
            case ShareDeviceState::Failed:
                title = L"Couldn't send to " + utf8_to_wide(n.display_name) + L".";
                detail = d.message.empty() ? L"The transfer stopped." : utf8_to_wide(d.message);
                break;
            case ShareDeviceState::Found:
                break;
            }
        }

        bool has_buttons = is_in
                               ? (n.inc.state != ShareIncomingState::Receiving)
                               : (n.dev.state == ShareDeviceState::Sent || n.dev.state == ShareDeviceState::Declined ||
                                  n.dev.state == ShareDeviceState::Failed);
        float ph = show_bar ? 84.0f : (has_buttons ? 104.0f : 78.0f);
        float px = std::clamp(n.cx - pw * 0.5f, x + 12.0f, x + w - pw - 12.0f);
        float py = std::max(y + 10.0f, n.cy - avatar_r - 18.0f - ph);
        D2D1_RECT_F pbox = D2D1::RectF(px, py, px + pw, py + ph);

        draw_speech_popover(d2d_factory_, rt, brush_, pbox, n.cx, 12.0f,
                            ap.popover_bg, ap.popover_border, ap.shadow);

        // Preview thumbnail (46x46 rounded square on the left)
        D2D1_RECT_F thumb_rc = D2D1::RectF(px + 14.0f, py + 16.0f, px + 60.0f, py + 62.0f);
        ID2D1Bitmap* tbmp = !thumb_path.empty() ? load_share_bitmap(thumb_path) : nullptr;
        if (tbmp) {
            D2D1_SIZE_F sz = tbmp->GetSize();
            float side = std::max(1.0f, std::min(sz.width, sz.height));
            float sc = 46.0f / side;
            float ox = (thumb_rc.left + thumb_rc.right) * 0.5f - (sz.width * sc) * 0.5f;
            float oy = (thumb_rc.top + thumb_rc.bottom) * 0.5f - (sz.height * sc) * 0.5f;
            ID2D1BitmapBrush* bbrush = nullptr;
            D2D1_BITMAP_BRUSH_PROPERTIES bprops = D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (SUCCEEDED(rt->CreateBitmapBrush(tbmp, bprops, &bbrush)) && bbrush) {
                bbrush->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(ox, oy));
                rt->FillRoundedRectangle(D2D1::RoundedRect(thumb_rc, 6.0f, 6.0f), bbrush);
                bbrush->Release();
            }
            brush_->SetColor(to_d2d(ap.stage_border));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(thumb_rc, 6.0f, 6.0f), brush_, 1.0f);
        } else {
            draw_file_icon_tile(rt, brush_, share_round_cap_, thumb_rc, is_link, is_image, dark_);
        }

        // Right text + progress + buttons column
        float rx = thumb_rc.right + 12.0f;
        float rw = pbox.right - 14.0f - rx;
        label(rt, title.c_str(), fonts_.ui_semibold(), rx, py + 14.0f, rw, ap.text);

        if (show_bar) {
            float bar_y = py + 39.0f;
            float bar_w = show_cancel_x ? (rw - 28.0f) : rw;
            D2D1_RECT_F track = D2D1::RectF(rx, bar_y, rx + bar_w, bar_y + 6.0f);
            fill(rt, track, ap.track_bg, 3.0f);
            float fill_w = std::max(6.0f, static_cast<float>(bar_frac) * bar_w);
            fill(rt, D2D1::RectF(rx, bar_y, rx + std::min(bar_w, fill_w), bar_y + 6.0f), ap.blue, 3.0f);
            if (show_cancel_x) {
                float xcx = pbox.right - 22.0f;
                float xcy = bar_y + 3.0f;
                D2D1_RECT_F xrc = D2D1::RectF(xcx - 10.0f, xcy - 10.0f, xcx + 10.0f, xcy + 10.0f);
                Rgba xbg = hover_bg(xrc) ? ap.muted : ap.cancel_bg;
                brush_->SetColor(to_d2d(xbg));
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(xcx, xcy), 8.5f, 8.5f), brush_);
                brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
                rt->DrawLine(D2D1::Point2F(xcx - 3.0f, xcy - 3.0f), D2D1::Point2F(xcx + 3.0f, xcy + 3.0f), brush_, 1.5f, share_round_cap_);
                rt->DrawLine(D2D1::Point2F(xcx + 3.0f, xcy - 3.0f), D2D1::Point2F(xcx - 3.0f, xcy + 3.0f), brush_, 1.5f, share_round_cap_);
                share_hits_.push_back({ xrc, x_action, x_val });
            }
            label(rt, detail.c_str(), fonts_.ui_sub(), rx, py + 53.0f, rw, ap.sub);
        } else {
            label(rt, detail.c_str(), fonts_.ui_sub(), rx, py + 35.0f, rw, ap.sub);
        }

        if (has_buttons) {
            float by = pbox.bottom - 38.0f;
            float bh = 26.0f;
            float right_edge = pbox.right - 14.0f;
            auto right_btn = [&](const wchar_t* lbl, bool primary, ShareActionKind kind, const std::string& val = {}) {
                float bw = std::max(60.0f, text_width(lbl, fonts_.ui_sub()) + 24.0f);
                D2D1_RECT_F brc = D2D1::RectF(right_edge - bw, by, right_edge, by + bh);
                mac_button(brc, lbl, primary, false, kind, val);
                right_edge = brc.left - 8.0f;
            };
            if (is_in) {
                if (n.inc.state == ShareIncomingState::Asking) {
                    right_btn(L"Accept", true, ShareActionKind::Accept);
                    right_btn(L"Decline", false, ShareActionKind::Decline);
                } else if (n.inc.state == ShareIncomingState::Done && n.inc.links) {
                    if (!n.inc.saved.empty() && share_link_opens(n.inc.saved[0])) {
                        right_btn(L"Open Link", true, ShareActionKind::OpenLink, n.inc.saved[0]);
                    }
                    right_btn(L"OK", false, ShareActionKind::Dismiss);
                } else if (n.inc.state == ShareIncomingState::Done) {
                    right_btn(L"Show in Folder", true, ShareActionKind::ShowFolder,
                              n.inc.saved.empty() ? std::string() : n.inc.saved[0]);
                    right_btn(L"OK", false, ShareActionKind::Dismiss);
                } else if (n.inc.state == ShareIncomingState::Failed) {
                    right_btn(L"OK", false, ShareActionKind::Dismiss);
                }
            } else {
                if (n.dev.state == ShareDeviceState::Declined || n.dev.state == ShareDeviceState::Failed) {
                    right_btn(L"Send Again", true, ShareActionKind::SendAgain, n.key);
                }
                right_btn(L"OK", false, ShareActionKind::DismissDevice, n.key);
            }
        }
        break; // Show one clean popover at a time so popovers never pile over each other
    }

    // --------------------------------------------------------- Overlay backdrop + menus/modals
    if (overlay_open) {
        if (share_contacts_open_) fill(rt, stage, ap.backdrop, stage_r);
        share_hits_.push_back({ stage, ShareActionKind::CloseOverlays, {} });
    }

    // 1) "Allow me to be discovered by:" macOS dropdown menu
    if (share_mode_menu_open_) {
        static const ShareDiscoverMode kModes[3] = {
            ShareDiscoverMode::Off,
            ShareDiscoverMode::ContactsOnly,
            ShareDiscoverMode::Everyone,
        };
        const float item_h = 30.0f;
        const float mw = 236.0f;
        const float mh = 12.0f + item_h * 3.0f;
        float mx = std::clamp(cx - mw * 0.5f, x + 12.0f, x + w - mw - 12.0f);
        float my = trig_rc.top - mh - 6.0f;
        if (my < y + 12.0f) my = trig_rc.bottom + 6.0f;
        D2D1_RECT_F mrc = D2D1::RectF(mx, my, mx + mw, my + mh);

        // Soft shadow + rounded menu card
        fill(rt, D2D1::RectF(mx, my + 4.0f, mx + mw, my + mh + 4.0f), ap.shadow, 10.0f);
        fill(rt, mrc, ap.popover_bg, 10.0f);
        brush_->SetColor(to_d2d(ap.popover_border));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(mrc, 10.0f, 10.0f), brush_, 1.0f);

        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F irc = D2D1::RectF(mx + 6.0f, my + 6.0f + i * item_h, mx + mw - 6.0f, my + 6.0f + (i + 1) * item_h);
            bool selected = (s.mode == kModes[i]);
            bool lit = hover_top(irc) || share_mode_menu_focus_ == i;
            if (lit) fill(rt, irc, ap.blue, 6.0f);
            Rgba fg = lit ? Rgba{ 1.0f, 1.0f, 1.0f, 1.0f } : ap.text;
            if (selected) draw_checkmark(rt, brush_, share_round_cap_, irc.left + 8.0f, (irc.top + irc.bottom) * 0.5f, fg);
            D2D1_RECT_F trc = D2D1::RectF(irc.left + 26.0f, irc.top, irc.right - 8.0f, irc.bottom);
            draw_centered_text(rt, fonts_.factory(), brush_, discover_mode_label(kModes[i]), fonts_.ui_body(), trc,
                               fg, DWRITE_TEXT_ALIGNMENT_LEADING);
            share_hits_.push_back({ irc, ShareActionKind::PickDiscoverMode, std::to_string(i) });
        }
    }

    // 2) Compact Adapter details popover
    if (share_adapter_open_) {
        const float aw = 316.0f;
        BleWakeState wake_state = opts_.dry_run
                                      ? (GetEnvironmentVariableW(L"LARES_DRY_BLUETOOTH", nullptr, 0) > 0 ? BleWakeState::RadioOff
                                                                                                         : BleWakeState::Off)
                                      : ble_.state();
        bool show_bt_btn = s.wake && wake_state == BleWakeState::RadioOff;
        bool show_giveback = !v.running;
        float ah = 148.0f + (show_bt_btn ? 52.0f : 0.0f) + (show_giveback ? 42.0f : 0.0f);
        float ax = std::clamp(adapter_rc.right - aw, x + 12.0f, x + w - aw - 12.0f);
        float ay = adapter_rc.bottom + 8.0f;
        D2D1_RECT_F arc = D2D1::RectF(ax, ay, ax + aw, ay + ah);

        fill(rt, D2D1::RectF(ax, ay + 4.0f, ax + aw, ay + ah + 4.0f), ap.shadow, 12.0f);
        fill(rt, arc, ap.popover_bg, 12.0f);
        brush_->SetColor(to_d2d(ap.popover_border));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(arc, 12.0f, 12.0f), brush_, 1.0f);
        // Absorb clicks inside the popover itself so clicking blank popover area doesn't dismiss it
        share_hits_.push_back({ arc, ShareActionKind::ToggleAdapter, "noop" });

        float cy_p = ay + 14.0f;
        label(rt, L"Wi-Fi Adapter", fonts_.ui_semibold(), ax + 16.0f, cy_p, aw - 32.0f, ap.text);
        cy_p += 22.0f;
        label(rt, L"Realtek RTL8188EU · 2.4 GHz only", fonts_.ui_sub(), ax + 16.0f, cy_p, aw - 32.0f, ap.sub);
        cy_p += 22.0f;
        const wchar_t* link_str = s.link == ShareLink::Up       ? L"Link: Active (AWDL)"
                                  : s.link == ShareLink::Starting ? L"Link: Starting…"
                                  : s.link == ShareLink::Failed   ? L"Link: Stopped"
                                                                  : L"Link: Idle";
        label(rt, link_str, fonts_.ui_sub(), ax + 16.0f, cy_p, aw - 32.0f,
              s.link == ShareLink::Up ? ap.green : ap.text);
        cy_p += 22.0f;

        const wchar_t* note = L"Lares drives this USB adapter for AirDrop alone. On 2.4 GHz, large photos can take about a minute.";
        IDWriteTextLayout* nl = nullptr;
        if (SUCCEEDED(fonts_.factory()->CreateTextLayout(note, static_cast<UINT32>(wcslen(note)),
                                                         fonts_.ui_sub(), aw - 32.0f, 60.0f, &nl)) && nl) {
            brush_->SetColor(to_d2d(ap.sub));
            rt->DrawTextLayout(D2D1::Point2F(std::round(ax + 16.0f), std::round(cy_p)), nl, brush_);
            nl->Release();
        }
        cy_p += 46.0f;

        if (show_bt_btn) {
            label(rt, L"Bluetooth is off (needed to wake iPhones).", fonts_.ui_sub(), ax + 16.0f, cy_p, aw - 32.0f, ap.red);
            cy_p += 18.0f;
            mac_button(D2D1::RectF(ax + 16.0f, cy_p, ax + 158.0f, cy_p + 26.0f),
                       L"Bluetooth Settings", false, true, ShareActionKind::OpenBluetoothSettings);
            cy_p += 34.0f;
        }
        if (show_giveback) {
            if (v.setup_busy) {
                label(rt, L"Giving adapter back to Windows…", fonts_.ui_sub(), ax + 16.0f, cy_p + 4.0f, aw - 32.0f, ap.sub);
            } else {
                mac_button(D2D1::RectF(ax + 16.0f, cy_p, ax + 186.0f, cy_p + 28.0f),
                           L"Give Back to Windows", false, true, ShareActionKind::GiveBack);
            }
        }
    }

    // 3) Inline macOS AirDrop Contacts modal sheet
    if (share_contacts_open_) {
        const float pad = 24.0f;
        const float mw = std::round(std::min(460.0f, w - 48.0f));
        const int shown_contacts = std::min<int>(static_cast<int>(v.contacts.size()), 4);
        const float row_h = 56.0f;
        const float row_gap = 6.0f;
        const float list_h = shown_contacts > 0
                                 ? static_cast<float>(shown_contacts) * row_h +
                                       static_cast<float>(shown_contacts - 1) * row_gap
                                 : 44.0f;
        const float mh = 283.0f + list_h;
        const float mx = std::round(cx - mw * 0.5f);
        const float my = std::round(y + std::max(18.0f, (h - mh) * 0.5f));
        D2D1_RECT_F mrc = D2D1::RectF(mx, my, mx + mw, my + mh);

        fill(rt, D2D1::RectF(mx, my + 8.0f, mx + mw, my + mh + 8.0f), ap.shadow, 14.0f);
        fill(rt, mrc, ap.popover_bg, 14.0f);
        brush_->SetColor(to_d2d(ap.popover_border));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(mrc, 14.0f, 14.0f), brush_, 1.0f);
        share_hits_.push_back({ mrc, ShareActionKind::ToggleContacts, "noop" });

        // Header: "AirDrop Contacts" + Done button
        float cy_m = my + 22.0f;
        D2D1_RECT_F title_rc = D2D1::RectF(mx + pad, cy_m, mx + mw - pad - 80.0f, cy_m + 32.0f);
        draw_centered_text(rt, fonts_.factory(), brush_, L"AirDrop Contacts", fonts_.ui_title(), title_rc,
                           ap.text, DWRITE_TEXT_ALIGNMENT_LEADING);
        mac_button(D2D1::RectF(mx + mw - pad - 68.0f, cy_m + 1.0f, mx + mw - pad, cy_m + 31.0f),
                   L"Done", false, true, ShareActionKind::CloseOverlays);
        cy_m += 32.0f + 16.0f;

        // Top divider
        fill(rt, D2D1::RectF(mx + pad, cy_m, mx + mw - pad, cy_m + 1.0f), ap.stage_border);
        cy_m += 15.0f;

        // Saved contacts list
        if (v.contacts.empty()) {
            D2D1_RECT_F emp_rc = D2D1::RectF(mx + pad, cy_m, mx + mw - pad, cy_m + 44.0f);
            draw_centered_text(rt, fonts_.factory(), brush_,
                               L"No contacts yet. Add one below or right-click a nearby device.",
                               fonts_.ui_sub(), emp_rc, ap.sub, DWRITE_TEXT_ALIGNMENT_LEADING);
            cy_m += 44.0f;
        } else {
            Rgba row_bg = dark_ ? Rgba{ 1.0f, 1.0f, 1.0f, 0.045f } : Rgba{ 0.0f, 0.0f, 0.0f, 0.030f };
            Rgba trash_bg = dark_ ? Rgba{ 1.0f, 0.271f, 0.227f, 0.16f } : Rgba{ 1.0f, 0.231f, 0.188f, 0.11f };
            Rgba trash_border = dark_ ? Rgba{ 1.0f, 0.271f, 0.227f, 0.28f } : Rgba{ 1.0f, 0.231f, 0.188f, 0.22f };

            for (int i = 0; i < shown_contacts; ++i) {
                const ShareContact& c = v.contacts[static_cast<size_t>(i)];
                D2D1_RECT_F rrc = D2D1::RectF(mx + pad, cy_m, mx + mw - pad, cy_m + row_h);
                fill(rt, rrc, row_bg, 10.0f);

                const float av_r = 18.0f;
                const float rcx = std::round(rrc.left + 10.0f + av_r);
                const float rcy = std::round(cy_m + row_h * 0.5f);
                D2D1_RECT_F photo_rc = D2D1::RectF(rcx - av_r, rcy - av_r, rcx + av_r, rcy + av_r);
                ID2D1Bitmap* cbmp = !c.photo.empty() ? load_share_bitmap(utf8_to_wide(c.photo)) : nullptr;
                if (cbmp) {
                    D2D1_SIZE_F sz = cbmp->GetSize();
                    float side = std::max(1.0f, std::min(sz.width, sz.height));
                    float sc = (av_r * 2.0f) / side;
                    ID2D1BitmapBrush* bbrush = nullptr;
                    D2D1_BITMAP_BRUSH_PROPERTIES bprops = D2D1::BitmapBrushProperties(
                        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    if (SUCCEEDED(rt->CreateBitmapBrush(cbmp, bprops, &bbrush)) && bbrush) {
                        bbrush->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) *
                                             D2D1::Matrix3x2F::Translation(rcx - sz.width * sc * 0.5f, rcy - sz.height * sc * 0.5f));
                        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rcx, rcy), av_r, av_r), bbrush);
                        bbrush->Release();
                    }
                } else {
                    auto [tc, bc] = avatar_gradient(c.name.empty() ? c.device : c.name);
                    D2D1_GRADIENT_STOP stops[2] = { { 0.0f, to_d2d(tc) }, { 1.0f, to_d2d(bc) } };
                    ID2D1GradientStopCollection* gsc = nullptr;
                    ID2D1LinearGradientBrush* gbrush = nullptr;
                    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &gsc)) && gsc) {
                        if (SUCCEEDED(rt->CreateLinearGradientBrush(
                                D2D1::LinearGradientBrushProperties(D2D1::Point2F(rcx, rcy - av_r),
                                                                    D2D1::Point2F(rcx, rcy + av_r)),
                                gsc, &gbrush)) && gbrush) {
                            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rcx, rcy), av_r, av_r), gbrush);
                            gbrush->Release();
                        }
                        gsc->Release();
                    }
                    std::wstring wi = utf8_to_wide(share_initials(c.name.empty() ? c.device : c.name));
                    draw_centered_text(rt, fonts_.factory(), brush_, wi.c_str(), fonts_.ui_semibold(), photo_rc,
                                       { 1.0f, 1.0f, 1.0f, 0.96f }, DWRITE_TEXT_ALIGNMENT_CENTER, false);
                }
                brush_->SetColor(to_d2d(ap.avatar_ring));
                rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(rcx, rcy), av_r, av_r), brush_, 1.0f);
                share_hits_.push_back({ photo_rc, ShareActionKind::ContactChangePhoto, std::to_string(i) });

                std::wstring cname = utf8_to_wide(c.name.empty() ? c.device : c.name);
                std::string sub_str = (!c.device.empty() && c.device != c.name) ? c.device : std::string();
                const float text_x = rrc.left + 58.0f;
                const float text_r = rrc.right - 90.0f;
                if (!sub_str.empty()) {
                    draw_centered_text(rt, fonts_.factory(), brush_, cname.c_str(), fonts_.ui_semibold(),
                                       D2D1::RectF(text_x, cy_m + 9.0f, text_r, cy_m + 28.0f),
                                       ap.text, DWRITE_TEXT_ALIGNMENT_LEADING);
                    draw_centered_text(rt, fonts_.factory(), brush_, utf8_to_wide(sub_str).c_str(), fonts_.ui_sub(),
                                       D2D1::RectF(text_x, cy_m + 29.0f, text_r, cy_m + 47.0f),
                                       ap.sub, DWRITE_TEXT_ALIGNMENT_LEADING);
                } else {
                    draw_centered_text(rt, fonts_.factory(), brush_, cname.c_str(), fonts_.ui_semibold(),
                                       D2D1::RectF(text_x, cy_m, text_r, cy_m + row_h),
                                       ap.text, DWRITE_TEXT_ALIGNMENT_LEADING);
                }

                // Edit icon button (32x32)
                D2D1_RECT_F edit_rc = D2D1::RectF(rrc.right - 80.0f, cy_m + 12.0f, rrc.right - 48.0f, cy_m + 44.0f);
                bool edit_hov = hover_top(edit_rc);
                fill(rt, edit_rc, edit_hov ? ap.btn_hover : ap.btn_bg, 8.0f);
                brush_->SetColor(to_d2d(ap.btn_border));
                rt->DrawRoundedRectangle(D2D1::RoundedRect(edit_rc, 8.0f, 8.0f), brush_, 1.0f);
                draw_edit_icon(d2d_factory_, rt, brush_, share_round_cap_,
                               (edit_rc.left + edit_rc.right) * 0.5f, (edit_rc.top + edit_rc.bottom) * 0.5f,
                               14.5f, ap.text);
                share_hits_.push_back({ edit_rc, ShareActionKind::ContactEdit, std::to_string(i) });

                // Trash icon button (32x32)
                D2D1_RECT_F del_rc = D2D1::RectF(rrc.right - 42.0f, cy_m + 12.0f, rrc.right - 10.0f, cy_m + 44.0f);
                bool del_hov = hover_top(del_rc);
                fill(rt, del_rc, del_hov ? ap.red : trash_bg, 8.0f);
                if (!del_hov) {
                    brush_->SetColor(to_d2d(trash_border));
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(del_rc, 8.0f, 8.0f), brush_, 1.0f);
                }
                draw_trash_icon(d2d_factory_, rt, brush_, share_round_cap_,
                                (del_rc.left + del_rc.right) * 0.5f, (del_rc.top + del_rc.bottom) * 0.5f,
                                15.0f, del_hov ? Rgba{ 1.0f, 1.0f, 1.0f, 1.0f } : ap.red);
                share_hits_.push_back({ del_rc, ShareActionKind::ContactRemove, std::to_string(i) });

                cy_m += row_h + (i + 1 < shown_contacts ? row_gap : 0.0f);
            }
        }

        // Bottom divider
        cy_m += 16.0f;
        fill(rt, D2D1::RectF(mx + pad, cy_m, mx + mw - pad, cy_m + 1.0f), ap.stage_border);
        cy_m += 18.0f;

        // Add / Edit contact form
        draw_centered_text(rt, fonts_.factory(), brush_,
                           share_editing_contact_ >= 0 ? L"Edit Contact" : L"Add Contact",
                           fonts_.ui_semibold(), D2D1::RectF(mx + pad, cy_m, mx + mw - pad, cy_m + 20.0f),
                           ap.text, DWRITE_TEXT_ALIGNMENT_LEADING);
        cy_m += 28.0f;

        auto draw_labeled_field = [&](int idx, float fx, float fy, float fw, const wchar_t* title_lbl,
                                      const wchar_t* placeholder, const std::string& val) {
            draw_centered_text(rt, fonts_.factory(), brush_, title_lbl, fonts_.ui_sub_semibold(),
                               D2D1::RectF(fx + 2.0f, fy, fx + fw, fy + 16.0f),
                               ap.sub, DWRITE_TEXT_ALIGNMENT_LEADING);
            float iy = fy + 22.0f;
            const float fh = 34.0f;
            D2D1_RECT_F frc = D2D1::RectF(fx, iy, fx + fw, iy + fh);
            bool focused = (share_contact_field_ == idx);
            fill(rt, frc, ap.field_bg, 8.0f);
            brush_->SetColor(to_d2d(focused ? ap.blue : ap.field_border));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(frc, 8.0f, 8.0f), brush_, focused ? 1.6f : 1.0f);
            std::wstring shown = val.empty() ? std::wstring(placeholder) : utf8_to_wide(val);
            if (focused && !val.empty()) shown += L"|";
            draw_centered_text(rt, fonts_.factory(), brush_, shown.c_str(), fonts_.ui_sub(),
                               D2D1::RectF(fx + 12.0f, iy, fx + fw - 12.0f, iy + fh),
                               val.empty() ? ap.sub : ap.text, DWRITE_TEXT_ALIGNMENT_LEADING);
            share_hits_.push_back({ frc, ShareActionKind::ContactFocusField, std::to_string(idx) });
        };

        const float col_gap = 14.0f;
        const float half_w = std::floor((mw - pad * 2.0f - col_gap) * 0.5f);
        draw_labeled_field(0, mx + pad, cy_m, half_w, L"Name", L"John", share_draft_name_);
        draw_labeled_field(3, mx + mw - pad - half_w, cy_m, half_w, L"Device Name", L"John's iPhone", share_draft_device_);
        cy_m += 78.0f;

        std::wstring photo_btn = share_draft_photo_.empty() ? L"Choose Photo…" : L"Photo Selected";
        mac_button(D2D1::RectF(mx + pad, cy_m, mx + pad + 136.0f, cy_m + 34.0f),
                   photo_btn.c_str(), false, true, ShareActionKind::ContactPickPhoto);
        if (share_editing_contact_ >= 0) {
            mac_button(D2D1::RectF(mx + mw - pad - 210.0f, cy_m, mx + mw - pad - 134.0f, cy_m + 34.0f),
                       L"Cancel", false, true, ShareActionKind::ContactCancelEdit);
        }
        mac_button(D2D1::RectF(mx + mw - pad - 124.0f, cy_m, mx + mw - pad, cy_m + 34.0f),
                   share_editing_contact_ >= 0 ? L"Save Contact" : L"Add Contact", true, true, ShareActionKind::ContactAdd);
    }

    // 4) macOS right-click context menu on a nearby device
    if (share_ctx_open_) {
        struct CtxItem {
            const wchar_t* label;
            ShareActionKind kind;
            std::string value;
        };
        bool known = share_find_contact(v.contacts, share_ctx_raw_name_) != nullptr;
        std::vector<CtxItem> items;
        if (share_ctx_has_device_) {
            items.push_back({ L"Send Files…", ShareActionKind::DeviceClick, share_ctx_key_ });
        }
        items.push_back({ known ? L"Edit Contact…" : L"Add to Contacts",
                          ShareActionKind::DeviceAddContact, share_ctx_raw_name_ });
        items.push_back({ L"Set Profile Photo…", ShareActionKind::DeviceSetPhoto, share_ctx_raw_name_ });

        const float item_h = 30.0f;
        const float cw = 196.0f;
        const float ch = 12.0f + item_h * static_cast<float>(items.size());
        float cx_m = std::round(std::clamp(share_ctx_x_, x + 12.0f, x + w - cw - 12.0f));
        float cy_m = std::round(std::clamp(share_ctx_y_, y + 12.0f, y + h - ch - 12.0f));
        D2D1_RECT_F crc = D2D1::RectF(cx_m, cy_m, cx_m + cw, cy_m + ch);

        fill(rt, D2D1::RectF(cx_m, cy_m + 4.0f, cx_m + cw, cy_m + ch + 4.0f), ap.shadow, 10.0f);
        fill(rt, crc, ap.popover_bg, 10.0f);
        brush_->SetColor(to_d2d(ap.popover_border));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(crc, 10.0f, 10.0f), brush_, 1.0f);

        for (size_t i = 0; i < items.size(); ++i) {
            D2D1_RECT_F irc = D2D1::RectF(cx_m + 6.0f, cy_m + 6.0f + static_cast<float>(i) * item_h,
                                          cx_m + cw - 6.0f, cy_m + 6.0f + static_cast<float>(i + 1) * item_h);
            bool lit = hover_top(irc);
            if (lit) fill(rt, irc, ap.blue, 6.0f);
            Rgba fg = lit ? Rgba{ 1.0f, 1.0f, 1.0f, 1.0f } : ap.text;
            draw_centered_text(rt, fonts_.factory(), brush_, items[i].label, fonts_.ui_body(),
                               D2D1::RectF(irc.left + 12.0f, irc.top, irc.right - 12.0f, irc.bottom),
                               fg, DWRITE_TEXT_ALIGNMENT_LEADING);
            share_hits_.push_back({ irc, items[i].kind, items[i].value });
        }
    }

    content_height_ = h + kPagePadTop * 2.0f;
}

// --------------------------------------------------------------- File & Photo pickers

std::vector<std::wstring> MainWindow::pick_files_to_send(const wchar_t* title)
{
    std::vector<std::wstring> out;
    if (opts_.dry_run) {
        wchar_t buf[1024] = {};
        DWORD n = GetEnvironmentVariableW(L"LARES_DRY_FILES", buf, 1024);
        std::wstring all(buf, n < 1024 ? n : 0);
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find(L';', at);
            if (end == std::wstring::npos) end = all.size();
            if (end > at) out.push_back(all.substr(at, end - at));
            at = end + 1;
        }
        return out;
    }
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return out;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(title && *title ? title : L"Choose files to send");
    dialog->SetOkButtonLabel(L"Send");
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items)) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) out.emplace_back(path);
                CoTaskMemFree(path);
                item->Release();
            }
            items->Release();
        }
    }
    dialog->Release();
    return out;
}

std::wstring MainWindow::pick_photo_file()
{
    if (opts_.dry_run) return {};
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    static const COMDLG_FILTERSPEC kFilters[] = {
        { L"Images", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif" },
        { L"All files", L"*.*" },
    };
    dialog->SetFileTypes(2, kFilters);
    dialog->SetTitle(L"Choose profile photo");
    dialog->SetOkButtonLabel(L"Use Photo");
    std::wstring out;
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) out = path;
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dialog->Release();
    return out;
}

// --------------------------------------------------------------- Input & Drag-and-Drop

bool MainWindow::share_click(float lx, float ly)
{
    for (auto it = share_hits_.rbegin(); it != share_hits_.rend(); ++it) {
        if (!point_in_rect(lx, ly, it->rect)) continue;
        ShareActionKind kind = it->kind;
        std::string value = it->value;
        switch (kind) {
        case ShareActionKind::Setup:
            share_.run_setup();
            break;
        case ShareActionKind::OpenDiscoverMenu:
            share_mode_menu_open_ = !share_mode_menu_open_;
            share_mode_menu_focus_ = -1;
            share_adapter_open_ = false;
            share_contacts_open_ = false;
            share_ctx_open_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case ShareActionKind::PickDiscoverMode: {
            int idx = std::atoi(value.c_str());
            ShareDiscoverMode m = idx == 2 ? ShareDiscoverMode::Everyone
                                  : idx == 1 ? ShareDiscoverMode::ContactsOnly
                                             : ShareDiscoverMode::Off;
            share_mode_menu_open_ = false;
            share_mode_menu_focus_ = -1;
            share_.set_discover_mode(m);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        case ShareActionKind::CloseOverlays:
            share_mode_menu_open_ = false;
            share_mode_menu_focus_ = -1;
            share_adapter_open_ = false;
            share_contacts_open_ = false;
            share_ctx_open_ = false;
            share_editing_contact_ = -1;
            share_contact_field_ = -1;
            share_draft_name_.clear();
            share_draft_device_.clear();
            share_draft_email_.clear();
            share_draft_phone_.clear();
            share_draft_photo_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case ShareActionKind::DeviceClick: {
            share_ctx_open_ = false;
            if (share_view_.s.sending && !share_view_.files.empty()) {
                share_.send_to(value);
            } else {
                std::wstring title = L"Choose files to send";
                for (const auto& c : share_circles_) {
                    if (c.key == value && !c.name.empty()) {
                        title = L"Send files to " + utf8_to_wide(c.name);
                        break;
                    }
                }
                std::vector<std::wstring> files = pick_files_to_send(title.c_str());
                if (!files.empty()) share_.send_files_to(value, files);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        case ShareActionKind::DeviceAddContact: {
            share_ctx_open_ = false;
            share_mode_menu_open_ = false;
            share_adapter_open_ = false;
            const ShareContact* existing = share_find_contact(share_view_.contacts, value);
            if (existing) {
                share_contacts_open_ = true;
                share_editing_contact_ = -1;
                for (size_t i = 0; i < share_view_.contacts.size(); ++i) {
                    if (&share_view_.contacts[i] == existing) {
                        share_editing_contact_ = static_cast<int>(i);
                        break;
                    }
                }
                share_draft_name_ = existing->name;
                share_draft_email_ = existing->email;
                share_draft_phone_ = existing->phone;
                share_draft_device_ = existing->device.empty() ? value : existing->device;
                share_draft_photo_ = utf8_to_wide(existing->photo);
                share_contact_field_ = 0;
            } else {
                // Immediately save the nearby device with all information sent by it
                auto list = share_view_.contacts;
                ShareContact c;
                c.name = suggest_person_name(value);
                if (c.name.empty()) c.name = value;
                c.device = value;
                list.push_back(std::move(c));
                share_.save_contacts(list);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        case ShareActionKind::DeviceSetPhoto: {
            share_ctx_open_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            std::wstring p = pick_photo_file();
            if (!p.empty()) share_.set_device_photo(value, p);
            break;
        }
        case ShareActionKind::Accept:
            share_.answer(true);
            break;
        case ShareActionKind::Decline:
            share_.answer(false);
            break;
        case ShareActionKind::Dismiss:
            share_.dismiss_incoming();
            break;
        case ShareActionKind::OpenLink:
            if (share_link_opens(value) && !opts_.dry_run)
                ShellExecuteW(hwnd_, L"open", utf8_to_wide(value).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ShareActionKind::ShowFolder: {
            std::wstring path = utf8_to_wide(value);
            if (!path.empty() && path.find(L'"') == std::wstring::npos && !opts_.dry_run) {
                std::wstring args = L"/select,\"" + path + L"\"";
                ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case ShareActionKind::SendAgain:
            share_.send_to(value);
            break;
        case ShareActionKind::CancelSend:
            share_.cancel_send();
            break;
        case ShareActionKind::DismissDevice:
            share_.dismiss_device(value);
            break;
        case ShareActionKind::ClearStagedFiles:
            share_.stop_sending();
            break;
        case ShareActionKind::ToggleContacts:
            if (value == "noop") return true;
            share_contacts_open_ = !share_contacts_open_;
            share_mode_menu_open_ = false;
            share_adapter_open_ = false;
            share_ctx_open_ = false;
            share_editing_contact_ = -1;
            share_draft_name_.clear();
            share_draft_device_.clear();
            share_draft_email_.clear();
            share_draft_phone_.clear();
            share_draft_photo_.clear();
            share_contact_field_ = share_contacts_open_ ? 0 : -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case ShareActionKind::ToggleAdapter:
            if (value == "noop") return true;
            share_adapter_open_ = !share_adapter_open_;
            share_mode_menu_open_ = false;
            share_contacts_open_ = false;
            share_ctx_open_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case ShareActionKind::GiveBack:
            share_adapter_open_ = false;
            share_.give_back();
            break;
        case ShareActionKind::OpenBluetoothSettings:
            if (!opts_.dry_run) ShellExecuteW(hwnd_, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ShareActionKind::ContactFocusField:
            share_contact_field_ = std::clamp(std::atoi(value.c_str()), 0, 3);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case ShareActionKind::ContactPickPhoto: {
            std::wstring p = pick_photo_file();
            if (!p.empty()) {
                share_draft_photo_ = p;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        }
        case ShareActionKind::ContactAdd: {
            if (!share_draft_name_.empty() || !share_draft_email_.empty() ||
                !share_draft_phone_.empty() || !share_draft_device_.empty()) {
                auto list = share_view_.contacts;
                ShareContact c;
                c.name = share_draft_name_;
                c.email = share_draft_email_;
                c.phone = share_draft_phone_;
                c.device = share_draft_device_;
                c.photo = wide_to_utf8(share_draft_photo_);
                if (c.name.empty()) c.name = !c.device.empty() ? c.device : (!c.email.empty() ? c.email : c.phone);
                if (share_editing_contact_ >= 0 && share_editing_contact_ < static_cast<int>(list.size())) {
                    list[static_cast<size_t>(share_editing_contact_)] = std::move(c);
                } else {
                    list.push_back(std::move(c));
                }
                share_.save_contacts(list);
                share_draft_name_.clear();
                share_draft_email_.clear();
                share_draft_phone_.clear();
                share_draft_device_.clear();
                share_draft_photo_.clear();
                share_editing_contact_ = -1;
                share_contact_field_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        }
        case ShareActionKind::ContactEdit: {
            int idx = std::atoi(value.c_str());
            if (idx >= 0 && idx < static_cast<int>(share_view_.contacts.size())) {
                const ShareContact& c = share_view_.contacts[static_cast<size_t>(idx)];
                share_editing_contact_ = idx;
                share_draft_name_ = c.name;
                share_draft_device_ = c.device;
                share_draft_email_ = c.email;
                share_draft_phone_ = c.phone;
                share_draft_photo_ = utf8_to_wide(c.photo);
                share_contact_field_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        }
        case ShareActionKind::ContactCancelEdit:
            share_editing_contact_ = -1;
            share_draft_name_.clear();
            share_draft_device_.clear();
            share_draft_email_.clear();
            share_draft_phone_.clear();
            share_draft_photo_.clear();
            share_contact_field_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case ShareActionKind::ContactRemove: {
            int idx = std::atoi(value.c_str());
            auto list = share_view_.contacts;
            if (idx >= 0 && idx < static_cast<int>(list.size())) {
                list.erase(list.begin() + idx);
                if (share_editing_contact_ == idx) share_editing_contact_ = -1;
                else if (share_editing_contact_ > idx) --share_editing_contact_;
                share_.save_contacts(list);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        }
        case ShareActionKind::ContactChangePhoto: {
            int idx = std::atoi(value.c_str());
            auto list = share_view_.contacts;
            if (idx >= 0 && idx < static_cast<int>(list.size())) {
                std::wstring p = pick_photo_file();
                if (!p.empty()) {
                    list[static_cast<size_t>(idx)].photo = wide_to_utf8(p);
                    share_.save_contacts(list);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            break;
        }
        }
        return true;
    }
    return false;
}

bool MainWindow::share_right_click(float lx, float ly)
{
    if (!is_share_section_active() || share_contacts_open_) return false;
    share_mode_menu_open_ = false;
    share_adapter_open_ = false;
    share_ctx_open_ = false;
    for (const auto& c : share_circles_) {
        float dx = lx - c.cx;
        float dy = ly - c.cy;
        if (dx * dx + dy * dy <= c.r * c.r || point_in_rect(lx, ly, c.hit_rect)) {
            share_ctx_open_ = true;
            share_ctx_x_ = lx;
            share_ctx_y_ = ly;
            share_ctx_key_ = c.key;
            share_ctx_raw_name_ = c.raw_name.empty() ? c.name : c.raw_name;
            share_ctx_display_name_ = c.name;
            share_ctx_has_device_ = c.has_device;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return false;
}

bool MainWindow::share_key_down(WPARAM vk)
{
    if (!is_share_section_active()) return false;
    if (share_ctx_open_ && vk == VK_ESCAPE) {
        share_ctx_open_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (share_mode_menu_open_) {
        if (vk == VK_ESCAPE) {
            share_mode_menu_open_ = false;
            share_mode_menu_focus_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (vk == VK_UP || vk == VK_DOWN) {
            int cur = share_mode_menu_focus_ < 0 ? static_cast<int>(share_view_.s.mode) : share_mode_menu_focus_;
            cur = std::clamp(cur + (vk == VK_DOWN ? 1 : -1), 0, 2);
            share_mode_menu_focus_ = cur;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (vk == VK_RETURN || vk == VK_SPACE) {
            int idx = share_mode_menu_focus_ >= 0 ? share_mode_menu_focus_ : static_cast<int>(share_view_.s.mode);
            ShareDiscoverMode m = idx == 2 ? ShareDiscoverMode::Everyone
                                  : idx == 1 ? ShareDiscoverMode::ContactsOnly
                                             : ShareDiscoverMode::Off;
            share_mode_menu_open_ = false;
            share_mode_menu_focus_ = -1;
            share_.set_discover_mode(m);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        return false;
    }
    if (share_adapter_open_ && vk == VK_ESCAPE) {
        share_adapter_open_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (share_contacts_open_) {
        if (vk == VK_ESCAPE) {
            share_contacts_open_ = false;
            share_editing_contact_ = -1;
            share_contact_field_ = -1;
            share_draft_name_.clear();
            share_draft_device_.clear();
            share_draft_email_.clear();
            share_draft_phone_.clear();
            share_draft_photo_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (vk == VK_TAB) {
            share_contact_field_ = (share_contact_field_ == 0) ? 3 : 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (vk == VK_RETURN) {
            if (!share_draft_name_.empty() || !share_draft_email_.empty() ||
                !share_draft_phone_.empty() || !share_draft_device_.empty()) {
                auto list = share_view_.contacts;
                ShareContact c;
                c.name = share_draft_name_;
                c.email = share_draft_email_;
                c.phone = share_draft_phone_;
                c.device = share_draft_device_;
                c.photo = wide_to_utf8(share_draft_photo_);
                if (c.name.empty()) c.name = !c.device.empty() ? c.device : (!c.email.empty() ? c.email : c.phone);
                if (share_editing_contact_ >= 0 && share_editing_contact_ < static_cast<int>(list.size())) {
                    list[static_cast<size_t>(share_editing_contact_)] = std::move(c);
                } else {
                    list.push_back(std::move(c));
                }
                share_.save_contacts(list);
                share_draft_name_.clear();
                share_draft_email_.clear();
                share_draft_phone_.clear();
                share_draft_device_.clear();
                share_draft_photo_.clear();
                share_editing_contact_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        }
        if (vk == VK_BACK) {
            std::string* f = share_contact_field_ == 0 ? &share_draft_name_
                             : share_contact_field_ == 1 ? &share_draft_email_
                             : share_contact_field_ == 2 ? &share_draft_phone_
                             : share_contact_field_ == 3 ? &share_draft_device_
                                                         : nullptr;
            if (f && !f->empty()) {
                while (!f->empty() && (static_cast<unsigned char>(f->back()) & 0xC0) == 0x80) f->pop_back();
                if (!f->empty()) f->pop_back();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        }
        return true;
    }
    return false;
}

bool MainWindow::share_char(wchar_t ch)
{
    if (!is_share_section_active() || !share_contacts_open_ || share_contact_field_ < 0) return false;
    if (ch < 32 || ch == 127) return true;
    std::string* f = share_contact_field_ == 0 ? &share_draft_name_
                     : share_contact_field_ == 1 ? &share_draft_email_
                     : share_contact_field_ == 2 ? &share_draft_phone_
                     : share_contact_field_ == 3 ? &share_draft_device_
                                                 : nullptr;
    if (!f || f->size() >= 120) return true;
    wchar_t ws[2] = { ch, L'\0' };
    char utf8[8] = {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, 1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (n > 0) {
        f->append(utf8, static_cast<size_t>(n));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return true;
}

bool MainWindow::is_share_section_active() const
{
    return current_sec_ >= 0 && current_sec_ < static_cast<int>(sections_.size()) &&
           std::strcmp(sections_[current_sec_].id, "share") == 0;
}

void MainWindow::on_ole_drag(bool active, POINT screen_pt)
{
    if (!active || !hwnd_) {
        share_drag_update(false, 0.0f, 0.0f);
        return;
    }
    POINT pt = screen_pt;
    if (!ScreenToClient(hwnd_, &pt)) return;
    share_drag_update(true, static_cast<float>(pt.x) / scale_, static_cast<float>(pt.y) / scale_);
}

void MainWindow::on_ole_drop(POINT screen_pt, const std::vector<std::wstring>& paths)
{
    if (!hwnd_) return;
    POINT pt = screen_pt;
    if (!ScreenToClient(hwnd_, &pt)) return;
    share_drop_files(static_cast<float>(pt.x) / scale_, static_cast<float>(pt.y) / scale_, paths);
}

void MainWindow::share_drag_update(bool active, float lx, float ly)
{
    std::string hit_key;
    if (active) {
        for (const auto& c : share_circles_) {
            float dx = lx - c.cx;
            float dy = ly - c.cy;
            if (dx * dx + dy * dy <= (c.r + 14.0f) * (c.r + 14.0f)) {
                hit_key = c.key;
                break;
            }
        }
    }
    if (share_drag_active_ != active || share_drag_key_ != hit_key) {
        share_drag_active_ = active;
        share_drag_key_ = hit_key;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::share_drop_files(float lx, float ly, const std::vector<std::wstring>& paths)
{
    std::string hit_key;
    for (const auto& c : share_circles_) {
        float dx = lx - c.cx;
        float dy = ly - c.cy;
        if (dx * dx + dy * dy <= (c.r + 14.0f) * (c.r + 14.0f)) {
            hit_key = c.key;
            break;
        }
    }
    share_drag_active_ = false;
    share_drag_key_.clear();
    if (share_view_.setup == ShareSetup::Ready && !paths.empty()) {
        if (!hit_key.empty()) share_.send_files_to(hit_key, paths);
        else share_.send(paths);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// --------------------------------------------------------------- Share lifecycle & ticks

void MainWindow::on_share_updated()
{
    bool was_asking = share_view_.s.has_incoming && share_view_.s.incoming.state == ShareIncomingState::Asking;
    share_.resume();
    share_view_ = share_.view();
    bool now_asking = share_view_.s.has_incoming && share_view_.s.incoming.state == ShareIncomingState::Asking;
    if (!was_asking && now_asking) {
        /* Dismiss overlays so an incoming Accept/Decline prompt is never obscured. */
        share_mode_menu_open_ = false;
        share_mode_menu_focus_ = -1;
        share_adapter_open_ = false;
        share_ctx_open_ = false;
        share_contacts_open_ = false;
        share_contact_field_ = -1;
    }
    bool wake = share_view_.running && share_view_.s.wake && !opts_.dry_run;
    ble_.set_active(wake);
    sync_share_tick();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::sync_share_tick()
{
    if (!hwnd_) return;
    bool on_page = is_share_section_active();
    bool transferring = (share_view_.s.has_incoming && share_view_.s.incoming.state == ShareIncomingState::Receiving);
    if (!transferring) {
        for (const auto& d : share_view_.s.devices) {
            if (d.state == ShareDeviceState::Sending) { transferring = true; break; }
        }
    }
    bool want = on_page && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) &&
                (share_view_.s.mode == ShareDiscoverMode::Everyone || share_view_.s.wake || transferring);
    if (want && !share_tick_) {
        SetTimer(hwnd_, kShareTickTimerId, 1000, nullptr);
        share_tick_ = true;
    } else if (!want && share_tick_) {
        KillTimer(hwnd_, kShareTickTimerId);
        share_tick_ = false;
    }
}

void MainWindow::sync_share_browsing()
{
    bool visible = hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    bool want = visible && is_share_section_active() && share_view_.setup == ShareSetup::Ready;
    if (want != share_browsing_sent_) {
        share_browsing_sent_ = want;
        share_.set_browsing(want);
    }
}

} // namespace lares
