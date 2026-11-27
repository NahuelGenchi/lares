// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Settings and Remote pages, laid out as src/settings_view.c and
 * src/remote_view.c lay theirs. */
#include "ui.h"
#include "ui_internal.h"
extern "C" {
#include "art.h"
}
#include "changes.h"
#include "crash_view.h"
#include "safe.h"

#include <shellapi.h>

#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

namespace lares {

// ================================================================ Settings

void MainWindow::draw_settings_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    float ix = x + kCardPad, iw = w - kCardPad * 2.0f;
    float text_w = iw - 54.0f - kGap;

    // ui_group: a muted heading, 27 px in, 9 px over a padded card.
    auto heading = [&](const wchar_t* text) {
        label(rt, text, fonts_.body(), ix, y, iw, pal.muted);
        y += lh_ + kHalf;
    };
    auto toggle = [&](float cy, const wchar_t* title, const wchar_t* sub, const wchar_t* tag, bool* field,
                      bool enabled) {
        float alpha = enabled ? 1.0f : 0.5f;
        float h = setting(rt, pal, ix, cy, text_w, title, sub, tag, alpha, 27.0f);
        float sy = cy + std::round((h - 27.0f) * 0.5f);
        D2D1_RECT_F sw = D2D1::RectF(ix + iw - 54.0f, sy, ix + iw, sy + 27.0f);
        draw_switch(rt, pal, sw, field && *field, alpha);
        if (!measuring_) toggle_hits_.push_back({ sw, field, enabled });
        return cy + h;
    };

    /* A setting with its buttons under the text, at the right. */
    struct Button {
        const wchar_t* text;
        SettingsAction action;
    };
    auto actions = [&](float cy, const wchar_t* title, const wchar_t* sub, const std::vector<Button>& buttons) {
        float btn_h = lh_ + 18.0f;
        float by = cy + setting(rt, pal, ix, cy, iw, title, sub, nullptr, 1.0f, 27.0f) + kHalf;
        float right = ix + iw;
        for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
            float bw = chip_width(it->text);
            D2D1_RECT_F rc = D2D1::RectF(right - bw, by, right, by + btn_h);
            chip(rt, pal, rc, it->text, true);
            if (!measuring_) settings_hits_.push_back({ rc, it->action });
            right -= bw + kGap;
        }
        return by + btn_h;
    };

    /* Safe mode shows only while it is on (win/guard/safe.h). */
    if (lares_safe_mode_marked(safe_scope())) {
        heading(L"Safe mode");
        y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
            return actions(top, L"Safe mode is on",
                           L"Lares' bar and dock stay off and Windows' own taskbar is left alone, until you sign "
                           L"out or turn safe mode off.",
                           { { L"Turn safe mode off", SettingsAction::LeaveSafeMode } });
        });
        y += kPageGap;
    }

    /* Size: a dropdown at the right of its row. A size saved by hand
     * between the choices shows as it is, with none of them checked. */
    heading(L"Accessibility");
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        Dropdown d;
        d.key = "size";
        for (int i = 0; i < kSizeChoices; ++i) {
            d.choices.push_back(std::to_wstring(kSizePercents[i]) + L"%");
            d.values.push_back(kSizePercents[i]);
            if (kSizePercents[i] == pad_prefs_.size) d.checked = i;
        }
        d.shown = std::to_wstring(pad_prefs_.size) + L"%";
        d.pick = [this](int percent) { apply_size(percent); };
        return top + setting_dropdown(rt, pal, ix, top, iw, L"Size", nullptr, std::move(d));
    });
    y += kPageGap;

    heading(L"Background");
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        return toggle(top, L"Run in the background",
                      L"Keeps controller features and keyboard lighting on with Lares closed.",
                      L"Off saves ~2 MB RAM", &pad_prefs_.background, true);
    });
    y += kPageGap;

    heading(L"System tray");
    bool on = pad_prefs_.background;
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        float cy = top;
        // ui_subheading: the device's icon, 18 px, its name.
        Rgba ic = pal.accent, name = pal.text;
        if (!on) {
            ic.a *= 0.5f;
            name.a *= 0.5f;
        }
        float head_h = std::max(lh_, 18.0f);
        icon(rt, ART_GAMEPAD, 2.0f, ix, cy, 22.0f, head_h, ic);
        label(rt, L"Xbox controller", fonts_.body(), ix + 22.0f + kGap, cy + (head_h - lh_) * 0.5f,
              iw - 22.0f - kGap, name);
        cy += head_h + kGap;
        cy = toggle(cy, L"Show the icon", L"With battery and quick actions.", nullptr, &pad_prefs_.topbar, on);
        cy = setting_line(rt, pal, ix, cy, iw);
        cy = toggle(cy, L"Battery level beside the icon", L"Shows the lowest if several are connected.", nullptr,
                    &pad_prefs_.topbar_level, on);
        cy = setting_line(rt, pal, ix, cy, iw);
        return toggle(cy, L"Hide the icon without a controller", nullptr, nullptr, &pad_prefs_.topbar_hide, on);
    });
    y += kPageGap;

    /* What the change journal holds (win/journal/journal.h), counted when
     * the page opens and after an undo, not at every frame. */
    if (changes_open_ < 0) changes_open_ = changes_open(opts_.dry_run);
    heading(L"Windows");
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        std::wstring title = changes_open_ == 0   ? L"Lares has changed nothing in Windows"
                             : changes_open_ == 1 ? L"Lares has changed 1 Windows setting"
                                                  : L"Lares has changed " + std::to_wstring(changes_open_) +
                                                        L" Windows settings";
        const wchar_t* sub = !changes_said_.empty() ? changes_said_.c_str()
                             : changes_open_ == 0
                                 ? L"Every Windows setting Lares changes is written down first, so it can be put back."
                                 : L"Each was written down before it was changed. Undo puts them back as they "
                                   L"were, and leaves alone any you have changed yourself since.";
        if (changes_open_ == 0) return top + setting(rt, pal, ix, top, iw, title.c_str(), sub, nullptr, 1.0f, 27.0f);
        return actions(top, title.c_str(), sub, { { L"Undo all", SettingsAction::UndoChanges } });
    });
    y += kPageGap;

    /* The newest crash report (win/crash/crash.h), looked for when the page
     * opens. It is never sent: the user can read it and copy it. */
    if (!crash_looked_) {
        crash_looked_ = true;
        crash_report_ = crash_latest();
    }
    heading(L"Crash reports");
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        if (crash_report_.path.empty())
            return top + setting(rt, pal, ix, top, iw, L"No crash report on this PC",
                                 L"If a Lares program crashes, it writes a short report here, on this PC only. "
                                 L"Nothing is ever sent.",
                                 nullptr, 1.0f, 27.0f);
        std::wstring title = L"Last crash: " + crash_report_.when;
        std::wstring sub = crash_report_.program + L" stopped. ";
        sub += !crash_said_.empty() ? crash_said_
                                    : L"The report stays on this PC and holds no names of your files or windows, "
                                      L"and no account names. Copy it into a bug report if you like.";
        return actions(top, title.c_str(), sub.c_str(),
                       { { L"Show me the report", SettingsAction::ShowCrashReport },
                         { L"Copy report", SettingsAction::CopyCrashReport } });
    });

    content_height_ = (y - start_y) + kPagePadTop + kPagePadBottom;
}

void MainWindow::settings_action(SettingsAction action)
{
    switch (action) {
    case SettingsAction::LeaveSafeMode:
        lares_safe_mode_mark(safe_scope(), 0);
        break;
    case SettingsAction::UndoChanges: {
        std::string said;
        changes_undo(opts_.dry_run, said);
        changes_said_.assign(said.begin(), said.end()); // plain English, so each byte is a letter
        changes_open_ = -1;
        load_pad_prefs(pad_path_, pad_prefs_); // Run in the background may be off now
        break;
    }
    case SettingsAction::ShowCrashReport:
        if (!crash_report_.path.empty())
            ShellExecuteW(hwnd_, L"open", crash_report_.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case SettingsAction::CopyCrashReport:
        crash_said_ = crash_copy(hwnd_, crash_report_.path) ? L"Copied. Paste it into a bug report if you like."
                                                            : L"The report could not be copied.";
        break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ================================================================== Remote

void MainWindow::draw_remote_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    remote_hits_.clear();
    float btn_h = lh_ + 18.0f;

    /* .note: 18 px above and below and 27 px beside an icon in a 27 px
     * line, 18 px, then its text over its button, which sits at the right. */
    auto note = [&](const char* const* art, const Rgba& art_color, const wchar_t* text, const wchar_t* button,
                    RemoteActionKind action) {
        float tx = x + kCardPad + 18.0f + kGap;
        float tw = x + w - kCardPad - tx;
        float th = para_height(text, tw);
        float h = kGap * 2.0f + th + (button ? kGap + btn_h : 0.0f);
        fill(rt, D2D1::RectF(x, y, x + w, y + h), pal.note, kCardRadius);
        icon(rt, art, 2.0f, x + kCardPad, y + kGap, 18.0f, 27.0f, art_color);
        para(rt, text, tx, y + kGap, tw, pal.text);
        if (button) {
            float bw = chip_width(button);
            D2D1_RECT_F rc = D2D1::RectF(x + w - kCardPad - bw, y + kGap + th + kGap, x + w - kCardPad,
                                         y + kGap + th + kGap + btn_h);
            chip(rt, pal, rc, button, true);
            remote_hits_.push_back({ rc, action });
        }
        y += h;
    };
    auto line = [&](float cy) {
        fill(rt, D2D1::RectF(x + kHalf + kRowPad, cy, x + w - kHalf - kRowPad, cy + 2.0f), pal.line);
        return cy + 2.0f;
    };

    // The guide, until remote play is set up.
    if (!remote_cfg_.ready) {
        label(rt, L"Remote play", fonts_.title(), x, y, w, pal.text);
        y += lht_ + kHalf;
        const wchar_t* sub = L"Play this PC's games from another screen, with Tailscale, Sunshine and Moonlight.";
        para(rt, sub, x, y, w, pal.muted);
        y += para_height(sub, w) + kPageGap;

        struct Step {
            const wchar_t* title;
            const wchar_t* status;
            bool done;
            const wchar_t* button;
        };
        const Step steps[3] = {
            { L"Tailscale",
              remote_live_.tailscale_signed_in ? L"Signed in and ready."
                  : remote_live_.tailscale_installed ? L"Installed. Sign in to your tailnet."
                                                     : L"Install Tailscale and sign in.",
              remote_live_.tailscale_installed && remote_live_.tailscale_signed_in, nullptr },
            { L"Sunshine", remote_live_.sunshine_installed ? L"Installed on this PC." : L"Install Sunshine on this PC.",
              remote_live_.sunshine_installed, nullptr },
            { L"Moonlight", remote_live_.paired ? L"A device is paired." : L"Open Sunshine and pair Moonlight with it.",
              remote_live_.paired,
              (remote_live_.sunshine_installed && !remote_live_.paired) ? L"Open Sunshine" : nullptr },
        };
        // .card.steps: 9 px around steps of 18 px padding, a number in a 27 px box.
        y += boxed(rt, pal, x, y, w, kHalf, [&](float top) {
            float cy = top;
            for (int i = 0; i < 3; ++i) {
                const Step& s = steps[i];
                if (i) cy = line(cy);
                float sx = x + kHalf + kRowPad, sy = cy + kRowPad;
                float tx = sx + 27.0f + kGap, tw = x + w - kHalf - kRowPad - tx;
                if (s.done) {
                    icon(rt, ART_CHECK, 2.0f, sx, sy, 27.0f, 27.0f, pal.accent);
                } else {
                    wchar_t num[2] = { static_cast<wchar_t>(L'1' + i), 0 };
                    fill(rt, D2D1::RectF(sx, sy, sx + 27.0f, sy + 27.0f), pal.chip, 9.0f);
                    label(rt, num, fonts_.body(), sx, sy + (27.0f - lh_) * 0.5f, 27.0f, pal.text,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                label(rt, s.title, fonts_.body(), tx, sy + (27.0f - lh_) * 0.5f, tw, pal.text);
                float ty = sy + 27.0f + kHalf;
                para(rt, s.status, tx, ty, tw, s.done ? pal.accent : pal.muted);
                ty += para_height(s.status, tw);
                if (s.button) {
                    ty += kHalf;
                    D2D1_RECT_F rc = D2D1::RectF(tx, ty, tx + chip_width(s.button), ty + btn_h);
                    chip(rt, pal, rc, s.button, false);
                    if (!measuring_) remote_hits_.push_back({ rc, RemoteActionKind::OpenSunshineWeb });
                    ty += btn_h;
                }
                cy = ty + kRowPad;
            }
            return cy;
        });
        y += kPageGap;

        label(rt, L"This PC's address", fonts_.body(), x, y, w, pal.muted);
        y += lh_ + kHalf;
        std::wstring ip = remote_live_.tailscale_ip.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_ip);
        std::wstring host = remote_live_.tailscale_host.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_host);
        float ax = x;
        for (int i = 0; i < 3; ++i) {
            const std::wstring part = i == 0 ? ip : i == 1 ? std::wstring(L"or") : host;
            float pw = text_width(part.c_str(), fonts_.body());
            if (ax > x && ax + pw > x + w) {
                ax = x;
                y += lh_ + kHalf;
            }
            label(rt, part.c_str(), fonts_.body(), ax, y, pw, i == 1 ? pal.muted : pal.accent);
            ax += pw + kHalf;
        }
        y += lh_ + kPageGap;

        const wchar_t* open = L"Open modes";
        D2D1_RECT_F rc = D2D1::RectF(x + w - chip_width(open), y, x + w, y + btn_h);
        chip(rt, pal, rc, open, true);
        remote_hits_.push_back({ rc, RemoteActionKind::FinishGuide });
        y += btn_h;
        content_height_ = (y - start_y) + kPagePadTop + kPagePadBottom;
        return;
    }

    /* modeswitch: a track with 9 px around two halves, each 18 px around
     * its icon (3 px a dot), 18 px, and its name at the big size; the pill
     * is one half. */
    {
        float side_h = kRowPad * 2.0f + std::max(lhb_, 27.0f);
        float h = kHalf * 2.0f + side_h;
        fill(rt, D2D1::RectF(x, y, x + w, y + h), pal.track, kCardRadius);
        float inner_x = x + kHalf, half = std::floor((w - kHalf * 2.0f) * 0.5f);
        float pill_x = std::round(inner_x + ease_out_cubic_css(mode_t_) * half);
        fill(rt, D2D1::RectF(pill_x, y + kHalf, pill_x + half, y + kHalf + side_h), pal.accent, 9.0f);
        bool is_remote = remote_cfg_.mode == RemoteMode::Remote;
        for (int side = 0; side < 2; ++side) {
            const wchar_t* name = side ? L"Remote" : L"Home";
            const char* const* art = side ? ART_LAPTOP : ART_HOME;
            D2D1_RECT_F rc = D2D1::RectF(inner_x + side * half, y + kHalf, inner_x + (side + 1) * half,
                                         y + kHalf + side_h);
            bool selected = ((side == 1) == is_remote) && remote_cfg_.mode_known;
            Rgba c = selected ? pal.on_accent : hovered(rc) ? pal.text : pal.muted;
            float nw = text_width(name, fonts_.big());
            float cx = rc.left + std::round((half - (33.0f + kGap + nw)) * 0.5f);
            icon(rt, art, 3.0f, cx, rc.top, 33.0f, side_h, c);
            label(rt, name, fonts_.big(), cx + 33.0f + kGap, rc.top + (side_h - lhb_) * 0.5f, nw, c);
            remote_hits_.push_back({ rc, side ? RemoteActionKind::SwitchModeRemote : RemoteActionKind::SwitchModeHome });
        }
        y += h + kPageGap;
    }

    /* The connection card (.card of .field rows): 9 px around fields padded
     * 12 18 12 54, so their names start where the rows' names do; a value
     * keeps 6 px above and below, which the live badge fills. */
    {
        bool sun_on = remote_live_.sunshine == 1;
        bool live = sun_on && remote_live_.streaming;
        struct Field {
            const wchar_t* name;
            std::wstring value;
            bool dim;
        };
        const Field fields[3] = {
            { L"Moonlight",
              live ? L"live" : remote_live_.sunshine < 0 ? L"Unknown" : !sun_on ? L"Off"
                  : remote_live_.paired ? L"Ready" : L"No device paired",
              !(sun_on && remote_live_.paired) },
            /* Still this PC's address with Tailscale off, so it stays, but
             * muted: nothing reaches it until Tailscale is on. */
            { L"IP address", remote_live_.tailscale_ip.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_ip),
              remote_live_.tailscale_ip.empty() || remote_live_.tailscale != 1 },
            { L"Host name", remote_live_.tailscale_host.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_host),
              remote_live_.tailscale_host.empty() || remote_live_.tailscale != 1 },
        };
        float field_h = 12.0f + 6.0f + lh_ + 6.0f + 12.0f;
        float h = kHalf * 2.0f + field_h * 3.0f + 4.0f;
        card(rt, pal, x, y, w, h);
        float cy = y + kHalf;
        for (int i = 0; i < 3; ++i) {
            if (i) cy = line(cy);
            float left = x + kHalf + 54.0f, right = x + w - kHalf - kRowPad, ty = cy + 18.0f;
            float vw = text_width(fields[i].value.c_str(), fonts_.body());
            if (i == 0 && live) {
                Rgba ground = pal.live;
                ground.a = 0.18f;
                fill(rt, D2D1::RectF(right - vw - 28.0f, cy + 12.0f, right, cy + field_h - 12.0f), ground, 9.0f);
                label(rt, fields[i].value.c_str(), fonts_.body(), right - vw - 14.0f, ty, vw, pal.live);
                right -= vw + 28.0f;
            } else {
                label(rt, fields[i].value.c_str(), fonts_.body(), right - vw, ty, vw,
                      fields[i].dim ? pal.muted : pal.accent);
                right -= vw;
            }
            label(rt, fields[i].name, fonts_.body(), left, ty, right - kHalf - left, pal.text);
            cy += field_h;
        }
        y += h + kPageGap;
    }

    /* The rows (.card of .row): a head padded 18 px with its icon, name,
     * value and chevron 18 px apart; open, its choices padded 18 18 18 54
     * and 9 px apart; a row that drifted says so under them. */
    static const char* const* const kItemArt[kRemoteItemCount] = { ART_SCREEN, ART_SUN, ART_NET };
    bool any_drift = false;
    y += boxed(rt, pal, x, y, w, kHalf, [&](float top) {
        float cy = top;
        for (int i = 0; i < kRemoteItemCount; ++i) {
            auto item = static_cast<RemoteItem>(i);
            std::string want = remote_cfg_.value(remote_cfg_.mode, item);
            bool known = item == RemoteItem::Screen ? remote_live_.screen_known
                : item == RemoteItem::Sunshine ? remote_live_.sunshine >= 0 : remote_live_.tailscale >= 0;
            bool matches = !known || remote_matches(remote_live_, item, want);
            if (!matches) any_drift = true;
            bool open = expanded_remote_row_ == i;
            if (i) cy = line(cy);
            float head_h = kRowPad * 2.0f + lh_;
            D2D1_RECT_F head = D2D1::RectF(x + kHalf, cy, x + w - kHalf, cy + head_h);
            bool hover = hovered(head);
            if (open || hover) fill(rt, head, pal.hover, 9.0f);
            if (!measuring_) remote_hits_.push_back({ head, RemoteActionKind::ToggleRowExpand, item, std::to_string(i) });
            float left = head.left + kRowPad, right = head.right - kRowPad, ty = cy + kRowPad;
            icon(rt, kItemArt[i], 2.0f, left, ty, 18.0f, lh_, (open || hover) ? pal.accent : pal.muted);
            icon(rt, open ? chevron_up() : ART_CHEVRON, 2.0f, right - 18.0f, ty, 18.0f, lh_,
                 open ? pal.accent : pal.muted);
            right -= 18.0f + kGap;
            std::wstring value = utf8_to_wide(remote_describe(item, want));
            float vw = text_width(value.c_str(), fonts_.body());
            label(rt, value.c_str(), fonts_.body(), right - vw, ty, vw, pal.accent);
            std::wstring name = utf8_to_wide(remote_item_name(item));
            float name_x = left + 18.0f + kGap;
            label(rt, name.c_str(), fonts_.body(), name_x, ty, right - vw - kGap - name_x, pal.text);
            cy += head_h;

            float inset = x + kHalf + 54.0f, room = x + w - kHalf - kRowPad - inset;
            if (open) {
                struct Choice {
                    std::string value;
                    std::wstring text;
                };
                struct Group {
                    const wchar_t* title;
                    std::vector<Choice> choices;
                };
                std::vector<Group> groups;
                if (item == RemoteItem::Screen) {
                    Group g1610{ L"16:10, as on MacBooks and many laptops", {} };
                    Group g169{ L"16:9, as on most TVs and monitors", {} };
                    Group gown{ L"This monitor's own size", {} };
                    std::string own = remote_own_size(remote_live_);
                    bool own_listed = own.empty();
                    /* As src/remote_view.c writes them: the size alone at
                     * 60 Hz, the rate after it otherwise. 59 Hz is 60 by
                     * another name where the size has both. */
                    for (const auto& m : remote_live_.modes) {
                        if (m.w < 1280 || m.rate < 59) continue;
                        Group* g = (m.w * 10 == m.h * 16) ? &g1610 : (m.w * 9 == m.h * 16) ? &g169 : nullptr;
                        if (!g || g->choices.size() >= 8) continue;
                        if (m.spec() == own) own_listed = true;
                        if (m.rate == 59 && m.spec() != want && m.spec() != own) {
                            bool has_60 = false;
                            for (const auto& o : remote_live_.modes) has_60 |= o.w == m.w && o.h == m.h && o.rate == 60;
                            if (has_60) continue;
                        }
                        std::wstring text = std::to_wstring(m.w) + L"×" + std::to_wstring(m.h);
                        if (m.rate != 60) text += L" · " + std::to_wstring(m.rate) + L" Hz";
                        g->choices.push_back({ m.spec(), text });
                    }
                    if (!own_listed) {
                        int ow = 0, oh = 0;
                        double orate = 60.0;
                        if (parse_screen_spec(own, ow, oh, orate)) {
                            gown.choices.push_back({ own, utf8_to_wide(describe_screen_mode(ow, oh, orate)) });
                        }
                    }
                    for (Group* g : { &g1610, &g169, &gown }) {
                        if (!g->choices.empty()) groups.push_back(*g);
                    }
                } else {
                    groups.push_back({ nullptr, { { "on", L"On" }, { "off", L"Off" } } });
                }
                cy += kRowPad;
                for (size_t g = 0; g < groups.size(); ++g) {
                    if (g) cy += kHalf;
                    if (groups[g].title) {
                        label(rt, groups[g].title, fonts_.body(), inset, cy, room, pal.muted);
                        cy += lh_ + kHalf;
                    }
                    float cx = inset;
                    for (const auto& c : groups[g].choices) {
                        float cw = chip_width(c.text.c_str());
                        if (cx > inset && cx + cw > inset + room) {
                            cx = inset;
                            cy += btn_h + kHalf;
                        }
                        D2D1_RECT_F rc = D2D1::RectF(cx, cy, cx + cw, cy + btn_h);
                        chip(rt, pal, rc, c.text.c_str(), c.value == want);
                        if (!measuring_) remote_hits_.push_back({ rc, RemoteActionKind::PickChip, item, c.value });
                        cx += cw + kHalf;
                    }
                    cy += btn_h;
                }
                cy += kRowPad;
            }
            if (!matches) {
                std::wstring now = utf8_to_wide(remote_describe_live(remote_live_, item));
                if (!now.empty()) {
                    para(rt, now.c_str(), inset, cy, room, pal.warn);
                    cy += para_height(now.c_str(), room) + kRowPad;
                }
            }
        }
        return cy;
    });

    // What needs saying or doing, under the rows.
    if (!remote_err_msg_.empty() && !sunshine_needs_perm_) {
        y += kPageGap;
        std::wstring err = utf8_to_wide(remote_err_msg_);
        note(ART_BANG, pal.bad, err.c_str(), L"Try again", RemoteActionKind::ReapplyMode);
    } else if (sunshine_needs_perm_) {
        y += kPageGap;
        note(ART_KEY, pal.accent, L"Lares can switch Sunshine once you allow it. Windows asks for that one time.",
             L"Allow", RemoteActionKind::AllowSunshine);
    } else if (!remote_.is_applying() && any_drift) {
        y += kPageGap;
        note(ART_BANG, pal.warn, L"Something was changed outside Lares.", L"Set it again",
             RemoteActionKind::ReapplyMode);
    }

    content_height_ = (y - start_y) + kPagePadTop + kPagePadBottom;
}

} // namespace lares
