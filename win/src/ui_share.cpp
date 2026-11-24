// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Share page, laid out as src/share_view.c lays its own: AirDrop's two
 * settings, what's coming in, the devices nearby while sending, and the
 * Wi-Fi card. What's different on Windows is the step before it works:
 * giving the adapter Windows' WinUSB driver, once (share_win.h). */
#include "ui.h"
#include "ui_internal.h"
extern "C" {
#include "art.h"
}

#include <cmath>
#include <cwchar>
#include <shellapi.h>
#include <shobjidl.h>

namespace lares {

namespace {

constexpr wchar_t kIntro[] = L"Send and receive files with iPhones, iPads and Macs nearby.";

std::wstring device_line(const ShareDevice& d)
{
    switch (d.state) {
    case ShareDeviceState::Found: return L"Ready";
    case ShareDeviceState::Asking: return L"Waiting for them to accept…";
    case ShareDeviceState::Sending:
        if (d.total) {
            wchar_t buf[48];
            swprintf(buf, 48, L"Sending… %d%%", static_cast<int>(100.0 * static_cast<double>(d.sent) / static_cast<double>(d.total)));
            return buf;
        }
        return L"Sending…";
    case ShareDeviceState::Sent: return L"Sent";
    case ShareDeviceState::Declined: return L"Declined";
    case ShareDeviceState::Failed: return d.message.empty() ? std::wstring(L"Couldn't send.") : utf8_to_wide(d.message);
    }
    return {};
}

double fraction(uint64_t part, uint64_t whole)
{
    return whole ? std::min(1.0, static_cast<double>(part) / static_cast<double>(whole)) : 0.0;
}

} // namespace

void MainWindow::draw_share_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    share_hits_.clear();
    const ShareView& v = share_view_;
    const ShareSession& s = v.s;
    float ix = x + kCardPad, iw = w - kCardPad * 2.0f;
    float btn_h = lh_ + 18.0f;

    // ui_group: a muted heading, 27 px in, 9 px over a padded card.
    auto heading = [&](const wchar_t* text) {
        label(rt, text, fonts_.body(), ix, y, iw, pal.muted);
        y += lh_ + kHalf;
    };
    auto button = [&](float bx, float by, const wchar_t* text, bool primary, ShareActionKind kind,
                      const std::string& value = {}) {
        float bw = chip_width(text);
        D2D1_RECT_F rc = D2D1::RectF(bx, by, bx + bw, by + btn_h);
        chip(rt, pal, rc, text, primary);
        if (!measuring_) share_hits_.push_back({ rc, kind, value });
        return bw;
    };
    auto text_block = [&](float cy, const std::wstring& text, const Rgba& color) {
        para(rt, text.c_str(), ix, cy, iw, color);
        return cy + para_height(text.c_str(), iw);
    };
    /* A phone's picture beside texts laid out by `body`, each centred on
     * the other; returns the row's bottom. */
    auto beside_phone = [&](float cy, float pixel, bool on, float right_w, float texts_h, auto body) {
        float aw = 0.0f, ah = 0.0f;
        art_size(ART_PHONE, pixel, aw, ah);
        float h = std::max(ah, texts_h);
        Rgba ic = on ? pal.accent : pal.muted;
        icon(rt, ART_PHONE, pixel, ix, cy, aw, h, ic);
        float tx = ix + aw + kGap;
        body(tx, cy + std::round((h - texts_h) * 0.5f), ix + iw - right_w - tx, h);
        return cy + h;
    };

    // ----------------------------------------------------------- AirDrop
    heading(L"AirDrop");
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        float cy = top;
        if (v.setup != ShareSetup::Ready) {
            std::wstring text = kIntro;
            if (v.setup == ShareSetup::NoAdapter) {
                text += L" On Windows, AirDrop needs a USB Wi-Fi adapter of its own, and the one Lares knows, a Realtek "
                        L"RTL8188EU, isn't plugged in.";
            } else {
                text += L" On Windows, Lares drives the USB Wi-Fi adapter itself. Setting that up gives the adapter "
                        L"Windows' own WinUSB driver, so it's AirDrop's alone from then on, and asks for administrator "
                        L"rights, one time.";
            }
            cy = text_block(cy, text, pal.text);
            if (v.setup == ShareSetup::NeedsSetup) {
                cy += kGap;
                if (v.setup_busy) {
                    const wchar_t* busy = L"Setting up…";
                    D2D1_RECT_F rc = D2D1::RectF(ix, cy, ix + chip_width(busy), cy + btn_h);
                    fill(rt, rc, pal.chip, 9.0f);
                    label(rt, busy, fonts_.body(), rc.left, rc.top + (btn_h - lh_) * 0.5f, rc.right - rc.left, pal.muted,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    button(ix, cy, L"Set up", true, ShareActionKind::Setup);
                }
                cy += btn_h;
                if (v.setup_busy) {
                    cy += kGap;
                    cy = text_block(cy, L"Windows asks for permission once.", pal.muted);
                }
            }
            if (!v.setup_error.empty()) {
                cy += kGap;
                cy = text_block(cy, utf8_to_wide(v.setup_error), pal.warn);
            }
            return cy;
        }

        // Receive, and under it who sees this PC and for how long.
        {
            float text_w = iw - 54.0f - kGap;
            float h = setting(rt, pal, ix, cy, text_w, L"Receive",
                              L"Nearby iPhones, iPads and Macs can send you files for 10 minutes.", nullptr, 1.0f, 27.0f);
            float sy = cy + std::round((h - 27.0f) * 0.5f);
            D2D1_RECT_F sw = D2D1::RectF(ix + iw - 54.0f, sy, ix + iw, sy + 27.0f);
            draw_switch(rt, pal, sw, s.receiving, 1.0f);
            if (!measuring_) share_hits_.push_back({ sw, ShareActionKind::ToggleReceive, {} });
            cy += h;
        }
        if (s.receiving) {
            ULONGLONG gone = (GetTickCount64() - v.left_at_ms) / 1000;
            int left = std::max(0, s.left - static_cast<int>(std::min<ULONGLONG>(gone, 24 * 3600)));
            wchar_t time[32];
            swprintf(time, 32, L" · %d:%02d left", left / 60, left % 60);
            std::wstring status = s.link == ShareLink::Up ? L"Visible as " : L"Starting AirDrop for ";
            status += utf8_to_wide(s.name.empty() ? std::string("this PC") : s.name);
            status += time;
            cy += kGap;
            cy = text_block(cy, status, pal.muted);
        }
        cy = setting_line(rt, pal, ix, cy, iw);

        // Send: files are chosen, then a device among those found.
        {
            const wchar_t* text = s.sending ? L"Stop" : L"Choose files…";
            float bw = chip_width(text);
            float text_w = iw - bw - kGap;
            float h = setting(rt, pal, ix, cy, text_w, L"Send",
                              L"To an iPhone, iPad or Mac set to Everyone for 10 Minutes.", nullptr, 1.0f, btn_h);
            button(ix + iw - bw, cy + std::round((h - btn_h) * 0.5f), text, !s.sending, ShareActionKind::ChooseOrStop);
            cy += h;
        }

        std::wstring warn;
        if (s.link == ShareLink::Failed) warn = s.error.empty() ? std::wstring(L"AirDrop stopped.") : utf8_to_wide(s.error);
        else if (s.link == ShareLink::Up && !s.error.empty()) warn = utf8_to_wide(s.error);
        else if (!v.error.empty()) warn = utf8_to_wide(v.error);
        if (!warn.empty()) {
            cy += kGap;
            cy = text_block(cy, warn, pal.warn);
        }
        if (s.quiet && s.link == ShareLink::Up) {
            cy += kGap;
            cy = text_block(cy,
                            L"No Apple device heard yet. On the iPhone, open the share sheet with AirDrop set to "
                            L"Everyone for 10 Minutes. If it still isn't heard, unplug the Wi-Fi adapter and plug it "
                            L"back in.",
                            pal.muted);
        }
        /* A dry run shows the Bluetooth note with LARES_DRY_BLUETOOTH=off. */
        BleWakeState wake_state = opts_.dry_run
                                      ? (GetEnvironmentVariableW(L"LARES_DRY_BLUETOOTH", nullptr, 0) > 0 ? BleWakeState::RadioOff
                                                                                                         : BleWakeState::Off)
                                      : ble_.state();
        if (s.wake && wake_state == BleWakeState::RadioOff) {
            cy += kGap;
            cy = text_block(cy,
                            L"Bluetooth is off, so Lares can't call nearby iPhones, iPads and Macs awake. Turn it on "
                            L"to send to them.",
                            pal.muted);
            cy += kGap;
            button(ix, cy, L"Bluetooth settings", false, ShareActionKind::OpenBluetoothSettings);
            cy += btn_h;
        } else if (s.wake && wake_state == BleWakeState::Failed) {
            cy += kGap;
            cy = text_block(cy,
                            L"This PC's Bluetooth can't call nearby devices awake. Open AirDrop on the other device "
                            L"first, so it's already listening.",
                            pal.muted);
        }
        return cy;
    });

    // ---------------------------------------------------------- Incoming
    if (v.setup == ShareSetup::Ready && s.has_incoming) {
        const ShareIncoming& in = s.incoming;
        y += kPageGap;
        heading(L"Incoming");
        std::wstring what = utf8_to_wide(share_offer_text(in));
        std::wstring line;
        switch (in.state) {
        case ShareIncomingState::Asking: line = L"Wants to send you " + what + L"."; break;
        case ShareIncomingState::Receiving: line = L"Sending you " + what + L"."; break;
        case ShareIncomingState::Done:
            line = in.links ? std::wstring(L"Sent you a link.") : L"Sent you " + what + L". Saved in Downloads.";
            break;
        case ShareIncomingState::Failed:
            line = L"Couldn't receive " + what + L": " + (in.message.empty() ? std::wstring(L"it stopped.") : utf8_to_wide(in.message));
            break;
        }
        std::wstring sender = utf8_to_wide(in.sender.empty() ? std::string("Someone nearby") : in.sender);
        bool failed = in.state == ShareIncomingState::Failed;
        bool progress = in.state == ShareIncomingState::Receiving;
        y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
            float aw = 0.0f, ah = 0.0f;
            art_size(ART_PHONE, 4.0f, aw, ah);
            float tw = iw - aw - kGap;
            float texts_h = lhb_ + kHalf + para_height(line.c_str(), tw) + (progress ? kHalf + lh_ : 0.0f);
            float cy = beside_phone(top, 4.0f, !failed, 0.0f, texts_h, [&](float tx, float ty, float width, float) {
                label(rt, sender.c_str(), fonts_.big(), tx, ty, width, pal.text);
                ty += lhb_ + kHalf;
                para(rt, line.c_str(), tx, ty, width, failed ? pal.warn : pal.muted);
                ty += para_height(line.c_str(), width);
                if (progress) {
                    ty += kHalf;
                    meter_row(rt, pal, tx, ty, lh_, 20, 2.0f, fraction(in.done, in.total), false);
                    std::wstring amount = utf8_to_wide(share_format_size(in.done));
                    if (in.total) amount += L" of " + utf8_to_wide(share_format_size(in.total));
                    float mw = (20.0f * 4.0f - 1.0f) * 2.0f;
                    label(rt, amount.c_str(), fonts_.body(), tx + mw + kHalf, ty, width - mw - kHalf, pal.muted);
                }
            });
            cy += kGap;
            float bx = ix;
            if (in.state == ShareIncomingState::Asking) {
                bx += button(bx, cy, L"Accept", true, ShareActionKind::Accept) + kHalf;
                button(bx, cy, L"Decline", false, ShareActionKind::Decline);
            } else if (in.state == ShareIncomingState::Done && in.links) {
                for (size_t i = 0; i < in.saved.size() && i < 4; ++i) {
                    if (!share_link_opens(in.saved[i])) continue;
                    bx += button(bx, cy, L"Open link", true, ShareActionKind::OpenLink, in.saved[i]) + kHalf;
                }
                button(bx, cy, L"Done", false, ShareActionKind::Dismiss);
            } else if (in.state == ShareIncomingState::Done) {
                bx += button(bx, cy, L"Show in folder", true, ShareActionKind::ShowFolder,
                             in.saved.empty() ? std::string() : in.saved[0]) + kHalf;
                button(bx, cy, L"Done", false, ShareActionKind::Dismiss);
            } else if (in.state == ShareIncomingState::Failed) {
                button(bx, cy, L"Done", false, ShareActionKind::Dismiss);
            } else {
                return cy - kGap;   // receiving: nothing to press
            }
            return cy + btn_h;
        });
    }

    // ------------------------------------------------------------ Nearby
    if (v.setup == ShareSetup::Ready && s.sending) {
        y += kPageGap;
        heading(L"Nearby");
        std::wstring what = L"Sending ";
        what += v.files.empty() ? std::wstring(L"files") : v.files[0];
        if (v.files.size() > 1) what += L" and " + std::to_wstring(v.files.size() - 1) + L" more";
        what += L".";
        y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
            float cy = text_block(top, what, pal.muted);
            if (s.devices.empty()) {
                cy += kGap;
                return text_block(cy,
                                  s.link == ShareLink::Up
                                      ? L"Looking for nearby devices. The other device needs AirDrop set to Everyone "
                                        L"for 10 Minutes."
                                      : L"Starting AirDrop…",
                                  pal.muted);
            }
            bool first = true;
            for (const ShareDevice& d : s.devices) {
                cy = first ? cy + kGap : setting_line(rt, pal, ix, cy, iw);
                first = false;
                bool busy = d.state == ShareDeviceState::Asking || d.state == ShareDeviceState::Sending;
                const wchar_t* action = busy ? L"Cancel" : d.state == ShareDeviceState::Found ? L"Send" : L"Send again";
                float bw = chip_width(action);
                std::wstring name = utf8_to_wide(d.name.empty() ? std::string("A device nearby") : d.name);
                std::wstring status = device_line(d);
                bool bad = d.state == ShareDeviceState::Failed || d.state == ShareDeviceState::Declined;
                bool sending = d.state == ShareDeviceState::Sending;
                float aw = 0.0f, ah = 0.0f;
                art_size(ART_PHONE, 3.0f, aw, ah);
                float tw = iw - aw - kGap - bw - kGap;
                float texts_h = lh_ + kHalf + para_height(status.c_str(), tw) + (sending ? kHalf + 6.0f : 0.0f);
                float row_top = cy;
                float row_h = 0.0f;
                cy = beside_phone(cy, 3.0f, d.state != ShareDeviceState::Failed, bw + kGap, texts_h,
                                  [&](float tx, float ty, float width, float h) {
                                      row_h = h;
                                      label(rt, name.c_str(), fonts_.body(), tx, ty, width, pal.text);
                                      ty += lh_ + kHalf;
                                      para(rt, status.c_str(), tx, ty, width, bad ? pal.warn : pal.muted);
                                      ty += para_height(status.c_str(), width);
                                      if (sending) draw_meter(rt, pal, tx, ty + kHalf, 20, 2.0f, fraction(d.sent, d.total), false);
                                  });
                button(ix + iw - bw, row_top + std::round((row_h - btn_h) * 0.5f), action, !busy,
                       busy ? ShareActionKind::CancelSend : ShareActionKind::SendTo, d.key);
            }
            return cy;
        });
    }

    // -------------------------------------------------------- Wi-Fi card
    y += kPageGap;
    heading(L"Wi-Fi card");
    bool have_card = v.setup != ShareSetup::NoAdapter;
    y += boxed(rt, pal, x, y, w, kCardPad, [&](float top) {
        const wchar_t* value = have_card ? L"Realtek USB adapter, 2.4 GHz only" : L"None";
        float vw = text_width(value, fonts_.body());
        float h = setting(rt, pal, ix, top, iw - vw - kGap, L"Card", nullptr, nullptr, 1.0f, lh_);
        label(rt, value, fonts_.body(), ix + iw - vw, top + std::round((h - lh_) * 0.5f), vw, pal.muted);
        float cy = top + h;
        if (have_card) {
            cy += kGap;
            cy = text_block(cy,
                            v.setup == ShareSetup::Ready
                                ? L"Lares drives this adapter for AirDrop alone: Windows doesn't use it for Wi-Fi. On "
                                  L"2.4 GHz alone, an iPhone gives AirDrop little time: photos take about a minute each."
                                : L"Windows uses this adapter for Wi-Fi now.",
                            pal.muted);
            /* Set up's change, undone the same way: one permission prompt. */
            if (v.setup == ShareSetup::Ready && !v.running) {
                cy += kGap;
                if (v.setup_busy) {
                    const wchar_t* busy = L"Giving it back…";
                    D2D1_RECT_F rc = D2D1::RectF(ix, cy, ix + chip_width(busy), cy + btn_h);
                    fill(rt, rc, pal.chip, 9.0f);
                    label(rt, busy, fonts_.body(), rc.left, rc.top + (btn_h - lh_) * 0.5f, rc.right - rc.left, pal.muted,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    button(ix, cy, L"Give back to Windows", false, ShareActionKind::GiveBack);
                }
                cy += btn_h;
            }
        }
        return cy;
    });

    y += kPageGap;
    const wchar_t* tip = L"On an iPhone: open Control Center, touch and hold the network controls, tap AirDrop, then "
                         L"choose Everyone for 10 Minutes.";
    para(rt, tip, x, y, w, pal.muted);
    y += para_height(tip, w);

    content_height_ = (y - start_y) + kPagePadTop + kPagePadBottom;
}

/* The files to send, from Windows' own picker; empty if it's cancelled. */
std::vector<std::wstring> MainWindow::pick_files_to_send()
{
    std::vector<std::wstring> out;
    if (opts_.dry_run) {
        /* A dry run opens no dialog: LARES_DRY_FILES names what would be
         * chosen, ";" between files, and none means the pick is cancelled. */
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
    dialog->SetTitle(L"Choose files to send");
    dialog->SetOkButtonLabel(L"Choose");
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

bool MainWindow::share_click(float lx, float ly)
{
    for (const ShareHit& hit : share_hits_) {
        if (!point_in_rect(lx, ly, hit.rect)) continue;
        /* A copy: what follows may redraw the page, and its hits with it. */
        ShareActionKind kind = hit.kind;
        std::string value = hit.value;
        switch (kind) {
        case ShareActionKind::Setup:
            share_.run_setup();
            break;
        case ShareActionKind::ToggleReceive:
            share_.set_receiving(!share_view_.s.receiving);
            break;
        case ShareActionKind::ChooseOrStop:
            if (share_view_.s.sending) {
                share_.stop_sending();
            } else {
                /* AirDrop starts while the files are picked, so the link
                 * and the phones' wake-up don't wait for the choice. */
                share_.set_looking(true);
                std::vector<std::wstring> files = pick_files_to_send();
                if (files.empty()) share_.set_looking(false);
                else share_.send(files);
            }
            break;
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
            /* A link opens only when its button is pressed, and only a web one. */
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
        case ShareActionKind::SendTo:
            share_.send_to(value);
            break;
        case ShareActionKind::CancelSend:
            share_.cancel_send();
            break;
        case ShareActionKind::GiveBack:
            share_.give_back();
            break;
        case ShareActionKind::OpenBluetoothSettings:
            if (!opts_.dry_run) ShellExecuteW(hwnd_, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        return true;
    }
    return false;
}

/* What the share manager said changed: the page, the Bluetooth wake-up and
 * the countdown's tick follow it. */
void MainWindow::on_share_updated()
{
    share_.resume();
    share_view_ = share_.view();
    bool wake = share_view_.running && share_view_.s.wake && !opts_.dry_run;
    ble_.set_active(wake);
    sync_share_tick();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

/* The countdown under Receive, and the Bluetooth wake-up's own news, are
 * looked at once a second, and only while they're on screen. */
void MainWindow::sync_share_tick()
{
    if (!hwnd_) return;
    bool on_page = std::strcmp(sections_[current_sec_].id, "share") == 0;
    bool want = on_page && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) &&
                (share_view_.s.receiving || share_view_.s.wake);
    if (want && !share_tick_) {
        SetTimer(hwnd_, kShareTickTimerId, 1000, nullptr);
        share_tick_ = true;
    } else if (!want && share_tick_) {
        KillTimer(hwnd_, kShareTickTimerId);
        share_tick_ = false;
    }
}

} // namespace lares
