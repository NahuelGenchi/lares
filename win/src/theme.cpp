// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "theme.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lares {

static constexpr Rgba hex(uint32_t rgb, float alpha = 1.0f)
{
    return {
        static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
        static_cast<float>(rgb & 0xff) / 255.0f,
        alpha,
    };
}

static constexpr Rgba rgba_u8(uint8_t r, uint8_t g, uint8_t b, float a)
{
    return { r / 255.0f, g / 255.0f, b / 255.0f, a };
}

static Rgba lerp_rgba(const Rgba& a, const Rgba& b, float t)
{
    return {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t,
    };
}

Palette palette_for(bool dark, bool remote)
{
    if (!dark && !remote) {
        return {
            hex(0xf3e9da),
            hex(0xfbf3e7),
            hex(0xe8d9c3),
            rgba_u8(84, 58, 32, 0.10f),
            hex(0x3b2c1e),
            hex(0x8a735c),
            hex(0xc4602a),
            hex(0xfff5ea),
            rgba_u8(196, 96, 42, 0.08f),
            rgba_u8(84, 58, 32, 0.07f),
            rgba_u8(196, 96, 42, 0.10f),
            hex(0xa96d07),
            hex(0xbb3a2b),
            hex(0xd33a2c),
        };
    }
    if (!dark && remote) {
        return {
            hex(0xe1eaf6),
            hex(0xeef4fb),
            hex(0xd1deef),
            rgba_u8(29, 52, 92, 0.10f),
            hex(0x1c2a3d),
            hex(0x62768e),
            hex(0x2b6bd6),
            hex(0xeef4ff),
            rgba_u8(43, 107, 214, 0.08f),
            rgba_u8(29, 52, 92, 0.07f),
            rgba_u8(43, 107, 214, 0.10f),
            hex(0xa96d07),
            hex(0xbb3a2b),
            hex(0xd33a2c),
        };
    }
    if (dark && !remote) {
        return {
            hex(0x1d1712),
            hex(0x282019),
            hex(0x2f261e),
            rgba_u8(255, 228, 196, 0.07f),
            hex(0xf1e4d3),
            hex(0xa8917a),
            hex(0xf09a5a),
            hex(0x2a1a0e),
            rgba_u8(240, 154, 90, 0.09f),
            rgba_u8(255, 228, 196, 0.06f),
            rgba_u8(240, 154, 90, 0.12f),
            hex(0xe7b24a),
            hex(0xf07a68),
            hex(0xff7b6b),
        };
    }
    return {
        hex(0x111a27),
        hex(0x182335),
        hex(0x1c2a3f),
        rgba_u8(190, 215, 255, 0.07f),
        hex(0xdce7f6),
        hex(0x8296b2),
        hex(0x5f9dff),
        hex(0x0d1a2e),
        rgba_u8(95, 157, 255, 0.09f),
        rgba_u8(190, 215, 255, 0.06f),
        rgba_u8(95, 157, 255, 0.12f),
        hex(0xe7b24a),
        hex(0xf07a68),
        hex(0xff7b6b),
    };
}

Palette palette_lerp(const Palette& a, const Palette& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {
        lerp_rgba(a.bg, b.bg, t),
        lerp_rgba(a.card, b.card, t),
        lerp_rgba(a.track, b.track, t),
        lerp_rgba(a.line, b.line, t),
        lerp_rgba(a.text, b.text, t),
        lerp_rgba(a.muted, b.muted, t),
        lerp_rgba(a.accent, b.accent, t),
        lerp_rgba(a.on_accent, b.on_accent, t),
        lerp_rgba(a.hover, b.hover, t),
        lerp_rgba(a.chip, b.chip, t),
        lerp_rgba(a.note, b.note, t),
        lerp_rgba(a.warn, b.warn, t),
        lerp_rgba(a.bad, b.bad, t),
        lerp_rgba(a.live, b.live, t),
    };
}

// Fast smooth cubic-bezier(0.22, 1, 0.36, 1) evaluation using Newton-Raphson.
float ease_out_cubic_css(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    constexpr float x1 = 0.22f, y1 = 1.0f, x2 = 0.36f, y2 = 1.0f;
    float u = x;
    for (int i = 0; i < 6; ++i) {
        float inv = 1.0f - u;
        float bx = 3.0f * inv * inv * u * x1 + 3.0f * inv * u * u * x2 + u * u * u;
        float dbx = 3.0f * inv * inv * x1 + 6.0f * inv * u * (x2 - x1) + 3.0f * u * u * (1.0f - x2);
        if (std::fabs(dbx) < 1e-5f) break;
        u = std::clamp(u - (bx - x) / dbx, 0.0f, 1.0f);
    }
    float inv = 1.0f - u;
    return 3.0f * inv * inv * u * y1 + 3.0f * inv * u * u * y2 + u * u * u;
}

uint32_t rgba_to_colorref(const Rgba& c)
{
    uint32_t r = static_cast<uint32_t>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255.0f));
    uint32_t g = static_cast<uint32_t>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255.0f));
    uint32_t b = static_cast<uint32_t>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255.0f));
    return r | (g << 8) | (b << 16);
}

bool system_prefers_dark()
{
#ifdef _WIN32
    DWORD val = 1;
    DWORD sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &val, &sz) == ERROR_SUCCESS) {
        return val == 0;
    }
#endif
    return false;
}

} // namespace lares
