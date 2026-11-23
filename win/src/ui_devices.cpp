// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Headphones, Keyboard, Mouse and Controller pages, laid out as
 * src/device_view.c and src/pad_view.c lay theirs: padded cards whose rows
 * are 18 px apart, settings split by a 2 px line with 18 px either side,
 * chips as wide as their text and 9 px apart, wrapping as they need. */
#include "ui.h"
#include "ui_internal.h"
extern "C" {
#include "art.h"
}

#include <cmath>
#include <cwchar>

namespace lares {

namespace {

constexpr uint32_t kSwatchColours[9] = {
    0xff0000, 0xff5000, 0xffc800, 0x00ff00, 0x00ffff, 0x0000ff, 0x8000ff, 0xff00a0, 0xffffff
};
constexpr int kTimerMinutes[5] = { 1, 2, 5, 10, 15 };

float meter_width(int blocks, float pixel)
{
    return pixel * (4.0f * blocks - 1.0f);
}

int count_bits(uint32_t v)
{
    int c = 0;
    for (; v; v &= v - 1) ++c;
    return c;
}

} // namespace

// ============================================== Headphones, Keyboard, Mouse

void MainWindow::draw_devices_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w, DevKind kind)
{
    float start_y = y;
    device_hits_.clear();
    float ix = x + kCardPad, iw = w - kCardPad * 2.0f, btn_h = lh_ + 18.0f;
    bool any = false;

    for (const auto& d : dev_list_) {
        if (d.kind != kind) continue;
        if (any) y += kGap;
        any = true;

        y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
            float cy = top;
            auto hit = [&](const D2D1_RECT_F& rc, DeviceActionKind act, int value) {
                if (!measuring_) device_hits_.push_back({ rc, d.id, act, value });
            };
            auto next_setting = [&]() { cy = setting_line(rt, pal, ix, cy, iw); };

            // The head: its picture (4 px a dot, in 44 px), then its name, state and battery.
            {
                const char* const* art = kind == DevKind::Audio ? ART_HEADPHONES
                    : kind == DevKind::Keyboard ? ART_KEYBOARD : ART_MOUSE;
                const wchar_t* status = !d.connected ? L"Not connected"
                    : (d.charging && d.battery >= 0) ? L"Charging" : nullptr;
                bool battery = d.connected && d.battery >= 0;
                float texts_h = lh_ + (status ? kHalf + lh_ : 0.0f) + (battery ? kHalf + lh_ : 0.0f);
                float head_h = std::max(36.0f, texts_h);
                icon(rt, art, 4.0f, ix, cy, 44.0f, head_h, d.connected ? pal.accent : pal.muted);
                float tx = ix + 44.0f + kGap, tw = ix + iw - tx;
                float ty = cy + std::round((head_h - texts_h) * 0.5f);
                std::wstring name = utf8_to_wide(d.name);
                label(rt, name.c_str(), fonts_.body(), tx, ty, tw, pal.text);
                ty += lh_;
                if (status) {
                    ty += kHalf;
                    label(rt, status, fonts_.body(), tx, ty, tw, pal.muted);
                    ty += lh_;
                }
                if (battery) {
                    ty += kHalf;
                    bool low = d.battery <= 20;
                    meter_row(rt, pal, tx, ty, lh_, 10, 2.0f, d.battery / 100.0, low);
                    std::wstring pct = std::to_wstring(d.battery) + L"%";
                    label(rt, pct.c_str(), fonts_.body(), tx + meter_width(10, 2.0f) + kHalf, ty, 90.0f,
                          low ? pal.bad : pal.accent);
                }
                cy += head_h;
            }

            // A detail: its name, muted, and its value at the right, in a row of 36 px.
            auto detail = [&](const wchar_t* title, const std::wstring& value) {
                next_setting();
                float vw = std::min(text_width(value.c_str(), fonts_.body()), iw * 0.6f);
                label(rt, title, fonts_.body(), ix, cy + (36.0f - lh_) * 0.5f, iw - vw - kGap, pal.muted);
                label(rt, value.c_str(), fonts_.body(), ix + iw - vw, cy + (36.0f - lh_) * 0.5f, vw, pal.text);
                cy += 36.0f;
            };
            if (!d.vendor_name.empty()) detail(L"Made by", utf8_to_wide(d.vendor_name));
            detail(L"Connection", utf8_to_wide(dev_link_text(d.link)) + (d.asleep ? L", asleep" : L""));

            /* A setting picked from a dropdown at its row's right; `shown`
             * names a value set elsewhere that's none of the choices. */
            auto pick_row = [&](const char* what, const wchar_t* title, const wchar_t* sub,
                                std::vector<std::wstring> choices, std::vector<int> values, int now,
                                DeviceActionKind act, std::wstring shown) {
                next_setting();
                Dropdown dd;
                dd.key = std::string(what) + ":" + d.id;
                dd.choices = std::move(choices);
                dd.values = std::move(values);
                for (size_t i = 0; i < dd.values.size(); ++i) {
                    if (dd.values[i] == now) dd.checked = static_cast<int>(i);
                }
                dd.shown = std::move(shown);
                std::string id = d.id;
                dd.pick = [this, id, act](int value) { device_action(id, act, value); };
                cy += setting_dropdown(rt, pal, ix, cy, iw, title, sub, std::move(dd));
            };

            struct Chip {
                std::wstring text;
                bool on;
                DeviceActionKind act;
                int value;
            };
            auto chips = [&](const std::vector<Chip>& items) {
                float cx = ix;
                for (const auto& c : items) {
                    float cw = chip_width(c.text.c_str());
                    if (cx > ix && cx + cw > ix + iw) {
                        cx = ix;
                        cy += btn_h + kHalf;
                    }
                    D2D1_RECT_F rc = D2D1::RectF(cx, cy, cx + cw, cy + btn_h);
                    chip(rt, pal, rc, c.text.c_str(), c.on);
                    hit(rc, c.act, c.value);
                    cx += cw + kHalf;
                }
                cy += btn_h;
            };
            // A colour to pick: a chip of 9 px around an 18 px square of it.
            auto swatches = [&](uint32_t now, bool coloured, DeviceActionKind act) {
                float cx = ix;
                for (uint32_t rgb : kSwatchColours) {
                    if (cx > ix && cx + 36.0f > ix + iw) {
                        cx = ix;
                        cy += 36.0f + kHalf;
                    }
                    D2D1_RECT_F rc = D2D1::RectF(cx, cy, cx + 36.0f, cy + 36.0f);
                    bool on = coloured && rgb == now;
                    fill(rt, rc, on ? pal.accent : hovered(rc) ? pal.hover : pal.chip, 9.0f);
                    Rgba edge = pal.text;
                    edge.a *= 0.3f;
                    fill(rt, D2D1::RectF(cx + 9.0f, cy + 9.0f, cx + 27.0f, cy + 27.0f), edge);
                    Rgba colour = { ((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, 1.0f };
                    fill(rt, D2D1::RectF(cx + 11.0f, cy + 11.0f, cx + 25.0f, cy + 25.0f), colour);
                    hit(rc, act, static_cast<int>(rgb));
                    cx += 36.0f + kHalf;
                }
                cy += 36.0f;
            };
            /* Windows Dynamic Lighting paints over the device's own effects
             * while it's on for it: a note says so where they're picked,
             * with the way to Settings. */
            auto lighting_note = [&]() {
                if (!d.windows_lighting) return;
                const wchar_t* text = kind == DevKind::Keyboard
                    ? L"Windows Dynamic Lighting is painting this keyboard's lights, so these effects won't show "
                      L"until you turn it off for this keyboard."
                    : L"Windows Dynamic Lighting is painting this mouse's lights, so these effects won't show "
                      L"until you turn it off for this mouse.";
                const wchar_t* button = L"Open Dynamic Lighting";
                float tx = ix + kCardPad + 18.0f + kGap, tw = ix + iw - kCardPad - tx;
                float th = para_height(text, tw);
                float h = kGap * 2.0f + th + kGap + btn_h;
                fill(rt, D2D1::RectF(ix, cy, ix + iw, cy + h), pal.note, kCardRadius);
                icon(rt, ART_BANG, 2.0f, ix + kCardPad, cy + kGap, 18.0f, 27.0f, pal.warn);
                para(rt, text, tx, cy + kGap, tw, pal.text);
                float bw = chip_width(button);
                D2D1_RECT_F rc = D2D1::RectF(ix + iw - kCardPad - bw, cy + kGap + th + kGap, ix + iw - kCardPad,
                                             cy + kGap + th + kGap + btn_h);
                chip(rt, pal, rc, button, false, true);
                hit(rc, DeviceActionKind::OpenDynamicLighting, 0);
                cy += h + kGap;
            };
            /* Brightness: its meter (3 px a dot) and, 18 px on, its value
             * in 54 px at the right. A block counts once clicked; left of
             * the first is off. */
            auto brightness = [&](int blocks, double fraction, int percent, DeviceActionKind act,
                                  const std::vector<int>& values) {
                next_setting();
                float mw = meter_width(blocks, 3.0f);
                float mx = ix + iw - 54.0f - kGap - mw;
                label(rt, L"Brightness", fonts_.body(), ix, cy + (36.0f - lh_) * 0.5f, mx - kGap - ix, pal.text);
                meter_row(rt, pal, mx, cy, 36.0f, blocks, 3.0f, fraction, false);
                std::wstring pct = std::to_wstring(percent) + L"%";
                label(rt, pct.c_str(), fonts_.body(), ix + iw - 54.0f, cy + (36.0f - lh_) * 0.5f, 54.0f, pal.accent,
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
                hit(D2D1::RectF(mx - 18.0f, cy, mx, cy + 36.0f), act, values[0]);
                for (int b = 1; b <= blocks; ++b) {
                    float bx = mx + (b - 1) * 12.0f;
                    hit(D2D1::RectF(bx, cy, bx + 12.0f, cy + 36.0f), act, values[static_cast<size_t>(b)]);
                }
                cy += 36.0f;
            };

            bool show_settings = d.has_settings && d.settings_loaded;
            if (d.has_settings && !d.settings_loaded) {
                next_setting();
                const wchar_t* wait = !d.asleep ? L"Reading its settings…"
                    : kind == DevKind::Keyboard ? L"Press a key to wake it, and its settings show here."
                                                : L"Move the mouse to wake it, and its settings show here.";
                para(rt, wait, ix, cy, iw, pal.muted);
                cy += std::max(36.0f, para_height(wait, iw));
            }

            if (show_settings && kind == DevKind::Mouse) {
                const MouseSettings& m = d.mouse;

                // DPI: its steppers around its value in 72 px, then a chip a stage.
                next_setting();
                {
                    const wchar_t* minus = L"−";
                    const wchar_t* plus = L"+";
                    float sw = chip_width(plus);
                    float controls = sw + kGap + 72.0f + kGap + sw;
                    float h = setting(rt, pal, ix, cy, iw - controls - kGap, L"DPI",
                                      L"Its DPI button steps through these.", nullptr, 1.0f, btn_h);
                    float by = cy + std::round((h - btn_h) * 0.5f);
                    D2D1_RECT_F plus_rc = D2D1::RectF(ix + iw - sw, by, ix + iw, by + btn_h);
                    D2D1_RECT_F minus_rc = D2D1::RectF(ix + iw - controls, by, ix + iw - controls + sw, by + btn_h);
                    chip(rt, pal, minus_rc, minus, false);
                    chip(rt, pal, plus_rc, plus, false);
                    hit(minus_rc, DeviceActionKind::MouseDpiDelta, -100);
                    hit(plus_rc, DeviceActionKind::MouseDpiDelta, 100);
                    int dpi = (m.active >= 0 && m.active < m.stages) ? m.dpi[m.active] : 0;
                    std::wstring now = std::to_wstring(dpi);
                    label(rt, now.c_str(), fonts_.body(), minus_rc.right + kGap, by + 9.0f, 72.0f, pal.accent,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
                    cy += h + kGap;
                    std::vector<Chip> stages;
                    for (int s = 0; s < m.stages; ++s) {
                        stages.push_back({ std::to_wstring(m.dpi[s]), s == m.active, DeviceActionKind::MouseStage, s });
                    }
                    chips(stages);
                }

                {
                    std::vector<std::wstring> names;
                    std::vector<int> rates;
                    for (int hz = 125; hz <= (m.rate_max > 0 ? m.rate_max : 1000); hz *= 2) {
                        names.push_back(std::to_wstring(hz) + L" Hz");
                        rates.push_back(hz);
                    }
                    pick_row("rate", L"Polling rate", L"Higher is smoother, and uses more battery.", std::move(names),
                             std::move(rates), m.rate, DeviceActionKind::MouseRate, std::to_wstring(m.rate) + L" Hz");
                }

                pick_row("lift", L"Lift-off distance", nullptr, { L"1 mm", L"2 mm" }, { 1, 2 }, m.lift_mm,
                         DeviceActionKind::MouseLift, std::to_wstring(m.lift_mm) + L" mm");

                // The effect, then what says Dynamic Lighting paints over it, then its colour.
                pick_row("light", L"Lighting", nullptr, { L"Off", L"Solid", L"Breathing", L"Colour cycle" },
                         { 0, 1, 2, 3 }, static_cast<int>(m.light), DeviceActionKind::MouseLight, L"Other");
                cy += kGap;
                lighting_note();
                if (m.light == LightEffect::Other) {
                    para(rt, L"Now an effect another app saved to it.", ix, cy, iw, pal.muted);
                    cy += 36.0f;
                } else {
                    uint32_t rgb = (static_cast<uint32_t>(m.rgb[0]) << 16) | (static_cast<uint32_t>(m.rgb[1]) << 8) | m.rgb[2];
                    swatches(rgb, m.light == LightEffect::Solid || m.light == LightEffect::Breathing,
                             DeviceActionKind::MouseColour);
                }

                std::vector<int> values;
                for (int b = 0; b <= 10; ++b) values.push_back(b * 10);
                brightness(10, m.brightness / 100.0, m.brightness, DeviceActionKind::MouseBrightness, values);
            }

            if (show_settings && kind == DevKind::Keyboard) {
                const KeyboardSettings& k = d.keyboard;

                if (k.has_brightness) {
                    static constexpr int kDefault[5] = { 0, 12, 25, 50, 100 };
                    int levels = k.levels > 1 ? k.levels : 5;
                    std::vector<int> values;
                    int nearest = 0;
                    for (int i = 0; i < levels; ++i) {
                        values.push_back(k.levels > 1 ? k.level[i] : kDefault[i]);
                        if (std::abs(values[static_cast<size_t>(i)] - k.brightness) <
                            std::abs(values[static_cast<size_t>(nearest)] - k.brightness)) {
                            nearest = i;
                        }
                    }
                    brightness(levels - 1, static_cast<double>(nearest) / (levels - 1), k.brightness,
                               DeviceActionKind::KeyBrightness, values);
                }

                if (k.has_lighting) {
                    const wchar_t* sub = pad_prefs_.background
                        ? L"Wave, Ripple and Built-in stay on the keyboard. The others play while Lares runs in the "
                          L"background."
                        : L"Wave, Ripple and Built-in stay on the keyboard. The others play while Lares runs in the "
                          L"background, which is off in Settings.";
                    pick_row("keylight", L"Lighting", sub,
                             { L"Off", L"Solid", L"Breathing", L"Colour cycle", L"Wave", L"Ripple", L"Built-in" },
                             { 0, 1, 2, 3, 4, 5, 6 }, static_cast<int>(k.light), DeviceActionKind::KeyLight, L"Other");
                    cy += kGap;
                    lighting_note();
                    uint32_t rgb = (static_cast<uint32_t>(k.rgb[0]) << 16) | (static_cast<uint32_t>(k.rgb[1]) << 8) | k.rgb[2];
                    swatches(rgb, key_light_coloured(k.light), DeviceActionKind::KeyColour);
                    if (k.app_controls && !d.windows_lighting) {
                        const wchar_t* held = L"Another app holds its lighting now, so what shows may differ.";
                        cy += kGap;
                        para(rt, held, ix, cy, iw, pal.muted);
                        cy += para_height(held, iw);
                    }
                }

                auto key_switch = [&](const wchar_t* title, const wchar_t* sub, bool on, DeviceActionKind act) {
                    next_setting();
                    float h = setting(rt, pal, ix, cy, iw - 54.0f - kGap, title, sub, nullptr, 1.0f, 27.0f);
                    float sy = cy + std::round((h - 27.0f) * 0.5f);
                    D2D1_RECT_F rc = D2D1::RectF(ix + iw - 54.0f, sy, ix + iw, sy + 27.0f);
                    draw_switch(rt, pal, rc, on, 1.0f);
                    hit(rc, act, on ? 0 : 1);
                    cy += h;
                };
                if (k.has_startup) key_switch(L"Startup effect", nullptr, k.startup, DeviceActionKind::KeyStartup);
                if (k.has_shutdown) key_switch(L"Shutdown effect", nullptr, k.shutdown, DeviceActionKind::KeyShutdown);

                /* A time set elsewhere, such as G HUB's seconds, matches no
                 * choice: the button says it, with none checked. */
                auto timer = [&](const char* what, const wchar_t* title, const wchar_t* sub, int secs,
                                 DeviceActionKind act) {
                    std::vector<std::wstring> names;
                    std::vector<int> values;
                    for (int m : kTimerMinutes) {
                        names.push_back(std::to_wstring(m) + L" min");
                        values.push_back(m * 60);
                    }
                    std::wstring shown = secs < 1 ? L"…"
                        : secs < 60 ? std::to_wstring(secs) + L" s"
                        : secs % 60 == 0 ? std::to_wstring(secs / 60) + L" min"
                        : std::to_wstring(secs / 60) + L" min " + std::to_wstring(secs % 60) + L" s";
                    pick_row(what, title, sub, std::move(names), std::move(values), secs, act, shown);
                };
                if (k.has_timers) {
                    timer("dim", L"Dim after", L"Counted from the last key press.", k.dim_s, DeviceActionKind::KeyDim);
                    timer("off", L"Lights off after", nullptr, k.off_s, DeviceActionKind::KeyOff);
                }
            }

            if (!d.status_error.empty()) {
                std::wstring err = utf8_to_wide(d.status_error);
                cy += kGap;
                para(rt, err.c_str(), ix, cy, iw, pal.bad);
                cy += para_height(err.c_str(), iw);
            }
            return cy;
        });
    }

    content_height_ = (y - start_y) + kPagePadTop + kPagePadBottom;
}

// ============================================================== Controller

void MainWindow::draw_controller_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    pad_hits_.clear();
    float ix = x + kCardPad, iw = w - kCardPad * 2.0f, btn_h = lh_ + 18.0f;

    wchar_t env_tab[32] = {};
    int default_tab = 0;
    if (GetEnvironmentVariableW(L"LARES_PAD_TAB", env_tab, 32) > 0) {
        if (_wcsicmp(env_tab, L"sticks") == 0) default_tab = 1;
        else if (_wcsicmp(env_tab, L"vibration") == 0) default_tab = 2;
    }

    auto hit = [&](const D2D1_RECT_F& rc, const std::string& path, PadActionKind act, int value) {
        if (!measuring_) pad_hits_.push_back({ rc, path, act, value });
    };
    // A button as wide as its text; `primary` is the accent one.
    auto button = [&](float bx, float by, const wchar_t* text, bool primary, const std::string& path,
                      PadActionKind act, int value) {
        D2D1_RECT_F rc = D2D1::RectF(bx, by, bx + chip_width(text), by + btn_h);
        chip(rt, pal, rc, text, primary);
        hit(rc, path, act, value);
        return rc.right;
    };

    // The controllers' cards, 18 px apart.
    if (pad_list_.empty()) {
        y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
            PadState idle{};
            draw_pad_picture(rt, pal, ix, top, iw, idle, 0, true, 0);
            float cy = top + static_cast<float>(pad_map_.height) + kGap;
            const wchar_t* none = L"No controller connected. Plug in a USB cable, or connect one over Bluetooth.";
            para(rt, none, ix, cy, iw, pal.muted, DWRITE_TEXT_ALIGNMENT_CENTER);
            return cy + para_height(none, iw);
        });
    }
    bool first_pad = true;
    for (const auto& p : pad_list_) {
        auto it = pad_memory_.find(p.path);
        if (it == pad_memory_.end()) {
            PadCardMemory fresh;
            fresh.tab = default_tab;
            it = pad_memory_.emplace(p.path, fresh).first;
        }
        PadCardMemory& mem = it->second;
        int tab = p.rumble ? mem.tab : std::min(mem.tab, 1);
        if (!first_pad) y += kGap;
        first_pad = false;

        y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
            float cy = top;

            // The head: its picture, its name over where it is, and the pencil.
            {
                bool editing = editing_pad_path_ == p.path;
                float texts_h = (editing ? 27.0f : lh_) + kHalf + lh_;
                float head_h = std::max(36.0f, texts_h);
                icon(rt, ART_GAMEPAD, 4.0f, ix, cy, 44.0f, head_h, pal.accent);
                float tx = ix + 44.0f + kGap;
                float side_w = editing ? chip_width(L"Save") : 54.0f;
                float tw = ix + iw - side_w - kGap - tx;
                float ty = cy + std::round((head_h - texts_h) * 0.5f);
                float by = cy + std::round((head_h - btn_h) * 0.5f);
                if (editing) {
                    // window.lares entry: 27 px tall, 9 px beside its text, corners of 9.
                    D2D1_RECT_F box = D2D1::RectF(tx, ty, tx + tw, ty + 27.0f);
                    fill(rt, box, pal.chip, 9.0f);
                    if (!measuring_) {
                        brush_->SetColor(to_d2d(pal.accent));
                        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 1.0f, box.top + 1.0f,
                                                                               box.right - 1.0f, box.bottom - 1.0f),
                                                                   8.0f, 8.0f), brush_, 2.0f);
                    }
                    std::wstring text = editing_nickname_.empty() ? L"A nickname, such as Blue pad"
                                                                  : utf8_to_wide(editing_nickname_) + L"_";
                    label(rt, text.c_str(), fonts_.body(), tx + kHalf, ty + (27.0f - lh_) * 0.5f, tw - 18.0f,
                          editing_nickname_.empty() ? pal.muted : pal.text);
                    ty += 27.0f;
                    button(ix + iw - side_w, by, L"Save", true, p.path, PadActionKind::EditNickname, 1);
                } else {
                    std::wstring title = utf8_to_wide(pad_display_name(p));
                    label(rt, title.c_str(), fonts_.body(), tx, ty, tw, pal.text);
                    ty += lh_;
                    D2D1_RECT_F pen = D2D1::RectF(ix + iw - 54.0f, by, ix + iw, by + btn_h);
                    fill(rt, pen, hovered(pen) ? pal.hover : pal.chip, 9.0f);
                    icon(rt, ART_PENCIL, 2.0f, pen.left, pen.top, 54.0f, btn_h, pal.text);
                    hit(pen, p.path, PadActionKind::EditNickname, 0);
                }
                std::string where = !p.nickname.empty() ? p.name + " \xC2\xB7 " + pad_connection_name(p.connection)
                                                        : pad_connection_name(p.connection);
                std::wstring sub = utf8_to_wide(where);
                label(rt, sub.c_str(), fonts_.body(), tx, ty + kHalf, tw, pal.muted);
                cy += head_h + kGap;
            }

            // The tabs, chips 9 px apart.
            {
                static const wchar_t* const kTabs[3] = { L"Buttons", L"Sticks", L"Vibration" };
                float cx = ix;
                for (int t = 0; t < (p.rumble ? 3 : 2); ++t) {
                    D2D1_RECT_F rc = D2D1::RectF(cx, cy, cx + chip_width(kTabs[t]), cy + btn_h);
                    chip(rt, pal, rc, kTabs[t], tab == t);
                    hit(rc, p.path, PadActionKind::SelectTab, t);
                    cx = rc.right + kHalf;
                }
                cy += btn_h + kGap;
            }

            // A meter under its name and value, 9 px apart.
            auto meter_block = [&](float mx, float mw, const wchar_t* heading, const std::wstring& value, int blocks,
                                   double fraction) {
                float vw = text_width(value.c_str(), fonts_.body());
                label(rt, heading, fonts_.body(), mx, cy, mw - vw - kHalf, pal.text);
                label(rt, value.c_str(), fonts_.body(), mx + mw - vw, cy, vw, pal.accent);
                draw_meter(rt, pal, mx, cy + lh_ + kHalf, blocks, 2.0f, fraction, false);
            };

            if (tab == 0) {
                // The picture, 2 px a cell where it fits, over the count and Reset.
                float scale = iw >= pad_map_.width * 2.0f ? 2.0f : 1.0f;
                draw_pad_picture(rt, pal, ix, cy, iw, p.state, mem.seen, false, mem.shake_offset);
                cy += pad_map_.height * scale + kHalf;
                int total = count_bits(p.checklist), seen = count_bits(mem.seen & p.checklist);
                bool done = total > 0 && seen == total;
                std::wstring count = done ? L"Every button responded"
                    : L"Press every button: " + std::to_wstring(seen) + L" of " + std::to_wstring(total);
                float rw = chip_width(L"Reset");
                label(rt, count.c_str(), fonts_.body(), ix, cy + 9.0f, iw - rw - kGap, done ? pal.accent : pal.muted);
                button(ix + iw - rw, cy, L"Reset", false, p.path, PadActionKind::ResetChecklist, 0);
                cy += btn_h;
            } else if (tab == 1) {
                // Two columns, 18 px apart: a stick's name, its gauge, what it reads, and its trigger.
                float col_w = std::floor((iw - kGap) * 0.5f);
                static const wchar_t* const kSticks[2] = { L"Left stick", L"Right stick" };
                static const wchar_t* const kTriggers[2] = { L"Left trigger", L"Right trigger" };
                const float triggers[2] = { p.state.lt, p.state.rt };
                std::wstring reads[2];
                float read_h = 0.0f;
                for (int i = 0; i < 2; ++i) {
                    const StickTracker& st = mem.sticks[i];
                    std::wstring at = std::to_wstring(static_cast<int>(std::lround(st.x * 100.0))) + L"%, " +
                                      std::to_wstring(static_cast<int>(std::lround(-st.y * 100.0))) + L"% · ";
                    if (stick_drifting(st)) {
                        at += L"Rests " + std::to_wstring(static_cast<int>(std::lround(std::hypot(st.rx, st.ry) * 100.0))) +
                              L"% off centre";
                    } else {
                        at += st.has_rest ? L"Rests in the centre" : L"Hold still to check rest";
                    }
                    reads[i] = at;
                    read_h = std::max(read_h, para_height(at.c_str(), col_w));
                }
                float col_top = cy;
                for (int i = 0; i < 2; ++i) {
                    float sx = ix + i * (col_w + kGap);
                    const StickTracker& st = mem.sticks[i];
                    bool drift = stick_drifting(st);
                    cy = col_top;
                    label(rt, kSticks[i], fonts_.body(), sx, cy, col_w, pal.text);
                    cy += lh_ + kHalf;
                    if (!measuring_) {
                        float gx = sx + 63.0f, gy = cy + 63.0f, rad = 61.0f;
                        D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(gx, gy), rad, rad);
                        brush_->SetColor(to_d2d(pal.text, 0.07f));
                        rt->FillEllipse(outer, brush_);
                        brush_->SetColor(to_d2d(pal.text, 0.22f));
                        rt->DrawEllipse(outer, brush_, 1.0f);
                        brush_->SetColor(to_d2d(pal.text, 0.10f));
                        rt->DrawLine(D2D1::Point2F(gx - rad, gy), D2D1::Point2F(gx + rad, gy), brush_, 1.0f);
                        rt->DrawLine(D2D1::Point2F(gx, gy - rad), D2D1::Point2F(gx, gy + rad), brush_, 1.0f);
                        float dead = static_cast<float>(rad * kStickDeadZone);
                        D2D1_ELLIPSE dz = D2D1::Ellipse(D2D1::Point2F(gx, gy), dead, dead);
                        brush_->SetColor(to_d2d(pal.text, 0.16f));
                        rt->FillEllipse(dz, brush_);
                        brush_->SetColor(to_d2d(pal.text, 0.40f));
                        rt->DrawEllipse(dz, brush_, 1.0f);
                        auto at = [&](double px, double py) {
                            double reach = std::hypot(px, py);
                            if (reach > 1.0) {
                                px /= reach;
                                py /= reach;
                            }
                            return D2D1::Point2F(gx + static_cast<float>(px * rad), gy + static_cast<float>(py * rad));
                        };
                        if (st.has_rest) {
                            brush_->SetColor(drift ? to_d2d(pal.warn) : to_d2d(pal.text, 0.55f));
                            rt->DrawEllipse(D2D1::Ellipse(at(st.rx, st.ry), 6.0f, 6.0f), brush_, 1.5f);
                        }
                        brush_->SetColor(to_d2d(pal.live));
                        rt->FillEllipse(D2D1::Ellipse(at(st.x, st.y), 3.5f, 3.5f), brush_);
                    }
                    cy += 126.0f + kHalf;
                    para(rt, reads[i].c_str(), sx, cy, col_w, drift ? pal.warn : pal.muted);
                    cy += read_h + kHalf;
                    std::wstring pull = std::to_wstring(static_cast<int>(std::lround(triggers[i] * 100.0f))) + L"%";
                    meter_block(sx, col_w, kTriggers[i], pull, 15, triggers[i]);
                    cy += lh_ + kHalf + 6.0f;
                }
            } else {
                // Three meters to set, 18 px apart, then what they are and Play.
                auto slider = [&](const wchar_t* heading, const std::wstring& value, double fraction,
                                  PadActionKind act, int max_value, int step) {
                    meter_block(ix, iw, heading, value, 20, fraction);
                    float my = cy + lh_;
                    hit(D2D1::RectF(ix - 18.0f, my, ix, my + 24.0f), p.path, act, 0);
                    for (int b = 1; b <= 20; ++b) {
                        float bx = ix + (b - 1) * 8.0f;
                        hit(D2D1::RectF(bx, my, bx + 8.0f, my + 24.0f), p.path, act, ((b * max_value / 20) / step) * step);
                    }
                    cy += lh_ + kHalf + 6.0f + kGap;
                };
                wchar_t length[24];
                std::swprintf(length, std::size(length), L"%.1f s", mem.length_ms / 1000.0);
                slider(L"Strong motor, on the left", std::to_wstring(mem.strong) + L"%", mem.strong / 100.0,
                       PadActionKind::StrongMotor, 100, 5);
                slider(L"Weak motor, on the right", std::to_wstring(mem.weak) + L"%", mem.weak / 100.0,
                       PadActionKind::WeakMotor, 100, 5);
                slider(L"Length", length, mem.length_ms / 2000.0, PadActionKind::LengthMs, 2000, 100);
                const wchar_t* about = L"Windows drives these two motors; the ones in the triggers aren't part of it.";
                float pw = std::max(chip_width(L"Play"), chip_width(L"Playing…"));
                float aw = iw - pw - kGap;
                float ah = std::max(para_height(about, aw), btn_h);
                para(rt, about, ix, cy, aw, pal.muted);
                D2D1_RECT_F play = D2D1::RectF(ix + iw - pw, cy + std::round((ah - btn_h) * 0.5f), ix + iw,
                                               cy + std::round((ah - btn_h) * 0.5f) + btn_h);
                chip(rt, pal, play, p.playing_rumble ? L"Playing…" : L"Play", true);
                hit(play, p.path, PadActionKind::PlayVibration, 0);
                cy += ah;
            }
            cy += kGap;

            // The facts, 27 px apart: each a muted name over its value.
            {
                float fx = ix;
                auto fact = [&](const wchar_t* heading, float width) {
                    if (fx > ix && fx + width > ix + iw) {
                        fx = ix;
                        cy += lh_ * 2.0f + kHalf + kCardPad;
                    }
                    label(rt, heading, fonts_.body(), fx, cy, width, pal.muted);
                    float at = fx;
                    fx += width + kCardPad;
                    return at;
                };
                if (p.battery.known) {
                    bool low = pad_battery_low_stage(p.battery, pad_prefs_.low_level) > 0 && !pad_battery_charging(p.battery);
                    std::wstring text = utf8_to_wide(pad_battery_text(p.battery));
                    float tw = text_width(text.c_str(), fonts_.body());
                    float at = fact(L"Battery", meter_width(10, 2.0f) + kHalf + tw);
                    meter_row(rt, pal, at, cy + lh_ + kHalf, lh_, 10, 2.0f, pad_battery_fraction(p.battery), low);
                    label(rt, text.c_str(), fonts_.body(), at + meter_width(10, 2.0f) + kHalf, cy + lh_ + kHalf, tw,
                          low ? pal.bad : pal.text);
                } else {
                    const wchar_t* text = p.connection == PadConnection::Usb ? L"On USB power" : L"No reading yet";
                    float tw = text_width(text, fonts_.body());
                    float at = fact(L"Battery", tw);
                    label(rt, text, fonts_.body(), at, cy + lh_ + kHalf, tw, pal.text);
                }
                auto plain = [&](const wchar_t* heading, const std::string& value) {
                    if (value.empty()) return;
                    std::wstring text = utf8_to_wide(value);
                    float tw = std::max(text_width(text.c_str(), fonts_.body()), text_width(heading, fonts_.body()));
                    float at = fact(heading, tw);
                    label(rt, text.c_str(), fonts_.body(), at, cy + lh_ + kHalf, tw, pal.text);
                };
                plain(L"Model", p.model);
                plain(L"Firmware", p.firmware);
                cy += lh_ * 2.0f + kHalf + kGap;
            }

            std::wstring details = utf8_to_wide((p.driver.empty() ? "XInput" : p.driver) + " \xC2\xB7 " + p.event_node);
            label(rt, details.c_str(), fonts_.body(), ix, cy, iw, pal.muted);
            cy += lh_;

            // What can be done with it, buttons 9 px apart.
            bool test = p.rumble && tab != 2, find = p.rumble && p.connection != PadConnection::Usb;
            if (test || find) {
                cy += kGap;
                float bx = ix;
                if (test) bx = button(bx, cy, L"Test vibration", false, p.path, PadActionKind::TestVibration, 0) + kHalf;
                if (find) {
                    float fw = std::max(chip_width(L"Find controller"), chip_width(L"Stop finding"));
                    D2D1_RECT_F rc = D2D1::RectF(bx, cy, bx + fw, cy + btn_h);
                    chip(rt, pal, rc, p.finding ? L"Stop finding" : L"Find controller", p.finding);
                    hit(rc, p.path, PadActionKind::FindController, 0);
                }
                cy += btn_h;
            }
            return cy;
        });
    }
    y += kPageGap;

    // Pairing: what to do, in room for three lines, over its button at the right.
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        const wchar_t* how = L"Hold the pair button on top of the controller until the Xbox button flashes quickly.";
        para(rt, how, ix, top, iw, pal.muted);
        float cy = top + std::max(81.0f, para_height(how, iw)) + kGap;
        const wchar_t* pair = L"Pair a controller";
        button(ix + iw - chip_width(pair), cy, pair, true, "", PadActionKind::OpenBluetoothSettings, 0);
        return cy + btn_h;
    });
    y += kPageGap;

    if (!pad_prefs_.background) {
        y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
            const wchar_t* open = L"Open Settings";
            float bw = chip_width(open);
            float h = setting(rt, pal, ix, top, iw - bw - kGap, L"Background is off", L"Alerts and play features need it.",
                              nullptr, 1.0f, btn_h);
            button(ix + iw - bw, top + std::round((h - btn_h) * 0.5f), open, false, "", PadActionKind::OpenSettings, 0);
            return top + h;
        });
        y += kPageGap;
    }

    // The settings that need the background helper, dimmed while it's off.
    bool on = pad_prefs_.background;
    float alpha = on ? 1.0f : 0.5f;
    auto heading = [&](const wchar_t* text) {
        label(rt, text, fonts_.body(), ix, y, iw, pal.muted);
        y += lh_ + kHalf;
    };
    auto toggle = [&](float cy, const wchar_t* title, const wchar_t* sub, bool* field) {
        float h = setting(rt, pal, ix, cy, iw - 54.0f - kGap, title, sub, nullptr, alpha, 27.0f);
        float sy = cy + std::round((h - 27.0f) * 0.5f);
        D2D1_RECT_F rc = D2D1::RectF(ix + iw - 54.0f, sy, ix + iw, sy + 27.0f);
        draw_switch(rt, pal, rc, *field, alpha);
        if (!measuring_) toggle_hits_.push_back({ rc, field, on });
        return cy + h;
    };

    heading(L"Alerts");
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        float cy = toggle(top, L"Low battery alerts", L"Notifies when low, and again at 10%.", &pad_prefs_.alerts);
        cy = setting_line(rt, pal, ix, cy, iw);
        {
            // Its steppers around its value in 54 px.
            const wchar_t* minus = L"−";
            const wchar_t* plus = L"+";
            float sw = chip_width(plus);
            float controls = sw + kGap + 54.0f + kGap + sw;
            float h = setting(rt, pal, ix, cy, iw - controls - kGap, L"Low battery level",
                              L"The battery turns orange at this level.", nullptr, alpha, btn_h);
            float by = cy + std::round((h - btn_h) * 0.5f);
            float mx = ix + iw - controls;
            if (on) {
                button(mx, by, minus, false, "", PadActionKind::LowLevelDelta, -5);
                button(ix + iw - sw, by, plus, false, "", PadActionKind::LowLevelDelta, 5);
            } else {
                chip(rt, pal, D2D1::RectF(mx, by, mx + sw, by + btn_h), minus, false);
                chip(rt, pal, D2D1::RectF(ix + iw - sw, by, ix + iw, by + btn_h), plus, false);
            }
            std::wstring level = std::to_wstring(pad_prefs_.low_level) + L"%";
            Rgba accent = pal.accent;
            accent.a *= alpha;
            label(rt, level.c_str(), fonts_.body(), mx + sw + kGap, by + 9.0f, 54.0f, accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            cy += h;
        }
        cy = setting_line(rt, pal, ix, cy, iw);
        return toggle(cy, L"Connection notices", nullptr, &pad_prefs_.notices);
    });
    y += kPageGap;

    heading(L"While you play");
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        float cy = toggle(top, L"Keep the screen awake", L"No dimming or locking while you play.", &pad_prefs_.keep_awake);
        cy = setting_line(rt, pal, ix, cy, iw);
        cy = toggle(cy, L"Disconnect idle controllers", L"Saves controller battery. They stay paired.",
                    &pad_prefs_.idle_disconnect);
        // How long, under the switch while it's on: the choice nearest the saved minutes.
        if (pad_prefs_.idle_disconnect) {
            cy = setting_line(rt, pal, ix, cy, iw);
            Dropdown d;
            d.key = "idle";
            int nearest = 0;
            for (int i = 0; i < kPadIdleChoices; ++i) {
                d.choices.push_back(std::to_wstring(kPadIdleMinutes[i]) + L" min");
                d.values.push_back(kPadIdleMinutes[i]);
                if (std::abs(kPadIdleMinutes[i] - pad_prefs_.idle_minutes) <
                    std::abs(kPadIdleMinutes[nearest] - pad_prefs_.idle_minutes)) {
                    nearest = i;
                }
            }
            d.checked = nearest;
            d.enabled = on;
            d.pick = [this](int minutes) {
                pad_prefs_.idle_minutes = minutes;
                save_pad_prefs(pad_path_, pad_prefs_);
                pad_service_sync(pad_prefs_, opts_.dry_run);
                InvalidateRect(hwnd_, nullptr, FALSE);
            };
            cy += setting_dropdown(rt, pal, ix, cy, iw, L"Disconnect after", nullptr, std::move(d));
        }
        return cy;
    });

    if (!pad_prefs_.nicknames.empty()) {
        y += kPageGap;
        heading(L"Nicknames");
        y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
            float cy = top;
            bool first = true;
            for (const auto& kv : pad_prefs_.nicknames) {
                if (!first) cy = setting_line(rt, pal, ix, cy, iw);
                first = false;
                size_t tab_at = kv.second.find('\t');
                std::wstring title = utf8_to_wide(tab_at != std::string::npos ? kv.second.substr(0, tab_at) : kv.second);
                std::wstring sub = utf8_to_wide(tab_at != std::string::npos ? kv.second.substr(tab_at + 1) : "");
                const wchar_t* forget = L"Forget";
                float bw = chip_width(forget);
                float h = setting(rt, pal, ix, cy, iw - bw - kGap, title.c_str(), sub.empty() ? nullptr : sub.c_str(),
                                  nullptr, 1.0f, btn_h);
                button(ix + iw - bw, cy + std::round((h - btn_h) * 0.5f), forget, false, kv.first,
                       PadActionKind::ForgetNickname, 0);
                cy += h;
            }
            return cy;
        });
    }

    content_height_ = (y - start_y) + kPagePadTop + kPagePadBottom;
}

} // namespace lares
