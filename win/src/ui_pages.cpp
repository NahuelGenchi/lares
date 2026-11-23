// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The window's pages, each laid out as its GTK view lays its boxes
 * (src/overview.c, src/hardware_view.c, ...): the same order, the same
 * spacing and padding from data/style.css, text of the same size. Heights
 * come from the text's own line height, as GTK's do, never from a guess. */
#include "ui.h"
#include "ui_internal.h"
extern "C" {
#include "art.h"
}

#include <cmath>
#include <cwchar>

namespace lares {

namespace {

Rgba mix(const Rgba& a, const Rgba& b, float t)
{
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a };
}

std::wstring percent_text(double fraction)
{
    if (fraction < 0.0) return L"…";
    wchar_t buf[16];
    std::swprintf(buf, std::size(buf), L"%d%%", static_cast<int>(std::lround(fraction * 100.0)));
    return buf;
}

std::wstring celsius_text(double celsius, const wchar_t* otherwise)
{
    if (std::isnan(celsius)) return otherwise;
    wchar_t buf[32];
    std::swprintf(buf, std::size(buf), L"%d °C", static_cast<int>(std::lround(celsius)));
    return buf;
}

} // namespace

// ================================================================ Overview

void MainWindow::draw_overview_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    const HwInfo& info = hw_.info();
    const HwSample& s = hw_sample_;

    /* A tile (button.tile): 27 px around a column 9 px apart of its heading
     * with its icon, one big number, a line under it and, for loads and
     * space, a meter. */
    float icon_w = 0.0f, icon_h = 0.0f;
    art_size(ART_CHIP, 2.0f, icon_w, icon_h);
    float head_h = std::max(lh_, icon_h);
    auto tile_height = [&](bool meter) {
        return kCardPad * 2.0f + head_h + kHalf + lhb_ + kHalf + lh_ + (meter ? kHalf + 6.0f : 0.0f);
    };
    auto draw_tile = [&](float tx, float ty, float tw, const char* const* art, const wchar_t* heading,
                         const std::wstring& val, const std::wstring& sub, int meter_blocks, double fraction,
                         bool hot, const char* target) {
        float th = tile_height(meter_blocks > 0);
        D2D1_RECT_F rc = D2D1::RectF(tx, ty, tx + tw, ty + th);
        brush_->SetColor(to_d2d(hovered(rc) ? mix(pal.card, pal.accent, 0.06f) : pal.card));
        rt->FillRoundedRectangle(D2D1::RoundedRect(rc, kCardRadius, kCardRadius), brush_);
        card_hits_.push_back({ rc, target });

        float ix = tx + kCardPad, iw = tw - kCardPad * 2.0f, cy = ty + kCardPad;
        float aw = 0.0f, ah = 0.0f;
        art_size(art, 2.0f, aw, ah);
        icon(rt, art, 2.0f, ix, cy, aw, head_h, pal.accent);
        label(rt, heading, fonts_.body(), ix + aw + kHalf, cy + (head_h - lh_) * 0.5f, iw - aw - kHalf, pal.muted);
        cy += head_h + kHalf;
        label(rt, val.c_str(), fonts_.big(), ix, cy, iw, pal.text);
        cy += lhb_ + kHalf;
        label(rt, sub.c_str(), fonts_.body(), ix, cy, iw, pal.muted);
        cy += lh_ + kHalf;
        if (meter_blocks > 0) draw_meter(rt, pal, ix, cy, meter_blocks, 2.0f, fraction, hot);
    };

    // The tiles are one grid, as far apart down as across.
    bool has_gpu = !info.gpu_name.empty();
    int cols = has_gpu ? 3 : 2;
    float tile_w = std::floor((w - kGap * (cols - 1)) / static_cast<float>(cols));
    float tx = x;
    draw_tile(tx, y, tile_w, ART_CHIP, L"CPU", percent_text(s.cpu), celsius_text(s.cpu_temp, L"Load"), 14, s.cpu,
              s.cpu > 0.9, "hardware");
    tx += tile_w + kGap;
    if (has_gpu) {
        draw_tile(tx, y, tile_w, ART_SCREEN, L"GPU", percent_text(s.gpu), celsius_text(s.gpu_temp, L"Load"), 14,
                  s.gpu, s.gpu > 0.9, "hardware");
        tx += tile_w + kGap;
    }
    double mem_frac = s.mem_total ? static_cast<double>(s.mem_used) / static_cast<double>(s.mem_total) : -1.0;
    draw_tile(tx, y, x + w - tx, ART_MEMORY, L"Memory",
              s.mem_total ? utf8_to_wide(format_bytes(s.mem_used)) : L"…",
              s.mem_total ? L"of " + utf8_to_wide(format_bytes(s.mem_total)) : L"", 14, mem_frac, false, "hardware");
    y += tile_height(true) + kGap;

    // The system drive spans the width.
    double disk_frac = s.disk_total ? static_cast<double>(s.disk_used) / static_cast<double>(s.disk_total) : -1.0;
    uint64_t disk_free = s.disk_total > s.disk_used ? s.disk_total - s.disk_used : 0;
    draw_tile(x, y, w, ART_DISK, L"Storage",
              s.disk_total ? utf8_to_wide(format_bytes(disk_free)) + L" free" : L"…",
              s.disk_total ? L"of " + utf8_to_wide(format_bytes(s.disk_total)) : L"", 34, disk_frac,
              disk_frac > 0.9, "hardware");
    y += tile_height(true);

    /* One row per device (.card of .row-head rows): 9 px around the card,
     * 18 px around each row, a 2 px line between rows. */
    struct Row {
        const char* const* art;
        std::wstring name;
        const wchar_t* status;   // under the name, when there's something to say
        int battery;             // -1 for none
        bool low;
        std::wstring battery_text;
        const char* target;
    };
    std::vector<Row> rows;
    for (const auto& p : pad_list_) {
        Row r{ ART_GAMEPAD, utf8_to_wide(pad_display_name(p)), nullptr, -1, false, {}, "controller" };
        if (p.battery.known) {
            r.battery = static_cast<int>(std::lround(pad_battery_fraction(p.battery) * 100.0));
            r.low = pad_battery_low_stage(p.battery, pad_prefs_.low_level) > 0 && !pad_battery_charging(p.battery);
            r.battery_text = utf8_to_wide(pad_battery_charge(p.battery));
        }
        rows.push_back(r);
    }
    for (const auto& d : dev_list_) {
        Row r{ d.kind == DevKind::Audio ? ART_HEADPHONES : d.kind == DevKind::Keyboard ? ART_KEYBOARD : ART_MOUSE,
               utf8_to_wide(d.name), nullptr, -1, false, {},
               d.kind == DevKind::Audio ? "headphones" : d.kind == DevKind::Keyboard ? "keyboard" : "mouse" };
        if (!d.connected) r.status = L"Not connected";
        else if (d.asleep) r.status = L"Asleep";
        else if (d.battery >= 0) {
            r.battery = d.battery;
            r.low = d.battery <= 20;
            r.battery_text = std::to_wstring(d.battery) + L"%";
        }
        rows.push_back(r);
    }
    if (!rows.empty()) {
        y += kPageGap;
        float total = kHalf * 2.0f;
        for (size_t i = 0; i < rows.size(); ++i) {
            total += kRowPad * 2.0f + lh_ + (rows[i].status ? kHalf + lh_ : 0.0f) + (i ? 2.0f : 0.0f);
        }
        card(rt, pal, x, y, w, total);
        float ry = y + kHalf;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            if (i) {
                brush_->SetColor(to_d2d(pal.line));
                rt->FillRectangle(D2D1::RectF(x + kHalf + kRowPad, ry, x + w - kHalf - kRowPad, ry + 2.0f), brush_);
                ry += 2.0f;
            }
            float inner_h = lh_ + (r.status ? kHalf + lh_ : 0.0f);
            D2D1_RECT_F rc = D2D1::RectF(x + kHalf, ry, x + w - kHalf, ry + kRowPad * 2.0f + inner_h);
            bool hover = hovered(rc);
            if (hover) {
                brush_->SetColor(to_d2d(pal.hover));
                rt->FillRoundedRectangle(D2D1::RoundedRect(rc, 9.0f, 9.0f), brush_);
            }
            card_hits_.push_back({ rc, r.target });
            float ix = rc.left + kRowPad, right = rc.right - kRowPad, cy = ry + kRowPad;
            // Icons of different widths line up their names: each has 22 px.
            icon(rt, r.art, 2.0f, ix, cy, 22.0f, inner_h, hover ? pal.accent : pal.muted);
            float name_x = ix + 22.0f + kGap;
            if (r.battery >= 0) {
                float mw = 10 * 4.0f * 2.0f - 2.0f;
                meter_row(rt, pal, right - mw, cy, inner_h, 10, 2.0f, r.battery / 100.0, r.low);
                float bw = text_width(r.battery_text.c_str(), fonts_.body());
                float bx = right - mw - kGap - bw;
                label(rt, r.battery_text.c_str(), fonts_.body(), bx, cy + (inner_h - lh_) * 0.5f, bw,
                      r.low ? pal.bad : pal.accent);
                right = bx - kGap;
            }
            label(rt, r.name.c_str(), fonts_.body(), name_x, cy, right - name_x, pal.text);
            if (r.status) label(rt, r.status, fonts_.body(), name_x, cy + lh_ + kHalf, right - name_x, pal.muted);
            ry = rc.bottom;
        }
        y += total;
    }

    // The mode, and under it only news (shown once remote play is set up).
    if (remote_cfg_.ready) {
        y += kPageGap;
        std::wstring mval = remote_cfg_.mode_known ? utf8_to_wide(remote_mode_name(remote_cfg_.mode)) : L"…";
        std::wstring msub;
        if (remote_live_.streaming) {
            msub = L"Streaming to Moonlight now";
        } else if (remote_cfg_.mode_known) {
            auto differs = [&](RemoteItem item, int now, const wchar_t* name) -> std::wstring {
                if (now < 0 || remote_matches(remote_live_, item, remote_cfg_.value(remote_cfg_.mode, item))) return {};
                return std::wstring(name) + (now ? L" is on" : L" is off");
            };
            std::wstring sun = differs(RemoteItem::Sunshine, remote_live_.sunshine, L"Sunshine");
            std::wstring ts = differs(RemoteItem::Tailscale, remote_live_.tailscale, L"Tailscale");
            if (!sun.empty() && !ts.empty()) msub = sun + L" · " + ts;
            else if (!sun.empty() || !ts.empty()) msub = !sun.empty() ? sun : ts;
            else if (remote_cfg_.mode == RemoteMode::Remote && remote_live_.sunshine == 1) msub = L"Waiting for Moonlight";
        }
        draw_tile(x, y, w, ART_LAPTOP, L"Mode", mval, msub, 0, -1.0, false, "remote");
        y += tile_height(false);
    }

    content_height_ = (y - start_y) + kPagePadTop + kPagePadBottom;
}

// ================================================================ Hardware

void MainWindow::draw_hardware_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    const HwInfo& info = hw_.info();
    const HwSample& s = hw_sample_;

    /* A card (.card.padded): 27 px around a column 18 px apart of its name
     * and a detail, a big number with a second beside it, a graph or a
     * meter, and a line of small facts that wraps. */
    float ix = x + kCardPad, iw = w - kCardPad * 2.0f;
    auto facts_height = [&](const std::vector<std::wstring>& facts) {
        if (facts.empty()) return 0.0f;
        float fx = 0.0f, h = lh_;
        for (const auto& f : facts) {
            float fw = text_width(f.c_str(), fonts_.body());
            if (fx > 0.0f && fx + fw > iw) {
                fx = 0.0f;
                h += kHalf + lh_;
            }
            fx += fw + kCardPad;
        }
        return h;
    };
    auto draw_card = [&](const char* const* art, const std::wstring& name, const std::wstring& detail,
                         const std::wstring& value, const std::wstring& side, const GraphRing* graph,
                         double fraction, bool hot, const std::vector<std::wstring>& facts) {
        float aw = 0.0f, ah = 0.0f;
        art_size(art, 2.0f, aw, ah);
        float head_h = std::max(lh_, ah);
        float body_h = graph ? 54.0f : 6.0f;
        float facts_h = facts_height(facts);
        float h = kCardPad * 2.0f + head_h + kGap + lhb_ + kGap + body_h + (facts_h > 0.0f ? kGap + facts_h : 0.0f);
        card(rt, pal, x, y, w, h);
        float cy = y + kCardPad;

        icon(rt, art, 2.0f, ix, cy, aw, head_h, pal.muted);
        float dw = detail.empty() ? 0.0f : text_width(detail.c_str(), fonts_.body());
        float name_x = ix + aw + kGap;
        float name_w = iw - aw - kGap - (dw > 0.0f ? dw + kGap : 0.0f);
        label(rt, name.c_str(), fonts_.body(), name_x, cy + (head_h - lh_) * 0.5f, name_w, pal.text);
        if (dw > 0.0f) label(rt, detail.c_str(), fonts_.body(), ix + iw - dw, cy + (head_h - lh_) * 0.5f, dw, pal.muted);
        cy += head_h + kGap;

        float sw = side.empty() ? 0.0f : text_width(side.c_str(), fonts_.big());
        label(rt, value.c_str(), fonts_.big(), ix, cy, iw - (sw > 0.0f ? sw + kGap : 0.0f), pal.text);
        if (sw > 0.0f) label(rt, side.c_str(), fonts_.big(), ix + iw - sw, cy, sw, pal.muted);
        cy += lhb_ + kGap;

        if (graph) draw_graph(rt, pal, ix, cy, iw, 54.0f, 3.0f, *graph);
        else draw_meter(rt, pal, ix, cy, 34, 2.0f, fraction, hot);
        cy += body_h + kGap;

        float fx = 0.0f;
        for (const auto& f : facts) {
            float fw = text_width(f.c_str(), fonts_.body());
            if (fx > 0.0f && fx + fw > iw) {
                fx = 0.0f;
                cy += lh_ + kHalf;
            }
            label(rt, f.c_str(), fonts_.body(), ix + fx, cy, std::min(fw, iw), pal.muted);
            fx += fw + kCardPad;
        }
        y += h + kPageGap;
    };

    wchar_t threads[32];
    std::swprintf(threads, std::size(threads), L"%d threads", info.threads);
    draw_card(ART_CHIP, utf8_to_wide(info.cpu_name), threads, percent_text(s.cpu), celsius_text(s.cpu_temp, L""),
              &cpu_graph_, 0.0, false, { L"Load over the last minutes" });

    if (!info.gpu_name.empty()) {
        std::vector<std::wstring> facts;
        if (s.vram_total > 0) {
            facts.push_back(L"Video memory " + utf8_to_wide(format_bytes(s.vram_used)) + L" of " +
                            utf8_to_wide(format_bytes(s.vram_total)));
        }
        if (!std::isnan(s.gpu_fan_rpm)) {
            facts.push_back(L"Fan " + std::to_wstring(static_cast<int>(std::lround(s.gpu_fan_rpm))) + L" rpm");
        }
        draw_card(ART_SCREEN, utf8_to_wide(info.gpu_name), L"", percent_text(s.gpu), celsius_text(s.gpu_temp, L""),
                  &gpu_graph_, 0.0, false, facts);
    }

    double mfrac = s.mem_total ? static_cast<double>(s.mem_used) / static_cast<double>(s.mem_total) : -1.0;
    std::vector<std::wstring> mem_facts;
    if (s.mem_total) mem_facts.push_back(L"Used of " + utf8_to_wide(format_bytes(s.mem_total)));
    draw_card(ART_MEMORY, L"Memory", L"", s.mem_total ? utf8_to_wide(format_bytes(s.mem_used)) : L"…", L"",
              nullptr, mfrac, mfrac > 0.9, mem_facts);

    double dfrac = s.disk_total ? static_cast<double>(s.disk_used) / static_cast<double>(s.disk_total) : -1.0;
    uint64_t dfree = s.disk_total > s.disk_used ? s.disk_total - s.disk_used : 0;
    std::vector<std::wstring> disk_facts;
    if (s.disk_total) disk_facts.push_back(L"Free of " + utf8_to_wide(format_bytes(s.disk_total)));
    draw_card(ART_DISK, L"System drive", utf8_to_wide(info.disk_kind),
              s.disk_total ? utf8_to_wide(format_bytes(dfree)) : L"…", celsius_text(s.disk_temp, L""), nullptr,
              dfrac, dfrac > 0.9, disk_facts);

    content_height_ = (y - kPageGap - start_y) + kPagePadTop + kPagePadBottom;
}

} // namespace lares
