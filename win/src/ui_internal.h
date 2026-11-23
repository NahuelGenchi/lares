// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The measures of data/style.css and src/window.c, and the small helpers
 * ui.cpp (the window) and ui_pages.cpp (its pages) share. Everything is a
 * multiple of 9 px, as Monocraft's pixels are. */
#pragma once
#include "ui.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace lares {

inline constexpr float kSidebarWidth = 234.0f;   // nav { min-width }
inline constexpr float kNavPadX = 18.0f;         // nav { padding: 18px 9px 18px 18px }
inline constexpr float kNavPadRight = 9.0f;
inline constexpr float kNavPadTop = 18.0f;
inline constexpr float kContentMaxW = 620.0f;    // CONTENT_WIDTH
inline constexpr float kContentLead = 9.0f;      // CONTENT_LEAD
inline constexpr float kPagePadTop = 18.0f;      // .page { padding: 18px 36px 36px 18px }
inline constexpr float kPagePadRight = 36.0f;
inline constexpr float kPagePadBottom = 36.0f;
inline constexpr float kPagePadLeft = 18.0f;
inline constexpr float kPageGap = 36.0f;         // ui_page's spacing
inline constexpr float kCardPad = 27.0f;         // .card.padded, button.tile
inline constexpr float kCardRadius = 18.0f;
inline constexpr float kRowPad = 18.0f;          // .row-head
inline constexpr float kGap = 18.0f;             // the usual gap inside a card
inline constexpr float kHalf = 9.0f;             // and the small one

inline constexpr UINT_PTR kShareTickTimerId = 3;   // the Share page's countdown, a second at a time

inline D2D1_COLOR_F to_d2d(const Rgba& c, float alpha_mul = 1.0f)
{
    return D2D1::ColorF(c.r, c.g, c.b, c.a * alpha_mul);
}

inline bool point_in_rect(float x, float y, const D2D1_RECT_F& r)
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

inline std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

/* ART_CHEVRON turned over, for a list or a row that is open. */
const char* const* chevron_up();

/* A pixel icon's size at `pixel` per dot, from its art. */
inline void art_size(const char* const* art, float pixel, float& w, float& h)
{
    int rows = 0, cols = 0;
    for (; art && art[rows]; ++rows) cols = std::max(cols, static_cast<int>(std::strlen(art[rows])));
    w = cols * pixel;
    h = rows * pixel;
}

} // namespace lares
