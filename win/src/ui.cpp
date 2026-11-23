// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui.h"
extern "C" {
#include "art.h"
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

namespace lares {

static constexpr float kSidebarWidth = 234.0f;
static constexpr float kNavPadX = 18.0f;
static constexpr float kNavPadRight = 9.0f;
static constexpr float kNavPadTop = 18.0f;
static constexpr float kNavItemHeight = 45.0f;
static constexpr float kNavItemGap = 4.0f;
static constexpr float kContentMaxW = 620.0f;
static constexpr float kContentLead = 9.0f;
static constexpr float kCardPad = 27.0f;
static constexpr float kCardRadius = 18.0f;
static constexpr float kPageGap = 36.0f;
static constexpr UINT_PTR kAnimTimerId = 1;
static constexpr UINT_PTR kDevicesSettleTimerId = 2;
// GUID_DEVINTERFACE_HID
static constexpr GUID kHidInterfaceGuid = { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

static D2D1_COLOR_F to_d2d(const Rgba& c, float alpha_mul = 1.0f)
{
    return D2D1::ColorF(c.r, c.g, c.b, c.a * alpha_mul);
}

static bool point_in_rect(float x, float y, const D2D1_RECT_F& r)
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

MainWindow::MainWindow(HINSTANCE hinst, const CliOptions& opts)
    : hinst_(hinst), opts_(opts), hw_(opts.dry_run), remote_(opts.dry_run, opts.dry_stream),
      devices_(opts.dry_run), pads_(opts.dry_run)
{
    if (opts_.color_scheme == "dark") dark_ = true;
    else if (opts_.color_scheme == "light") dark_ = false;
    else dark_ = system_prefers_dark();

    pad_path_ = pad_ini_path(opts_.dry_run);
    load_pad_prefs(pad_path_, pad_prefs_);
    pad_map_load_resource(hinst_, pad_map_);
    pads_.apply_nicknames(pad_prefs_);
    pad_list_ = pads_.list();
    wchar_t env_tab[32] = {};
    int default_tab = 0;
    if (GetEnvironmentVariableW(L"LARES_PAD_TAB", env_tab, 32) > 0) {
        if (_wcsicmp(env_tab, L"sticks") == 0) default_tab = 1;
        else if (_wcsicmp(env_tab, L"vibration") == 0) default_tab = 2;
    }
    for (const auto& p : pad_list_) {
        auto& mem = pad_memory_[p.path];
        mem.tab = default_tab;
        uint32_t pressed = p.state.buttons;
        if (p.state.lt >= kTriggerPullThreshold) pressed |= pb_bit(PB_LT);
        if (p.state.rt >= kTriggerPullThreshold) pressed |= pb_bit(PB_RT);
        mem.seen |= (pressed & p.checklist);
        if (opts_.dry_run) {
            mem.seen |= pb_bit(PB_X) | pb_bit(PB_Y) | pb_bit(PB_UP);
        }
        stick_feed(mem.sticks[0], p.state.lx, p.state.ly, 1000);
        stick_feed(mem.sticks[0], p.state.lx, p.state.ly, 1700);
        stick_feed(mem.sticks[1], p.state.rx, p.state.ry, 1000);
        stick_feed(mem.sticks[1], p.state.rx, p.state.ry, 1700);
    }

    remote_cfg_ = remote_.config();
    remote_mode_ = (remote_cfg_.ready && remote_cfg_.mode_known && remote_cfg_.mode == RemoteMode::Remote);
    mode_t_ = remote_mode_ ? 1.0f : 0.0f;
    mode_target_ = mode_t_;
    devices_.set_live_key_light(key_light_from_name(pad_prefs_.key_light), pad_prefs_.key_rgb);
    dev_list_ = devices_.list();

    sections_ = {
        { "overview",   L"Overview",      ART_OVERVIEW,   true  },
        { "hardware",   L"Hardware",      ART_CHIP,       true  },
        { "controller", L"Controller",    ART_GAMEPAD,    true  },
        { "headphones", L"Headphones",    ART_HEADPHONES, devices_.any_of(DevKind::Audio) },
        { "keyboard",   L"Keyboard",      ART_KEYBOARD,   devices_.any_of(DevKind::Keyboard) },
        { "mouse",      L"Mouse",         ART_MOUSE,      devices_.any_of(DevKind::Mouse) },
        { "remote",     remote_cfg_.ready ? L"Remote" : L"Set up remote", ART_LAPTOP, true },
        { "settings",   L"Settings",      ART_SLIDERS,    true  },
    };
}

MainWindow::~MainWindow()
{
    release_backbuffer();
    if (brush_) { brush_->Release(); brush_ = nullptr; }
    if (dc_rt_) { dc_rt_->Release(); dc_rt_ = nullptr; }
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }
    fonts_.shutdown();
}

void MainWindow::release_backbuffer()
{
    if (mem_dc_) {
        if (old_bmp_) SelectObject(mem_dc_, old_bmp_);
        old_bmp_ = nullptr;
        if (mem_bmp_) DeleteObject(mem_bmp_);
        mem_bmp_ = nullptr;
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    bb_w_ = 0;
    bb_h_ = 0;
}

bool MainWindow::ensure_backbuffer(int px_w, int px_h)
{
    if (px_w <= 0 || px_h <= 0) return false;
    if (mem_dc_ && bb_w_ == px_w && bb_h_ == px_h) return true;
    release_backbuffer();
    HDC screen_dc = GetDC(hwnd_);
    if (!screen_dc) return false;
    mem_dc_ = CreateCompatibleDC(screen_dc);
    mem_bmp_ = CreateCompatibleBitmap(screen_dc, px_w, px_h);
    ReleaseDC(hwnd_, screen_dc);
    if (!mem_dc_ || !mem_bmp_) {
        release_backbuffer();
        return false;
    }
    old_bmp_ = static_cast<HBITMAP>(SelectObject(mem_dc_, mem_bmp_));
    bb_w_ = px_w;
    bb_h_ = px_h;
    return true;
}

bool MainWindow::init_d2d()
{
    if (!fonts_.init(hinst_)) return false;

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        &d2d_factory_);
    if (FAILED(hr) || !d2d_factory_) return false;

    // Software Direct2D target: keeps GPU drivers out of the UI process
    // (mirrors Linux GTK's Cairo software renderer for low RAM).
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT);

    hr = d2d_factory_->CreateDCRenderTarget(&props, &dc_rt_);
    if (FAILED(hr) || !dc_rt_) return false;

    dc_rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    hr = dc_rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
    return SUCCEEDED(hr) && brush_ != nullptr;
}

void MainWindow::apply_titlebar_theme()
{
    if (!hwnd_) return;
    BOOL use_dark = dark_ ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE (20), DWMWA_CAPTION_COLOR (35), DWMWA_TEXT_COLOR (36)
    DwmSetWindowAttribute(hwnd_, 20, &use_dark, sizeof(use_dark));
    Palette pal = current_palette();
    COLORREF bg = rgba_to_colorref(pal.bg);
    COLORREF txt = rgba_to_colorref(pal.muted);
    DwmSetWindowAttribute(hwnd_, 35, &bg, sizeof(bg));
    DwmSetWindowAttribute(hwnd_, 36, &txt, sizeof(txt));
}

bool MainWindow::create()
{
    if (!init_d2d()) return false;

    const wchar_t* cls_name = opts_.dry_run ? kWindowClassDry : kWindowClass;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWindow::wnd_proc;
    wc.hInstance = hinst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = cls_name;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    dpi_scale_ = static_cast<float>(dpi) / 96.0f;

    int win_w = static_cast<int>(std::lround(900.0f * dpi_scale_));
    int win_h = static_cast<int>(std::lround(720.0f * dpi_scale_));

    const wchar_t* title = opts_.dry_run ? L"Lares (dry run)" : L"Lares";
    hwnd_ = CreateWindowExW(
        0, cls_name, title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
        nullptr, nullptr, hinst_, this);
    if (!hwnd_) return false;

    dpi = GetDpiForWindow(hwnd_);
    dpi_scale_ = static_cast<float>(dpi) / 96.0f;
    hw_.set_notify_hwnd(hwnd_);
    remote_.set_notify_hwnd(hwnd_);
    devices_.set_notify_hwnd(hwnd_);
    pads_.set_notify_hwnd(hwnd_);
    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kHidInterfaceGuid;
    dev_notify_ = RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    remote_.request_refresh();
    pads_.request_refresh();
    pad_service_sync(pad_prefs_, opts_.dry_run);

    apply_titlebar_theme();
    if (!opts_.section.empty()) {
        std::string wanted = opts_.section;
        select_section(wanted, false);
        if (sections_[current_sec_].id == wanted) {
            opts_.section.clear();
        }
    } else {
        select_section("overview", false);
    }
    return true;
}

void MainWindow::show(int cmd_show)
{
    if (!hwnd_) return;
    ShowWindow(hwnd_, cmd_show);
    update_sampling_state();
    UpdateWindow(hwnd_);
}

void MainWindow::update_sampling_state()
{
    if (!hwnd_ || IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) {
        hw_.set_wanted(false, false);
        remote_.set_wanted(false);
        devices_.set_wanted(false, false, false);
        pads_.set_wanted(false, false);
        return;
    }
    const char* id = sections_[current_sec_].id;
    bool is_ov = (std::strcmp(id, "overview") == 0);
    bool is_hw = (std::strcmp(id, "hardware") == 0);
    bool is_pad = (std::strcmp(id, "controller") == 0);
    bool is_rem = (std::strcmp(id, "remote") == 0);
    bool is_hp = (std::strcmp(id, "headphones") == 0);
    bool is_kbd = (std::strcmp(id, "keyboard") == 0);
    bool is_mse = (std::strcmp(id, "mouse") == 0);
    hw_.set_wanted(is_ov || is_hw, is_hw);
    remote_.set_wanted(is_ov || is_rem);
    devices_.set_wanted(is_ov || is_hp || is_kbd || is_mse, is_mse, is_kbd);
    pads_.set_wanted(is_ov || is_pad, is_pad);
}

int MainWindow::visible_index_of(int sec_idx) const
{
    int v = 0;
    for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
        if (!sections_[i].visible) continue;
        if (i == sec_idx) return v;
        ++v;
    }
    return 0;
}

void MainWindow::select_section(const std::string& id, bool animate)
{
    for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
        if (sections_[i].visible && id == sections_[i].id) {
            current_sec_ = i;
            scroll_y_ = 0.0f;
            scroll_target_y_ = 0.0f;
            editing_pad_path_.clear();
            editing_nickname_.clear();
            toggle_hits_.clear();
            card_hits_.clear();
            remote_hits_.clear();
            device_hits_.clear();
            pad_hits_.clear();
            int vis = visible_index_of(i);
            nav_pill_target_y_ = kNavPadTop + vis * (kNavItemHeight + kNavItemGap);
            if (!animate || !nav_pill_init_) {
                nav_pill_y_ = nav_pill_target_y_;
                nav_pill_vel_ = 0.0f;
                nav_pill_init_ = true;
            } else {
                start_anim_timer();
            }
            update_sampling_state();
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
}

Palette MainWindow::current_palette() const
{
    Palette home = palette_for(dark_, false);
    Palette rem = palette_for(dark_, true);
    return palette_lerp(home, rem, ease_out_cubic_css(mode_t_));
}

void MainWindow::start_anim_timer()
{
    if (!timer_running_ && hwnd_) {
        last_tick_ms_ = GetTickCount64();
        SetTimer(hwnd_, kAnimTimerId, 16, nullptr);
        timer_running_ = true;
    }
}

void MainWindow::stop_anim_timer_if_idle()
{
    bool pill_done = std::fabs(nav_pill_y_ - nav_pill_target_y_) < 0.25f && std::fabs(nav_pill_vel_) < 0.5f;
    bool mode_done = std::fabs(mode_t_ - mode_target_) < 0.002f;
    bool scroll_done = std::fabs(scroll_y_ - scroll_target_y_) < 0.5f;
    bool shake_done = true;
    for (const auto& kv : pad_memory_) {
        if (kv.second.shake_start_ms != 0) {
            shake_done = false;
            break;
        }
    }
    if (pill_done && mode_done && scroll_done && shake_done && timer_running_ && hwnd_) {
        nav_pill_y_ = nav_pill_target_y_;
        nav_pill_vel_ = 0.0f;
        mode_t_ = mode_target_;
        scroll_y_ = scroll_target_y_;
        KillTimer(hwnd_, kAnimTimerId);
        timer_running_ = false;
    }
}

void MainWindow::on_timer()
{
    ULONGLONG now = GetTickCount64();
    float dt = static_cast<float>(now - last_tick_ms_) / 1000.0f;
    last_tick_ms_ = now;
    dt = std::clamp(dt, 0.001f, 0.05f);

    // Critically damped spring for the sidebar selection highlight.
    constexpr float stiffness = 360.0f;
    constexpr float damping = 36.0f;
    float disp = nav_pill_target_y_ - nav_pill_y_;
    float acc = disp * stiffness - nav_pill_vel_ * damping;
    nav_pill_vel_ += acc * dt;
    nav_pill_y_ += nav_pill_vel_ * dt;

    // Smooth wheel scroll easing
    if (std::fabs(scroll_target_y_ - scroll_y_) >= 0.5f) {
        scroll_y_ += (scroll_target_y_ - scroll_y_) * std::min(1.0f, dt * 18.0f);
    } else {
        scroll_y_ = scroll_target_y_;
    }

    // 520 ms mode color transition.
    if (std::fabs(mode_t_ - mode_target_) >= 0.002f) {
        float step = dt / 0.52f;
        if (mode_t_ < mode_target_) mode_t_ = std::min(mode_target_, mode_t_ + step);
        else mode_t_ = std::max(mode_target_, mode_t_ - step);
        apply_titlebar_theme();
    }

    // Controller picture shake animation (8 steps x 45 ms, matching src/pad_picture.c)
    for (auto& kv : pad_memory_) {
        auto& mem = kv.second;
        if (mem.shake_start_ms != 0) {
            double elapsed = static_cast<double>(now - mem.shake_start_ms);
            if (elapsed >= 45.0 * 8.0) {
                mem.shake_start_ms = 0;
                mem.shake_offset = 0;
            } else {
                double phase = std::fmod(elapsed / 45.0, 2.0);
                double p = (phase < 1.0) ? phase : (2.0 - phase);
                double eased = 0.5 - 0.5 * std::cos(3.14159265358979323846 * p);
                mem.shake_offset = static_cast<int>(std::lround(2.0 * eased - 1.0));
            }
        }
    }

    stop_anim_timer_if_idle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::draw_pixel_art(ID2D1RenderTarget* rt, const char* const* art,
                                float x, float y, float scale,
                                const Rgba& solid, const Rgba& faint)
{
    if (!art || !brush_) return;
    for (int row = 0; art[row]; ++row) {
        const char* line = art[row];
        for (int col = 0; line[col]; ++col) {
            char ch = line[col];
            if (ch != '#' && ch != '+') continue;
            brush_->SetColor(to_d2d(ch == '#' ? solid : faint));
            D2D1_RECT_F r = D2D1::RectF(
                x + col * scale,
                y + row * scale,
                x + (col + 1) * scale,
                y + (row + 1) * scale);
            rt->FillRectangle(r, brush_);
        }
    }
}

void MainWindow::draw_text(ID2D1RenderTarget* rt, const wchar_t* text, IDWriteTextFormat* fmt,
                           const D2D1_RECT_F& rect, const Rgba& color, DWRITE_TEXT_ALIGNMENT align)
{
    if (!text || !fmt || !brush_) return;
    IDWriteTextLayout* layout = nullptr;
    UINT32 len = static_cast<UINT32>(wcslen(text));
    float max_w = std::max(1.0f, rect.right - rect.left);
    float max_h = std::max(1.0f, rect.bottom - rect.top);
    if (SUCCEEDED(fonts_.factory()->CreateTextLayout(text, len, fmt, max_w, max_h, &layout)) && layout) {
        layout->SetTextAlignment(align);
        if (fonts_.no_ligatures()) {
            DWRITE_TEXT_RANGE range = { 0, len };
            layout->SetTypography(fonts_.no_ligatures(), range);
        }
        brush_->SetColor(to_d2d(color));
        rt->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        layout->Release();
    }
}

void MainWindow::clamp_scroll()
{
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float lh = static_cast<float>(rc.bottom - rc.top) / dpi_scale_;
    float max_scroll = std::max(0.0f, content_height_ - lh);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);
    scroll_target_y_ = std::clamp(scroll_target_y_, 0.0f, max_scroll);
}

bool MainWindow::interactive_rect_at(float lx, float ly, D2D1_RECT_F* out) const
{
    auto found = [&](const D2D1_RECT_F& r) {
        if (out) *out = r;
        return true;
    };
    if (lx < kSidebarWidth) {
        if (lx >= kNavPadX && lx <= kSidebarWidth - kNavPadX) {
            int vis = 0;
            for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
                if (!sections_[i].visible) continue;
                float iy = kNavPadTop + vis * (kNavItemHeight + kNavItemGap);
                if (ly >= iy && ly <= iy + kNavItemHeight) {
                    return found(D2D1::RectF(kNavPadX, iy, kSidebarWidth - kNavPadX, iy + kNavItemHeight));
                }
                ++vis;
            }
        }
        return false;
    }
    // In the order on_mouse_down tries them.
    for (const auto& h : toggle_hits_) { if (h.enabled && point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (const auto& h : card_hits_)   { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (const auto& h : remote_hits_) { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (const auto& h : device_hits_) { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (const auto& h : pad_hits_)    { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    return false;
}

bool MainWindow::point_over_interactive(float lx, float ly) const
{
    return interactive_rect_at(lx, ly, nullptr);
}

bool MainWindow::hovered(const D2D1_RECT_F& rc) const
{
    return mouse_in_ && point_in_rect(mouse_x_, mouse_y_, rc);
}

float MainWindow::measure_text_height(const wchar_t* text, IDWriteTextFormat* fmt, float max_w)
{
    if (!text || !*text || !fmt) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    UINT32 len = static_cast<UINT32>(wcslen(text));
    float h = 0.0f;
    if (SUCCEEDED(fonts_.factory()->CreateTextLayout(text, len, fmt, std::max(1.0f, max_w), 2000.0f, &layout)) && layout) {
        if (fonts_.no_ligatures()) {
            DWRITE_TEXT_RANGE range = { 0, len };
            layout->SetTypography(fonts_.no_ligatures(), range);
        }
        DWRITE_TEXT_METRICS m = {};
        if (SUCCEEDED(layout->GetMetrics(&m))) {
            h = std::ceil(m.height);
        }
        layout->Release();
    }
    return h;
}

void MainWindow::draw_sidebar(ID2D1RenderTarget* rt, const Palette& pal, float client_h)
{
    (void)client_h;
    float pill_x = kNavPadX;
    float pill_w = kSidebarWidth - kNavPadX - kNavPadRight;

    // Hover pill (matches data/style.css nav button.nav-item:hover, radius 9px)
    if (hover_sec_ >= 0 && hover_sec_ != current_sec_ && sections_[hover_sec_].visible) {
        int hv = visible_index_of(hover_sec_);
        float hy = kNavPadTop + hv * (kNavItemHeight + kNavItemGap);
        brush_->SetColor(to_d2d(pal.hover));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(pill_x, hy, pill_x + pill_w, hy + kNavItemHeight), 9.0f, 9.0f),
            brush_);
    }

    // Active spring highlight pill (borderless var(--card), radius 9px)
    brush_->SetColor(to_d2d(pal.card));
    D2D1_ROUNDED_RECT active_pill = D2D1::RoundedRect(
        D2D1::RectF(pill_x, nav_pill_y_, pill_x + pill_w, nav_pill_y_ + kNavItemHeight), 9.0f, 9.0f);
    rt->FillRoundedRectangle(active_pill, brush_);

    int vis = 0;
    for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
        if (!sections_[i].visible) continue;
        float iy = kNavPadTop + vis * (kNavItemHeight + kNavItemGap);
        bool selected = (i == current_sec_);

        Rgba icon_solid = selected ? pal.accent : pal.muted;
        Rgba icon_faint = icon_solid;
        icon_faint.a *= 0.35f;

        draw_pixel_art(rt, sections_[i].icon, pill_x + 18.0f, iy + 14.0f, 2.0f, icon_solid, icon_faint);

        D2D1_RECT_F tr = D2D1::RectF(pill_x + 48.0f, iy + 12.0f, pill_x + pill_w - 12.0f, iy + kNavItemHeight - 4.0f);
        draw_text(rt, sections_[i].label, fonts_.body(), tr, selected ? pal.text : pal.muted);
        ++vis;
    }
}

void MainWindow::draw_toggle_row(ID2D1RenderTarget* rt, const Palette& pal,
                                 float x, float& y, float w,
                                 const wchar_t* title, const wchar_t* sub, const wchar_t* saving,
                                 bool* field, bool enabled)
{
    // Matches data/style.css window.lares switch: 54x27, radius 9px; knob 21x21, radius 6px, margin 3px
    float alpha = enabled ? 1.0f : 0.50f;
    float sw_w = 54.0f;
    float sw_h = 27.0f;
    float text_w = w - sw_w - 18.0f;

    float h_title = measure_text_height(title, fonts_.body(), text_w);
    float h_sub = sub ? measure_text_height(sub, fonts_.caption(), text_w) : 0.0f;
    float h_sav = saving ? 27.0f : 0.0f;

    float total_h = h_title + (h_sub > 0 ? 6.0f + h_sub : 0.0f) + (h_sav > 0 ? 9.0f + h_sav : 0.0f);
    total_h = std::max(total_h, 36.0f);

    Rgba c_title = pal.text; c_title.a *= alpha;
    Rgba c_muted = pal.muted; c_muted.a *= alpha;

    float cy = (sub || saving) ? y : (y + (total_h - h_title) * 0.5f);
    draw_text(rt, title, fonts_.body(), D2D1::RectF(x, cy, x + text_w, cy + h_title + 4.0f), c_title);
    cy += h_title;
    if (sub && h_sub > 0) {
        cy += 6.0f;
        draw_text(rt, sub, fonts_.caption(), D2D1::RectF(x, cy, x + text_w, cy + h_sub + 4.0f), c_muted);
        cy += h_sub;
    }
    if (saving && h_sav > 0) {
        cy += 9.0f;
        // .saving tag: background var(--note), color var(--accent), border-radius 9px, height 27px
        float tag_w = std::min(text_w, static_cast<float>(wcslen(saving)) * 9.2f + 22.0f);
        D2D1_RECT_F tag_rc = D2D1::RectF(x, cy, x + tag_w, cy + 27.0f);
        brush_->SetColor(to_d2d(pal.note, alpha));
        rt->FillRoundedRectangle(D2D1::RoundedRect(tag_rc, 9.0f, 9.0f), brush_);
        Rgba c_sav = pal.accent; c_sav.a *= alpha;
        draw_text(rt, saving, fonts_.caption(), D2D1::RectF(tag_rc.left + 9.0f, tag_rc.top + 5.0f, tag_rc.right - 6.0f, tag_rc.bottom), c_sav);
    }

    float sw_x = x + w - sw_w;
    float sw_y = y + (total_h - sw_h) * 0.5f;
    D2D1_RECT_F sw_rect = D2D1::RectF(sw_x, sw_y, sw_x + sw_w, sw_y + sw_h);

    bool on = field && *field;
    brush_->SetColor(to_d2d(on ? pal.accent : pal.track, alpha));
    rt->FillRoundedRectangle(D2D1::RoundedRect(sw_rect, 9.0f, 9.0f), brush_);

    float knob_x = on ? (sw_rect.right - 24.0f) : (sw_rect.left + 3.0f);
    D2D1_RECT_F knob_rc = D2D1::RectF(knob_x, sw_rect.top + 3.0f, knob_x + 21.0f, sw_rect.top + 24.0f);
    brush_->SetColor(to_d2d(pal.card, alpha));
    rt->FillRoundedRectangle(D2D1::RoundedRect(knob_rc, 6.0f, 6.0f), brush_);

    toggle_hits_.push_back({ sw_rect, field, enabled });
    y += total_h + 18.0f;
}

void MainWindow::draw_settings_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;

    // Group 1: Background (matches src/settings_view.c: muted heading indented 27px + .card.padded)
    draw_text(rt, L"Background", fonts_.body(), D2D1::RectF(x + kCardPad, y, x + w, y + 26.0f), pal.muted);
    y += 30.0f;

    float card1_top = y;
    float inner_x = x + kCardPad;
    float inner_w = w - kCardPad * 2.0f;
    float text_w = inner_w - 72.0f;

    float h_t = measure_text_height(L"Run in the background", fonts_.body(), text_w);
    float h_s = measure_text_height(L"Keeps controller features and keyboard lighting on with Lares closed.", fonts_.caption(), text_w);
    float card1_h = kCardPad * 2.0f + std::max(36.0f, h_t + 6.0f + h_s + 9.0f + 27.0f);

    brush_->SetColor(to_d2d(pal.card));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, card1_top, x + w, card1_top + card1_h), kCardRadius, kCardRadius), brush_);

    float probe_y = card1_top + kCardPad;
    draw_toggle_row(rt, pal, inner_x, probe_y, inner_w,
                    L"Run in the background",
                    L"Keeps controller features and keyboard lighting on with Lares closed.",
                    L"Off saves ~2 MB RAM",
                    &pad_prefs_.background, true);

    y = card1_top + card1_h + kPageGap;

    // Group 2: System tray (grouped inside ONE .card.padded with 2px dividers, matching src/ui.c)
    draw_text(rt, L"System tray", fonts_.body(), D2D1::RectF(x + kCardPad, y, x + w, y + 26.0f), pal.muted);
    y += 30.0f;

    float card2_top = y;
    bool tray_enabled = pad_prefs_.background;
    float r1_h = std::max(36.0f, measure_text_height(L"Show the icon", fonts_.body(), text_w) + 6.0f +
                                 measure_text_height(L"With battery and quick actions.", fonts_.caption(), text_w));
    float r2_h = std::max(36.0f, measure_text_height(L"Battery level beside the icon", fonts_.body(), text_w) + 6.0f +
                                 measure_text_height(L"Shows the lowest if several are connected.", fonts_.caption(), text_w));
    float r3_h = std::max(36.0f, measure_text_height(L"Hide the icon without a controller", fonts_.body(), text_w));
    float card2_h = kCardPad + 36.0f + r1_h + 36.0f + r2_h + 36.0f + r3_h + kCardPad;

    brush_->SetColor(to_d2d(pal.card));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, card2_top, x + w, card2_top + card2_h), kCardRadius, kCardRadius), brush_);

    float cy = card2_top + kCardPad;
    Rgba sub_col = pal.accent;
    if (!tray_enabled) sub_col.a *= 0.50f;
    Rgba sub_faint = sub_col; sub_faint.a *= 0.35f;
    draw_pixel_art(rt, ART_GAMEPAD, inner_x, cy + 2.0f, 2.0f, sub_col, sub_faint);
    Rgba sub_txt = pal.text;
    if (!tray_enabled) sub_txt.a *= 0.50f;
    draw_text(rt, L"Xbox controller", fonts_.body(), D2D1::RectF(inner_x + 32.0f, cy - 2.0f, inner_x + inner_w, cy + 26.0f), sub_txt);
    cy += 36.0f;

    auto draw_setting_sep = [&]() {
        brush_->SetColor(to_d2d(pal.line));
        rt->FillRectangle(D2D1::RectF(inner_x, cy, inner_x + inner_w, cy + 2.0f), brush_);
        cy += 18.0f;
    };

    draw_toggle_row(rt, pal, inner_x, cy, inner_w,
                    L"Show the icon",
                    L"With battery and quick actions.",
                    nullptr,
                    &pad_prefs_.topbar, tray_enabled);
    draw_setting_sep();
    draw_toggle_row(rt, pal, inner_x, cy, inner_w,
                    L"Battery level beside the icon",
                    L"Shows the lowest if several are connected.",
                    nullptr,
                    &pad_prefs_.topbar_level, tray_enabled);
    draw_setting_sep();
    draw_toggle_row(rt, pal, inner_x, cy, inner_w,
                    L"Hide the icon without a controller",
                    nullptr,
                    nullptr,
                    &pad_prefs_.topbar_hide, tray_enabled);

    y = card2_top + card2_h;
    content_height_ = (y - start_y) + 54.0f;
}

static std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

static void draw_info_card(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                           MainWindow*, const Palette& pal,
                           float x, float y, float w, float h)
{
    // Matches data/style.css .card: flat var(--card) surface, border-radius 18px, NO border stroke
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), kCardRadius, kCardRadius);
    brush->SetColor(to_d2d(pal.card));
    rt->FillRoundedRectangle(card, brush);
}

void MainWindow::draw_meter(ID2D1RenderTarget* rt, const Palette& pal,
                            float x, float y, int blocks, float pixel, double fraction, bool hot)
{
    if (!brush_ || blocks <= 0) return;
    Rgba on = hot ? pal.bad : pal.accent;
    Rgba off = on;
    off.a *= 0.16f;
    int filled = meter_filled_blocks(fraction, blocks);
    for (int i = 0; i < blocks; ++i) {
        brush_->SetColor(to_d2d(i < filled ? on : off));
        float bx = x + i * 4.0f * pixel;
        rt->FillRectangle(D2D1::RectF(bx, y, bx + 3.0f * pixel, y + 3.0f * pixel), brush_);
    }
}

void MainWindow::draw_graph(ID2D1RenderTarget* rt, const Palette& pal,
                            float x, float y, float w, float h, float pixel, const GraphRing& ring)
{
    if (!brush_ || w <= 0.0f || h <= 0.0f || pixel <= 0.0f) return;
    Rgba c = pal.accent;
    Rgba base = c;
    base.a *= 0.14f;

    brush_->SetColor(to_d2d(base));
    rt->FillRectangle(D2D1::RectF(x, y + h - pixel, x + w, y + h), brush_);

    int steps = static_cast<int>(h / pixel);
    int max_cols = static_cast<int>(w / pixel);
    int cols = std::min(max_cols, ring.len);
    brush_->SetColor(to_d2d(c));
    for (int i = 0; i < cols; ++i) {
        int sh = graph_step_height(ring.latest(i), steps);
        if (sh <= 0) continue;
        float bar_h = sh * pixel;
        float bx = x + w - (i + 1) * pixel;
        float by = y + h - bar_h;
        rt->FillRectangle(D2D1::RectF(bx, by, bx + pixel, by + bar_h), brush_);
    }
}

void MainWindow::draw_overview_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    const HwInfo& info = hw_.info();
    const HwSample& s = hw_sample_;

    bool has_gpu = !info.gpu_name.empty();
    int cols = has_gpu ? 3 : 2;
    float gap = 18.0f;
    float tile_w = (w - gap * (cols - 1)) / static_cast<float>(cols);
    float tile_h = 152.0f;

    auto draw_tile = [&](float tx, float ty, float tw, const char* const* art, const wchar_t* heading,
                         const std::wstring& val, const std::wstring& sub,
                         int meter_blocks, double fraction, bool hot, const char* target_sec) {
        draw_info_card(rt, brush_, this, pal, tx, ty, tw, tile_h);
        card_hits_.push_back({ D2D1::RectF(tx, ty, tx + tw, ty + tile_h), target_sec });

        float ix = tx + kCardPad;
        float iy = ty + kCardPad;
        Rgba ic = pal.accent;
        Rgba ic_f = ic; ic_f.a *= 0.35f;
        draw_pixel_art(rt, art, ix, iy + 1.0f, 2.0f, ic, ic_f);
        draw_text(rt, heading, fonts_.caption(), D2D1::RectF(ix + 26.0f, iy, tx + tw - kCardPad, iy + 22.0f), pal.muted);

        draw_text(rt, val.c_str(), fonts_.big(), D2D1::RectF(ix, iy + 28.0f, tx + tw - kCardPad, iy + 62.0f), pal.text);
        draw_text(rt, sub.c_str(), fonts_.caption(), D2D1::RectF(ix, iy + 64.0f, tx + tw - kCardPad, iy + 86.0f), pal.muted);

        if (meter_blocks > 0) {
            draw_meter(rt, pal, ix, ty + tile_h - kCardPad - 6.0f, meter_blocks, 2.0f, fraction, hot);
        }
    };

    // CPU tile
    wchar_t cpu_val[32];
    if (s.cpu < 0.0) std::swprintf(cpu_val, std::size(cpu_val), L"...");
    else std::swprintf(cpu_val, std::size(cpu_val), L"%d%%", static_cast<int>(std::lround(s.cpu * 100.0)));
    wchar_t cpu_sub[32];
    if (std::isnan(s.cpu_temp)) std::swprintf(cpu_sub, std::size(cpu_sub), L"Load");
    else std::swprintf(cpu_sub, std::size(cpu_sub), L"%d °C", static_cast<int>(std::lround(s.cpu_temp)));
    draw_tile(x, y, tile_w, ART_CHIP, L"CPU", cpu_val, cpu_sub, 14, s.cpu, s.cpu > 0.9, "hardware");

    float next_x = x + tile_w + gap;

    // GPU tile
    if (has_gpu) {
        wchar_t gpu_val[32];
        if (s.gpu < 0.0) std::swprintf(gpu_val, std::size(gpu_val), L"...");
        else std::swprintf(gpu_val, std::size(gpu_val), L"%d%%", static_cast<int>(std::lround(s.gpu * 100.0)));
        wchar_t gpu_sub[32];
        if (std::isnan(s.gpu_temp)) std::swprintf(gpu_sub, std::size(gpu_sub), L"Load");
        else std::swprintf(gpu_sub, std::size(gpu_sub), L"%d °C", static_cast<int>(std::lround(s.gpu_temp)));
        draw_tile(next_x, y, tile_w, ART_SCREEN, L"GPU", gpu_val, gpu_sub, 14, s.gpu, s.gpu > 0.9, "hardware");
        next_x += tile_w + gap;
    }

    // Memory tile
    double mem_frac = s.mem_total ? static_cast<double>(s.mem_used) / static_cast<double>(s.mem_total) : -1.0;
    std::wstring mem_val = s.mem_total ? utf8_to_wide(format_bytes(s.mem_used)) : L"...";
    std::wstring mem_sub = s.mem_total ? (L"of " + utf8_to_wide(format_bytes(s.mem_total))) : L"";
    draw_tile(next_x, y, tile_w, ART_MEMORY, L"Memory", mem_val, mem_sub, 14, mem_frac, mem_frac > 0.9, "hardware");

    y += tile_h + gap;

    // Storage tile spans the full width (34-block meter)
    double disk_frac = s.disk_total ? static_cast<double>(s.disk_used) / static_cast<double>(s.disk_total) : -1.0;
    uint64_t disk_free = s.disk_total > s.disk_used ? (s.disk_total - s.disk_used) : 0;
    std::wstring disk_val = s.disk_total ? (utf8_to_wide(format_bytes(disk_free)) + L" free") : L"...";
    std::wstring disk_sub = s.disk_total ? (L"of " + utf8_to_wide(format_bytes(s.disk_total))) : L"";
    draw_tile(x, y, w, ART_DISK, L"Storage", disk_val, disk_sub, 34, disk_frac, disk_frac > 0.9, "hardware");
    y += tile_h + gap;

    // Devices card (one row per connected controller or paired/connected device, matching src/overview.c)
    size_t total_rows = pad_list_.size() + dev_list_.size();
    if (total_rows > 0) {
        float row_h = 58.0f;
        float card_h = static_cast<float>(total_rows) * row_h;
        draw_info_card(rt, brush_, this, pal, x, y, w, card_h);
        float ix = x + 18.0f, iw = w - 36.0f;
        size_t row_idx = 0;

        for (const auto& p : pad_list_) {
            float ry = y + static_cast<float>(row_idx) * row_h;
            if (row_idx > 0) {
                brush_->SetColor(to_d2d(pal.line));
                rt->FillRectangle(D2D1::RectF(ix, ry, ix + iw, ry + 1.0f), brush_);
            }
            card_hits_.push_back({ D2D1::RectF(x, ry, x + w, ry + row_h), "controller" });

            Rgba ic = pal.muted, icf = ic; icf.a *= 0.35f;
            draw_pixel_art(rt, ART_GAMEPAD, ix, ry + 20.0f, 2.0f, ic, icf);

            std::wstring pname = utf8_to_wide(pad_display_name(p));
            draw_text(rt, pname.c_str(), fonts_.body(), D2D1::RectF(ix + 30.0f, ry + 16.0f, ix + iw - 112.0f, ry + 44.0f), pal.text);

            if (p.battery.known) {
                std::wstring btxt = utf8_to_wide(pad_battery_charge(p.battery));
                bool low = (pad_battery_low_stage(p.battery, pad_prefs_.low_level) > 0 && !pad_battery_charging(p.battery));
                draw_text(rt, btxt.c_str(), fonts_.caption(), D2D1::RectF(ix + iw - 140.0f, ry + 19.0f, ix + iw - 87.0f, ry + 44.0f),
                          low ? pal.bad : pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
                draw_meter(rt, pal, ix + iw - 78.0f, ry + 26.0f, 10, 2.0f, pad_battery_fraction(p.battery), low);
            }
            ++row_idx;
        }

        for (size_t i = 0; i < dev_list_.size(); ++i, ++row_idx) {
            const auto& d = dev_list_[i];
            float ry = y + static_cast<float>(row_idx) * row_h;
            if (row_idx > 0) {
                brush_->SetColor(to_d2d(pal.line));
                rt->FillRectangle(D2D1::RectF(ix, ry, ix + iw, ry + 1.0f), brush_);
            }
            const char* target_sec = (d.kind == DevKind::Audio) ? "headphones"
                : (d.kind == DevKind::Keyboard) ? "keyboard" : "mouse";
            card_hits_.push_back({ D2D1::RectF(x, ry, x + w, ry + row_h), target_sec });

            const char* const* d_art = (d.kind == DevKind::Audio) ? ART_HEADPHONES
                : (d.kind == DevKind::Keyboard) ? ART_KEYBOARD : ART_MOUSE;
            Rgba ic = pal.muted, icf = ic; icf.a *= 0.35f;
            draw_pixel_art(rt, d_art, ix, ry + 20.0f, 2.0f, ic, icf);

            std::wstring dname = utf8_to_wide(d.name);
            draw_text(rt, dname.c_str(), fonts_.body(), D2D1::RectF(ix + 30.0f, ry + 16.0f, ix + iw - 112.0f, ry + 44.0f), pal.text);

            if (!d.connected || d.asleep) {
                draw_text(rt, d.asleep ? L"Asleep" : L"Not connected", fonts_.caption(),
                          D2D1::RectF(ix + iw - 140.0f, ry + 19.0f, ix + iw, ry + 44.0f), pal.muted,
                          DWRITE_TEXT_ALIGNMENT_TRAILING);
            } else if (d.battery >= 0) {
                wchar_t btxt[16];
                std::swprintf(btxt, std::size(btxt), L"%d%%", d.battery);
                bool low = (d.battery <= 20);
                draw_text(rt, btxt, fonts_.caption(), D2D1::RectF(ix + iw - 140.0f, ry + 19.0f, ix + iw - 87.0f, ry + 44.0f),
                          low ? pal.bad : pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
                draw_meter(rt, pal, ix + iw - 78.0f, ry + 26.0f, 10, 2.0f, d.battery / 100.0, low);
            }
        }
        y += card_h + gap;
    }

    // Mode tile (shown once remote play is set up, matching src/overview.c)
    if (remote_cfg_.ready) {
        std::wstring mval = remote_cfg_.mode_known
            ? utf8_to_wide(remote_mode_name(remote_cfg_.mode))
            : L"...";
        std::wstring msub;
        if (remote_live_.streaming) {
            msub = L"Streaming to Moonlight now";
        } else if (remote_cfg_.mode_known) {
            bool sun_diff = !remote_matches(remote_live_, RemoteItem::Sunshine, remote_cfg_.value(remote_cfg_.mode, RemoteItem::Sunshine));
            bool ts_diff = !remote_matches(remote_live_, RemoteItem::Tailscale, remote_cfg_.value(remote_cfg_.mode, RemoteItem::Tailscale));
            std::wstring sun_news = (remote_live_.sunshine >= 0 && sun_diff)
                ? (remote_live_.sunshine ? L"Sunshine is on" : L"Sunshine is off") : L"";
            std::wstring ts_news = (remote_live_.tailscale >= 0 && ts_diff)
                ? (remote_live_.tailscale ? L"Tailscale is on" : L"Tailscale is off") : L"";
            if (!sun_news.empty() && !ts_news.empty()) msub = sun_news + L" · " + ts_news;
            else if (!sun_news.empty() || !ts_news.empty()) msub = !sun_news.empty() ? sun_news : ts_news;
            else if (remote_cfg_.mode == RemoteMode::Remote && remote_live_.sunshine == 1) msub = L"Waiting for Moonlight";
        }
        float mode_h = 122.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, mode_h);
        card_hits_.push_back({ D2D1::RectF(x, y, x + w, y + mode_h), "remote" });
        float ix = x + kCardPad, iy = y + kCardPad;
        Rgba ic = pal.accent, ic_f = ic; ic_f.a *= 0.35f;
        draw_pixel_art(rt, ART_LAPTOP, ix, iy + 1.0f, 2.0f, ic, ic_f);
        draw_text(rt, L"Mode", fonts_.caption(), D2D1::RectF(ix + 26.0f, iy, x + w - kCardPad, iy + 22.0f), pal.muted);
        draw_text(rt, mval.c_str(), fonts_.big(), D2D1::RectF(ix, iy + 26.0f, x + w - kCardPad, iy + 62.0f), pal.text);
        if (!msub.empty()) {
            draw_text(rt, msub.c_str(), fonts_.caption(), D2D1::RectF(ix, iy + 62.0f, x + w - kCardPad, iy + 86.0f), pal.muted);
        }
        y += mode_h + gap;
    }

    content_height_ = (y - start_y) + 54.0f;
}

void MainWindow::draw_hardware_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    const HwInfo& info = hw_.info();
    const HwSample& s = hw_sample_;
    float gap = 18.0f;

    // 1. CPU card
    {
        float card_h = 186.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, card_h);
        float ix = x + 18.0f, iy = y + 18.0f, iw = w - 36.0f;
        Rgba ic = pal.muted, ic_f = ic; ic_f.a *= 0.35f;
        draw_pixel_art(rt, ART_CHIP, ix, iy + 2.0f, 2.0f, ic, ic_f);
        std::wstring cname = utf8_to_wide(info.cpu_name);
        draw_text(rt, cname.c_str(), fonts_.body(), D2D1::RectF(ix + 30.0f, iy, ix + iw - 120.0f, iy + 26.0f), pal.text);

        wchar_t threads_buf[32];
        std::swprintf(threads_buf, std::size(threads_buf), L"%d threads", info.threads);
        float tw = 120.0f;
        draw_text(rt, threads_buf, fonts_.caption(), D2D1::RectF(ix + iw - tw, iy + 3.0f, ix + iw, iy + 26.0f),
                  pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);

        wchar_t val_buf[32];
        if (s.cpu < 0.0) std::swprintf(val_buf, std::size(val_buf), L"...");
        else std::swprintf(val_buf, std::size(val_buf), L"%d%%", static_cast<int>(std::lround(s.cpu * 100.0)));
        draw_text(rt, val_buf, fonts_.big(), D2D1::RectF(ix, iy + 34.0f, ix + 180.0f, iy + 70.0f), pal.text);

        if (!std::isnan(s.cpu_temp)) {
            wchar_t t_buf[32];
            std::swprintf(t_buf, std::size(t_buf), L"%d °C", static_cast<int>(std::lround(s.cpu_temp)));
            draw_text(rt, t_buf, fonts_.big(), D2D1::RectF(ix + iw - 120.0f, iy + 34.0f, ix + iw, iy + 70.0f),
                      pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        draw_graph(rt, pal, ix, iy + 74.0f, iw, 54.0f, 3.0f, cpu_graph_);
        draw_text(rt, L"Load over the last minutes", fonts_.caption(), D2D1::RectF(ix, iy + 136.0f, ix + iw, iy + 158.0f), pal.muted);
        y += card_h + gap;
    }

    // 2. GPU card (if present)
    if (!info.gpu_name.empty()) {
        std::wstring facts;
        if (s.vram_total > 0) {
            facts = L"Video memory " + utf8_to_wide(format_bytes(s.vram_used)) + L" of " + utf8_to_wide(format_bytes(s.vram_total));
        }
        if (!std::isnan(s.gpu_fan_rpm)) {
            wchar_t fbuf[48];
            std::swprintf(fbuf, std::size(fbuf), L"Fan %d rpm", static_cast<int>(std::lround(s.gpu_fan_rpm)));
            if (!facts.empty()) facts += L"   ·   ";
            facts += fbuf;
        }
        float card_h = facts.empty() ? 164.0f : 186.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, card_h);
        float ix = x + 18.0f, iy = y + 18.0f, iw = w - 36.0f;
        Rgba ic = pal.muted, ic_f = ic; ic_f.a *= 0.35f;
        draw_pixel_art(rt, ART_SCREEN, ix, iy + 2.0f, 2.0f, ic, ic_f);
        std::wstring gname = utf8_to_wide(info.gpu_name);
        draw_text(rt, gname.c_str(), fonts_.body(), D2D1::RectF(ix + 30.0f, iy, ix + iw, iy + 26.0f), pal.text);

        wchar_t val_buf[32];
        if (s.gpu < 0.0) std::swprintf(val_buf, std::size(val_buf), L"...");
        else std::swprintf(val_buf, std::size(val_buf), L"%d%%", static_cast<int>(std::lround(s.gpu * 100.0)));
        draw_text(rt, val_buf, fonts_.big(), D2D1::RectF(ix, iy + 34.0f, ix + 180.0f, iy + 70.0f), pal.text);

        if (!std::isnan(s.gpu_temp)) {
            wchar_t t_buf[32];
            std::swprintf(t_buf, std::size(t_buf), L"%d °C", static_cast<int>(std::lround(s.gpu_temp)));
            draw_text(rt, t_buf, fonts_.big(), D2D1::RectF(ix + iw - 120.0f, iy + 34.0f, ix + iw, iy + 70.0f),
                      pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        draw_graph(rt, pal, ix, iy + 74.0f, iw, 54.0f, 3.0f, gpu_graph_);
        if (!facts.empty()) {
            draw_text(rt, facts.c_str(), fonts_.caption(), D2D1::RectF(ix, iy + 136.0f, ix + iw, iy + 158.0f), pal.muted);
        }
        y += card_h + gap;
    }

    // 3. Memory card
    {
        float card_h = 136.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, card_h);
        float ix = x + 18.0f, iy = y + 18.0f, iw = w - 36.0f;
        Rgba ic = pal.muted, ic_f = ic; ic_f.a *= 0.35f;
        draw_pixel_art(rt, ART_MEMORY, ix, iy + 2.0f, 2.0f, ic, ic_f);
        draw_text(rt, L"Memory", fonts_.body(), D2D1::RectF(ix + 30.0f, iy, ix + iw, iy + 26.0f), pal.text);

        std::wstring mval = s.mem_total ? utf8_to_wide(format_bytes(s.mem_used)) : L"...";
        draw_text(rt, mval.c_str(), fonts_.big(), D2D1::RectF(ix, iy + 34.0f, ix + iw, iy + 70.0f), pal.text);

        double mfrac = s.mem_total ? static_cast<double>(s.mem_used) / static_cast<double>(s.mem_total) : -1.0;
        draw_meter(rt, pal, ix, iy + 74.0f, 34, 2.0f, mfrac, mfrac > 0.9);

        if (s.mem_total > 0) {
            std::wstring mfact = L"Used of " + utf8_to_wide(format_bytes(s.mem_total));
            draw_text(rt, mfact.c_str(), fonts_.caption(), D2D1::RectF(ix, iy + 88.0f, ix + iw, iy + 110.0f), pal.muted);
        }
        y += card_h + gap;
    }

    // 4. System drive card
    {
        float card_h = 136.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, card_h);
        float ix = x + 18.0f, iy = y + 18.0f, iw = w - 36.0f;
        Rgba ic = pal.muted, ic_f = ic; ic_f.a *= 0.35f;
        draw_pixel_art(rt, ART_DISK, ix, iy + 2.0f, 2.0f, ic, ic_f);
        draw_text(rt, L"System drive", fonts_.body(), D2D1::RectF(ix + 30.0f, iy, ix + iw - 80.0f, iy + 26.0f), pal.text);

        if (!info.disk_kind.empty()) {
            std::wstring dkind = utf8_to_wide(info.disk_kind);
            draw_text(rt, dkind.c_str(), fonts_.caption(), D2D1::RectF(ix + iw - 80.0f, iy + 3.0f, ix + iw, iy + 26.0f),
                      pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        uint64_t dfree = s.disk_total > s.disk_used ? (s.disk_total - s.disk_used) : 0;
        std::wstring dval = s.disk_total ? utf8_to_wide(format_bytes(dfree)) : L"...";
        draw_text(rt, dval.c_str(), fonts_.big(), D2D1::RectF(ix, iy + 34.0f, ix + 240.0f, iy + 70.0f), pal.text);

        if (!std::isnan(s.disk_temp)) {
            wchar_t t_buf[32];
            std::swprintf(t_buf, std::size(t_buf), L"%d °C", static_cast<int>(std::lround(s.disk_temp)));
            draw_text(rt, t_buf, fonts_.big(), D2D1::RectF(ix + iw - 120.0f, iy + 34.0f, ix + iw, iy + 70.0f),
                      pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        double dfrac = s.disk_total ? static_cast<double>(s.disk_used) / static_cast<double>(s.disk_total) : -1.0;
        draw_meter(rt, pal, ix, iy + 74.0f, 34, 2.0f, dfrac, dfrac > 0.9);

        if (s.disk_total > 0) {
            std::wstring dfact = L"Free of " + utf8_to_wide(format_bytes(s.disk_total));
            draw_text(rt, dfact.c_str(), fonts_.caption(), D2D1::RectF(ix, iy + 88.0f, ix + iw, iy + 110.0f), pal.muted);
        }
        y += card_h + gap;
    }

    content_height_ = (y - start_y) + 54.0f;
}

void MainWindow::draw_remote_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    remote_hits_.clear();
    float gap = 18.0f;

    // 1. Setup guide when remote play isn't ready yet
    if (!remote_cfg_.ready) {
        draw_text(rt, L"Set up remote", fonts_.title(), D2D1::RectF(x, y, x + w, y + 48.0f), pal.text);
        y += 54.0f;

        struct StepRow {
            const wchar_t* num;
            const wchar_t* title;
            const wchar_t* sub;
            bool done;
        };
        StepRow steps[3] = {
            { L"1", L"Tailscale",
              remote_live_.tailscale_signed_in ? L"Signed in and ready" :
              (remote_live_.tailscale_installed ? L"Installed · sign in to your tailnet" : L"Install Tailscale and sign in"),
              remote_live_.tailscale_installed && remote_live_.tailscale_signed_in },
            { L"2", L"Sunshine",
              remote_live_.sunshine_installed ? L"Installed on this PC" : L"Install Sunshine on this PC",
              remote_live_.sunshine_installed },
            { L"3", L"Moonlight",
              remote_live_.paired ? L"Device paired" : L"Open https://localhost:47990 and pair Moonlight",
              remote_live_.paired },
        };

        for (int i = 0; i < 3; ++i) {
            float ch = 82.0f;
            draw_info_card(rt, brush_, this, pal, x, y, w, ch);
            float ix = x + 18.0f, iy = y + 18.0f;
            if (steps[i].done) {
                Rgba ok_f = pal.accent; ok_f.a *= 0.35f;
                draw_pixel_art(rt, ART_CHECK, ix, iy + 3.0f, 2.0f, pal.accent, ok_f);
            } else {
                draw_text(rt, steps[i].num, fonts_.body(), D2D1::RectF(ix, iy, ix + 24.0f, iy + 26.0f), pal.accent);
            }
            draw_text(rt, steps[i].title, fonts_.body(), D2D1::RectF(ix + 30.0f, iy, x + w - 160.0f, iy + 26.0f), pal.text);
            draw_text(rt, steps[i].sub, fonts_.caption(), D2D1::RectF(ix + 30.0f, iy + 28.0f, x + w - 24.0f, iy + 54.0f), pal.muted);

            if (i == 2 && remote_live_.sunshine_installed && !remote_live_.paired) {
                D2D1_RECT_F btn = D2D1::RectF(x + w - 162.0f, y + 24.0f, x + w - 18.0f, y + 58.0f);
                brush_->SetColor(to_d2d(pal.chip));
                rt->FillRoundedRectangle(D2D1::RoundedRect(btn, 10.0f, 10.0f), brush_);
                draw_text(rt, L"Open Sunshine", fonts_.caption(), D2D1::RectF(btn.left + 14.0f, btn.top + 7.0f, btn.right, btn.bottom), pal.text);
                remote_hits_.push_back({ btn, RemoteActionKind::OpenSunshineWeb });
            }
            y += ch + gap;
        }

        // Address card + "Use Remote switch" action
        float addr_h = 112.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, addr_h);
        std::wstring ip_w = remote_live_.tailscale_ip.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_ip);
        std::wstring host_w = remote_live_.tailscale_host.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_host);
        draw_text(rt, L"Add this PC in Moonlight", fonts_.body(), D2D1::RectF(x + 18.0f, y + 16.0f, x + w - 180.0f, y + 42.0f), pal.text);
        std::wstring addr_line = L"IP: " + ip_w + L"   ·   Host: " + host_w;
        draw_text(rt, addr_line.c_str(), fonts_.caption(), D2D1::RectF(x + 18.0f, y + 48.0f, x + w - 24.0f, y + 76.0f), pal.muted);

        D2D1_RECT_F skip_btn = D2D1::RectF(x + w - 178.0f, y + 18.0f, x + w - 18.0f, y + 52.0f);
        brush_->SetColor(to_d2d(pal.accent));
        rt->FillRoundedRectangle(D2D1::RoundedRect(skip_btn, 10.0f, 10.0f), brush_);
        draw_text(rt, L"Open modes", fonts_.caption(), D2D1::RectF(skip_btn.left + 26.0f, skip_btn.top + 7.0f, skip_btn.right, skip_btn.bottom), pal.on_accent);
        remote_hits_.push_back({ skip_btn, RemoteActionKind::FinishGuide });

        content_height_ = (y + addr_h - start_y) + 54.0f;
        return;
    }

    // 2. Home | Remote big segmented mode switch (matches modeswitch in data/style.css:
    //    var(--track) outer radius 18px, padding 9px; var(--accent) pill radius 9px; big text)
    float sw_h = 76.0f;
    D2D1_RECT_F sw_track = D2D1::RectF(x, y, x + w, y + sw_h);
    brush_->SetColor(to_d2d(pal.track));
    rt->FillRoundedRectangle(D2D1::RoundedRect(sw_track, kCardRadius, kCardRadius), brush_);

    float half_w = (w - 18.0f) * 0.5f;
    float pill_x = x + 9.0f + ease_out_cubic_css(mode_t_) * half_w;
    D2D1_ROUNDED_RECT sw_pill = D2D1::RoundedRect(
        D2D1::RectF(pill_x, y + 9.0f, pill_x + half_w, y + sw_h - 9.0f), 9.0f, 9.0f);
    brush_->SetColor(to_d2d(pal.accent));
    rt->FillRoundedRectangle(sw_pill, brush_);

    bool is_rem = (remote_cfg_.mode == RemoteMode::Remote);
    // Home side
    {
        float hx = x + 9.0f + half_w * 0.5f - 48.0f;
        Rgba hc = !is_rem ? pal.on_accent : pal.muted;
        Rgba hcf = hc; hcf.a *= 0.35f;
        draw_pixel_art(rt, ART_HOME, hx, y + 29.0f, 2.0f, hc, hcf);
        draw_text(rt, L"Home", fonts_.big(), D2D1::RectF(hx + 30.0f, y + 23.0f, x + 9.0f + half_w, y + 58.0f), hc);
        remote_hits_.push_back({ D2D1::RectF(x, y, x + half_w + 9.0f, y + sw_h), RemoteActionKind::SwitchModeHome });
    }
    // Remote side
    {
        float rx = x + 9.0f + half_w + half_w * 0.5f - 60.0f;
        Rgba rc = is_rem ? pal.on_accent : pal.muted;
        Rgba rcf = rc; rcf.a *= 0.35f;
        draw_pixel_art(rt, ART_LAPTOP, rx, y + 29.0f, 2.0f, rc, rcf);
        draw_text(rt, L"Remote", fonts_.big(), D2D1::RectF(rx + 30.0f, y + 23.0f, x + w, y + 58.0f), rc);
        remote_hits_.push_back({ D2D1::RectF(x + half_w + 9.0f, y, x + w, y + sw_h), RemoteActionKind::SwitchModeRemote });
    }
    y += sw_h + gap;

    // 3. Connection status card (Moonlight, IP address, Host name)
    {
        float conn_h = 136.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, conn_h);
        float ix = x + 18.0f, iw = w - 36.0f;

        // Row 1: Moonlight
        float ry = y + 16.0f;
        draw_text(rt, L"Moonlight", fonts_.body(), D2D1::RectF(ix, ry, ix + 180.0f, ry + 26.0f), pal.text);
        bool sun_on = (remote_live_.sunshine == 1);
        bool stream_live = sun_on && remote_live_.streaming;
        if (stream_live) {
            D2D1_RECT_F badge = D2D1::RectF(ix + iw - 66.0f, ry + 1.0f, ix + iw, ry + 25.0f);
            Rgba bg_live = pal.live; bg_live.a = 0.16f;
            brush_->SetColor(to_d2d(bg_live));
            rt->FillRoundedRectangle(D2D1::RoundedRect(badge, 8.0f, 8.0f), brush_);
            draw_text(rt, L"live", fonts_.caption(), D2D1::RectF(badge.left + 16.0f, badge.top + 2.0f, badge.right, badge.bottom), pal.live);
        } else {
            const wchar_t* mstatus = (remote_live_.sunshine < 0) ? L"Unknown"
                : !sun_on ? L"Off"
                : remote_live_.paired ? L"Ready" : L"No device paired";
            draw_text(rt, mstatus, fonts_.body(), D2D1::RectF(ix + iw - 240.0f, ry, ix + iw, ry + 26.0f),
                      (sun_on && remote_live_.paired) ? pal.text : pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        // Row 2: IP address
        ry += 36.0f;
        brush_->SetColor(to_d2d(pal.line));
        rt->FillRectangle(D2D1::RectF(ix, ry - 6.0f, ix + iw, ry - 5.0f), brush_);
        draw_text(rt, L"IP address", fonts_.body(), D2D1::RectF(ix, ry, ix + 180.0f, ry + 26.0f), pal.text);
        std::wstring ip_w = remote_live_.tailscale_ip.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_ip);
        draw_text(rt, ip_w.c_str(), fonts_.body(), D2D1::RectF(ix + iw - 260.0f, ry, ix + iw, ry + 26.0f),
                  remote_live_.tailscale_ip.empty() ? pal.muted : pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);

        // Row 3: Host name
        ry += 36.0f;
        brush_->SetColor(to_d2d(pal.line));
        rt->FillRectangle(D2D1::RectF(ix, ry - 6.0f, ix + iw, ry - 5.0f), brush_);
        draw_text(rt, L"Host name", fonts_.body(), D2D1::RectF(ix, ry, ix + 180.0f, ry + 26.0f), pal.text);
        std::wstring host_w = remote_live_.tailscale_host.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_host);
        draw_text(rt, host_w.c_str(), fonts_.body(), D2D1::RectF(ix + iw - 260.0f, ry, ix + iw, ry + 26.0f),
                  remote_live_.tailscale_host.empty() ? pal.muted : pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);

        y += conn_h + gap;
    }

    // 4. Error / Permission / Drift note banner
    bool any_drift = false;
    for (int i = 0; i < kRemoteItemCount; ++i) {
        auto item = static_cast<RemoteItem>(i);
        bool known = (item == RemoteItem::Screen) ? remote_live_.screen_known
            : (item == RemoteItem::Sunshine) ? (remote_live_.sunshine >= 0)
                                             : (remote_live_.tailscale >= 0);
        if (known && !remote_matches(remote_live_, item, remote_cfg_.value(remote_cfg_.mode, item))) {
            any_drift = true;
            break;
        }
    }

    if (!remote_err_msg_.empty() && !sunshine_needs_perm_) {
        std::wstring ew = utf8_to_wide(remote_err_msg_);
        float eh = std::max(44.0f, measure_text_height(ew.c_str(), fonts_.caption(), w - 64.0f) + 22.0f);
        D2D1_ROUNDED_RECT err_rc = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + eh), 14.0f, 14.0f);
        brush_->SetColor(to_d2d(pal.note));
        rt->FillRoundedRectangle(err_rc, brush_);
        Rgba ec = pal.bad, ecf = ec; ecf.a *= 0.35f;
        draw_pixel_art(rt, ART_BANG, x + 18.0f, y + (eh - 18.0f) * 0.5f, 2.0f, ec, ecf);
        draw_text(rt, ew.c_str(), fonts_.caption(), D2D1::RectF(x + 46.0f, y + 11.0f, x + w - 18.0f, y + eh - 8.0f), pal.bad);
        y += eh + gap;
    }

    if (sunshine_needs_perm_ || (!remote_.is_applying() && any_drift)) {
        float note_h = 64.0f;
        D2D1_ROUNDED_RECT note_rc = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + note_h), 14.0f, 14.0f);
        brush_->SetColor(to_d2d(pal.note));
        rt->FillRoundedRectangle(note_rc, brush_);
        float ix = x + 18.0f;
        const char* const* n_art = sunshine_needs_perm_ ? ART_KEY : ART_BANG;
        Rgba nc = sunshine_needs_perm_ ? pal.accent : pal.warn;
        Rgba ncf = nc; ncf.a *= 0.35f;
        draw_pixel_art(rt, n_art, ix, y + 23.0f, 2.0f, nc, ncf);

        const wchar_t* n_txt = sunshine_needs_perm_
            ? L"SunshineService needs permission once so Lares can switch it."
            : L"Changed outside Lares.";
        float text_top = sunshine_needs_perm_ ? (y + 12.0f) : (y + 22.0f);
        draw_text(rt, n_txt, fonts_.caption(), D2D1::RectF(ix + 28.0f, text_top, x + w - 148.0f, y + note_h - 8.0f), pal.text);

        D2D1_RECT_F btn = D2D1::RectF(x + w - 138.0f, y + 15.0f, x + w - 16.0f, y + 49.0f);
        brush_->SetColor(to_d2d(pal.accent));
        rt->FillRoundedRectangle(D2D1::RoundedRect(btn, 10.0f, 10.0f), brush_);
        const wchar_t* b_lbl = sunshine_needs_perm_ ? L"Allow" : L"Set it again";
        draw_text(rt, b_lbl, fonts_.caption(), D2D1::RectF(btn.left, btn.top + 8.0f, btn.right, btn.bottom),
                  pal.on_accent, DWRITE_TEXT_ALIGNMENT_CENTER);
        remote_hits_.push_back({
            btn,
            sunshine_needs_perm_ ? RemoteActionKind::AllowSunshine : RemoteActionKind::ReapplyMode
        });
        y += note_h + gap;
    }

    // 5. Three expandable rows: Screen, Sunshine, Tailscale
    const char* const* item_icons[kRemoteItemCount] = { ART_SCREEN, ART_SUN, ART_NET };
    for (int i = 0; i < kRemoteItemCount; ++i) {
        auto item = static_cast<RemoteItem>(i);
        std::string want_val = remote_cfg_.value(remote_cfg_.mode, item);
        std::wstring name_w = utf8_to_wide(remote_item_name(item));
        std::wstring desc_w = utf8_to_wide(remote_describe(item, want_val));
        bool known = (item == RemoteItem::Screen) ? remote_live_.screen_known
            : (item == RemoteItem::Sunshine) ? (remote_live_.sunshine >= 0)
                                             : (remote_live_.tailscale >= 0);
        bool matches = !known || remote_matches(remote_live_, item, want_val);
        std::wstring live_w = !matches ? utf8_to_wide(remote_describe_live(remote_live_, item)) : L"";
        bool expanded = (expanded_remote_row_ == i);

        // Build chip choices when expanded (filtered like src/remote_view.c: >=1280, >=59Hz, 16:9 or 16:10 + monitor's own)
        struct ChipChoice { std::string val; std::wstring label; };
        std::vector<ChipChoice> choices;
        if (expanded) {
            if (item == RemoteItem::Screen) {
                std::string own = remote_own_size(remote_live_);
                bool own_added = own.empty();
                for (const auto& m : remote_live_.modes) {
                    bool is_16_10 = (m.w * 10 == m.h * 16);
                    bool is_16_9 = (m.w * 9 == m.h * 16);
                    std::string sp = m.spec();
                    bool is_own = (sp == own);
                    if (!is_own && (m.w < 1280 || m.rate < 59 || (!is_16_10 && !is_16_9))) continue;
                    if (is_own) own_added = true;
                    choices.push_back({ sp, utf8_to_wide(m.label()) });
                    if (choices.size() >= 12) break;
                }
                if (!own_added) {
                    int ow = 0, oh = 0; double orate = 60.0;
                    if (parse_screen_spec(own, ow, oh, orate)) {
                        choices.push_back({ own, utf8_to_wide(describe_screen_mode(ow, oh, orate)) });
                    }
                }
            } else {
                choices.push_back({ "on", L"On" });
                choices.push_back({ "off", L"Off" });
            }
        }

        float base_h = live_w.empty() ? 58.0f : 76.0f;
        int chip_rows = expanded ? static_cast<int>((choices.size() + 1) / 2) : 0;
        float chips_h = expanded ? (14.0f + chip_rows * 40.0f) : 0.0f;
        float row_h = base_h + chips_h;

        draw_info_card(rt, brush_, this, pal, x, y, w, row_h);
        float ix = x + 18.0f, iw = w - 36.0f;

        D2D1_RECT_F head_hit = D2D1::RectF(x, y, x + w, y + base_h);
        remote_hits_.push_back({ head_hit, RemoteActionKind::ToggleRowExpand, item, std::to_string(i) });

        Rgba ic = pal.muted, icf = ic; icf.a *= 0.35f;
        draw_pixel_art(rt, item_icons[i], ix, y + 20.0f, 2.0f, ic, icf);
        draw_text(rt, name_w.c_str(), fonts_.body(), D2D1::RectF(ix + 30.0f, y + 17.0f, ix + 220.0f, y + 44.0f), pal.text);
        if (!live_w.empty()) {
            draw_text(rt, live_w.c_str(), fonts_.caption(), D2D1::RectF(ix + 30.0f, y + 44.0f, ix + 340.0f, y + 68.0f), pal.warn);
        }

        draw_text(rt, desc_w.c_str(), fonts_.body(), D2D1::RectF(ix + iw - 290.0f, y + 17.0f, ix + iw - 28.0f, y + 44.0f),
                  pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        draw_pixel_art(rt, ART_CHEVRON, ix + iw - 18.0f, y + 23.0f, 2.0f, ic, icf);

        if (expanded && !choices.empty()) {
            float cy = y + base_h + 4.0f;
            float cw = (iw - 12.0f) * 0.5f;
            for (size_t c = 0; c < choices.size(); ++c) {
                float cx = ix + (c % 2) * (cw + 12.0f);
                float r_y = cy + (c / 2) * 40.0f;
                D2D1_RECT_F c_rc = D2D1::RectF(cx, r_y, cx + cw, r_y + 32.0f);
                bool active = (choices[c].val == want_val);
                brush_->SetColor(to_d2d(active ? pal.accent : pal.chip));
                rt->FillRoundedRectangle(D2D1::RoundedRect(c_rc, 9.0f, 9.0f), brush_);
                draw_text(rt, choices[c].label.c_str(), fonts_.caption(),
                          D2D1::RectF(c_rc.left + 14.0f, c_rc.top + 6.0f, c_rc.right - 8.0f, c_rc.bottom),
                          active ? pal.on_accent : pal.text);
                remote_hits_.push_back({ c_rc, RemoteActionKind::PickChip, item, choices[c].val });
            }
        }

        y += row_h + 12.0f;
    }

    content_height_ = (y - start_y) + 54.0f;
}

void MainWindow::draw_devices_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w, DevKind kind)
{
    float start_y = y;
    device_hits_.clear();
    static constexpr uint32_t kSwatchColours[9] = {
        0xff0000, 0xff5000, 0xffc800, 0x00ff00, 0x00ffff, 0x0000ff, 0x8000ff, 0xff00a0, 0xffffff
    };
    static constexpr int kTimerMinutes[5] = { 1, 2, 5, 10, 15 };

    float gap = 18.0f;
    for (const auto& d : dev_list_) {
        if (d.kind != kind) continue;
        float ix = x + kCardPad, iw = w - kCardPad * 2.0f;
        float err_extra = d.status_error.empty() ? 0.0f : 38.0f;

        // Calculate card height according to device kind & settings
        bool show_settings = d.has_settings && d.settings_loaded;
        bool show_waiting = d.has_settings && !d.settings_loaded;
        float app_note_extra = (kind == DevKind::Keyboard && show_settings && d.keyboard.app_controls &&
                                !d.windows_lighting) ? 32.0f : 0.0f;

        /* Windows Dynamic Lighting paints over the device's own effects
         * while it's on for it: say so where the effects are picked, with a
         * way to Settings, where it's turned off. */
        const wchar_t* dl_text = (kind == DevKind::Keyboard)
            ? L"Windows Dynamic Lighting is painting this keyboard's lights, so the effects below won't show "
              L"until you turn it off for this keyboard."
            : L"Windows Dynamic Lighting is painting this mouse's lights, so the effects below won't show "
              L"until you turn it off for this mouse.";
        float dl_text_h = measure_text_height(dl_text, fonts_.caption(), iw - 36.0f);
        float dl_note_h = (show_settings && d.windows_lighting) ? (14.0f + dl_text_h + 10.0f + 30.0f + 14.0f) : 0.0f;
        float dl_extra = dl_note_h > 0.0f ? dl_note_h + 12.0f : 0.0f;

        const wchar_t* key_desc = pad_prefs_.background
            ? L"Wave, Ripple and Built-in stay on the keyboard. The others play while Lares runs in the background."
            : L"Wave, Ripple and Built-in stay on the keyboard. The others play while Lares runs in the "
              L"background, which is off in Settings.";
        float key_desc_h = measure_text_height(key_desc, fonts_.caption(), iw);
        float key_head_h = 30.0f + key_desc_h + 10.0f;

        float card_h = 166.0f + err_extra + (show_waiting ? 46.0f : 0.0f);
        if (show_settings && kind == DevKind::Mouse) card_h = 582.0f + err_extra + dl_extra;
        else if (show_settings && kind == DevKind::Keyboard) {
            card_h = 660.0f - 70.0f + key_head_h + err_extra + app_note_extra + dl_extra;
        }

        draw_info_card(rt, brush_, this, pal, x, y, w, card_h);
        float cy = y + kCardPad;

        // Header: icon + name + battery or sleep state
        const char* const* d_art = (kind == DevKind::Audio) ? ART_HEADPHONES
            : (kind == DevKind::Keyboard) ? ART_KEYBOARD : ART_MOUSE;
        Rgba ic = pal.accent, icf = ic; icf.a *= 0.35f;
        draw_pixel_art(rt, d_art, ix, cy + 2.0f, 2.0f, ic, icf);
        std::wstring dname = utf8_to_wide(d.name);
        draw_text(rt, dname.c_str(), fonts_.body(), D2D1::RectF(ix + 30.0f, cy, ix + iw - 112.0f, cy + 28.0f), pal.text);

        if (d.asleep) {
            draw_text(rt, L"Asleep", fonts_.caption(), D2D1::RectF(ix + iw - 90.0f, cy + 3.0f, ix + iw, cy + 26.0f),
                      pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (d.battery >= 0) {
            wchar_t btxt[16];
            std::swprintf(btxt, std::size(btxt), L"%d%%", d.battery);
            bool low = (d.battery <= 20);
            draw_text(rt, btxt, fonts_.caption(), D2D1::RectF(ix + iw - 140.0f, cy + 3.0f, ix + iw - 87.0f, cy + 26.0f),
                      low ? pal.bad : pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_meter(rt, pal, ix + iw - 78.0f, cy + 9.0f, 10, 2.0f, d.battery / 100.0, low);
        }
        cy += 38.0f;

        if (!d.status_error.empty()) {
            std::wstring ew = utf8_to_wide(d.status_error);
            D2D1_RECT_F erc = D2D1::RectF(ix, cy, ix + iw, cy + 28.0f);
            brush_->SetColor(to_d2d(pal.note));
            rt->FillRoundedRectangle(D2D1::RoundedRect(erc, 9.0f, 9.0f), brush_);
            draw_text(rt, ew.c_str(), fonts_.caption(), D2D1::RectF(erc.left + 12.0f, erc.top + 5.0f, erc.right - 8.0f, erc.bottom), pal.warn);
            cy += 38.0f;
        }

        // Detail rows: Made by & Connection
        if (!d.vendor_name.empty()) {
            draw_text(rt, L"Made by", fonts_.caption(), D2D1::RectF(ix, cy, ix + 160.0f, cy + 24.0f), pal.muted);
            std::wstring vname = utf8_to_wide(d.vendor_name);
            draw_text(rt, vname.c_str(), fonts_.caption(), D2D1::RectF(ix + iw - 220.0f, cy, ix + iw, cy + 24.0f),
                      pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
            cy += 28.0f;
        }
        draw_text(rt, L"Connection", fonts_.caption(), D2D1::RectF(ix, cy, ix + 160.0f, cy + 24.0f), pal.muted);
        std::wstring ltxt = utf8_to_wide(dev_link_text(d.link));
        draw_text(rt, ltxt.c_str(), fonts_.caption(), D2D1::RectF(ix + iw - 220.0f, cy, ix + iw, cy + 24.0f),
                  pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
        cy += 32.0f;

        auto draw_divider = [&]() {
            brush_->SetColor(to_d2d(pal.line));
            rt->FillRectangle(D2D1::RectF(ix, cy, ix + iw, cy + 1.0f), brush_);
            cy += 14.0f;
        };

        auto draw_chip = [&](float cx, float r_y, float cw, const wchar_t* lbl, bool active,
                             DeviceActionKind act, int val) {
            D2D1_RECT_F rc = D2D1::RectF(cx, r_y, cx + cw, r_y + 30.0f);
            brush_->SetColor(to_d2d(active ? pal.accent : hovered(rc) ? pal.hover : pal.chip));
            rt->FillRoundedRectangle(D2D1::RoundedRect(rc, 9.0f, 9.0f), brush_);
            draw_text(rt, lbl, fonts_.caption(),
                      D2D1::RectF(rc.left + 6.0f, rc.top + 5.0f, rc.right - 6.0f, rc.bottom),
                      active ? pal.on_accent : pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            device_hits_.push_back({ rc, d.id, act, val });
        };

        auto draw_dl_note = [&]() {
            if (dl_note_h <= 0.0f) return;
            D2D1_RECT_F nrc = D2D1::RectF(ix, cy, ix + iw, cy + dl_note_h);
            brush_->SetColor(to_d2d(pal.note));
            rt->FillRoundedRectangle(D2D1::RoundedRect(nrc, 9.0f, 9.0f), brush_);
            draw_text(rt, dl_text, fonts_.caption(),
                      D2D1::RectF(nrc.left + 18.0f, nrc.top + 14.0f, nrc.right - 18.0f, nrc.top + 14.0f + dl_text_h + 4.0f),
                      pal.text);
            float by = nrc.top + 14.0f + dl_text_h + 10.0f;
            const wchar_t* blbl = L"Open Dynamic Lighting";
            D2D1_RECT_F brc = D2D1::RectF(nrc.left + 9.0f, by, nrc.left + 9.0f + 216.0f, by + 30.0f);
            if (hovered(brc)) {
                brush_->SetColor(to_d2d(pal.hover));
                rt->FillRoundedRectangle(D2D1::RoundedRect(brc, 9.0f, 9.0f), brush_);
            }
            draw_text(rt, blbl, fonts_.caption(), D2D1::RectF(brc.left + 9.0f, brc.top + 5.0f, brc.right - 9.0f, brc.bottom),
                      pal.accent);
            device_hits_.push_back({ brc, d.id, DeviceActionKind::OpenDynamicLighting, 0 });
            cy += dl_note_h + 12.0f;
        };

        auto draw_swatches = [&](uint32_t cur_rgb, DeviceActionKind act) {
            float sw_size = 26.0f;
            float sw_gap = 10.0f;
            for (int c = 0; c < 9; ++c) {
                uint32_t rgb = kSwatchColours[c];
                float sx = ix + c * (sw_size + sw_gap);
                D2D1_RECT_F s_rc = D2D1::RectF(sx, cy, sx + sw_size, cy + sw_size);
                bool active = (rgb == cur_rgb);
                brush_->SetColor(to_d2d(active ? pal.accent : hovered(s_rc) ? pal.muted : pal.line));
                rt->FillRoundedRectangle(D2D1::RoundedRect(s_rc, 6.0f, 6.0f), brush_);
                D2D1_RECT_F inner = D2D1::RectF(sx + 3.0f, cy + 3.0f, sx + sw_size - 3.0f, cy + sw_size - 3.0f);
                Rgba col = {
                    static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                    static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                    static_cast<float>(rgb & 0xff) / 255.0f,
                    1.0f
                };
                brush_->SetColor(to_d2d(col));
                rt->FillRoundedRectangle(D2D1::RoundedRect(inner, 4.0f, 4.0f), brush_);
                device_hits_.push_back({ s_rc, d.id, act, static_cast<int>(rgb) });
            }
            cy += 38.0f;
        };

        if (show_waiting) {
            draw_divider();
            const wchar_t* wait_msg = d.asleep
                ? (kind == DevKind::Keyboard
                    ? L"It's asleep. Press a key to wake it and show its settings."
                    : L"It's asleep. Move it to wake it and show its settings.")
                : L"Reading its settings...";
            draw_text(rt, wait_msg, fonts_.caption(), D2D1::RectF(ix, cy, ix + iw, cy + 32.0f), pal.muted);
        }

        // Mouse settings (HyperX Pulsefire Haste 2 Wireless)
        if (show_settings && kind == DevKind::Mouse) {
            const MouseSettings& m = d.mouse;
            draw_divider();

            // 1. DPI row + [- / +] buttons + 5 stage chips
            draw_text(rt, L"DPI", fonts_.body(), D2D1::RectF(ix, cy, ix + 160.0f, cy + 26.0f), pal.text);
            int cur_dpi = (m.active >= 0 && m.active < m.stages) ? m.dpi[m.active] : 1600;
            wchar_t dpi_txt[32];
            std::swprintf(dpi_txt, std::size(dpi_txt), L"%d", cur_dpi);
            draw_chip(ix + iw - 166.0f, cy - 2.0f, 36.0f, L"-", false, DeviceActionKind::MouseDpiDelta, -100);
            draw_text(rt, dpi_txt, fonts_.body(), D2D1::RectF(ix + iw - 118.0f, cy, ix + iw - 44.0f, cy + 26.0f), pal.text);
            draw_chip(ix + iw - 36.0f, cy - 2.0f, 36.0f, L"+", false, DeviceActionKind::MouseDpiDelta, 100);
            cy += 34.0f;

            float st_w = (iw - 8.0f * (m.stages - 1)) / static_cast<float>(std::max(1, m.stages));
            for (int s = 0; s < m.stages; ++s) {
                wchar_t slbl[24];
                std::swprintf(slbl, std::size(slbl), L"%d", m.dpi[s]);
                draw_chip(ix + s * (st_w + 8.0f), cy, st_w, slbl, s == m.active, DeviceActionKind::MouseStage, s);
            }
            cy += 44.0f;
            draw_divider();

            // 2. Polling rate
            draw_text(rt, L"Polling rate", fonts_.body(), D2D1::RectF(ix, cy, ix + 240.0f, cy + 26.0f), pal.text);
            cy += 32.0f;
            static constexpr int kRates[4] = { 125, 250, 500, 1000 };
            float rw = (iw - 24.0f) / 4.0f;
            for (int r = 0; r < 4; ++r) {
                wchar_t rlbl[24];
                std::swprintf(rlbl, std::size(rlbl), L"%d Hz", kRates[r]);
                draw_chip(ix + r * (rw + 8.0f), cy, rw, rlbl, m.rate == kRates[r], DeviceActionKind::MouseRate, kRates[r]);
            }
            cy += 44.0f;
            draw_divider();

            // 3. Lift-off distance
            draw_text(rt, L"Lift-off distance", fonts_.body(), D2D1::RectF(ix, cy, ix + 240.0f, cy + 26.0f), pal.text);
            draw_chip(ix + iw - 148.0f, cy - 2.0f, 68.0f, L"1 mm", m.lift_mm == 1, DeviceActionKind::MouseLift, 1);
            draw_chip(ix + iw - 70.0f, cy - 2.0f, 68.0f, L"2 mm", m.lift_mm == 2, DeviceActionKind::MouseLift, 2);
            cy += 40.0f;
            draw_divider();

            // 4. Lighting + 9 colour swatches
            draw_text(rt, L"Lighting", fonts_.body(), D2D1::RectF(ix, cy, ix + 240.0f, cy + 26.0f), pal.text);
            cy += 32.0f;
            draw_dl_note();
            static const wchar_t* const kMouseFx[4] = { L"Off", L"Solid", L"Breathing", L"Colour cycle" };
            float fw = (iw - 24.0f) / 4.0f;
            for (int f = 0; f < 4; ++f) {
                draw_chip(ix + f * (fw + 8.0f), cy, fw, kMouseFx[f],
                          static_cast<int>(m.light) == f, DeviceActionKind::MouseLight, f);
            }
            cy += 40.0f;
            uint32_t cur_rgb = (static_cast<uint32_t>(m.rgb[0]) << 16) |
                               (static_cast<uint32_t>(m.rgb[1]) << 8) | m.rgb[2];
            draw_swatches(cur_rgb, DeviceActionKind::MouseColour);
            draw_divider();

            // 5. Brightness (10-block interactive meter, 4*3=12px stride)
            draw_text(rt, L"Brightness", fonts_.body(), D2D1::RectF(ix, cy, ix + 180.0f, cy + 26.0f), pal.text);
            wchar_t b_pct[16];
            std::swprintf(b_pct, std::size(b_pct), L"%d%%", m.brightness);
            draw_text(rt, b_pct, fonts_.caption(), D2D1::RectF(ix + iw - 60.0f, cy + 3.0f, ix + iw, cy + 26.0f),
                      pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
            float mx = ix + iw - 196.0f;
            draw_meter(rt, pal, mx, cy + 7.0f, 10, 3.0f, m.brightness / 100.0, false);
            device_hits_.push_back({ D2D1::RectF(mx - 16.0f, cy, mx, cy + 26.0f), d.id, DeviceActionKind::MouseBrightness, 0 });
            for (int b = 1; b <= 10; ++b) {
                float bx = mx + (b - 1) * 12.0f;
                device_hits_.push_back({ D2D1::RectF(bx, cy, bx + 12.0f, cy + 26.0f), d.id, DeviceActionKind::MouseBrightness, b * 10 });
            }
        }

        // Keyboard settings (Logitech G715 HID++ 2.0)
        if (show_settings && kind == DevKind::Keyboard) {
            const KeyboardSettings& k = d.keyboard;
            draw_divider();

            // 1. Brightness (4-block stepped meter for 0, 12, 25, 50, 100%, matching src/device_view.c)
            draw_text(rt, L"Brightness", fonts_.body(), D2D1::RectF(ix, cy, ix + 180.0f, cy + 26.0f), pal.text);
            wchar_t b_pct[16];
            std::swprintf(b_pct, std::size(b_pct), L"%d%%", k.brightness);
            draw_text(rt, b_pct, fonts_.caption(), D2D1::RectF(ix + iw - 60.0f, cy + 3.0f, ix + iw, cy + 26.0f),
                      pal.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
            float mx = ix + iw - 122.0f;
            int n_levels = (k.levels > 1) ? k.levels : 5;
            static constexpr int kDefaultLevels[5] = { 0, 12, 25, 50, 100 };
            int near_lvl = 0;
            for (int i = 1; i < n_levels; ++i) {
                int lv = (k.levels > 1) ? k.level[i] : kDefaultLevels[i];
                int best = (k.levels > 1) ? k.level[near_lvl] : kDefaultLevels[near_lvl];
                if (std::abs(lv - k.brightness) < std::abs(best - k.brightness)) near_lvl = i;
            }
            int blocks = n_levels - 1;
            double kfrac = static_cast<double>(near_lvl) / static_cast<double>(blocks);
            draw_meter(rt, pal, mx, cy + 7.0f, blocks, 3.0f, kfrac, false);
            int lv0 = (k.levels > 1) ? k.level[0] : kDefaultLevels[0];
            device_hits_.push_back({ D2D1::RectF(mx - 16.0f, cy, mx, cy + 26.0f), d.id, DeviceActionKind::KeyBrightness, lv0 });
            for (int b = 1; b < n_levels; ++b) {
                float bx = mx + (b - 1) * 12.0f;
                int lv = (k.levels > 1) ? k.level[b] : kDefaultLevels[b];
                device_hits_.push_back({ D2D1::RectF(bx, cy, bx + 12.0f, cy + 26.0f), d.id, DeviceActionKind::KeyBrightness, lv });
            }
            cy += 38.0f;
            draw_divider();

            // 2. Lighting (7 effects in two rows + 9 colour swatches)
            draw_text(rt, L"Lighting", fonts_.body(), D2D1::RectF(ix, cy, ix + 240.0f, cy + 26.0f), pal.text);
            draw_text(rt, key_desc, fonts_.caption(), D2D1::RectF(ix, cy + 30.0f, ix + iw, cy + 30.0f + key_desc_h + 4.0f),
                      pal.muted);
            cy += key_head_h;
            draw_dl_note();
            static const wchar_t* const kKeyFx[7] = {
                L"Off", L"Solid", L"Breathing", L"Colour cycle", L"Wave", L"Ripple", L"Built-in"
            };
            float fw = (iw - 24.0f) / 4.0f;
            for (int f = 0; f < 7; ++f) {
                float cx = ix + (f % 4) * (fw + 8.0f);
                float ry = cy + (f / 4) * 36.0f;
                draw_chip(cx, ry, fw, kKeyFx[f], static_cast<int>(k.light) == f, DeviceActionKind::KeyLight, f);
            }
            cy += 78.0f;
            uint32_t cur_rgb = (static_cast<uint32_t>(k.rgb[0]) << 16) |
                               (static_cast<uint32_t>(k.rgb[1]) << 8) | k.rgb[2];
            draw_swatches(cur_rgb, DeviceActionKind::KeyColour);
            if (k.app_controls && !d.windows_lighting) {
                draw_text(rt, L"Another app holds its lighting now, so what shows may differ.",
                          fonts_.caption(), D2D1::RectF(ix, cy, ix + iw, cy + 28.0f), pal.muted);
                cy += 32.0f;
            }
            draw_divider();

            // 3. Startup & Shutdown effect switches (54x27px switch matching Linux)
            auto draw_key_switch = [&](const wchar_t* label, bool on, DeviceActionKind act) {
                draw_text(rt, label, fonts_.body(), D2D1::RectF(ix, cy + 1.0f, ix + iw - 70.0f, cy + 27.0f), pal.text);
                D2D1_RECT_F sw_rc = D2D1::RectF(ix + iw - 54.0f, cy, ix + iw, cy + 27.0f);
                brush_->SetColor(to_d2d(on ? pal.accent : pal.chip));
                rt->FillRoundedRectangle(D2D1::RoundedRect(sw_rc, 9.0f, 9.0f), brush_);
                float kx = on ? (sw_rc.right - 24.0f) : (sw_rc.left + 3.0f);
                D2D1_RECT_F knob = D2D1::RectF(kx, sw_rc.top + 3.0f, kx + 21.0f, sw_rc.bottom - 3.0f);
                brush_->SetColor(to_d2d(on ? pal.on_accent : pal.card));
                rt->FillRoundedRectangle(D2D1::RoundedRect(knob, 6.0f, 6.0f), brush_);
                device_hits_.push_back({ sw_rc, d.id, act, on ? 0 : 1 });
            };
            draw_key_switch(L"Startup effect", k.startup, DeviceActionKind::KeyStartup);
            cy += 36.0f;
            draw_key_switch(L"Shutdown effect", k.shutdown, DeviceActionKind::KeyShutdown);
            cy += 40.0f;
            draw_divider();

            // 4. Dim after & Lights off after (1, 2, 5, 10, 15 min)
            float tw = (iw - 32.0f) / 5.0f;
            /* A time set elsewhere, such as G HUB's seconds, matches no chip:
             * it's said beside the row's name instead of showing nothing. */
            auto draw_timer_now = [&](int secs) {
                for (int m : kTimerMinutes) {
                    if (secs == m * 60) return;
                }
                if (secs <= 0) return;
                wchar_t now_txt[32];
                if (secs < 60) std::swprintf(now_txt, std::size(now_txt), L"Now %d s", secs);
                else if (secs % 60 == 0) std::swprintf(now_txt, std::size(now_txt), L"Now %d min", secs / 60);
                else std::swprintf(now_txt, std::size(now_txt), L"Now %d min %d s", secs / 60, secs % 60);
                draw_text(rt, now_txt, fonts_.caption(), D2D1::RectF(ix + iw - 200.0f, cy + 3.0f, ix + iw, cy + 26.0f),
                          pal.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
            };
            draw_text(rt, L"Dim after", fonts_.body(), D2D1::RectF(ix, cy, ix + 240.0f, cy + 26.0f), pal.text);
            draw_timer_now(k.dim_s);
            cy += 30.0f;
            for (int t = 0; t < 5; ++t) {
                wchar_t tlbl[16];
                std::swprintf(tlbl, std::size(tlbl), L"%d min", kTimerMinutes[t]);
                int secs = kTimerMinutes[t] * 60;
                draw_chip(ix + t * (tw + 8.0f), cy, tw, tlbl, k.dim_s == secs, DeviceActionKind::KeyDim, secs);
            }
            cy += 42.0f;

            draw_text(rt, L"Lights off after", fonts_.body(), D2D1::RectF(ix, cy, ix + 240.0f, cy + 26.0f), pal.text);
            draw_timer_now(k.off_s);
            cy += 30.0f;
            for (int t = 0; t < 5; ++t) {
                wchar_t tlbl[16];
                std::swprintf(tlbl, std::size(tlbl), L"%d min", kTimerMinutes[t]);
                int secs = kTimerMinutes[t] * 60;
                draw_chip(ix + t * (tw + 8.0f), cy, tw, tlbl, k.off_s == secs, DeviceActionKind::KeyOff, secs);
            }
        }

        y += card_h + gap;
    }

    content_height_ = (y - start_y) + 54.0f;
}

static int count_bits32(uint32_t v)
{
    int c = 0;
    while (v) {
        v &= (v - 1);
        ++c;
    }
    return c;
}

void MainWindow::draw_pad_picture(ID2D1RenderTarget* rt, const Palette& pal,
                                  float x, float y, float w,
                                  const PadState& state, uint32_t seen,
                                  bool placeholder, int shake_px)
{
    if (pad_map_.width <= 0 || pad_map_.height <= 0 || !brush_) return;

    float scale = (!placeholder && w >= pad_map_.width * 2.0f) ? 2.0f : 1.0f;
    float pic_w = pad_map_.width * scale;
    float ox = x + std::floor((w - pic_w) * 0.5f) + shake_px * scale;
    float oy = y;

    PadLook look = pad_compute_look(pad_map_, state, seen, placeholder);
    float strength = placeholder ? 0.30f : 1.0f;

    // Compose a temporary grid of PadPaint for the two passes (static + moving caps)
    size_t total = static_cast<size_t>(pad_map_.width) * static_cast<size_t>(pad_map_.height);
    std::vector<PadPaint> grid(total, PadPaint::None);

    for (int pass = 0; pass < 2; ++pass) {
        for (int py = 0; py < pad_map_.height; ++py) {
            for (int px = 0; px < pad_map_.width; ++px) {
                uint8_t cell = pad_map_.at(px, py);
                PadCellKind kind = pad_cell_kind(cell);
                PadButton b = pad_cell_button(cell);
                bool is_cap = (kind == PAD_CELL_CAP_LINE || kind == PAD_CELL_CAP_AREA);
                if (is_cap != (pass == 1)) continue;
                if (placeholder && kind == PAD_CELL_GAUGE) continue;

                uint32_t held = look.held;
                if (kind == PAD_CELL_AREA && (b == PB_LT || b == PB_RT) && pad_gauge_lit(pad_map_, look, b, px)) {
                    held |= pb_bit(b);
                }
                int dx = 0, dy = 0;
                if (is_cap) {
                    int idx = (b == PB_RS) ? 1 : 0;
                    dx = look.caps[idx][0];
                    dy = look.caps[idx][1];
                }
                int tx = px + dx;
                int ty = py + dy;
                if (tx < 0 || ty < 0 || tx >= pad_map_.width || ty >= pad_map_.height) continue;

                PadPaint pt = pad_cell_paint(cell, held, look.seen);
                if (pt != PadPaint::None) {
                    grid[static_cast<size_t>(ty) * static_cast<size_t>(pad_map_.width) + static_cast<size_t>(tx)] = pt;
                }
            }
        }
    }

    auto paint_color = [&](PadPaint pt) -> D2D1_COLOR_F {
        switch (pt) {
        case PadPaint::Line:  return to_d2d(pal.text, strength);
        case PadPaint::Faint: return to_d2d(pal.text, 0.40f * strength);
        case PadPaint::Lit:   return to_d2d(pal.accent, strength);
        case PadPaint::Seen:  return to_d2d(pal.accent, 0.36f * strength);
        default:              return D2D1::ColorF(0, 0, 0, 0);
        }
    };

    // Draw merged horizontal runs for crisp, fast Direct2D rendering
    for (int py = 0; py < pad_map_.height; ++py) {
        int px = 0;
        while (px < pad_map_.width) {
            PadPaint pt = grid[static_cast<size_t>(py) * static_cast<size_t>(pad_map_.width) + static_cast<size_t>(px)];
            if (pt == PadPaint::None) {
                ++px;
                continue;
            }
            int run_end = px + 1;
            while (run_end < pad_map_.width &&
                   grid[static_cast<size_t>(py) * static_cast<size_t>(pad_map_.width) + static_cast<size_t>(run_end)] == pt) {
                ++run_end;
            }
            brush_->SetColor(paint_color(pt));
            rt->FillRectangle(D2D1::RectF(
                ox + px * scale,
                oy + py * scale,
                ox + run_end * scale,
                oy + (py + 1) * scale), brush_);
            px = run_end;
        }
    }
}

void MainWindow::draw_controller_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    float start_y = y;
    pad_hits_.clear();
    float gap = 18.0f;

    wchar_t env_tab[32] = {};
    int default_tab = 0;
    if (GetEnvironmentVariableW(L"LARES_PAD_TAB", env_tab, 32) > 0) {
        if (_wcsicmp(env_tab, L"sticks") == 0) default_tab = 1;
        else if (_wcsicmp(env_tab, L"vibration") == 0) default_tab = 2;
    }

    auto draw_chip_btn = [&](float cx, float cy, float cw, const wchar_t* lbl, bool active,
                             const std::string& path, PadActionKind act, int val = 0) {
        D2D1_RECT_F rc = D2D1::RectF(cx, cy, cx + cw, cy + 32.0f);
        brush_->SetColor(to_d2d(active ? pal.accent : hovered(rc) ? pal.hover : pal.chip));
        rt->FillRoundedRectangle(D2D1::RoundedRect(rc, 9.0f, 9.0f), brush_);
        draw_text(rt, lbl, fonts_.caption(),
                  D2D1::RectF(rc.left + 6.0f, rc.top + 6.0f, rc.right - 6.0f, rc.bottom),
                  active ? pal.on_accent : pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        pad_hits_.push_back({ rc, path, act, val });
    };

    // 1. Connected controller cards (or placeholder card if none connected)
    if (pad_list_.empty()) {
        float empty_h = 248.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, empty_h);
        PadState idle{};
        draw_pad_picture(rt, pal, x + 18.0f, y + 18.0f, w - 36.0f, idle, 0, true, 0);
        draw_text(rt, L"No controller connected. Plug in a USB cable, or connect one over Bluetooth.",
                  fonts_.caption(), D2D1::RectF(x + 24.0f, y + 196.0f, x + w - 24.0f, y + 236.0f), pal.muted);
        y += empty_h + gap;
    } else {
        for (const auto& p : pad_list_) {
            auto it = pad_memory_.find(p.path);
            if (it == pad_memory_.end()) {
                PadCardMemory init_m;
                init_m.tab = default_tab;
                it = pad_memory_.emplace(p.path, init_m).first;
            }
            PadCardMemory& mem = it->second;
            int tab = p.rumble ? mem.tab : std::min(mem.tab, 1);

            float tab_body_h = (tab == 0) ? 368.0f : (tab == 1) ? 264.0f : 230.0f;
            float card_h = 116.0f + tab_body_h + 166.0f;
            draw_info_card(rt, brush_, this, pal, x, y, w, card_h);
            float ix = x + 18.0f, iw = w - 36.0f, cy = y + 18.0f;

            // Header: gamepad art + nickname/name + connection subtitle + pencil button
            Rgba ac_f = pal.accent; ac_f.a *= 0.35f;
            draw_pixel_art(rt, ART_GAMEPAD, ix, cy + 4.0f, 3.0f, pal.accent, ac_f);

            if (editing_pad_path_ == p.path) {
                D2D1_RECT_F box = D2D1::RectF(ix + 44.0f, cy - 2.0f, ix + iw - 92.0f, cy + 28.0f);
                brush_->SetColor(to_d2d(pal.bg));
                rt->FillRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), brush_);
                brush_->SetColor(to_d2d(pal.accent));
                rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), brush_, 1.5f);
                std::wstring edit_w = editing_nickname_.empty()
                    ? L"Type nickname + Enter"
                    : (utf8_to_wide(editing_nickname_) + L"_");
                draw_text(rt, edit_w.c_str(), fonts_.body(),
                          D2D1::RectF(box.left + 8.0f, box.top + 2.0f, box.right - 8.0f, box.bottom),
                          editing_nickname_.empty() ? pal.muted : pal.text);
                draw_chip_btn(ix + iw - 82.0f, cy - 2.0f, 82.0f, L"Save", true, p.path, PadActionKind::EditNickname, 1);
            } else {
                std::wstring title_w = utf8_to_wide(pad_display_name(p));
                draw_text(rt, title_w.c_str(), fonts_.body(),
                          D2D1::RectF(ix + 44.0f, cy, ix + iw - 44.0f, cy + 26.0f), pal.text);
                D2D1_RECT_F pen_rc = D2D1::RectF(ix + iw - 36.0f, cy, ix + iw, cy + 32.0f);
                brush_->SetColor(to_d2d(pal.chip));
                rt->FillRoundedRectangle(D2D1::RoundedRect(pen_rc, 8.0f, 8.0f), brush_);
                Rgba mc = pal.text, mcf = mc; mcf.a *= 0.35f;
                draw_pixel_art(rt, ART_PENCIL, pen_rc.left + 9.0f, pen_rc.top + 7.0f, 2.0f, mc, mcf);
                pad_hits_.push_back({ pen_rc, p.path, PadActionKind::EditNickname, 0 });
            }

            std::string sub_utf8 = !p.nickname.empty()
                ? (p.name + " \xC2\xB7 " + pad_connection_name(p.connection))
                : pad_connection_name(p.connection);
            std::wstring sub_w = utf8_to_wide(sub_utf8);
            draw_text(rt, sub_w.c_str(), fonts_.caption(),
                      D2D1::RectF(ix + 44.0f, cy + 26.0f, ix + iw - 44.0f, cy + 48.0f), pal.muted);
            cy += 56.0f;

            // Tabs: Buttons | Sticks | Vibration
            static const wchar_t* const kTabs[3] = { L"Buttons", L"Sticks", L"Vibration" };
            int n_tabs = p.rumble ? 3 : 2;
            for (int t = 0; t < n_tabs; ++t) {
                draw_chip_btn(ix + t * 116.0f, cy, 108.0f, kTabs[t], tab == t, p.path, PadActionKind::SelectTab, t);
            }
            cy += 44.0f;

            if (tab == 0) {
                // Buttons tab: 2x pixel-art controller canvas + checklist count + Reset
                draw_pad_picture(rt, pal, ix, cy, iw, p.state, mem.seen, false, mem.shake_offset);
                cy += 328.0f;

                int total_btn = count_bits32(p.checklist);
                int seen_btn = count_bits32(mem.seen & p.checklist);
                wchar_t cnt_buf[64];
                if (seen_btn == total_btn && total_btn > 0) {
                    std::swprintf(cnt_buf, std::size(cnt_buf), L"Every button responded");
                } else {
                    std::swprintf(cnt_buf, std::size(cnt_buf), L"Press every button: %d of %d", seen_btn, total_btn);
                }
                draw_text(rt, cnt_buf, fonts_.caption(), D2D1::RectF(ix, cy + 6.0f, ix + iw - 96.0f, cy + 32.0f),
                          (seen_btn == total_btn && total_btn > 0) ? pal.text : pal.muted);
                draw_chip_btn(ix + iw - 84.0f, cy, 84.0f, L"Reset", false, p.path, PadActionKind::ResetChecklist, 0);
                cy += 40.0f;
            } else if (tab == 1) {
                // Sticks tab: two circular stick gauges + drift check + 15-block trigger meters
                float col_w = (iw - 18.0f) * 0.5f;
                const wchar_t* s_names[2] = { L"Left stick", L"Right stick" };
                const wchar_t* t_names[2] = { L"Left trigger", L"Right trigger" };
                float trig_vals[2] = { p.state.lt, p.state.rt };

                for (int i = 0; i < 2; ++i) {
                    float sx = ix + i * (col_w + 18.0f);
                    draw_text(rt, s_names[i], fonts_.body(), D2D1::RectF(sx, cy, sx + col_w, cy + 24.0f), pal.text);

                    // 126x126 circular stick gauge (ported from draw_gauge in src/pad_view.c)
                    float gx = sx + 63.0f, gy = cy + 28.0f + 63.0f, rad = 59.0f;
                    D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(gx, gy), rad, rad);
                    brush_->SetColor(to_d2d(pal.text, 0.07f));
                    rt->FillEllipse(outer, brush_);
                    brush_->SetColor(to_d2d(pal.text, 0.22f));
                    rt->DrawEllipse(outer, brush_, 1.0f);

                    brush_->SetColor(to_d2d(pal.text, 0.12f));
                    rt->DrawLine(D2D1::Point2F(gx - rad, gy), D2D1::Point2F(gx + rad, gy), brush_, 1.0f);
                    rt->DrawLine(D2D1::Point2F(gx, gy - rad), D2D1::Point2F(gx, gy + rad), brush_, 1.0f);

                    D2D1_ELLIPSE dz = D2D1::Ellipse(D2D1::Point2F(gx, gy),
                                                    static_cast<float>(rad * kStickDeadZone),
                                                    static_cast<float>(rad * kStickDeadZone));
                    brush_->SetColor(to_d2d(pal.text, 0.16f));
                    rt->FillEllipse(dz, brush_);
                    brush_->SetColor(to_d2d(pal.text, 0.40f));
                    rt->DrawEllipse(dz, brush_, 1.0f);

                    const StickTracker& st = mem.sticks[i];
                    bool drift = stick_drifting(st);
                    if (st.has_rest) {
                        double rx = st.rx, ry = st.ry, r_reach = std::hypot(rx, ry);
                        if (r_reach > 1.0) { rx /= r_reach; ry /= r_reach; }
                        D2D1_ELLIPSE rest_el = D2D1::Ellipse(
                            D2D1::Point2F(gx + static_cast<float>(rx * rad), gy + static_cast<float>(ry * rad)),
                            6.0f, 6.0f);
                        brush_->SetColor(drift ? to_d2d(pal.warn) : to_d2d(pal.text, 0.55f));
                        rt->DrawEllipse(rest_el, brush_, 1.5f);
                    }

                    double cur_x = st.x, cur_y = st.y, c_reach = std::hypot(cur_x, cur_y);
                    if (c_reach > 1.0) { cur_x /= c_reach; cur_y /= c_reach; }
                    D2D1_ELLIPSE dot_el = D2D1::Ellipse(
                        D2D1::Point2F(gx + static_cast<float>(cur_x * rad), gy + static_cast<float>(cur_y * rad)),
                        4.0f, 4.0f);
                    brush_->SetColor(to_d2d(pal.live));
                    rt->FillEllipse(dot_el, brush_);

                    int px_pct = static_cast<int>(std::lround(st.x * 100.0));
                    int py_pct = static_cast<int>(std::lround(-st.y * 100.0));
                    wchar_t st_txt[96];
                    if (drift) {
                        int off_pct = static_cast<int>(std::lround(std::hypot(st.rx, st.ry) * 100.0));
                        std::swprintf(st_txt, std::size(st_txt), L"%d%%, %d%% · Rests %d%% off centre", px_pct, py_pct, off_pct);
                    } else if (st.has_rest) {
                        std::swprintf(st_txt, std::size(st_txt), L"%d%%, %d%% · Rests in the centre", px_pct, py_pct);
                    } else {
                        std::swprintf(st_txt, std::size(st_txt), L"%d%%, %d%% · Hold still to check rest", px_pct, py_pct);
                    }
                    draw_text(rt, st_txt, fonts_.caption(),
                              D2D1::RectF(sx, cy + 158.0f, sx + col_w, cy + 198.0f),
                              drift ? pal.warn : pal.muted);

                    // Trigger meter row (15 blocks)
                    draw_text(rt, t_names[i], fonts_.caption(),
                              D2D1::RectF(sx, cy + 204.0f, sx + col_w - 50.0f, cy + 226.0f), pal.text);
                    wchar_t tp_txt[16];
                    std::swprintf(tp_txt, std::size(tp_txt), L"%d%%", static_cast<int>(std::lround(trig_vals[i] * 100.0f)));
                    draw_text(rt, tp_txt, fonts_.caption(),
                              D2D1::RectF(sx + col_w - 48.0f, cy + 204.0f, sx + col_w, cy + 226.0f), pal.text);
                    draw_meter(rt, pal, sx, cy + 232.0f, 15, 2.0f, trig_vals[i], false);
                }
                cy += 264.0f;
            } else {
                // Vibration tab: Strong motor, Weak motor, Length + Play button
                auto draw_vib_slider = [&](const wchar_t* heading, const wchar_t* val_lbl, double frac,
                                           PadActionKind act, int max_val, int step) {
                    draw_text(rt, heading, fonts_.body(), D2D1::RectF(ix, cy, ix + iw - 80.0f, cy + 24.0f), pal.text);
                    draw_text(rt, val_lbl, fonts_.caption(), D2D1::RectF(ix + iw - 68.0f, cy + 2.0f, ix + iw, cy + 24.0f), pal.text);
                    draw_meter(rt, pal, ix, cy + 28.0f, 20, 2.0f, frac, false);
                    pad_hits_.push_back({ D2D1::RectF(ix - 12.0f, cy + 22.0f, ix, cy + 44.0f), p.path, act, 0 });
                    for (int b = 1; b <= 20; ++b) {
                        int v = ((b * max_val / 20) / step) * step;
                        float bx = ix + (b - 1) * 8.0f;
                        pad_hits_.push_back({ D2D1::RectF(bx, cy + 22.0f, bx + 8.0f, cy + 44.0f), p.path, act, v });
                    }
                    cy += 52.0f;
                };

                wchar_t s_lbl[16], w_lbl[16], l_lbl[24];
                std::swprintf(s_lbl, std::size(s_lbl), L"%d%%", mem.strong);
                std::swprintf(w_lbl, std::size(w_lbl), L"%d%%", mem.weak);
                std::swprintf(l_lbl, std::size(l_lbl), L"%.1f s", mem.length_ms / 1000.0);

                draw_vib_slider(L"Strong motor, on the left", s_lbl, mem.strong / 100.0, PadActionKind::StrongMotor, 100, 5);
                draw_vib_slider(L"Weak motor, on the right", w_lbl, mem.weak / 100.0, PadActionKind::WeakMotor, 100, 5);
                draw_vib_slider(L"Length", l_lbl, mem.length_ms / 2000.0, PadActionKind::LengthMs, 2000, 100);

                draw_text(rt, L"XInput drives these two motors; the ones in the triggers aren't part of it.",
                          fonts_.caption(), D2D1::RectF(ix, cy + 4.0f, ix + iw - 118.0f, cy + 48.0f), pal.muted);
                draw_chip_btn(ix + iw - 104.0f, cy + 6.0f, 104.0f,
                              p.playing_rumble ? L"Playing…" : L"Play", true,
                              p.path, PadActionKind::PlayVibration, 0);
                cy += 74.0f;
            }

            // Divider before Facts & Actions
            brush_->SetColor(to_d2d(pal.line));
            rt->FillRectangle(D2D1::RectF(ix, cy, ix + iw, cy + 1.0f), brush_);
            cy += 14.0f;

            // Facts row: Battery, Model, Firmware
            draw_text(rt, L"Battery", fonts_.caption(), D2D1::RectF(ix, cy, ix + 140.0f, cy + 20.0f), pal.muted);
            if (p.battery.known) {
                bool low = (pad_battery_low_stage(p.battery, pad_prefs_.low_level) > 0 && !pad_battery_charging(p.battery));
                draw_meter(rt, pal, ix, cy + 26.0f, 10, 2.0f, pad_battery_fraction(p.battery), low);
                std::wstring bt = utf8_to_wide(pad_battery_text(p.battery));
                draw_text(rt, bt.c_str(), fonts_.body(), D2D1::RectF(ix + 87.0f, cy + 20.0f, ix + 220.0f, cy + 44.0f),
                          low ? pal.bad : pal.text);
            } else {
                draw_text(rt, p.connection == PadConnection::Usb ? L"On USB power" : L"No reading yet",
                          fonts_.body(), D2D1::RectF(ix, cy + 20.0f, ix + 220.0f, cy + 44.0f), pal.text);
            }

            if (!p.model.empty()) {
                draw_text(rt, L"Model", fonts_.caption(), D2D1::RectF(ix + 230.0f, cy, ix + 340.0f, cy + 20.0f), pal.muted);
                std::wstring mw = utf8_to_wide(p.model);
                draw_text(rt, mw.c_str(), fonts_.body(), D2D1::RectF(ix + 230.0f, cy + 20.0f, ix + 340.0f, cy + 44.0f), pal.text);
            }
            if (!p.firmware.empty()) {
                draw_text(rt, L"Firmware", fonts_.caption(), D2D1::RectF(ix + 350.0f, cy, ix + iw, cy + 20.0f), pal.muted);
                std::wstring fw = utf8_to_wide(p.firmware);
                draw_text(rt, fw.c_str(), fonts_.body(), D2D1::RectF(ix + 350.0f, cy + 20.0f, ix + iw, cy + 44.0f), pal.text);
            }
            cy += 50.0f;

            std::wstring det_w = utf8_to_wide((p.driver.empty() ? "XInput" : p.driver) + " \xC2\xB7 " + p.event_node);
            draw_text(rt, det_w.c_str(), fonts_.caption(), D2D1::RectF(ix, cy, ix + iw, cy + 24.0f), pal.muted);
            cy += 32.0f;

            // Action chips row: Test vibration & Find controller (matching src/pad_view.c)
            if (p.rumble) {
                float ax = ix;
                if (tab != 2) {
                    draw_chip_btn(ax, cy, 158.0f, L"Test vibration", false,
                                  p.path, PadActionKind::TestVibration, 0);
                    ax += 168.0f;
                }
                if (p.connection != PadConnection::Usb) {
                    draw_chip_btn(ax, cy, 168.0f,
                                  p.finding ? L"Stop finding" : L"Find controller", p.finding,
                                  p.path, PadActionKind::FindController, 0);
                }
            }

            y += card_h + gap;
        }
    }

    // 2. Pair a controller card
    {
        float pair_h = 98.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, pair_h);
        float ix = x + 18.0f, iw = w - 36.0f;
        draw_text(rt, L"Hold the pair button on top of the controller until the Xbox button flashes quickly.",
                  fonts_.caption(), D2D1::RectF(ix, y + 16.0f, ix + iw, y + 56.0f), pal.muted);
        draw_chip_btn(ix + iw - 188.0f, y + 54.0f, 188.0f, L"Pair a controller", true,
                      "", PadActionKind::OpenBluetoothSettings, 0);
        y += pair_h + gap;
    }

    // 3. Background off banner if background helper is disabled
    if (!pad_prefs_.background) {
        float off_h = 74.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, off_h);
        float ix = x + 18.0f, iw = w - 36.0f;
        draw_text(rt, L"Background is off", fonts_.body(), D2D1::RectF(ix, y + 14.0f, ix + iw - 150.0f, y + 38.0f), pal.text);
        draw_text(rt, L"Alerts and play features need it.", fonts_.caption(),
                  D2D1::RectF(ix, y + 38.0f, ix + iw - 150.0f, y + 62.0f), pal.muted);
        draw_chip_btn(ix + iw - 134.0f, y + 21.0f, 134.0f, L"Open Settings", false,
                      "", PadActionKind::OpenSettings, 0);
        y += off_h + gap;
    }

    // 4. Alerts group (muted heading indented 27px + single .card.padded with 2px dividers)
    draw_text(rt, L"Alerts", fonts_.body(), D2D1::RectF(x + kCardPad, y, x + w, y + 26.0f), pal.muted);
    y += 30.0f;
    {
        float ix = x + kCardPad, iw = w - kCardPad * 2.0f;
        float alerts_h = kCardPad * 2.0f + 56.0f + 20.0f + 56.0f + 20.0f + 56.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, alerts_h);
        float cy = y + kCardPad;
        draw_toggle_row(rt, pal, ix, cy, iw,
                        L"Low battery alerts",
                        L"Notifies when low, and again at 10%.",
                        nullptr, &pad_prefs_.alerts, pad_prefs_.background);
        brush_->SetColor(to_d2d(pal.line));
        rt->FillRectangle(D2D1::RectF(ix, cy - 2.0f, ix + iw, cy), brush_);
        cy += 16.0f;

        draw_text(rt, L"Low battery level", fonts_.body(), D2D1::RectF(ix, cy, ix + iw - 160.0f, cy + 24.0f), pal.text);
        draw_text(rt, L"The battery turns orange at this level.", fonts_.caption(),
                  D2D1::RectF(ix, cy + 24.0f, ix + iw - 160.0f, cy + 48.0f), pal.muted);
        draw_chip_btn(ix + iw - 144.0f, cy + 7.0f, 36.0f, L"-", false, "", PadActionKind::LowLevelDelta, -5);
        wchar_t l_txt[16];
        std::swprintf(l_txt, std::size(l_txt), L"%d%%", pad_prefs_.low_level);
        draw_text(rt, l_txt, fonts_.body(), D2D1::RectF(ix + iw - 98.0f, cy + 10.0f, ix + iw - 40.0f, cy + 36.0f), pal.text);
        draw_chip_btn(ix + iw - 36.0f, cy + 7.0f, 36.0f, L"+", false, "", PadActionKind::LowLevelDelta, 5);
        cy += 60.0f;

        brush_->SetColor(to_d2d(pal.line));
        rt->FillRectangle(D2D1::RectF(ix, cy - 2.0f, ix + iw, cy), brush_);
        cy += 16.0f;
        draw_toggle_row(rt, pal, ix, cy, iw,
                        L"Connection notices",
                        L"Notifies when a controller connects or leaves.",
                        nullptr, &pad_prefs_.notices, pad_prefs_.background);
        y += alerts_h + kPageGap;
    }

    // 5. While you play group
    draw_text(rt, L"While you play", fonts_.body(), D2D1::RectF(x + kCardPad, y, x + w, y + 26.0f), pal.muted);
    y += 30.0f;
    {
        float ix = x + kCardPad, iw = w - kCardPad * 2.0f;
        float play_h = kCardPad * 2.0f + 56.0f + 20.0f + 56.0f + (pad_prefs_.idle_disconnect ? 46.0f : 0.0f);
        draw_info_card(rt, brush_, this, pal, x, y, w, play_h);
        float cy = y + kCardPad;
        draw_toggle_row(rt, pal, ix, cy, iw,
                        L"Keep the screen awake",
                        L"No dimming or locking while you play.",
                        nullptr, &pad_prefs_.keep_awake, pad_prefs_.background);
        brush_->SetColor(to_d2d(pal.line));
        rt->FillRectangle(D2D1::RectF(ix, cy - 2.0f, ix + iw, cy), brush_);
        cy += 16.0f;
        draw_toggle_row(rt, pal, ix, cy, iw,
                        L"Disconnect idle controllers",
                        L"Saves controller battery. They stay paired.",
                        nullptr, &pad_prefs_.idle_disconnect, pad_prefs_.background);
        if (pad_prefs_.idle_disconnect) {
            float cw = (iw - 27.0f) / 4.0f;
            int nearest = 0;
            for (int i = 0; i < kPadIdleChoices; ++i) {
                if (std::abs(kPadIdleMinutes[i] - pad_prefs_.idle_minutes) <
                    std::abs(kPadIdleMinutes[nearest] - pad_prefs_.idle_minutes)) {
                    nearest = i;
                }
            }
            for (int i = 0; i < kPadIdleChoices; ++i) {
                wchar_t mlbl[16];
                std::swprintf(mlbl, std::size(mlbl), L"%d min", kPadIdleMinutes[i]);
                draw_chip_btn(ix + i * (cw + 9.0f), cy, cw, mlbl, i == nearest,
                              "", PadActionKind::IdleMinutesChip, kPadIdleMinutes[i]);
            }
        }
        y += play_h + kPageGap;
    }

    // 6. Saved Nicknames group
    if (!pad_prefs_.nicknames.empty()) {
        y += 14.0f;
        draw_text(rt, L"Nicknames", fonts_.body(), D2D1::RectF(x, y, x + w, y + 30.0f), pal.text);
        y += 34.0f;
        for (const auto& kv : pad_prefs_.nicknames) {
            float nh = 68.0f;
            draw_info_card(rt, brush_, this, pal, x, y, w, nh);
            float ix = x + 18.0f, iw = w - 36.0f;
            size_t tab_pos = kv.second.find('\t');
            std::string n_title = (tab_pos != std::string::npos) ? kv.second.substr(0, tab_pos) : kv.second;
            std::string n_sub = (tab_pos != std::string::npos) ? kv.second.substr(tab_pos + 1) : "";
            std::wstring nw = utf8_to_wide(n_title);
            std::wstring sw = utf8_to_wide(n_sub);
            draw_text(rt, nw.c_str(), fonts_.body(), D2D1::RectF(ix, y + 12.0f, ix + iw - 110.0f, y + 36.0f), pal.text);
            draw_text(rt, sw.c_str(), fonts_.caption(), D2D1::RectF(ix, y + 36.0f, ix + iw - 110.0f, y + 58.0f), pal.muted);
            draw_chip_btn(ix + iw - 88.0f, y + 18.0f, 88.0f, L"Forget", false,
                          kv.first, PadActionKind::ForgetNickname, 0);
            y += nh + 10.0f;
        }
    }

    content_height_ = (y - start_y) + 54.0f;
}

void MainWindow::draw_page(ID2D1RenderTarget* rt, const Palette& pal, float client_w, float client_h)
{
    toggle_hits_.clear();
    card_hits_.clear();
    remote_hits_.clear();
    device_hits_.clear();
    pad_hits_.clear();
    float col_x = kSidebarWidth + kContentLead + 18.0f;
    float avail_w = std::max(260.0f, client_w - col_x - 36.0f);
    float col_w = std::min(kContentMaxW, avail_w);
    float col_y = 18.0f - scroll_y_;

    D2D1_RECT_F clip = D2D1::RectF(kSidebarWidth, 0.0f, client_w, client_h);
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

    const char* id = sections_[current_sec_].id;
    if (std::strcmp(id, "settings") == 0) draw_settings_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "hardware") == 0) draw_hardware_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "controller") == 0) draw_controller_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "remote") == 0) draw_remote_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "headphones") == 0) draw_devices_page(rt, pal, col_x, col_y, col_w, DevKind::Audio);
    else if (std::strcmp(id, "keyboard") == 0) draw_devices_page(rt, pal, col_x, col_y, col_w, DevKind::Keyboard);
    else if (std::strcmp(id, "mouse") == 0) draw_devices_page(rt, pal, col_x, col_y, col_w, DevKind::Mouse);
    else draw_overview_page(rt, pal, col_x, col_y, col_w);

    rt->PopAxisAlignedClip();
}

void MainWindow::on_paint()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int px_w = rc.right - rc.left;
    int px_h = rc.bottom - rc.top;

    if (dc_rt_ && ensure_backbuffer(px_w, px_h)) {
        RECT bind_rc = { 0, 0, px_w, px_h };
        if (SUCCEEDED(dc_rt_->BindDC(mem_dc_, &bind_rc))) {
            Palette pal = current_palette();
            float lw = static_cast<float>(px_w) / dpi_scale_;
            float lh = static_cast<float>(px_h) / dpi_scale_;

            dc_rt_->BeginDraw();
            dc_rt_->SetTransform(D2D1::Matrix3x2F::Scale(dpi_scale_, dpi_scale_));
            dc_rt_->Clear(to_d2d(pal.bg));
            draw_sidebar(dc_rt_, pal, lh);
            draw_page(dc_rt_, pal, lw, lh);
            dc_rt_->EndDraw();

            BitBlt(hdc, 0, 0, px_w, px_h, mem_dc_, 0, 0, SRCCOPY);
        }
    }
    EndPaint(hwnd_, &ps);
}

void MainWindow::on_mouse_move(float lx, float ly)
{
    if (!tracking_mouse_ && hwnd_) {
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        tracking_mouse_ = (TrackMouseEvent(&tme) != FALSE);
    }
    int new_hover = -1;
    if (lx >= kNavPadX && lx <= kSidebarWidth - kNavPadX) {
        int vis = 0;
        for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
            if (!sections_[i].visible) continue;
            float iy = kNavPadTop + vis * (kNavItemHeight + kNavItemGap);
            if (ly >= iy && ly <= iy + kNavItemHeight) {
                new_hover = i;
                break;
            }
            ++vis;
        }
    }
    mouse_in_ = true;
    mouse_x_ = lx;
    mouse_y_ = ly;
    D2D1_RECT_F rc = {};
    bool on = lx >= kSidebarWidth && interactive_rect_at(lx, ly, &rc);
    bool moved = on != hover_on_ || (on && std::memcmp(&rc, &hover_rect_, sizeof(rc)) != 0);
    hover_on_ = on;
    hover_rect_ = rc;
    if (new_hover != hover_sec_ || moved) {
        hover_sec_ = new_hover;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::on_mouse_press(float lx, float ly)
{
    pressed_ = true;
    press_x_ = lx;
    press_y_ = ly;
    SetCapture(hwnd_);
    SetFocus(hwnd_);
}

/* A click acts once it's let go over the control it began on, so a press
 * dragged away is no click. */
void MainWindow::on_mouse_release(float lx, float ly)
{
    if (!pressed_) return;
    pressed_ = false;
    ReleaseCapture();
    D2D1_RECT_F rc = {};
    if (interactive_rect_at(press_x_, press_y_, &rc) && point_in_rect(lx, ly, rc)) on_mouse_down(press_x_, press_y_);
}

void MainWindow::on_mouse_down(float lx, float ly)
{
    if (lx < kSidebarWidth) {
        int vis = 0;
        for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
            if (!sections_[i].visible) continue;
            float iy = kNavPadTop + vis * (kNavItemHeight + kNavItemGap);
            if (lx >= kNavPadX && lx <= kSidebarWidth - kNavPadX && ly >= iy && ly <= iy + kNavItemHeight) {
                select_section(sections_[i].id, true);
                return;
            }
            ++vis;
        }
        return;
    }

    for (const auto& hit : toggle_hits_) {
        if (hit.enabled && hit.field && point_in_rect(lx, ly, hit.rect)) {
            *hit.field = !(*hit.field);
            save_pad_prefs(pad_path_, pad_prefs_);
            pad_service_sync(pad_prefs_, opts_.dry_run);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    for (const auto& card : card_hits_) {
        if (card.target_section && point_in_rect(lx, ly, card.rect)) {
            select_section(card.target_section, true);
            return;
        }
    }

    for (const auto& rh : remote_hits_) {
        if (!point_in_rect(lx, ly, rh.rect)) continue;
        switch (rh.kind) {
        case RemoteActionKind::SwitchModeHome:
            if (remote_cfg_.mode != RemoteMode::Home || !remote_cfg_.mode_known) {
                remote_err_msg_.clear();
                sunshine_needs_perm_ = false;
                remote_.apply_mode_async(RemoteMode::Home);
            }
            return;
        case RemoteActionKind::SwitchModeRemote:
            if (remote_cfg_.mode != RemoteMode::Remote || !remote_cfg_.mode_known) {
                remote_err_msg_.clear();
                sunshine_needs_perm_ = false;
                remote_.apply_mode_async(RemoteMode::Remote);
            }
            return;
        case RemoteActionKind::ToggleRowExpand: {
            int idx = std::atoi(rh.value.c_str());
            expanded_remote_row_ = (expanded_remote_row_ == idx) ? -1 : idx;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        case RemoteActionKind::PickChip:
            remote_err_msg_.clear();
            sunshine_needs_perm_ = false;
            remote_.apply_single_async(rh.item, rh.value);
            return;
        case RemoteActionKind::AllowSunshine:
            remote_err_msg_.clear();
            if (remote_.allow_sunshine_service()) {
                sunshine_needs_perm_ = false;
                remote_.apply_mode_async(remote_cfg_.mode);
            }
            return;
        case RemoteActionKind::ReapplyMode:
            remote_err_msg_.clear();
            sunshine_needs_perm_ = false;
            remote_.apply_mode_async(remote_cfg_.mode);
            return;
        case RemoteActionKind::FinishGuide:
            remote_cfg_.ready = true;
            remote_.set_config(remote_cfg_, true);
            for (auto& sec : sections_) {
                if (std::strcmp(sec.id, "remote") == 0) sec.label = L"Remote";
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case RemoteActionKind::OpenSunshineWeb:
            ShellExecuteW(hwnd_, L"open", L"https://localhost:47990", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }

    for (const auto& dh : device_hits_) {
        if (!point_in_rect(lx, ly, dh.rect)) continue;
        if (dh.kind == DeviceActionKind::OpenDynamicLighting) {
            ShellExecuteW(hwnd_, L"open", L"ms-settings:personalization-lighting", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        for (auto& d : dev_list_) {
            if (d.id != dh.dev_id) continue;
            if (d.kind == DevKind::Mouse) {
                MouseSettings want = d.mouse;
                switch (dh.kind) {
                case DeviceActionKind::MouseStage:
                    want.active = std::clamp(dh.value, 0, std::max(0, want.stages - 1));
                    break;
                case DeviceActionKind::MouseDpiDelta:
                    if (want.active >= 0 && want.active < want.stages) {
                        int dpi = ((want.dpi[want.active] + dh.value) / 50) * 50;
                        want.dpi[want.active] = std::clamp(dpi, want.dpi_min, want.dpi_max);
                    }
                    break;
                case DeviceActionKind::MouseRate:
                    want.rate = dh.value;
                    break;
                case DeviceActionKind::MouseLift:
                    want.lift_mm = dh.value;
                    break;
                case DeviceActionKind::MouseLight:
                    want.light = static_cast<LightEffect>(dh.value);
                    if ((want.light == LightEffect::Solid || want.light == LightEffect::Breathing) &&
                        want.rgb[0] == 0 && want.rgb[1] == 0 && want.rgb[2] == 0) {
                        want.rgb[0] = 0xff;
                    }
                    break;
                case DeviceActionKind::MouseColour:
                    if (want.light != LightEffect::Solid && want.light != LightEffect::Breathing) {
                        want.light = LightEffect::Solid;
                    }
                    want.rgb[0] = static_cast<uint8_t>((dh.value >> 16) & 0xff);
                    want.rgb[1] = static_cast<uint8_t>((dh.value >> 8) & 0xff);
                    want.rgb[2] = static_cast<uint8_t>(dh.value & 0xff);
                    break;
                case DeviceActionKind::MouseBrightness:
                    want.brightness = std::clamp(dh.value, 0, 100);
                    break;
                default:
                    break;
                }
                devices_.apply_mouse_async(d.id, want);
            } else if (d.kind == DevKind::Keyboard) {
                KeyboardSettings want = d.keyboard;
                switch (dh.kind) {
                case DeviceActionKind::KeyBrightness:
                    want.brightness = std::clamp(dh.value, 0, 100);
                    break;
                case DeviceActionKind::KeyLight:
                    want.light = static_cast<KeyLight>(dh.value);
                    if (key_light_coloured(want.light) &&
                        want.rgb[0] == 0 && want.rgb[1] == 0 && want.rgb[2] == 0) {
                        want.rgb[0] = 0xff;
                    }
                    break;
                case DeviceActionKind::KeyColour:
                    if (!key_light_coloured(want.light)) want.light = KeyLight::Solid;
                    want.rgb[0] = static_cast<uint8_t>((dh.value >> 16) & 0xff);
                    want.rgb[1] = static_cast<uint8_t>((dh.value >> 8) & 0xff);
                    want.rgb[2] = static_cast<uint8_t>(dh.value & 0xff);
                    break;
                case DeviceActionKind::KeyStartup:
                    want.startup = (dh.value != 0);
                    break;
                case DeviceActionKind::KeyShutdown:
                    want.shutdown = (dh.value != 0);
                    break;
                case DeviceActionKind::KeyDim:
                    want.dim_s = dh.value;
                    break;
                case DeviceActionKind::KeyOff:
                    want.off_s = dh.value;
                    break;
                default:
                    break;
                }
                // Live keyboard lighting effects also save to pad.ini for the helper.
                bool is_light_pick = (dh.kind == DeviceActionKind::KeyLight || dh.kind == DeviceActionKind::KeyColour);
                if (is_light_pick) {
                    static const char* const kLiveNames[4] = { "off", "solid", "breathing", "cycle" };
                    if (key_light_live(want.light)) {
                        pad_prefs_.key_light = kLiveNames[static_cast<int>(want.light)];
                        std::memcpy(pad_prefs_.key_rgb, want.rgb, 3);
                    } else {
                        pad_prefs_.key_light = "own";
                    }
                    save_pad_prefs(pad_path_, pad_prefs_);
                    pad_service_sync(pad_prefs_, opts_.dry_run);
                }
                if (is_light_pick) devices_.set_live_key_light(want.light, want.rgb);
                devices_.apply_keyboard_async(d.id, want);
            }
            return;
        }
    }

    for (const auto& ph : pad_hits_) {
        if (!point_in_rect(lx, ly, ph.rect)) continue;
        switch (ph.kind) {
        case PadActionKind::SelectTab:
            pad_memory_[ph.pad_path].tab = std::clamp(ph.value, 0, 2);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::ResetChecklist:
            pad_memory_[ph.pad_path].seen = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::TestVibration:
            pad_memory_[ph.pad_path].shake_start_ms = GetTickCount64();
            start_anim_timer();
            pads_.rumble_async(ph.pad_path, kPadRumbleStrength, kPadRumbleStrength, kPadRumbleMs);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::FindController:
            pads_.toggle_find_controller(ph.pad_path);
            return;
        case PadActionKind::PlayVibration: {
            const auto& mem = pad_memory_[ph.pad_path];
            pads_.rumble_async(ph.pad_path, mem.strong, mem.weak, mem.length_ms);
            return;
        }
        case PadActionKind::StrongMotor:
            pad_memory_[ph.pad_path].strong = std::clamp(ph.value, 0, 100);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::WeakMotor:
            pad_memory_[ph.pad_path].weak = std::clamp(ph.value, 0, 100);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::LengthMs:
            pad_memory_[ph.pad_path].length_ms = std::clamp(ph.value, 100, 2000);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::EditNickname:
            if (ph.value == 0) {
                editing_pad_path_ = ph.pad_path;
                editing_nickname_.clear();
                for (const auto& p : pad_list_) {
                    if (p.path == ph.pad_path) {
                        editing_nickname_ = p.nickname;
                        break;
                    }
                }
            } else {
                if (pads_.set_nickname(pad_prefs_, ph.pad_path, editing_nickname_)) {
                    save_pad_prefs(pad_path_, pad_prefs_);
                    pad_service_sync(pad_prefs_, opts_.dry_run);
                    pad_list_ = pads_.list();
                }
                editing_pad_path_.clear();
                editing_nickname_.clear();
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::LowLevelDelta:
            pad_prefs_.low_level = std::clamp(pad_prefs_.low_level + ph.value, 5, 50);
            save_pad_prefs(pad_path_, pad_prefs_);
            pad_service_sync(pad_prefs_, opts_.dry_run);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::IdleMinutesChip:
            pad_prefs_.idle_minutes = ph.value;
            save_pad_prefs(pad_path_, pad_prefs_);
            pad_service_sync(pad_prefs_, opts_.dry_run);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::ForgetNickname:
            pad_prefs_.nicknames.erase(ph.pad_path);
            save_pad_prefs(pad_path_, pad_prefs_);
            pad_service_sync(pad_prefs_, opts_.dry_run);
            pads_.apply_nicknames(pad_prefs_);
            pad_list_ = pads_.list();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case PadActionKind::OpenSettings:
            select_section("settings", true);
            return;
        case PadActionKind::OpenBluetoothSettings:
            ShellExecuteW(hwnd_, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }
}

void MainWindow::on_mouse_wheel(int delta)
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float lh = static_cast<float>(rc.bottom - rc.top) / dpi_scale_;
    float max_scroll = std::max(0.0f, content_height_ - lh);
    scroll_target_y_ = std::clamp(scroll_target_y_ - (static_cast<float>(delta) / 120.0f) * 72.0f, 0.0f, max_scroll);
    start_anim_timer();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::on_key_down(WPARAM vk)
{
    if (!editing_pad_path_.empty()) {
        if (vk == VK_ESCAPE) {
            editing_pad_path_.clear();
            editing_nickname_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (vk == VK_RETURN) {
            if (pads_.set_nickname(pad_prefs_, editing_pad_path_, editing_nickname_)) {
                save_pad_prefs(pad_path_, pad_prefs_);
                pad_service_sync(pad_prefs_, opts_.dry_run);
                pad_list_ = pads_.list();
            }
            editing_pad_path_.clear();
            editing_nickname_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (vk == VK_BACK) {
            if (!editing_nickname_.empty()) {
                while (!editing_nickname_.empty() &&
                       (static_cast<unsigned char>(editing_nickname_.back()) & 0xC0) == 0x80) {
                    editing_nickname_.pop_back();
                }
                if (!editing_nickname_.empty()) editing_nickname_.pop_back();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        return;
    }

    if (vk == VK_PRIOR || vk == VK_NEXT) {
        on_mouse_wheel(vk == VK_PRIOR ? 360 : -360);
        return;
    }

    if (vk == VK_UP || vk == VK_DOWN) {
        int dir = (vk == VK_DOWN) ? 1 : -1;
        int n = static_cast<int>(sections_.size());
        for (int step = 1; step < n; ++step) {
            int next = (current_sec_ + dir * step + n) % n;
            if (sections_[next].visible) {
                select_section(sections_[next].id, true);
                break;
            }
        }
    }
}

void MainWindow::on_char(wchar_t ch)
{
    if (editing_pad_path_.empty()) return;
    if (ch < 32 || ch == 127) return;
    if (editing_nickname_.size() >= kPadNicknameLength * 2) return;
    wchar_t ws[2] = { ch, L'\0' };
    char utf8[8] = {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, 1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (n > 0) {
        editing_nickname_.append(utf8, static_cast<size_t>(n));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handle_msg(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handle_msg(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        on_paint();
        return 0;
    case WM_TIMER:
        if (wp == kAnimTimerId) {
            on_timer();
        } else if (wp == kDevicesSettleTimerId) {
            KillTimer(hwnd_, kDevicesSettleTimerId);
            devices_.request_refresh();
        }
        return 0;
    case WM_MOUSEMOVE:
        on_mouse_move(static_cast<float>(GET_X_LPARAM(lp)) / dpi_scale_,
                      static_cast<float>(GET_Y_LPARAM(lp)) / dpi_scale_);
        return 0;
    case WM_MOUSELEAVE:
        tracking_mouse_ = false;
        mouse_in_ = false;
        if (hover_sec_ != -1 || hover_on_) {
            hover_sec_ = -1;
            hover_on_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt = {};
            if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt)) {
                float lx = static_cast<float>(pt.x) / dpi_scale_;
                float ly = static_cast<float>(pt.y) / dpi_scale_;
                SetCursor(LoadCursorW(nullptr, point_over_interactive(lx, ly) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        on_mouse_press(static_cast<float>(GET_X_LPARAM(lp)) / dpi_scale_,
                       static_cast<float>(GET_Y_LPARAM(lp)) / dpi_scale_);
        return 0;
    case WM_LBUTTONUP:
        on_mouse_release(static_cast<float>(GET_X_LPARAM(lp)) / dpi_scale_,
                         static_cast<float>(GET_Y_LPARAM(lp)) / dpi_scale_);
        return 0;
    case WM_CAPTURECHANGED:
        pressed_ = false;
        return 0;
    case WM_DEVICECHANGE:
        /* A device came or went: look again once the burst of changes one
         * plug makes settles, rather than polling. */
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED) {
            SetTimer(hwnd_, kDevicesSettleTimerId, 500, nullptr);
        }
        return TRUE;
    case WM_ACTIVATE:
        /* Back from Settings, say: what Windows paints may have changed. */
        if (LOWORD(wp) != WA_INACTIVE) InvalidateRect(hwnd_, nullptr, FALSE);
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_MOUSEWHEEL:
        on_mouse_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_KEYDOWN:
        on_key_down(wp);
        return 0;
    case WM_CHAR:
        on_char(static_cast<wchar_t>(wp));
        return 0;
    case WM_DPICHANGED: {
        UINT new_dpi = HIWORD(wp);
        dpi_scale_ = static_cast<float>(new_dpi) / 96.0f;
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (opts_.color_scheme.empty()) {
            bool now_dark = system_prefers_dark();
            if (now_dark != dark_) {
                dark_ = now_dark;
                apply_titlebar_theme();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cds && cds->dwData == kCopyDataSection && cds->lpData && cds->cbData > 0) {
            std::string sec(static_cast<const char*>(cds->lpData), cds->cbData);
            if (!sec.empty() && sec.back() == '\0') sec.pop_back();
            if (!sec.empty()) select_section(sec, true);
            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
            return TRUE;
        }
        return FALSE;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = static_cast<LONG>(std::lround(640.0f * dpi_scale_));
        mmi->ptMinTrackSize.y = static_cast<LONG>(std::lround(480.0f * dpi_scale_));
        return 0;
    }
    case WM_SIZE:
        clamp_scroll();
        update_sampling_state();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SHOWWINDOW:
        update_sampling_state();
        return 0;
    case kMsgHwSample: {
        hw_sample_ = hw_.latest_sample();
        if (hw_sample_.cpu >= 0.0) cpu_graph_.push(hw_sample_.cpu);
        if (hw_sample_.gpu >= 0.0) gpu_graph_.push(hw_sample_.gpu);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case kMsgRemoteLive: {
        remote_cfg_ = remote_.config();
        remote_live_ = remote_.live();
        for (auto& sec : sections_) {
            if (std::strcmp(sec.id, "remote") == 0) {
                sec.label = remote_cfg_.ready ? L"Remote" : L"Set up remote";
            }
        }
        bool want_rem_colors = (remote_cfg_.ready && remote_cfg_.mode_known && remote_cfg_.mode == RemoteMode::Remote);
        float new_target = want_rem_colors ? 1.0f : 0.0f;
        if (std::fabs(new_target - mode_target_) > 0.001f) {
            mode_target_ = new_target;
            start_anim_timer();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case kMsgRemoteStepDone: {
        auto* res = reinterpret_cast<RemoteStepResult*>(lp);
        if (res) {
            if (!res->ok) {
                remote_err_msg_ = res->message;
                if (res->needs_permission) sunshine_needs_perm_ = true;
            }
            delete res;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case kMsgDevicesUpdated: {
        dev_list_ = devices_.list();
        for (auto& sec : sections_) {
            if (std::strcmp(sec.id, "headphones") == 0) sec.visible = devices_.any_of(DevKind::Audio);
            else if (std::strcmp(sec.id, "keyboard") == 0) sec.visible = devices_.any_of(DevKind::Keyboard);
            else if (std::strcmp(sec.id, "mouse") == 0) sec.visible = devices_.any_of(DevKind::Mouse);
        }
        if (!opts_.section.empty()) {
            std::string wanted = opts_.section;
            select_section(wanted, false);
            if (sections_[current_sec_].id == wanted) {
                opts_.section.clear();
            }
        }
        if (!sections_[current_sec_].visible) {
            select_section("overview", true);
        }
        int vis = visible_index_of(current_sec_);
        nav_pill_target_y_ = kNavPadTop + vis * (kNavItemHeight + kNavItemGap);
        if (!timer_running_) nav_pill_y_ = nav_pill_target_y_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case kMsgPadsUpdated: {
        pad_list_ = pads_.list();
        ULONGLONG now_ms = GetTickCount64();
        for (const auto& p : pad_list_) {
            auto& mem = pad_memory_[p.path];
            uint32_t pressed = p.state.buttons;
            if (p.state.lt >= kTriggerPullThreshold) pressed |= pb_bit(PB_LT);
            if (p.state.rt >= kTriggerPullThreshold) pressed |= pb_bit(PB_RT);
            mem.seen |= (pressed & p.checklist);
            stick_feed(mem.sticks[0], p.state.lx, p.state.ly, now_ms);
            stick_feed(mem.sticks[1], p.state.rx, p.state.ry, now_ms);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        if (dev_notify_) {
            UnregisterDeviceNotification(dev_notify_);
            dev_notify_ = nullptr;
        }
        hw_.set_wanted(false, false);
        hw_.set_notify_hwnd(nullptr);
        remote_.set_wanted(false);
        remote_.set_notify_hwnd(nullptr);
        devices_.set_wanted(false, false, false);
        devices_.set_notify_hwnd(nullptr);
        pads_.set_wanted(false, false);
        pads_.set_notify_hwnd(nullptr);
        if (timer_running_) {
            KillTimer(hwnd_, kAnimTimerId);
            timer_running_ = false;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

} // namespace lares
