// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui.h"
#include "ui_internal.h"
extern "C" {
#include "art.h"
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dbt.h>
#include <dwmapi.h>
#include <ole2.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <windowsx.h>

namespace lares {

namespace {

/* Native OLE IDropTarget so dragging files from Explorer highlights the
 * exact AirDrop device circle under the pointer and sends on drop. */
class ShareDropTarget final : public IDropTarget {
public:
    explicit ShareDropTarget(MainWindow* win) : win_(win) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        has_files_ = data && has_hdrop(data);
        has_url_ = !has_files_ && data && !extract_url(data).empty();
        bool ok = (has_files_ || has_url_) && win_ && win_->is_share_section_active();
        if (effect) *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (ok) win_->on_ole_drag(true, POINT{ pt.x, pt.y });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override
    {
        bool ok = (has_files_ || has_url_) && win_ && win_->is_share_section_active();
        if (effect) *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (ok) win_->on_ole_drag(true, POINT{ pt.x, pt.y });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        has_files_ = false;
        has_url_ = false;
        if (win_) win_->on_ole_drag(false, POINT{ 0, 0 });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        bool ok = (has_files_ || has_url_) && data && win_ && win_->is_share_section_active();
        bool files_mode = has_files_;
        has_files_ = false;
        has_url_ = false;
        if (effect) *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (ok) {
            if (files_mode) {
                std::vector<std::wstring> paths = extract_hdrop(data);
                if (!paths.empty()) win_->on_ole_drop(POINT{ pt.x, pt.y }, paths);
                else win_->on_ole_drag(false, POINT{ 0, 0 });
            } else {
                std::string url = extract_url(data);
                if (!url.empty()) win_->on_ole_drop_url(POINT{ pt.x, pt.y }, url);
                else win_->on_ole_drag(false, POINT{ 0, 0 });
            }
        } else if (win_) {
            win_->on_ole_drag(false, POINT{ 0, 0 });
        }
        return S_OK;
    }

    void detach() { win_ = nullptr; }

private:
    static bool has_hdrop(IDataObject* data)
    {
        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return data->QueryGetData(&fmt) == S_OK;
    }

    static std::string extract_url(IDataObject* data)
    {
        FORMATETC fmt = { CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med = {};
        if (FAILED(data->GetData(&fmt, &med))) return {};
        std::string url;
        if (med.tymed == TYMED_HGLOBAL && med.hGlobal) {
            const wchar_t* ws = static_cast<const wchar_t*>(GlobalLock(med.hGlobal));
            if (ws) {
                size_t len = wcsnlen(ws, 2048);
                int n = WideCharToMultiByte(CP_UTF8, 0, ws, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
                if (n > 0) {
                    url.resize(static_cast<size_t>(n));
                    WideCharToMultiByte(CP_UTF8, 0, ws, static_cast<int>(len), url.data(), n, nullptr, nullptr);
                }
                GlobalUnlock(med.hGlobal);
            }
        }
        ReleaseStgMedium(&med);
        size_t a = 0, b = url.size();
        while (a < b && (url[a] == ' ' || url[a] == '\t' || url[a] == '\r' || url[a] == '\n')) ++a;
        while (b > a && (url[b - 1] == ' ' || url[b - 1] == '\t' || url[b - 1] == '\r' || url[b - 1] == '\n')) --b;
        url = url.substr(a, b - a);
        return share_link_opens(url) ? url : std::string();
    }

    static std::vector<std::wstring> extract_hdrop(IDataObject* data)
    {
        std::vector<std::wstring> out;
        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med = {};
        if (FAILED(data->GetData(&fmt, &med))) return out;
        if (med.tymed == TYMED_HGLOBAL && med.hGlobal) {
            HDROP drop = static_cast<HDROP>(GlobalLock(med.hGlobal));
            if (drop) {
                UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i) {
                    UINT len = DragQueryFileW(drop, i, nullptr, 0);
                    if (len == 0) continue;
                    std::wstring p(len, L'\0');
                    DragQueryFileW(drop, i, p.data(), len + 1);
                    out.push_back(std::move(p));
                }
                GlobalUnlock(med.hGlobal);
            }
        }
        ReleaseStgMedium(&med);
        return out;
    }

    volatile LONG refs_ = 1;
    MainWindow* win_ = nullptr;
    bool has_files_ = false;
    bool has_url_ = false;
};

} // namespace

static constexpr UINT_PTR kAnimTimerId = 1;
static constexpr UINT_PTR kDevicesSettleTimerId = 2;
// GUID_DEVINTERFACE_HID
static constexpr GUID kHidInterfaceGuid = { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

MainWindow::MainWindow(HINSTANCE hinst, const CliOptions& opts)
    : hinst_(hinst), opts_(opts), hw_(opts.dry_run), remote_(opts.dry_run, opts.dry_stream),
      share_(opts.dry_run), devices_(opts.dry_run), pads_(opts.dry_run)
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
    share_.set_original_media(pad_prefs_.share_original_media);
    share_view_ = share_.view();

    sections_ = {
        { "overview",   L"Overview",      ART_OVERVIEW,   true  },
        { "hardware",   L"Hardware",      ART_CHIP,       true  },
        { "controller", L"Controller",    ART_GAMEPAD,    true  },
        { "headphones", L"Headphones",    ART_HEADPHONES, devices_.any_of(DevKind::Audio) },
        { "keyboard",   L"Keyboard",      ART_KEYBOARD,   devices_.any_of(DevKind::Keyboard) },
        { "mouse",      L"Mouse",         ART_MOUSE,      devices_.any_of(DevKind::Mouse) },
        { "share",      L"Share",         ART_SHARE,      opts_.dry_run || share_adapter_present() },
        { "remote",     remote_cfg_.ready ? L"Remote" : L"Set up remote", ART_LAPTOP, true },
        { "settings",   L"Settings",      ART_SLIDERS,    true  },
    };
}

MainWindow::~MainWindow()
{
    release_share_bitmaps();
    release_backbuffer();
    if (share_round_cap_) { share_round_cap_->Release(); share_round_cap_ = nullptr; }
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
    D2D1_STROKE_STYLE_PROPERTIES cap_props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    d2d_factory_->CreateStrokeStyle(cap_props, nullptr, 0, &share_round_cap_);
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

    /* The height and baseline of a line at each size, which the layout
     * goes by as GTK goes by its labels' heights. */
    auto line_of = [&](IDWriteTextFormat* fmt, float* baseline) {
        float h = 0.0f;
        IDWriteTextLayout* layout = nullptr;
        if (SUCCEEDED(fonts_.factory()->CreateTextLayout(L"Ag", 2, fmt, 400.0f, 400.0f, &layout)) && layout) {
            DWRITE_LINE_METRICS lm = {};
            UINT32 n = 0;
            if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &n)) && n >= 1) {
                h = std::ceil(lm.height);
                if (baseline) *baseline = lm.baseline;
            }
            layout->Release();
        }
        return h;
    };
    if (float h = line_of(fonts_.body(), &base_)) lh_ = h;
    if (float h = line_of(fonts_.big(), nullptr)) lhb_ = h;
    if (float h = line_of(fonts_.title(), nullptr)) lht_ = h;

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
    update_scale();

    /* 900 by 720 at the Size setting, as far as the screen's work area
     * allows: a large size can ask for more than a small screen has. */
    int win_w = static_cast<int>(std::lround(900.0f * scale_));
    int win_h = static_cast<int>(std::lround(720.0f * scale_));
    RECT work = {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && work.right > work.left && work.bottom > work.top) {
        win_w = std::min(win_w, static_cast<int>(work.right - work.left));
        win_h = std::min(win_h, static_cast<int>(work.bottom - work.top));
    }

    const wchar_t* title = opts_.dry_run ? L"Lares (dry run)" : L"Lares";
    hwnd_ = CreateWindowExW(
        0, cls_name, title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
        nullptr, nullptr, hinst_, this);
    if (!hwnd_) return false;

    // A plain title bar, as on Linux: no icon and no text in it. The window
    // keeps both, so the taskbar and Alt+Tab still name it.
    WTA_OPTIONS plain = {};
    plain.dwFlags = WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON;
    plain.dwMask = plain.dwFlags;
    SetWindowThemeAttribute(hwnd_, WTA_NONCLIENT, &plain, sizeof(plain));

    dpi = GetDpiForWindow(hwnd_);
    dpi_scale_ = static_cast<float>(dpi) / 96.0f;
    update_scale();
    hw_.set_notify_hwnd(hwnd_);
    remote_.set_notify_hwnd(hwnd_);
    devices_.set_notify_hwnd(hwnd_);
    pads_.set_notify_hwnd(hwnd_);
    share_.set_notify_hwnd(hwnd_);
    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kHidInterfaceGuid;
    dev_notify_ = RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    auto* dt = new ShareDropTarget(this);
    if (SUCCEEDED(RegisterDragDrop(hwnd_, dt))) {
        drop_target_ = dt;
    } else {
        dt->detach();
        dt->Release();
    }
    /* The first look for devices may have ended before there was a window
     * to tell: take what it found, so their sections show whichever
     * section Lares opens on. */
    PostMessageW(hwnd_, kMsgDevicesUpdated, 0, 0);
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
        sync_share_browsing();
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
    sync_share_browsing();
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
            settings_hits_.clear();
            changes_open_ = -1;
            changes_said_.clear();
            crash_looked_ = false;
            crash_said_.clear();
            card_hits_.clear();
            drops_.clear();
            drop_open_.clear();
            drop_items_.clear();
            remote_hits_.clear();
            device_hits_.clear();
            pad_hits_.clear();
            share_hits_.clear();
            share_circles_.clear();
            share_mode_menu_open_ = false;
            share_adapter_open_ = false;
            share_contacts_open_ = false;
            share_ctx_open_ = false;
            share_editing_contact_ = -1;
            share_contact_field_ = -1;
            int vis = visible_index_of(i);
            nav_pill_target_y_ = kNavPadTop + vis * (nav_item_h() + kHalf);
            if (!animate || !nav_pill_init_) {
                nav_pill_y_ = nav_pill_target_y_;
                nav_pill_vel_ = 0.0f;
                nav_pill_init_ = true;
            } else {
                start_anim_timer();
            }
            if (id == "share") {
                /* What's set up may have changed since it was last shown. */
                share_.refresh();
                share_view_ = share_.view();
            }
            update_sampling_state();
            sync_share_tick();
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
    if (measuring_ || !art || !brush_) return;
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

void MainWindow::clamp_scroll()
{
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float lh = static_cast<float>(rc.bottom - rc.top) / scale_;
    float max_scroll = std::max(0.0f, content_height_ - lh);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);
    scroll_target_y_ = std::clamp(scroll_target_y_, 0.0f, max_scroll);
}

/* Settings > Accessibility > Size. The window grows or shrinks with what it
 * shows, as Windows resizes a window when the display scale changes, so the
 * layout stays as it was; a maximised window keeps its size. It stays inside
 * the screen's work area. */
void MainWindow::apply_size(int percent)
{
    percent = std::clamp(percent, kSizeMin, kSizeMax);
    if (percent == pad_prefs_.size) return;
    float before = scale_;
    pad_prefs_.size = percent;
    save_pad_prefs(pad_path_, pad_prefs_);
    update_scale();
    /* What is under the pointer has moved: no hover until it moves too. */
    mouse_in_ = false;
    hover_on_ = false;
    if (hwnd_ && !IsZoomed(hwnd_) && !IsIconic(hwnd_)) {
        RECT wr = {}, cr = {};
        GetWindowRect(hwnd_, &wr);
        GetClientRect(hwnd_, &cr);
        float k = scale_ / before;
        int w = static_cast<int>(std::lround(static_cast<float>(cr.right) * k)) + (wr.right - wr.left) - cr.right;
        int h = static_cast<int>(std::lround(static_cast<float>(cr.bottom) * k)) + (wr.bottom - wr.top) - cr.bottom;
        int x = wr.left, y = wr.top;
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            const RECT& wa = mi.rcWork;
            w = std::min(w, static_cast<int>(wa.right - wa.left));
            h = std::min(h, static_cast<int>(wa.bottom - wa.top));
            x = std::clamp(x, static_cast<int>(wa.left), static_cast<int>(wa.right) - w);
            y = std::clamp(y, static_cast<int>(wa.top), static_cast<int>(wa.bottom) - h);
        }
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    clamp_scroll();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

const char* const* chevron_up()
{
    static const std::vector<const char*> rows = [] {
        std::vector<const char*> r;
        for (int i = 0; ART_CHEVRON[i]; ++i) r.insert(r.begin(), ART_CHEVRON[i]);
        r.push_back(nullptr);
        return r;
    }();
    return rows.data();
}

const MainWindow::Dropdown* MainWindow::open_dropdown() const
{
    if (drop_open_.empty()) return nullptr;
    for (const auto& d : drops_) {
        if (d.key == drop_open_) return &d;
    }
    return nullptr;
}

void MainWindow::close_dropdown()
{
    if (drop_open_.empty()) return;
    drop_open_.clear();
    /* Its choices go now, not at the next paint. */
    drop_items_.clear();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

/* Picking the choice in use only closes the list: nothing is asked again. */
void MainWindow::pick_dropdown(int index)
{
    const Dropdown* d = open_dropdown();
    if (!d || index < 0 || index >= static_cast<int>(d->values.size()) || index == d->checked) {
        close_dropdown();
        return;
    }
    int value = d->values[static_cast<size_t>(index)];
    /* A copy: what it does may draw the page again, and its dropdowns with it. */
    std::function<void(int)> pick = d->pick;
    close_dropdown();
    if (pick) pick(value);
}

bool MainWindow::interactive_rect_at(float lx, float ly, D2D1_RECT_F* out, bool for_cursor) const
{
    auto found = [&](const D2D1_RECT_F& r) {
        if (out) *out = r;
        return true;
    };
    /* With a dropdown's list open only it and its button take the pointer: a
     * press anywhere else closes it. */
    if (!drop_open_.empty()) {
        for (const auto& r : drop_items_) { if (point_in_rect(lx, ly, r)) return found(r); }
        const Dropdown* d = open_dropdown();
        if (d && point_in_rect(lx, ly, d->button)) return found(d->button);
        return false;
    }
    if (lx < kSidebarWidth) {
        if (lx >= kNavPadX && lx <= kSidebarWidth - kNavPadX) {
            int vis = 0;
            for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
                if (!sections_[i].visible) continue;
                float iy = kNavPadTop + vis * (nav_item_h() + kHalf);
                if (ly >= iy && ly <= iy + nav_item_h()) {
                    return found(D2D1::RectF(kNavPadX, iy, kSidebarWidth - kNavPadX, iy + nav_item_h()));
                }
                ++vis;
            }
        }
        return false;
    }
    // In the order on_mouse_down tries them.
    for (const auto& h : toggle_hits_) { if (h.enabled && point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (const auto& h : settings_hits_) { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (const auto& d : drops_)      { if (d.enabled && point_in_rect(lx, ly, d.button)) return found(d.button); }
    for (const auto& h : card_hits_)  { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (auto it = share_hits_.rbegin(); it != share_hits_.rend(); ++it) {
        if (!point_in_rect(lx, ly, it->rect)) continue;
        if (it->kind == ShareActionKind::CloseOverlays) {
            return for_cursor ? false : found(it->rect);
        }
        return found(it->rect);
    }
    for (const auto& h : remote_hits_) { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (const auto& h : device_hits_) { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    for (const auto& h : pad_hits_)    { if (point_in_rect(lx, ly, h.rect)) return found(h.rect); }
    return false;
}

bool MainWindow::point_over_interactive(float lx, float ly) const
{
    return interactive_rect_at(lx, ly, nullptr, true);
}

/* Nothing under an open list looks pressable: a press there only closes it. */
bool MainWindow::hovered(const D2D1_RECT_F& rc) const
{
    return mouse_in_ && drop_open_.empty() && point_in_rect(mouse_x_, mouse_y_, rc);
}

/* The few pieces every page is laid out with, so each follows the 9 px grid
 * of data/style.css the way the GTK boxes do: a line of text is `lh_` tall
 * (`lhb_` for the big size), a paragraph's lines are 27 px apart, and a chip
 * or button is its text with 9 px above and below and 18 px beside it. */
static IDWriteTextLayout* make_layout(FontManager& fonts, const wchar_t* text, IDWriteTextFormat* fmt,
                                      float max_w, bool wrap)
{
    IDWriteTextLayout* layout = nullptr;
    UINT32 len = static_cast<UINT32>(wcslen(text));
    if (FAILED(fonts.factory()->CreateTextLayout(text, len, fmt, std::max(1.0f, max_w), 4000.0f, &layout)) || !layout) {
        return nullptr;
    }
    if (fonts.no_ligatures()) {
        DWRITE_TEXT_RANGE range = { 0, len };
        layout->SetTypography(fonts.no_ligatures(), range);
    }
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    return layout;
}

float MainWindow::text_width(const wchar_t* text, IDWriteTextFormat* fmt)
{
    if (!text || !*text || !fmt) return 0.0f;
    float w = 0.0f;
    if (IDWriteTextLayout* layout = make_layout(fonts_, text, fmt, 4000.0f, false)) {
        DWRITE_TEXT_METRICS m = {};
        if (SUCCEEDED(layout->GetMetrics(&m))) w = std::ceil(m.widthIncludingTrailingWhitespace);
        layout->Release();
    }
    return w;
}

/* One line, cut with an ellipsis where it doesn't fit, as GTK's labels are. */
void MainWindow::label(ID2D1RenderTarget* rt, const wchar_t* text, IDWriteTextFormat* fmt, float x, float y, float w,
                       const Rgba& color, DWRITE_TEXT_ALIGNMENT align)
{
    if (measuring_ || !text || !*text || !fmt || !brush_ || w <= 0.0f) return;
    IDWriteTextLayout* layout = make_layout(fonts_, text, fmt, w, false);
    if (!layout) return;
    layout->SetTextAlignment(align);
    IDWriteInlineObject* dots = nullptr;
    if (SUCCEEDED(fonts_.factory()->CreateEllipsisTrimmingSign(fmt, &dots)) && dots) {
        DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        layout->SetTrimming(&trim, dots);
        dots->Release();
    }
    brush_->SetColor(to_d2d(color));
    rt->DrawTextLayout(D2D1::Point2F(std::round(x), std::round(y)), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

static constexpr float kParaLine = 27.0f;   // .para { line-height: 1.5 }

static void para_spacing(IDWriteTextLayout* layout, float natural, float baseline)
{
    /* The extra room goes half above and half below each line, as CSS puts it. */
    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kParaLine, baseline + (kParaLine - natural) * 0.5f);
}

float MainWindow::para_height(const wchar_t* text, float w)
{
    if (!text || !*text) return 0.0f;
    float h = kParaLine;
    if (IDWriteTextLayout* layout = make_layout(fonts_, text, fonts_.body(), w, true)) {
        para_spacing(layout, lh_, base_);
        DWRITE_TEXT_METRICS m = {};
        if (SUCCEEDED(layout->GetMetrics(&m))) h = std::max(1u, m.lineCount) * kParaLine;
        layout->Release();
    }
    return h;
}

void MainWindow::para(ID2D1RenderTarget* rt, const wchar_t* text, float x, float y, float w, const Rgba& color,
                      DWRITE_TEXT_ALIGNMENT align)
{
    if (measuring_ || !text || !*text || !brush_) return;
    IDWriteTextLayout* layout = make_layout(fonts_, text, fonts_.body(), w, true);
    if (!layout) return;
    para_spacing(layout, lh_, base_);
    layout->SetTextAlignment(align);
    brush_->SetColor(to_d2d(color));
    rt->DrawTextLayout(D2D1::Point2F(std::round(x), std::round(y)), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_NONE);
    layout->Release();
}

void MainWindow::icon(ID2D1RenderTarget* rt, const char* const* art, float pixel, float x, float y, float box_w,
                      float box_h, const Rgba& color)
{
    float w = 0.0f, h = 0.0f;
    art_size(art, pixel, w, h);
    Rgba faint = color;
    faint.a *= 0.45f;
    draw_pixel_art(rt, art, std::round(x + (box_w - w) * 0.5f), std::round(y + (box_h - h) * 0.5f), pixel, color, faint);
}

void MainWindow::fill(ID2D1RenderTarget* rt, const D2D1_RECT_F& rc, const Rgba& color, float radius)
{
    if (measuring_ || !brush_) return;
    brush_->SetColor(to_d2d(color));
    if (radius > 0.0f) rt->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), brush_);
    else rt->FillRectangle(rc, brush_);
}

void MainWindow::card(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w, float h)
{
    fill(rt, D2D1::RectF(x, y, x + w, y + h), pal.card, kCardRadius);
}

float MainWindow::chip_width(const wchar_t* text)
{
    return text_width(text, fonts_.body()) + 36.0f;
}

/* .chip and button.quiet: 9 px above and below its text, 18 px beside it,
 * corners of 9 px; `active` is a checked chip or a button.primary. In a
 * note a quiet button has no ground until hovered. */
void MainWindow::chip(ID2D1RenderTarget* rt, const Palette& pal, const D2D1_RECT_F& rc, const wchar_t* text,
                      bool active, bool quiet_note)
{
    bool hover = hovered(rc);
    if (active || hover || !quiet_note) {
        Rgba ground = active ? pal.accent : hover ? pal.hover : pal.chip;
        if (active && hover) ground.a *= 0.9f;
        fill(rt, rc, ground, 9.0f);
    }
    float h = rc.bottom - rc.top;
    label(rt, text, fonts_.body(), rc.left, rc.top + (h - lh_) * 0.5f, rc.right - rc.left,
          active ? pal.on_accent : quiet_note ? pal.accent : pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
}

/* A meter centred in a row of `row_h`. */
void MainWindow::meter_row(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float row_h, int blocks,
                           float pixel, double fraction, bool hot)
{
    draw_meter(rt, pal, x, std::round(y + (row_h - 3.0f * pixel) * 0.5f), blocks, pixel, fraction, hot);
}

/* window.lares switch: 54 by 27, corners of 9; its slider 21 by 21, 3 in. */
void MainWindow::draw_switch(ID2D1RenderTarget* rt, const Palette& pal, const D2D1_RECT_F& rc, bool on, float alpha)
{
    Rgba track = on ? pal.accent : pal.track, knob = pal.card;
    track.a *= alpha;
    knob.a *= alpha;
    fill(rt, rc, track, 9.0f);
    float kx = on ? rc.right - 24.0f : rc.left + 3.0f;
    fill(rt, D2D1::RectF(kx, rc.top + 3.0f, kx + 21.0f, rc.top + 24.0f), knob, 6.0f);
}

/* ui_setting: a title, an optional paragraph 9 px under it and an optional
 * tag (.saving, 27 px tall) 9 px under that, centred beside a control. */
float MainWindow::setting(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float text_w,
                          const wchar_t* title, const wchar_t* sub, const wchar_t* tag, float alpha, float control_h)
{
    float sub_h = sub ? para_height(sub, text_w) : 0.0f;
    float texts_h = lh_ + (sub ? kHalf + sub_h : 0.0f) + (tag ? kHalf + 27.0f : 0.0f);
    float h = std::max({ 36.0f, texts_h, control_h });
    Rgba text = pal.text, muted = pal.muted, accent = pal.accent, note = pal.note;
    text.a *= alpha;
    muted.a *= alpha;
    accent.a *= alpha;
    note.a *= alpha;
    float cy = y + std::round((h - texts_h) * 0.5f);
    label(rt, title, fonts_.body(), x, cy, text_w, text);
    cy += lh_;
    if (sub) {
        cy += kHalf;
        para(rt, sub, x, cy, text_w, muted);
        cy += sub_h;
    }
    if (tag) {
        cy += kHalf;
        float tw = text_width(tag, fonts_.body()) + 18.0f;
        fill(rt, D2D1::RectF(x, cy, x + tw, cy + 27.0f), note, 9.0f);
        label(rt, tag, fonts_.body(), x + 9.0f, cy + (27.0f - lh_) * 0.5f, tw - 18.0f, accent);
    }
    return h;
}

/* A setting's texts with a dropdown at the right of its row. The button is
 * as wide as its widest choice and looks like a chip: its value 18 px in, its
 * chevron 18 px from its right edge. Where the title wouldn't fit beside it,
 * or its line would wrap past three lines there, the texts take the row and
 * the button goes under them, still at the right. A dropdown that isn't
 * enabled is dimmed, as a switch is, and takes no click. */
float MainWindow::setting_dropdown(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w,
                                  const wchar_t* title, const wchar_t* sub, Dropdown d)
{
    bool listed = d.checked >= 0 && d.checked < static_cast<int>(d.choices.size());
    std::wstring now = listed ? d.choices[static_cast<size_t>(d.checked)] : d.shown;
    float tw = text_width(now.c_str(), fonts_.body());
    for (const auto& c : d.choices) tw = std::max(tw, text_width(c.c_str(), fonts_.body()));
    float bw = 18.0f + tw + kGap + 18.0f + 18.0f;
    float btn_h = lh_ + 18.0f;
    float alpha = d.enabled ? 1.0f : 0.5f;

    float h = 0.0f, by = 0.0f, room = w - bw - kGap;
    bool beside = text_width(title, fonts_.body()) <= room && (!sub || para_height(sub, room) <= 3.0f * kParaLine);
    if (beside) {
        h = setting(rt, pal, x, y, room, title, sub, nullptr, alpha, btn_h);
        by = y + std::round((h - btn_h) * 0.5f);
    } else {
        float texts_h = setting(rt, pal, x, y, w, title, sub, nullptr, alpha, 0.0f);
        by = y + texts_h + kGap;
        h = texts_h + kGap + btn_h;
    }
    D2D1_RECT_F rc = D2D1::RectF(x + w - bw, by, x + w, by + btn_h);
    bool open = !drop_open_.empty() && drop_open_ == d.key;
    Rgba ground = (open || (d.enabled && hovered(rc))) ? pal.hover : pal.chip;
    Rgba text = pal.text, chevron = open ? pal.accent : pal.muted;
    ground.a *= alpha;
    text.a *= alpha;
    chevron.a *= alpha;
    fill(rt, rc, ground, 9.0f);
    label(rt, now.c_str(), fonts_.body(), rc.left + 18.0f, by + (btn_h - lh_) * 0.5f, tw, text);
    icon(rt, open ? chevron_up() : ART_CHEVRON, 2.0f, rc.right - 36.0f, by, 18.0f, btn_h, chevron);
    if (!measuring_) {
        d.button = rc;
        drops_.push_back(std::move(d));
    }
    return h;
}

/* The open dropdown's list: a card as wide as its button, 9 px under it (or
 * over it, where there's no room below), ringed by a 2 px line. Its choices
 * sit 9 px in, their text in line with the button's; the one in use is lit as
 * a checked chip, and the one the pointer or the arrow keys are on hovered.
 * Drawn after the page, so it covers what's under it. */
void MainWindow::draw_dropdown_list(ID2D1RenderTarget* rt, const Palette& pal)
{
    drop_items_.clear();
    if (drop_open_.empty()) return;
    const Dropdown* d = open_dropdown();
    if (!d) {
        drop_open_.clear();   // its setting is no longer on the page
        return;
    }
    const D2D1_RECT_F& button = d->button;
    float item_h = lh_ + 18.0f;
    float h = kHalf * 2.0f + item_h * static_cast<float>(d->choices.size());
    float top = button.bottom + kHalf;
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    float view_h = static_cast<float>(rc.bottom) / scale_;
    if (top + h > view_h && button.top - kHalf - h >= 0.0f) top = button.top - kHalf - h;
    D2D1_RECT_F panel = D2D1::RectF(button.left, top, button.right, top + h);
    fill(rt, D2D1::RectF(panel.left - 2.0f, panel.top - 2.0f, panel.right + 2.0f, panel.bottom + 2.0f), pal.line, 11.0f);
    fill(rt, panel, pal.card, 9.0f);
    float iy = top + kHalf;
    for (int i = 0; i < static_cast<int>(d->choices.size()); ++i) {
        D2D1_RECT_F item = D2D1::RectF(panel.left + kHalf, iy, panel.right - kHalf, iy + item_h);
        bool lit = i == drop_focus_;
        if (i == d->checked) {
            Rgba ground = pal.accent;
            if (lit) ground.a *= 0.9f;
            fill(rt, item, ground, 9.0f);
        } else if (lit) {
            fill(rt, item, pal.hover, 9.0f);
        }
        const std::wstring& text = d->choices[static_cast<size_t>(i)];
        label(rt, text.c_str(), fonts_.body(), item.left + kHalf, iy + (item_h - lh_) * 0.5f,
              item.right - item.left - 18.0f, i == d->checked ? pal.on_accent : pal.text);
        drop_items_.push_back(item);
        iy += item_h;
    }
}

/* .setting ~ .setting: 18 px, a 2 px line, 18 px. */
float MainWindow::setting_line(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w)
{
    fill(rt, D2D1::RectF(x, y + kGap, x + w, y + kGap + 2.0f), pal.line);
    return y + kGap + 2.0f + kGap;
}

void MainWindow::draw_sidebar(ID2D1RenderTarget* rt, const Palette& pal, float client_h)
{
    (void)client_h;
    float pill_x = kNavPadX;
    float pill_w = kSidebarWidth - kNavPadX - kNavPadRight;

    // Hover pill (matches data/style.css nav button.nav-item:hover, radius 9px)
    if (hover_sec_ >= 0 && hover_sec_ != current_sec_ && sections_[hover_sec_].visible) {
        int hv = visible_index_of(hover_sec_);
        float hy = kNavPadTop + hv * (nav_item_h() + kHalf);
        brush_->SetColor(to_d2d(pal.hover));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(pill_x, hy, pill_x + pill_w, hy + nav_item_h()), 9.0f, 9.0f),
            brush_);
    }

    // Active spring highlight pill (borderless var(--card), radius 9px)
    brush_->SetColor(to_d2d(pal.card));
    D2D1_ROUNDED_RECT active_pill = D2D1::RoundedRect(
        D2D1::RectF(pill_x, nav_pill_y_, pill_x + pill_w, nav_pill_y_ + nav_item_h()), 9.0f, 9.0f);
    rt->FillRoundedRectangle(active_pill, brush_);

    int vis = 0;
    for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
        if (!sections_[i].visible) continue;
        float iy = kNavPadTop + vis * (nav_item_h() + kHalf);
        bool selected = (i == current_sec_);

        /* LaresNav's item: 18 px beside and 9 px above and below an icon in
         * 22 px, 18 px, then its label. */
        bool lit = selected || i == hover_sec_;
        icon(rt, sections_[i].icon, 2.0f, pill_x + 18.0f, iy, 22.0f, nav_item_h(), selected ? pal.accent : pal.muted);
        float tx = pill_x + 18.0f + 22.0f + 18.0f;
        label(rt, sections_[i].label, fonts_.body(), tx, iy + 9.0f, pill_x + pill_w - 18.0f - tx,
              lit ? pal.text : pal.muted);
        ++vis;
    }
}

void MainWindow::draw_meter(ID2D1RenderTarget* rt, const Palette& pal,
                            float x, float y, int blocks, float pixel, double fraction, bool hot)
{
    if (measuring_ || !brush_ || blocks <= 0) return;
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
    if (measuring_ || !brush_ || w <= 0.0f || h <= 0.0f || pixel <= 0.0f) return;
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

void MainWindow::draw_pad_picture(ID2D1RenderTarget* rt, const Palette& pal,
                                  float x, float y, float w,
                                  const PadState& state, uint32_t seen,
                                  bool placeholder, int shake_px)
{
    if (measuring_ || pad_map_.width <= 0 || pad_map_.height <= 0 || !brush_) return;

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

void MainWindow::draw_page(ID2D1RenderTarget* rt, const Palette& pal, float client_w, float client_h)
{
    toggle_hits_.clear();
    settings_hits_.clear();
    card_hits_.clear();
    drops_.clear();
    remote_hits_.clear();
    device_hits_.clear();
    pad_hits_.clear();
    share_hits_.clear();
    /* LaresClamp: the page is at most 620 wide, 9 px from the sidebar, and
     * .page pads it 18 px on the left and 36 px on the right. */
    float page_w = std::min(kContentMaxW, client_w - kSidebarWidth - kContentLead);
    float col_x = kSidebarWidth + kContentLead + kPagePadLeft;
    float col_w = std::max(260.0f, page_w - kPagePadLeft - kPagePadRight);
    float col_y = kPagePadTop - scroll_y_;

    D2D1_RECT_F clip = D2D1::RectF(kSidebarWidth, 0.0f, client_w, client_h);
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

    const char* id = sections_[current_sec_].id;
    if (std::strcmp(id, "settings") == 0) draw_settings_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "hardware") == 0) draw_hardware_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "controller") == 0) draw_controller_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "remote") == 0) draw_remote_page(rt, pal, col_x, col_y, col_w);
    else if (std::strcmp(id, "share") == 0) {
        float sx = kSidebarWidth + kContentLead;
        float sy = kPagePadTop - scroll_y_;
        float sw = std::max(440.0f, client_w - sx - kPagePadLeft);
        float sh = std::max(520.0f, client_h - kPagePadTop * 2.0f);
        draw_share_page(rt, pal, sx, sy, sw, sh);
    }
    else if (std::strcmp(id, "headphones") == 0) draw_devices_page(rt, pal, col_x, col_y, col_w, DevKind::Audio);
    else if (std::strcmp(id, "keyboard") == 0) draw_devices_page(rt, pal, col_x, col_y, col_w, DevKind::Keyboard);
    else if (std::strcmp(id, "mouse") == 0) draw_devices_page(rt, pal, col_x, col_y, col_w, DevKind::Mouse);
    else draw_overview_page(rt, pal, col_x, col_y, col_w);
    draw_dropdown_list(rt, pal);

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
            float lw = static_cast<float>(px_w) / scale_;
            float lh = static_cast<float>(px_h) / scale_;

            dc_rt_->BeginDraw();
            dc_rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
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
    if (!drop_open_.empty()) {
        for (int i = 0; i < static_cast<int>(drop_items_.size()); ++i) {
            if (i != drop_focus_ && point_in_rect(lx, ly, drop_items_[static_cast<size_t>(i)])) {
                drop_focus_ = i;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
    } else if (lx >= kNavPadX && lx <= kSidebarWidth - kNavPadX) {
        int vis = 0;
        for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
            if (!sections_[i].visible) continue;
            float iy = kNavPadTop + vis * (nav_item_h() + kHalf);
            if (ly >= iy && ly <= iy + nav_item_h()) {
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
    if (!drop_open_.empty() && !interactive_rect_at(lx, ly, nullptr)) {
        close_dropdown();
        return;
    }
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
    /* A dropdown: with its list open, a choice is taken and closes it, and
     * a click anywhere else only closes it; closed, its button opens it. */
    if (!drop_open_.empty()) {
        for (int i = 0; i < static_cast<int>(drop_items_.size()); ++i) {
            if (point_in_rect(lx, ly, drop_items_[static_cast<size_t>(i)])) {
                pick_dropdown(i);
                return;
            }
        }
        close_dropdown();
        return;
    }
    for (const auto& d : drops_) {
        if (!d.enabled || !point_in_rect(lx, ly, d.button)) continue;
        /* On the choice in use; on none, for a value set elsewhere. */
        drop_open_ = d.key;
        drop_focus_ = d.checked;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (lx < kSidebarWidth) {
        int vis = 0;
        for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
            if (!sections_[i].visible) continue;
            float iy = kNavPadTop + vis * (nav_item_h() + kHalf);
            if (lx >= kNavPadX && lx <= kSidebarWidth - kNavPadX && ly >= iy && ly <= iy + nav_item_h()) {
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

    for (const auto& hit : settings_hits_) {
        if (point_in_rect(lx, ly, hit.rect)) {
            settings_action(hit.action);
            return;
        }
    }

    for (const auto& card : card_hits_) {
        if (card.target_section && point_in_rect(lx, ly, card.rect)) {
            select_section(card.target_section, true);
            return;
        }
    }

    if (share_click(lx, ly)) return;

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
        } else {
            device_action(dh.dev_id, dh.kind, dh.value);
        }
        return;
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

/* What a mouse's or keyboard's control asks: the device's settings as they
 * are, with that one changed, sent to it. */
void MainWindow::device_action(const std::string& id, DeviceActionKind kind, int value)
{
    for (auto& d : dev_list_) {
        if (d.id != id) continue;
        if (d.kind == DevKind::Mouse) {
            MouseSettings want = d.mouse;
            switch (kind) {
            case DeviceActionKind::MouseStage:
                want.active = std::clamp(value, 0, std::max(0, want.stages - 1));
                break;
            case DeviceActionKind::MouseDpiDelta:
                if (want.active >= 0 && want.active < want.stages) {
                    int dpi = ((want.dpi[want.active] + value) / 50) * 50;
                    want.dpi[want.active] = std::clamp(dpi, want.dpi_min, want.dpi_max);
                }
                break;
            case DeviceActionKind::MouseRate:
                want.rate = value;
                break;
            case DeviceActionKind::MouseLift:
                want.lift_mm = value;
                break;
            case DeviceActionKind::MouseLight:
                want.light = static_cast<LightEffect>(value);
                if ((want.light == LightEffect::Solid || want.light == LightEffect::Breathing) &&
                    want.rgb[0] == 0 && want.rgb[1] == 0 && want.rgb[2] == 0) {
                    want.rgb[0] = 0xff;
                }
                break;
            case DeviceActionKind::MouseColour:
                if (want.light != LightEffect::Solid && want.light != LightEffect::Breathing) {
                    want.light = LightEffect::Solid;
                }
                want.rgb[0] = static_cast<uint8_t>((value >> 16) & 0xff);
                want.rgb[1] = static_cast<uint8_t>((value >> 8) & 0xff);
                want.rgb[2] = static_cast<uint8_t>(value & 0xff);
                break;
            case DeviceActionKind::MouseBrightness:
                want.brightness = std::clamp(value, 0, 100);
                break;
            default:
                break;
            }
            devices_.apply_mouse_async(d.id, want);
        } else if (d.kind == DevKind::Keyboard) {
            KeyboardSettings want = d.keyboard;
            switch (kind) {
            case DeviceActionKind::KeyBrightness:
                want.brightness = std::clamp(value, 0, 100);
                break;
            case DeviceActionKind::KeyLight:
                want.light = static_cast<KeyLight>(value);
                if (key_light_coloured(want.light) && want.rgb[0] == 0 && want.rgb[1] == 0 && want.rgb[2] == 0) {
                    want.rgb[0] = 0xff;
                }
                break;
            case DeviceActionKind::KeyColour:
                if (!key_light_coloured(want.light)) want.light = KeyLight::Solid;
                want.rgb[0] = static_cast<uint8_t>((value >> 16) & 0xff);
                want.rgb[1] = static_cast<uint8_t>((value >> 8) & 0xff);
                want.rgb[2] = static_cast<uint8_t>(value & 0xff);
                break;
            case DeviceActionKind::KeyStartup:
                want.startup = (value != 0);
                break;
            case DeviceActionKind::KeyShutdown:
                want.shutdown = (value != 0);
                break;
            case DeviceActionKind::KeyDim:
                want.dim_s = value;
                break;
            case DeviceActionKind::KeyOff:
                want.off_s = value;
                break;
            default:
                break;
            }
            // Live keyboard lighting effects also save to pad.ini for the helper.
            bool is_light_pick = (kind == DeviceActionKind::KeyLight || kind == DeviceActionKind::KeyColour);
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
                devices_.set_live_key_light(want.light, want.rgb);
            }
            devices_.apply_keyboard_async(d.id, want);
        }
        return;
    }
}

void MainWindow::on_mouse_wheel(int delta)
{
    close_dropdown();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float lh = static_cast<float>(rc.bottom - rc.top) / scale_;
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

    /* A dropdown's list, open: the arrows move along it, Enter or Space
     * takes the choice, Escape closes it. */
    if (!drop_open_.empty()) {
        const Dropdown* d = open_dropdown();
        if (!d || vk == VK_ESCAPE) {
            close_dropdown();
        } else if (vk == VK_UP || vk == VK_DOWN) {
            /* From none, Down goes to the first and Up to the last. */
            int last = std::max(0, static_cast<int>(d->choices.size()) - 1);
            if (drop_focus_ < 0) drop_focus_ = vk == VK_DOWN ? 0 : last;
            else drop_focus_ = std::clamp(drop_focus_ + (vk == VK_DOWN ? 1 : -1), 0, last);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (vk == VK_RETURN || vk == VK_SPACE) {
            pick_dropdown(drop_focus_);
        }
        return;
    }

    if (share_key_down(vk)) return;

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
    if (share_char(ch)) return;
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
            /* The Wi-Fi adapter AirDrop borrows may have come or gone too. */
            if (!opts_.dry_run) {
                for (auto& sec : sections_) {
                    if (std::strcmp(sec.id, "share") == 0 && !sec.visible && share_adapter_present()) {
                        sec.visible = true;
                        PostMessageW(hwnd_, kMsgDevicesUpdated, 0, 0);
                    }
                }
                if (std::strcmp(sections_[current_sec_].id, "share") == 0) {
                    share_.refresh();
                    share_view_ = share_.view();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
        } else if (wp == kShareTickTimerId) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        on_mouse_move(static_cast<float>(GET_X_LPARAM(lp)) / scale_,
                      static_cast<float>(GET_Y_LPARAM(lp)) / scale_);
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
                float lx = static_cast<float>(pt.x) / scale_;
                float ly = static_cast<float>(pt.y) / scale_;
                SetCursor(LoadCursorW(nullptr, point_over_interactive(lx, ly) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        on_mouse_press(static_cast<float>(GET_X_LPARAM(lp)) / scale_,
                       static_cast<float>(GET_Y_LPARAM(lp)) / scale_);
        return 0;
    case WM_LBUTTONUP:
        on_mouse_release(static_cast<float>(GET_X_LPARAM(lp)) / scale_,
                         static_cast<float>(GET_Y_LPARAM(lp)) / scale_);
        return 0;
    case WM_RBUTTONUP:
        share_right_click(static_cast<float>(GET_X_LPARAM(lp)) / scale_,
                          static_cast<float>(GET_Y_LPARAM(lp)) / scale_);
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
        else {
            close_dropdown();
            if (share_mode_menu_open_ || share_adapter_open_ || share_ctx_open_) {
                share_mode_menu_open_ = false;
                share_adapter_open_ = false;
                share_ctx_open_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
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
        update_scale();
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
        /* 640 by 480 at the Size setting, so the layout is never narrower
         * than at 100%, but no more than the screen's work area. */
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        LONG min_w = static_cast<LONG>(std::lround(640.0f * scale_));
        LONG min_h = static_cast<LONG>(std::lround(480.0f * scale_));
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            min_w = std::min(min_w, mi.rcWork.right - mi.rcWork.left);
            min_h = std::min(min_h, mi.rcWork.bottom - mi.rcWork.top);
        }
        mmi->ptMinTrackSize.x = min_w;
        mmi->ptMinTrackSize.y = min_h;
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
        nav_pill_target_y_ = kNavPadTop + vis * (nav_item_h() + kHalf);
        if (!timer_running_) nav_pill_y_ = nav_pill_target_y_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case kMsgShareUpdated:
        on_share_updated();
        return 0;
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
        if (drop_target_) {
            RevokeDragDrop(hwnd_);
            static_cast<ShareDropTarget*>(drop_target_)->detach();
            drop_target_->Release();
            drop_target_ = nullptr;
        }
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
        share_.set_browsing(false);
        share_.set_notify_hwnd(nullptr);
        ble_.set_active(false);
        if (share_tick_) {
            KillTimer(hwnd_, kShareTickTimerId);
            share_tick_ = false;
        }
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
