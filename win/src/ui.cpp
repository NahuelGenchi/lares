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
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

namespace lares {

static constexpr float kSidebarWidth = 216.0f;
static constexpr float kNavPadX = 18.0f;
static constexpr float kNavPadTop = 18.0f;
static constexpr float kNavItemHeight = 45.0f;
static constexpr float kNavItemGap = 4.0f;
static constexpr float kContentMaxW = 620.0f;
static constexpr float kContentLead = 9.0f;
static constexpr UINT_PTR kAnimTimerId = 1;

static D2D1_COLOR_F to_d2d(const Rgba& c, float alpha_mul = 1.0f)
{
    return D2D1::ColorF(c.r, c.g, c.b, c.a * alpha_mul);
}

static bool point_in_rect(float x, float y, const D2D1_RECT_F& r)
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

MainWindow::MainWindow(HINSTANCE hinst, const CliOptions& opts)
    : hinst_(hinst), opts_(opts), hw_(opts.dry_run), remote_(opts.dry_run, opts.dry_stream)
{
    if (opts_.color_scheme == "dark") dark_ = true;
    else if (opts_.color_scheme == "light") dark_ = false;
    else dark_ = system_prefers_dark();

    pad_path_ = pad_ini_path(opts_.dry_run);
    load_pad_prefs(pad_path_, pad_prefs_);
    remote_cfg_ = remote_.config();
    remote_mode_ = (remote_cfg_.ready && remote_cfg_.mode_known && remote_cfg_.mode == RemoteMode::Remote);
    mode_t_ = remote_mode_ ? 1.0f : 0.0f;
    mode_target_ = mode_t_;

    sections_ = {
        { "overview",   L"Overview",      ART_OVERVIEW,   true  },
        { "hardware",   L"Hardware",      ART_CHIP,       true  },
        { "controller", L"Controller",    ART_GAMEPAD,    false },
        { "headphones", L"Headphones",    ART_HEADPHONES, false },
        { "keyboard",   L"Keyboard",      ART_KEYBOARD,   false },
        { "mouse",      L"Mouse",         ART_MOUSE,      false },
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
    remote_.request_refresh();

    apply_titlebar_theme();
    if (!opts_.section.empty()) {
        select_section(opts_.section, false);
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
        return;
    }
    const char* id = sections_[current_sec_].id;
    bool is_ov = (std::strcmp(id, "overview") == 0);
    bool is_hw = (std::strcmp(id, "hardware") == 0);
    bool is_rem = (std::strcmp(id, "remote") == 0);
    hw_.set_wanted(is_ov || is_hw, is_hw);
    remote_.set_wanted(is_ov || is_rem);
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
    if (pill_done && mode_done && timer_running_ && hwnd_) {
        nav_pill_y_ = nav_pill_target_y_;
        nav_pill_vel_ = 0.0f;
        mode_t_ = mode_target_;
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

    // 520 ms mode color transition.
    if (std::fabs(mode_t_ - mode_target_) >= 0.002f) {
        float step = dt / 0.52f;
        if (mode_t_ < mode_target_) mode_t_ = std::min(mode_target_, mode_t_ + step);
        else mode_t_ = std::max(mode_target_, mode_t_ - step);
        apply_titlebar_theme();
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
                           const D2D1_RECT_F& rect, const Rgba& color)
{
    if (!text || !fmt || !brush_) return;
    IDWriteTextLayout* layout = nullptr;
    UINT32 len = static_cast<UINT32>(wcslen(text));
    float max_w = std::max(1.0f, rect.right - rect.left);
    float max_h = std::max(1.0f, rect.bottom - rect.top);
    if (SUCCEEDED(fonts_.factory()->CreateTextLayout(text, len, fmt, max_w, max_h, &layout)) && layout) {
        if (fonts_.no_ligatures()) {
            DWRITE_TEXT_RANGE range = { 0, len };
            layout->SetTypography(fonts_.no_ligatures(), range);
        }
        brush_->SetColor(to_d2d(color));
        rt->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        layout->Release();
    }
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
    float pill_w = kSidebarWidth - kNavPadX * 2.0f;

    // Hover pill
    if (hover_sec_ >= 0 && hover_sec_ != current_sec_ && sections_[hover_sec_].visible) {
        int hv = visible_index_of(hover_sec_);
        float hy = kNavPadTop + hv * (kNavItemHeight + kNavItemGap);
        brush_->SetColor(to_d2d(pal.hover));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(pill_x, hy, pill_x + pill_w, hy + kNavItemHeight), 12.0f, 12.0f),
            brush_);
    }

    // Active spring highlight pill
    brush_->SetColor(to_d2d(pal.card));
    D2D1_ROUNDED_RECT active_pill = D2D1::RoundedRect(
        D2D1::RectF(pill_x, nav_pill_y_, pill_x + pill_w, nav_pill_y_ + kNavItemHeight), 12.0f, 12.0f);
    rt->FillRoundedRectangle(active_pill, brush_);
    brush_->SetColor(to_d2d(pal.line));
    rt->DrawRoundedRectangle(active_pill, brush_, 1.0f);

    int vis = 0;
    for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
        if (!sections_[i].visible) continue;
        float iy = kNavPadTop + vis * (kNavItemHeight + kNavItemGap);
        bool selected = (i == current_sec_);

        Rgba icon_solid = selected ? pal.accent : pal.muted;
        Rgba icon_faint = icon_solid;
        icon_faint.a *= 0.35f;

        draw_pixel_art(rt, sections_[i].icon, pill_x + 14.0f, iy + 13.0f, 2.0f, icon_solid, icon_faint);

        D2D1_RECT_F tr = D2D1::RectF(pill_x + 44.0f, iy + 11.0f, pill_x + pill_w - 10.0f, iy + kNavItemHeight - 4.0f);
        draw_text(rt, sections_[i].label, fonts_.body(), tr, selected ? pal.text : pal.muted);
        ++vis;
    }
}

void MainWindow::draw_toggle_row(ID2D1RenderTarget* rt, const Palette& pal,
                                 float x, float& y, float w,
                                 const wchar_t* title, const wchar_t* sub, const wchar_t* saving,
                                 bool* field, bool enabled)
{
    float alpha = enabled ? 1.0f : 0.45f;
    float sw_w = 50.0f;
    float sw_h = 28.0f;
    float text_w = w - sw_w - 20.0f;

    float h_title = measure_text_height(title, fonts_.body(), text_w);
    float h_sub = sub ? measure_text_height(sub, fonts_.caption(), text_w) : 0.0f;
    float h_sav = saving ? measure_text_height(saving, fonts_.caption(), text_w) : 0.0f;

    float total_h = h_title + (h_sub > 0 ? 4.0f + h_sub : 0.0f) + (h_sav > 0 ? 4.0f + h_sav : 0.0f);
    total_h = std::max(total_h, sw_h);

    Rgba c_title = pal.text; c_title.a *= alpha;
    Rgba c_muted = pal.muted; c_muted.a *= alpha;

    float cy = y;
    draw_text(rt, title, fonts_.body(), D2D1::RectF(x, cy, x + text_w, cy + h_title + 4.0f), c_title);
    cy += h_title;
    if (sub && h_sub > 0) {
        cy += 4.0f;
        draw_text(rt, sub, fonts_.caption(), D2D1::RectF(x, cy, x + text_w, cy + h_sub + 4.0f), c_muted);
        cy += h_sub;
    }
    if (saving && h_sav > 0) {
        cy += 4.0f;
        Rgba c_sav = pal.muted; c_sav.a *= (0.75f * alpha);
        draw_text(rt, saving, fonts_.caption(), D2D1::RectF(x, cy, x + text_w, cy + h_sav + 4.0f), c_sav);
    }

    float sw_x = x + w - sw_w;
    float sw_y = y + (total_h - sw_h) * 0.5f;
    D2D1_RECT_F sw_rect = D2D1::RectF(sw_x, sw_y, sw_x + sw_w, sw_y + sw_h);

    bool on = field && *field;
    brush_->SetColor(to_d2d(on ? pal.accent : pal.track, alpha));
    rt->FillRoundedRectangle(D2D1::RoundedRect(sw_rect, sw_h * 0.5f, sw_h * 0.5f), brush_);

    float knob_r = 10.0f;
    float knob_cx = on ? (sw_rect.right - 14.0f) : (sw_rect.left + 14.0f);
    float knob_cy = sw_rect.top + sw_h * 0.5f;
    brush_->SetColor(to_d2d(on ? pal.on_accent : pal.muted, alpha));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_cx, knob_cy), knob_r, knob_r), brush_);

    toggle_hits_.push_back({ sw_rect, field, enabled });
    y += total_h + 18.0f;
}

void MainWindow::draw_settings_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    draw_text(rt, L"Settings", fonts_.title(), D2D1::RectF(x, y, x + w, y + 48.0f), pal.text);
    y += 54.0f;

    // Group 1: Background
    draw_text(rt, L"Background", fonts_.caption(), D2D1::RectF(x + 4.0f, y, x + w, y + 24.0f), pal.muted);
    y += 26.0f;

    float card1_top = y;
    float inner_x = x + 18.0f;
    float inner_w = w - 36.0f;
    float probe_y = card1_top + 18.0f;

    // Pre-calculate card1 height cleanly
    float h_t = measure_text_height(L"Run in the background", fonts_.body(), inner_w - 70.0f);
    float h_s = measure_text_height(L"Keeps controller features and keyboard lighting on with Lares closed.", fonts_.caption(), inner_w - 70.0f);
    float h_sav = measure_text_height(L"Off saves ~2 MB RAM", fonts_.caption(), inner_w - 70.0f);
    float card1_h = 36.0f + h_t + 4.0f + h_s + 4.0f + h_sav;

    D2D1_ROUNDED_RECT card1 = D2D1::RoundedRect(D2D1::RectF(x, card1_top, x + w, card1_top + card1_h), 14.0f, 14.0f);
    brush_->SetColor(to_d2d(pal.card));
    rt->FillRoundedRectangle(card1, brush_);
    brush_->SetColor(to_d2d(pal.line));
    rt->DrawRoundedRectangle(card1, brush_, 1.0f);

    draw_toggle_row(rt, pal, inner_x, probe_y, inner_w,
                    L"Run in the background",
                    L"Keeps controller features and keyboard lighting on with Lares closed.",
                    L"Off saves ~2 MB RAM",
                    &pad_prefs_.background, true);

    y = card1_top + card1_h + 27.0f;

    // Group 2: System tray (Windows equivalent of GNOME Top bar)
    draw_text(rt, L"System tray", fonts_.caption(), D2D1::RectF(x + 4.0f, y, x + w, y + 24.0f), pal.muted);
    y += 26.0f;

    float card2_top = y;
    bool tray_enabled = pad_prefs_.background;
    float r1_h = std::max(28.0f, measure_text_height(L"Show the icon", fonts_.body(), inner_w - 70.0f) + 4.0f +
                                 measure_text_height(L"With battery and quick actions.", fonts_.caption(), inner_w - 70.0f));
    float r2_h = std::max(28.0f, measure_text_height(L"Battery level beside the icon", fonts_.body(), inner_w - 70.0f) + 4.0f +
                                 measure_text_height(L"Shows the lowest if several are connected.", fonts_.caption(), inner_w - 70.0f));
    float r3_h = std::max(28.0f, measure_text_height(L"Hide the icon without a controller", fonts_.body(), inner_w - 70.0f));
    float card2_h = 18.0f + 34.0f + r1_h + 18.0f + r2_h + 18.0f + r3_h + 18.0f;

    D2D1_ROUNDED_RECT card2 = D2D1::RoundedRect(D2D1::RectF(x, card2_top, x + w, card2_top + card2_h), 14.0f, 14.0f);
    brush_->SetColor(to_d2d(pal.card));
    rt->FillRoundedRectangle(card2, brush_);
    brush_->SetColor(to_d2d(pal.line));
    rt->DrawRoundedRectangle(card2, brush_, 1.0f);

    float cy = card2_top + 18.0f;
    Rgba sub_col = pal.muted;
    if (!tray_enabled) sub_col.a *= 0.45f;
    Rgba sub_faint = sub_col; sub_faint.a *= 0.35f;
    draw_pixel_art(rt, ART_GAMEPAD, inner_x, cy + 2.0f, 2.0f, sub_col, sub_faint);
    draw_text(rt, L"Xbox controller", fonts_.body(), D2D1::RectF(inner_x + 30.0f, cy - 2.0f, inner_x + inner_w, cy + 26.0f), sub_col);
    cy += 34.0f;

    draw_toggle_row(rt, pal, inner_x, cy, inner_w,
                    L"Show the icon",
                    L"With battery and quick actions.",
                    nullptr,
                    &pad_prefs_.topbar, tray_enabled);
    draw_toggle_row(rt, pal, inner_x, cy, inner_w,
                    L"Battery level beside the icon",
                    L"Shows the lowest if several are connected.",
                    nullptr,
                    &pad_prefs_.topbar_level, tray_enabled);
    draw_toggle_row(rt, pal, inner_x, cy, inner_w,
                    L"Hide the icon without a controller",
                    nullptr,
                    nullptr,
                    &pad_prefs_.topbar_hide, tray_enabled);

    content_height_ = card2_top + card2_h + 36.0f;
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
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 14.0f, 14.0f);
    brush->SetColor(to_d2d(pal.card));
    rt->FillRoundedRectangle(card, brush);
    brush->SetColor(to_d2d(pal.line));
    rt->DrawRoundedRectangle(card, brush, 1.0f);
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
    const HwInfo& info = hw_.info();
    const HwSample& s = hw_sample_;

    bool has_gpu = !info.gpu_name.empty();
    int cols = has_gpu ? 3 : 2;
    float gap = 18.0f;
    float tile_w = (w - gap * (cols - 1)) / static_cast<float>(cols);
    float tile_h = 132.0f;

    auto draw_tile = [&](float tx, float ty, float tw, const char* const* art, const wchar_t* heading,
                         const std::wstring& val, const std::wstring& sub,
                         int meter_blocks, double fraction, bool hot, const char* target_sec) {
        draw_info_card(rt, brush_, this, pal, tx, ty, tw, tile_h);
        card_hits_.push_back({ D2D1::RectF(tx, ty, tx + tw, ty + tile_h), target_sec });

        float ix = tx + 18.0f;
        float iy = ty + 16.0f;
        Rgba ic = pal.muted;
        Rgba ic_f = ic; ic_f.a *= 0.35f;
        draw_pixel_art(rt, art, ix, iy + 2.0f, 2.0f, ic, ic_f);
        draw_text(rt, heading, fonts_.caption(), D2D1::RectF(ix + 26.0f, iy, tx + tw - 14.0f, iy + 22.0f), pal.muted);

        draw_text(rt, val.c_str(), fonts_.big(), D2D1::RectF(ix, iy + 26.0f, tx + tw - 14.0f, iy + 62.0f), pal.text);
        draw_text(rt, sub.c_str(), fonts_.caption(), D2D1::RectF(ix, iy + 64.0f, tx + tw - 14.0f, iy + 86.0f), pal.muted);

        if (meter_blocks > 0) {
            draw_meter(rt, pal, ix, ty + tile_h - 20.0f, meter_blocks, 2.0f, fraction, hot);
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
        float mode_h = 104.0f;
        draw_info_card(rt, brush_, this, pal, x, y, w, mode_h);
        card_hits_.push_back({ D2D1::RectF(x, y, x + w, y + mode_h), "remote" });
        float ix = x + 18.0f, iy = y + 16.0f;
        Rgba ic = pal.muted, ic_f = ic; ic_f.a *= 0.35f;
        draw_pixel_art(rt, ART_LAPTOP, ix, iy + 2.0f, 2.0f, ic, ic_f);
        draw_text(rt, L"Mode", fonts_.caption(), D2D1::RectF(ix + 26.0f, iy, x + w - 14.0f, iy + 22.0f), pal.muted);
        draw_text(rt, mval.c_str(), fonts_.big(), D2D1::RectF(ix, iy + 26.0f, x + w - 14.0f, iy + 62.0f), pal.text);
        if (!msub.empty()) {
            draw_text(rt, msub.c_str(), fonts_.caption(), D2D1::RectF(ix, iy + 64.0f, x + w - 14.0f, iy + 86.0f), pal.muted);
        }
        y += mode_h + gap;
    }

    content_height_ = y + 18.0f;
}

void MainWindow::draw_hardware_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
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
        float tw = 115.0f;
        draw_text(rt, threads_buf, fonts_.caption(), D2D1::RectF(ix + iw - tw, iy + 3.0f, ix + iw, iy + 26.0f), pal.muted);

        wchar_t val_buf[32];
        if (s.cpu < 0.0) std::swprintf(val_buf, std::size(val_buf), L"...");
        else std::swprintf(val_buf, std::size(val_buf), L"%d%%", static_cast<int>(std::lround(s.cpu * 100.0)));
        draw_text(rt, val_buf, fonts_.big(), D2D1::RectF(ix, iy + 34.0f, ix + 180.0f, iy + 70.0f), pal.text);

        if (!std::isnan(s.cpu_temp)) {
            wchar_t t_buf[32];
            std::swprintf(t_buf, std::size(t_buf), L"%d °C", static_cast<int>(std::lround(s.cpu_temp)));
            draw_text(rt, t_buf, fonts_.big(), D2D1::RectF(ix + iw - 110.0f, iy + 34.0f, ix + iw, iy + 70.0f), pal.muted);
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
            draw_text(rt, t_buf, fonts_.big(), D2D1::RectF(ix + iw - 110.0f, iy + 34.0f, ix + iw, iy + 70.0f), pal.muted);
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
            draw_text(rt, dkind.c_str(), fonts_.caption(), D2D1::RectF(ix + iw - 60.0f, iy + 3.0f, ix + iw, iy + 26.0f), pal.muted);
        }

        uint64_t dfree = s.disk_total > s.disk_used ? (s.disk_total - s.disk_used) : 0;
        std::wstring dval = s.disk_total ? utf8_to_wide(format_bytes(dfree)) : L"...";
        draw_text(rt, dval.c_str(), fonts_.big(), D2D1::RectF(ix, iy + 34.0f, ix + 240.0f, iy + 70.0f), pal.text);

        if (!std::isnan(s.disk_temp)) {
            wchar_t t_buf[32];
            std::swprintf(t_buf, std::size(t_buf), L"%d °C", static_cast<int>(std::lround(s.disk_temp)));
            draw_text(rt, t_buf, fonts_.big(), D2D1::RectF(ix + iw - 110.0f, iy + 34.0f, ix + iw, iy + 70.0f), pal.muted);
        }

        double dfrac = s.disk_total ? static_cast<double>(s.disk_used) / static_cast<double>(s.disk_total) : -1.0;
        draw_meter(rt, pal, ix, iy + 74.0f, 34, 2.0f, dfrac, dfrac > 0.9);

        if (s.disk_total > 0) {
            std::wstring dfact = L"Free of " + utf8_to_wide(format_bytes(s.disk_total));
            draw_text(rt, dfact.c_str(), fonts_.caption(), D2D1::RectF(ix, iy + 88.0f, ix + iw, iy + 110.0f), pal.muted);
        }
        y += card_h + gap;
    }

    content_height_ = y + 18.0f;
}

void MainWindow::draw_remote_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
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

        content_height_ = y + addr_h + 36.0f;
        return;
    }

    // 2. Home | Remote big segmented mode switch (matches modeswitch in data/style.css)
    float sw_h = 56.0f;
    D2D1_RECT_F sw_track = D2D1::RectF(x, y, x + w, y + sw_h);
    brush_->SetColor(to_d2d(pal.track));
    rt->FillRoundedRectangle(D2D1::RoundedRect(sw_track, 18.0f, 18.0f), brush_);

    float half_w = (w - 8.0f) * 0.5f;
    float pill_x = x + 4.0f + ease_out_cubic_css(mode_t_) * half_w;
    D2D1_ROUNDED_RECT sw_pill = D2D1::RoundedRect(
        D2D1::RectF(pill_x, y + 4.0f, pill_x + half_w, y + sw_h - 4.0f), 14.0f, 14.0f);
    brush_->SetColor(to_d2d(pal.card));
    rt->FillRoundedRectangle(sw_pill, brush_);
    brush_->SetColor(to_d2d(pal.line));
    rt->DrawRoundedRectangle(sw_pill, brush_, 1.0f);

    bool is_rem = (remote_cfg_.mode == RemoteMode::Remote);
    // Home side
    {
        float hx = x + 4.0f + half_w * 0.5f - 46.0f;
        Rgba hc = !is_rem ? pal.accent : pal.muted;
        Rgba hcf = hc; hcf.a *= 0.35f;
        draw_pixel_art(rt, ART_HOME, hx, y + 19.0f, 2.0f, hc, hcf);
        draw_text(rt, L"Home", fonts_.body(), D2D1::RectF(hx + 30.0f, y + 16.0f, x + 4.0f + half_w, y + 44.0f), !is_rem ? pal.text : pal.muted);
        remote_hits_.push_back({ D2D1::RectF(x, y, x + half_w + 4.0f, y + sw_h), RemoteActionKind::SwitchModeHome });
    }
    // Remote side
    {
        float rx = x + 4.0f + half_w + half_w * 0.5f - 56.0f;
        Rgba rc = is_rem ? pal.accent : pal.muted;
        Rgba rcf = rc; rcf.a *= 0.35f;
        draw_pixel_art(rt, ART_LAPTOP, rx, y + 19.0f, 2.0f, rc, rcf);
        draw_text(rt, L"Remote", fonts_.body(), D2D1::RectF(rx + 30.0f, y + 16.0f, x + w, y + 44.0f), is_rem ? pal.text : pal.muted);
        remote_hits_.push_back({ D2D1::RectF(x + half_w + 4.0f, y, x + w, y + sw_h), RemoteActionKind::SwitchModeRemote });
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
            draw_text(rt, mstatus, fonts_.body(), D2D1::RectF(ix + iw - 190.0f, ry, ix + iw, ry + 26.0f),
                      (sun_on && remote_live_.paired) ? pal.text : pal.muted);
        }

        // Row 2: IP address
        ry += 36.0f;
        brush_->SetColor(to_d2d(pal.line));
        rt->FillRectangle(D2D1::RectF(ix, ry - 6.0f, ix + iw, ry - 5.0f), brush_);
        draw_text(rt, L"IP address", fonts_.body(), D2D1::RectF(ix, ry, ix + 180.0f, ry + 26.0f), pal.text);
        std::wstring ip_w = remote_live_.tailscale_ip.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_ip);
        draw_text(rt, ip_w.c_str(), fonts_.body(), D2D1::RectF(ix + iw - 220.0f, ry, ix + iw, ry + 26.0f),
                  remote_live_.tailscale_ip.empty() ? pal.muted : pal.text);

        // Row 3: Host name
        ry += 36.0f;
        brush_->SetColor(to_d2d(pal.line));
        rt->FillRectangle(D2D1::RectF(ix, ry - 6.0f, ix + iw, ry - 5.0f), brush_);
        draw_text(rt, L"Host name", fonts_.body(), D2D1::RectF(ix, ry, ix + 180.0f, ry + 26.0f), pal.text);
        std::wstring host_w = remote_live_.tailscale_host.empty() ? L"Unknown" : utf8_to_wide(remote_live_.tailscale_host);
        draw_text(rt, host_w.c_str(), fonts_.body(), D2D1::RectF(ix + iw - 220.0f, ry, ix + iw, ry + 26.0f),
                  remote_live_.tailscale_host.empty() ? pal.muted : pal.text);

        y += conn_h + gap;
    }

    // 4. Note banner (Permission Allow or Drift Set-it-again)
    bool any_drift = false;
    for (int i = 0; i < kRemoteItemCount; ++i) {
        auto item = static_cast<RemoteItem>(i);
        if (!remote_matches(remote_live_, item, remote_cfg_.value(remote_cfg_.mode, item))) {
            any_drift = true;
            break;
        }
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
        draw_text(rt, n_txt, fonts_.caption(), D2D1::RectF(ix + 28.0f, y + 22.0f, x + w - 148.0f, y + 48.0f), pal.text);

        D2D1_RECT_F btn = D2D1::RectF(x + w - 138.0f, y + 15.0f, x + w - 16.0f, y + 49.0f);
        brush_->SetColor(to_d2d(pal.accent));
        rt->FillRoundedRectangle(D2D1::RoundedRect(btn, 10.0f, 10.0f), brush_);
        const wchar_t* b_lbl = sunshine_needs_perm_ ? L"Allow" : L"Set it again";
        float b_off = sunshine_needs_perm_ ? 36.0f : 14.0f;
        draw_text(rt, b_lbl, fonts_.caption(), D2D1::RectF(btn.left + b_off, btn.top + 8.0f, btn.right, btn.bottom), pal.on_accent);
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
        bool matches = remote_matches(remote_live_, item, want_val);
        std::wstring live_w = !matches ? utf8_to_wide(remote_describe_live(remote_live_, item)) : L"";
        bool expanded = (expanded_remote_row_ == i);

        // Build chip choices when expanded
        struct ChipChoice { std::string val; std::wstring label; };
        std::vector<ChipChoice> choices;
        if (expanded) {
            if (item == RemoteItem::Screen) {
                for (const auto& m : remote_live_.modes) {
                    if (choices.size() >= 8) break;
                    choices.push_back({ m.spec(), utf8_to_wide(m.label()) });
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

        draw_text(rt, desc_w.c_str(), fonts_.body(), D2D1::RectF(ix + iw - 280.0f, y + 17.0f, ix + iw - 26.0f, y + 44.0f), pal.muted);
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

    content_height_ = y + 24.0f;
}

void MainWindow::draw_page(ID2D1RenderTarget* rt, const Palette& pal, float client_w, float client_h)
{
    toggle_hits_.clear();
    card_hits_.clear();
    float col_x = kSidebarWidth + kContentLead + 18.0f;
    float avail_w = std::max(260.0f, client_w - col_x - 36.0f);
    float col_w = std::min(kContentMaxW, avail_w);
    float col_y = 18.0f - scroll_y_;

    D2D1_RECT_F clip = D2D1::RectF(kSidebarWidth, 0.0f, client_w, client_h);
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

    const char* id = sections_[current_sec_].id;
    if (std::strcmp(id, "settings") == 0) draw_settings_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "hardware") == 0) draw_hardware_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "remote") == 0) draw_remote_page(rt, pal, col_x, col_y, col_w);
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
    if (new_hover != hover_sec_) {
        hover_sec_ = new_hover;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
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
                remote_.apply_mode_async(RemoteMode::Home);
            }
            return;
        case RemoteActionKind::SwitchModeRemote:
            if (remote_cfg_.mode != RemoteMode::Remote || !remote_cfg_.mode_known) {
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
            sunshine_needs_perm_ = false;
            remote_.apply_single_async(rh.item, rh.value);
            return;
        case RemoteActionKind::AllowSunshine:
            if (remote_.allow_sunshine_service()) {
                sunshine_needs_perm_ = false;
                remote_.apply_mode_async(remote_cfg_.mode);
            }
            return;
        case RemoteActionKind::ReapplyMode:
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
}

void MainWindow::on_mouse_wheel(int delta)
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float lh = static_cast<float>(rc.bottom - rc.top) / dpi_scale_;
    float max_scroll = std::max(0.0f, content_height_ - lh);
    scroll_y_ = std::clamp(scroll_y_ - static_cast<float>(delta) * 0.4f, 0.0f, max_scroll);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::on_key_down(WPARAM vk)
{
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
        if (wp == kAnimTimerId) on_timer();
        return 0;
    case WM_MOUSEMOVE:
        on_mouse_move(static_cast<float>(GET_X_LPARAM(lp)) / dpi_scale_,
                      static_cast<float>(GET_Y_LPARAM(lp)) / dpi_scale_);
        return 0;
    case WM_LBUTTONDOWN:
        on_mouse_down(static_cast<float>(GET_X_LPARAM(lp)) / dpi_scale_,
                      static_cast<float>(GET_Y_LPARAM(lp)) / dpi_scale_);
        return 0;
    case WM_MOUSEWHEEL:
        on_mouse_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_KEYDOWN:
        on_key_down(wp);
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
        update_sampling_state();
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
    case WM_DESTROY:
        hw_.set_wanted(false, false);
        hw_.set_notify_hwnd(nullptr);
        remote_.set_wanted(false);
        remote_.set_notify_hwnd(nullptr);
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
