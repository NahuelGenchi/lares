// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "cli.h"
#include "config.h"
#include "devices.h"
#include "font.h"
#include "helper.h"
#include "hw.h"
#include "pad.h"
#include "remote.h"
#include "share_win.h"
#include "theme.h"
#include "ble_wake.h"

#include <d2d1.h>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

namespace lares {

inline constexpr wchar_t kWindowClass[] = L"LaresWindowClass";
inline constexpr wchar_t kWindowClassDry[] = L"LaresWindowClassDry";
inline constexpr ULONG_PTR kCopyDataSection = 0x4C415253; // "LARS"

class MainWindow {
public:
    MainWindow(HINSTANCE hinst, const CliOptions& opts);
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create();
    void show(int cmd_show);
    void select_section(const std::string& id, bool animate = true);
    HWND hwnd() const { return hwnd_; }

private:
    struct SectionDef {
        const char* id;
        const wchar_t* label;
        const char* const* icon;
        bool visible;
    };

    struct ToggleHit {
        D2D1_RECT_F rect;
        bool* field;
        bool enabled;
    };

    struct NavCardHit {
        D2D1_RECT_F rect;
        const char* target_section;
    };

    struct SizeHit {
        D2D1_RECT_F rect;
        int percent; // 0: the dropdown's button
    };

    enum class RemoteActionKind {
        SwitchModeHome,
        SwitchModeRemote,
        ToggleRowExpand,
        PickChip,
        AllowSunshine,
        ReapplyMode,
        FinishGuide,
        OpenSunshineWeb,
    };

    struct RemoteHit {
        D2D1_RECT_F rect;
        RemoteActionKind kind;
        RemoteItem item = RemoteItem::Screen;
        std::string value;
    };

    enum class ShareActionKind {
        Setup,
        ToggleReceive,
        ChooseOrStop,
        Accept,
        Decline,
        Dismiss,
        OpenLink,
        ShowFolder,
        SendTo,
        CancelSend,
    };

    struct ShareHit {
        D2D1_RECT_F rect;
        ShareActionKind kind;
        std::string value;
    };

    enum class DeviceActionKind {
        MouseStage,
        MouseDpiDelta,
        MouseRate,
        MouseLift,
        MouseLight,
        MouseColour,
        MouseBrightness,
        KeyBrightness,
        KeyLight,
        KeyColour,
        KeyStartup,
        KeyShutdown,
        KeyDim,
        KeyOff,
        OpenDynamicLighting,
    };

    struct DeviceHit {
        D2D1_RECT_F rect;
        std::string dev_id;
        DeviceActionKind kind;
        int value = 0;
    };

    enum class PadActionKind {
        SelectTab,
        ResetChecklist,
        TestVibration,
        FindController,
        PlayVibration,
        StrongMotor,
        WeakMotor,
        LengthMs,
        EditNickname,
        LowLevelDelta,
        IdleMinutesChip,
        ForgetNickname,
        OpenSettings,
        OpenBluetoothSettings,
    };

    struct PadHit {
        D2D1_RECT_F rect;
        std::string pad_path;
        PadActionKind kind;
        int value = 0;
    };

    struct PadCardMemory {
        int tab = 0; // 0 = Buttons, 1 = Sticks, 2 = Vibration
        uint32_t seen = 0;
        int strong = kPadRumbleStrength;
        int weak = kPadRumbleStrength;
        int length_ms = kPadRumbleMs;
        StickTracker sticks[2];
        ULONGLONG shake_start_ms = 0;
        int shake_offset = 0;
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_msg(UINT msg, WPARAM wp, LPARAM lp);

    bool init_d2d();
    void release_backbuffer();
    bool ensure_backbuffer(int px_w, int px_h);
    void apply_titlebar_theme();
    void on_paint();
    void on_timer();
    void on_mouse_move(float lx, float ly);
    void on_mouse_press(float lx, float ly);
    void on_mouse_release(float lx, float ly);
    void on_mouse_down(float lx, float ly);
    void on_mouse_wheel(int delta);
    void on_key_down(WPARAM vk);
    void on_char(wchar_t ch);

    void draw_sidebar(ID2D1RenderTarget* rt, const Palette& pal, float client_h);
    void draw_page(ID2D1RenderTarget* rt, const Palette& pal, float client_w, float client_h);
    void draw_settings_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w);
    void draw_overview_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w);
    void draw_hardware_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w);
    void draw_remote_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w);
    void draw_share_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w);
    std::vector<std::wstring> pick_files_to_send();
    bool share_click(float lx, float ly);
    void on_share_updated();
    void sync_share_tick();
    void draw_devices_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w, DevKind kind);
    void draw_controller_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w);
    void draw_pad_picture(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w,
                          const PadState& state, uint32_t seen, bool placeholder, int shake_px);
    void draw_pixel_art(ID2D1RenderTarget* rt, const char* const* art, float x, float y, float scale,
                        const Rgba& solid, const Rgba& faint);
    bool point_over_interactive(float lx, float ly) const;
    bool interactive_rect_at(float lx, float ly, D2D1_RECT_F* out) const;
    bool hovered(const D2D1_RECT_F& rc) const;
    float nav_item_h() const { return lh_ + 18.0f; }   // nav button.nav-item { padding: 9px 18px }
    float text_width(const wchar_t* text, IDWriteTextFormat* fmt);
    void label(ID2D1RenderTarget* rt, const wchar_t* text, IDWriteTextFormat* fmt, float x, float y, float w,
               const Rgba& color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float para_height(const wchar_t* text, float w);
    void para(ID2D1RenderTarget* rt, const wchar_t* text, float x, float y, float w, const Rgba& color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    void fill(ID2D1RenderTarget* rt, const D2D1_RECT_F& rc, const Rgba& color, float radius = 0.0f);
    // A ui_setting's texts, centred in its row; returns the row's height (36 at least).
    float setting(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float text_w, const wchar_t* title,
                  const wchar_t* sub, const wchar_t* tag, float alpha, float control_h);
    // The line between two settings of a card: returns where the next begins.
    float setting_line(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w);
    // A card around what `body` lays out from its top: measured first, then drawn. Returns its height.
    template <class Body>
    float boxed(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w, float pad, Body body)
    {
        measuring_ = true;
        float bottom = body(y + pad);
        measuring_ = false;
        float h = bottom - y + pad;
        card(rt, pal, x, y, w, h);
        body(y + pad);
        return h;
    }
    void icon(ID2D1RenderTarget* rt, const char* const* art, float pixel, float x, float y, float box_w, float box_h,
              const Rgba& color);
    void card(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w, float h);
    // A chip or button as wide as its text asks: returns its width.
    float chip_width(const wchar_t* text);
    void chip(ID2D1RenderTarget* rt, const Palette& pal, const D2D1_RECT_F& rc, const wchar_t* text, bool active,
              bool quiet_note = false);
    void meter_row(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float row_h, int blocks, float pixel,
                   double fraction, bool hot);
    void draw_switch(ID2D1RenderTarget* rt, const Palette& pal, const D2D1_RECT_F& rc, bool on, float alpha);
    void clamp_scroll();
    void draw_meter(ID2D1RenderTarget* rt, const Palette& pal,
                    float x, float y, int blocks, float pixel, double fraction, bool hot);
    void draw_graph(ID2D1RenderTarget* rt, const Palette& pal,
                    float x, float y, float w, float h, float pixel, const GraphRing& ring);
    void update_sampling_state();

    // The Size setting: the window is drawn at the display scale times it.
    void update_scale() { scale_ = dpi_scale_ * static_cast<float>(pad_prefs_.size) / 100.0f; }
    void apply_size(int percent);
    // Its dropdown: the choice nearest the saved size, and the open list drawn over the page.
    int size_index() const;
    void draw_size_list(ID2D1RenderTarget* rt, const Palette& pal, const D2D1_RECT_F& button);
    void close_size_list();

    void start_anim_timer();
    void stop_anim_timer_if_idle();
    Palette current_palette() const;
    int visible_index_of(int sec_idx) const;

    HINSTANCE hinst_ = nullptr;
    CliOptions opts_;
    HWND hwnd_ = nullptr;
    float dpi_scale_ = 1.0f;   // Windows' display scale for the window's monitor
    float scale_ = 1.0f;       // and that times the Size setting: a layout pixel in screen pixels

    FontManager fonts_;
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    HDC mem_dc_ = nullptr;
    HBITMAP mem_bmp_ = nullptr;
    HBITMAP old_bmp_ = nullptr;
    int bb_w_ = 0;
    int bb_h_ = 0;

    bool dark_ = false;
    bool remote_mode_ = false;
    float mode_t_ = 0.0f;
    float mode_target_ = 0.0f;

    std::vector<SectionDef> sections_;
    int current_sec_ = 0;
    int hover_sec_ = -1;
    float nav_pill_y_ = 0.0f;
    float nav_pill_vel_ = 0.0f;
    float nav_pill_target_y_ = 0.0f;
    bool nav_pill_init_ = false;

    float scroll_y_ = 0.0f;
    float scroll_target_y_ = 0.0f;
    float content_height_ = 0.0f;

    PadPrefs pad_prefs_;
    std::wstring pad_path_;
    std::vector<ToggleHit> toggle_hits_;
    std::vector<NavCardHit> card_hits_;
    std::vector<SizeHit> size_hits_;
    bool size_open_ = false;   // the Size dropdown's list is showing
    int size_focus_ = 0;       // and the choice the arrow keys are on

    HardwareSampler hw_;
    HwSample hw_sample_;
    GraphRing cpu_graph_;
    GraphRing gpu_graph_;

    RemoteController remote_;
    RemoteConfig remote_cfg_;
    RemoteLive remote_live_;
    int expanded_remote_row_ = -1;
    bool sunshine_needs_perm_ = false;
    std::string remote_err_msg_;
    std::vector<RemoteHit> remote_hits_;

    ShareManager share_;
    ShareView share_view_;
    BleWake ble_;
    std::vector<ShareHit> share_hits_;
    bool share_tick_ = false;

    DevicesManager devices_;
    std::vector<DeviceEntry> dev_list_;
    std::vector<DeviceHit> device_hits_;

    PadManager pads_;
    PadMap pad_map_;
    std::vector<PadInfo> pad_list_;
    std::map<std::string, PadCardMemory> pad_memory_;
    std::vector<PadHit> pad_hits_;
    std::string editing_pad_path_;
    std::string editing_nickname_;

    bool timer_running_ = false;
    bool tracking_mouse_ = false;
    ULONGLONG last_tick_ms_ = 0;

    // The pointer, for hover, and where a click began: it acts on release
    // over the same control, as Windows buttons do.
    float lh_ = 24.0f;    // a line of 18 px text
    float base_ = 18.0f;  // and its baseline
    float lhb_ = 36.0f;   // a line of 27 px text
    float lht_ = 48.0f;   // a line of 36 px text
    bool measuring_ = false;   // a pass that only sizes things: nothing is drawn or made clickable
    bool mouse_in_ = false;
    float mouse_x_ = 0.0f;
    float mouse_y_ = 0.0f;
    bool hover_on_ = false;
    D2D1_RECT_F hover_rect_ = {};
    bool pressed_ = false;
    float press_x_ = 0.0f;
    float press_y_ = 0.0f;
    HDEVNOTIFY dev_notify_ = nullptr;
};

} // namespace lares
