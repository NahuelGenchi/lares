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
#include "theme.h"

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
    void draw_devices_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w, DevKind kind);
    void draw_controller_page(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w);
    void draw_pad_picture(ID2D1RenderTarget* rt, const Palette& pal, float x, float y, float w,
                          const PadState& state, uint32_t seen, bool placeholder, int shake_px);
    void draw_pixel_art(ID2D1RenderTarget* rt, const char* const* art, float x, float y, float scale,
                        const Rgba& solid, const Rgba& faint);
    void draw_text(ID2D1RenderTarget* rt, const wchar_t* text, IDWriteTextFormat* fmt,
                   const D2D1_RECT_F& rect, const Rgba& color);
    float measure_text_height(const wchar_t* text, IDWriteTextFormat* fmt, float max_w);
    void draw_toggle_row(ID2D1RenderTarget* rt, const Palette& pal,
                         float x, float& y, float w,
                         const wchar_t* title, const wchar_t* sub, const wchar_t* saving,
                         bool* field, bool enabled);
    void draw_meter(ID2D1RenderTarget* rt, const Palette& pal,
                    float x, float y, int blocks, float pixel, double fraction, bool hot);
    void draw_graph(ID2D1RenderTarget* rt, const Palette& pal,
                    float x, float y, float w, float h, float pixel, const GraphRing& ring);
    void update_sampling_state();

    void start_anim_timer();
    void stop_anim_timer_if_idle();
    Palette current_palette() const;
    int visible_index_of(int sec_idx) const;

    HINSTANCE hinst_ = nullptr;
    CliOptions opts_;
    HWND hwnd_ = nullptr;
    float dpi_scale_ = 1.0f;

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
    ULONGLONG last_tick_ms_ = 0;
};

} // namespace lares
