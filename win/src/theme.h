// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace lares {

struct Rgba {
    float r, g, b, a;
};

struct Palette {
    Rgba bg;
    Rgba card;
    Rgba track;
    Rgba line;
    Rgba text;
    Rgba muted;
    Rgba accent;
    Rgba on_accent;
    Rgba hover;
    Rgba chip;
    Rgba note;
    Rgba warn;
    Rgba bad;
    Rgba live;
};

// Exact palettes from data/style.css (Home warm, Remote blue, light & dark).
Palette palette_for(bool dark, bool remote);
Palette palette_lerp(const Palette& a, const Palette& b, float t);
float ease_out_cubic_css(float t); // matches cubic-bezier(0.22, 1, 0.36, 1)
uint32_t rgba_to_colorref(const Rgba& c);
bool system_prefers_dark();

} // namespace lares
