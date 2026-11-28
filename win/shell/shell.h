// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef LARES_SHELL_H
#define LARES_SHELL_H

/* lares-shell.exe (issue #145): the top bar and the dock. What the modules
 * share: messages, settings, the windows the shell tracks, readings. The
 * drawing layer is in draw.h, the settings file in shell_prefs.h. */

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include "draw.h"
#include "shell_prefs.h"
#include "share_bridge.h"

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* Messages to the shell's own host window. */
#define WM_LARES_WINDOWS_CHANGED   (WM_APP + 10)
#define WM_LARES_TELEMETRY_UPDATED (WM_APP + 11)
#define WM_LARES_MEDIA_UPDATED     (WM_APP + 12)
#define WM_LARES_TOGGLE_LAUNCHER   (WM_APP + 14)
#define WM_LARES_DOCK_SHORTCUT     (WM_APP + 15)
#define WM_LARES_TAILSCALE_UPDATED (WM_APP + 17)
#define WM_LARES_BT_SCAN_DONE      (WM_APP + 18)
#define WM_LARES_QUIT_CLEAN        (WM_APP + 20)
#define WM_LARES_RESTORE_TOPMOST   (WM_APP + 21)
#define WM_LARES_FILES_LOADED      (WM_APP + 19)
#define WM_LARES_TOGGLE_CLIPBOARD  (WM_APP + 25)

/* Lares' window and Settings ask a running shell to read its settings again
 * with this registered message, posted to the host window. */
#define LARES_SHELL_RELOAD_MESSAGE L"LaresShellReload"
#define LARES_SHELL_HOST_CLASS     L"LaresShellHost"
#define LARES_SHELL_HOST_CLASS_ISOLATED L"LaresShellHostIsolated"

#define LARES_MAX_DOCK_ITEMS  64
#define LARES_MAX_WINDOWS     128
#define LARES_MAX_ICON_CACHE  96

typedef enum {
    DOCK_GROUP_LEFT = 0,
    DOCK_GROUP_CENTER = 1,
    DOCK_GROUP_RIGHT = 2
} DockGroup;

typedef enum {
    DOCK_ITEM_APP = 0,
    DOCK_ITEM_MEDIA = 1
} DockItemType;

typedef enum {
    HIDE_NEVER = 0,      /* always shown: reserves its strip of the screen */
    HIDE_ALWAYS = 1,     /* auto-hide */
    HIDE_ON_OVERLAP = 2  /* hides when a window covers it */
} DockHideMode;

typedef enum {
    PROCMON_SORT_NAME = 0,
    PROCMON_SORT_PID  = 1,
    PROCMON_SORT_CPU  = 2,
    PROCMON_SORT_MEM  = 3,
    PROCMON_SORT_THR  = 4
} ProcMonSortCol;

typedef enum {
    SCREENSHOT_REGION = 0,
    SCREENSHOT_WINDOW = 1,
    SCREENSHOT_FULL   = 2
} ScreenshotMode;

typedef enum {
    POPUP_NONE = 0,
    POPUP_CALENDAR,
    POPUP_KEYBOARD,
    POPUP_BLUETOOTH,
    POPUP_NETWORK,
    POPUP_MEDIA,
    POPUP_NOTIFICATIONS,
    POPUP_SCREENSHOT,
    POPUP_POWER,
    POPUP_SHARE
} PopupKind;

/* The settings the shell runs with: shell.ini's values and fixed sizes. */
typedef struct {
    bool isolated;      /* a test copy: hides nothing, reserves nothing, no shortcuts */
    int  polling_interval_ms;
    bool animations;

    bool topbar_enabled;
    int  topbar_item_size;
    int  topbar_padding;
    int  topbar_margin;
    int  start_of_week; /* 0 = Sunday, 1 = Monday */
    bool topbar_autohide;

    bool dock_enabled;
    int  dock_item_size;
    int  dock_padding;
    int  dock_margin;
    int  dock_spacing;
    DockHideMode dock_hide_mode;
    int  dock_delay_hide_ms;
    int  dock_delay_show_ms;
    bool dock_show_instance_counter;
    bool dock_middle_click_new;
    bool dock_show_media_widget;

    bool launcher_enabled;
    bool launcher_win_key;
    bool clipboard_enabled;
    bool screenshot_enabled;
    bool viewers_enabled;
    bool files_enabled;
    bool airdrop_enabled;   /* the bar's AirDrop button (issue #155) */
} AppConfig;

typedef struct {
    DockGroup group;
    DockItemType type;
    wchar_t name[128];
    wchar_t umid[256];
    wchar_t path[MAX_PATH];
    wchar_t command[MAX_PATH];
    wchar_t args[LARES_PIN_ARGS];
    wchar_t workdir[MAX_PATH];
    wchar_t icon_rel[MAX_PATH];
    bool    is_square_icon;
    bool    pinned;
} DockItemConfig;

typedef struct {
    HWND    hwnd;
    DWORD   pid;
    wchar_t title[256];
    wchar_t app_name[128];
    wchar_t exe_path[MAX_PATH];
    wchar_t umid[256];
    RECT    rect;
    bool    is_minimized;
    bool    is_maximized;
    bool    is_focused;
    bool    is_flashing;
} TrackedWindow;

typedef struct {
    int     cpu_percent;
    int     gpu_percent;
    uint64_t ram_used_mb;
    uint64_t ram_total_mb;
    int     volume_percent; /* 0..100 */
    bool    volume_muted;
    bool    net_connected;
    bool    net_is_wifi;
    int     wifi_signal;    /* 0..100 */
    wchar_t net_name[128];
    wchar_t net_ipv4[64];
    bool    bt_present;
    bool    bt_enabled;
    int     bt_connected_count;
    wchar_t kbd_short[16];  /* e.g. "ENG" */
    wchar_t kbd_full[128];
    HKL     kbd_hkl;
    bool    dnd_enabled;
    int     notif_count;
    bool    ts_available;
    bool    ts_running;
    wchar_t ts_state[32];
    wchar_t ts_ip[64];
    wchar_t ts_hostname[128];
    wchar_t ts_exit_node[128];
    int     ts_peer_count;
    /* Tailscale's Windows service, read without administrator rights. */
    int     ts_svc_state;      /* 0 = not installed, else SERVICE_STOPPED (1) .. SERVICE_RUNNING (4) .. */
    int     ts_svc_start_type; /* SERVICE_AUTO_START (2), SERVICE_DEMAND_START (3), SERVICE_DISABLED (4), 0 = unknown */
    int     ts_svc_mem_mb;     /* own memory of tailscaled.exe while it runs */
    bool    ts_svc_busy;       /* a start or stop is under way */
    wchar_t ts_svc_msg[96];    /* the last start or stop result */
    DWORD   ts_svc_msg_tick;
} SystemTelemetry;

typedef struct {
    bool    has_session;
    bool    is_playing;
    wchar_t title[160];
    wchar_t artist[160];
    wchar_t app_umid[256];
    GpBitmap *thumbnail; /* owned by media.c, behind its lock */
    uint32_t thumb_version;
} MediaState;

extern HINSTANCE       g_hInst;
extern HWND            g_hHostWnd;
extern HWND            g_hTopBarWnd;
extern HWND            g_hDockWnd;
extern AppConfig       g_cfg;
extern DockItemConfig  g_dock_cfg[LARES_MAX_DOCK_ITEMS];
extern int             g_dock_cfg_count;
extern SystemTelemetry g_telemetry;

/* main.c: the primary screen; a test copy's lies far off it. */
void shell_screen_rect(RECT *out);

/* config.c: shell.ini through shell_prefs.h */
void config_load_all(void);
void config_save_dock(void);
void config_save_behaviour(void);
/* A pin's picture: absolute, or in Lares' settings folder. */
void config_resolve_path(const wchar_t *rel, wchar_t *out, size_t out_len);

/* win_tracker.c */
void win_tracker_init(HWND notify_hwnd);
void win_tracker_refresh(void);
void win_tracker_shutdown(void);
int  win_tracker_get_windows(TrackedWindow *out, int max_count);
bool win_tracker_get_focused(TrackedWindow *out);
bool win_tracker_is_rect_overlapped(const RECT *rc, HWND ignore1, HWND ignore2);
bool win_tracker_is_fullscreen_active(void);
GpBitmap *icon_cache_get(const wchar_t *umid, const wchar_t *exe_path, const wchar_t *custom_rel, bool *out_square);
void icon_cache_clear(void);
void shell_launch_app(const wchar_t *command, const wchar_t *args, const wchar_t *workdir, const wchar_t *umid);

/* telemetry.c */
void telemetry_init(HWND notify_hwnd);
void telemetry_update(bool poll_gpu);
void telemetry_shutdown(void);
void telemetry_set_volume(int percent);
void telemetry_toggle_mute(void);
void telemetry_step_volume(int delta);
void telemetry_refresh_tailscale_async(void);
void telemetry_tailscale_toggle_async(bool connect);
void telemetry_tailscale_set_exit_node_async(const wchar_t *node_ip);
/* Starts or stops Tailscale's service; startup_type 0 keeps its start type,
 * else SERVICE_AUTO_START or SERVICE_DEMAND_START. Asks for administrator
 * rights only when Windows refuses without them. */
void telemetry_tailscale_service_async(bool start, int startup_type);
/* The short-lived elevated copy: lares-shell.exe --ts-service start|stop [--ts-startup manual|auto]. */
int  telemetry_ts_service_helper(bool start, int startup_type);

/* media.c */
void media_init(HWND notify_hwnd);
void media_poll(void);
void media_get_state(MediaState *out); /* does not hand over the picture */
GpBitmap *media_lock_thumbnail(void);
void media_unlock_thumbnail(void);
void media_prev(void);
void media_play_pause(void);
void media_next(void);
void media_shutdown(void);

/* topbar.c */
bool topbar_create(HINSTANCE hInst);
void topbar_destroy(void);
void topbar_repaint(void);
void topbar_on_display_change(void);
void topbar_reassert_topmost(void);
int  topbar_get_height(void);
void topbar_on_windows_changed(void);
void topbar_on_explorer_restart(void);
void topbar_apply_settings(void);
int  topbar_snapshot(const wchar_t *png_path);

/* dock.c */
bool dock_create(HINSTANCE hInst);
void dock_destroy(void);
void dock_repaint(void);
void dock_on_windows_changed(void);
void dock_on_display_change(void);
void dock_reassert_topmost(void);
void dock_activate_index(int index);
void dock_reveal_temporarily(void);
void dock_on_explorer_restart(void);
void dock_apply_settings(void);
int  dock_snapshot(const wchar_t *png_path);

/* launcher.c (issue #148) */
void launcher_init(HINSTANCE hInst);
void launcher_toggle(void);
void launcher_hide(void);
HWND launcher_get_prev_fg(void);
void launcher_on_theme_changed(void);
void launcher_on_settings_changed(void);
void launcher_shutdown(void);
int  launcher_snapshot(const wchar_t *typed, const wchar_t *png_path);

/* clipboard.c (issue #148) */
void clipboard_init(HINSTANCE hInst);
void clipboard_toggle(void);
void clipboard_hide(void);
bool clipboard_is_open(void);
void clipboard_on_theme_changed(void);
void clipboard_shutdown(void);
int  clipboard_snapshot(const wchar_t *png_path);
void clipboard_add_sample(const wchar_t *text, bool pinned);

/* procmon.c: the process list (issue #149) */
void procmon_init(HINSTANCE hInst);
void procmon_open(ProcMonSortCol initial_sort);
void procmon_on_theme_changed(void);
bool procmon_is_open(void);
void procmon_shutdown(void);
int  procmon_snapshot(const wchar_t *png_path);

/* screenshot.c: the screenshot tool (issue #149), while its switch is on */
void screenshot_init(HINSTANCE hInst);
void screenshot_trigger(ScreenshotMode mode);
void screenshot_on_theme_changed(void);
bool screenshot_is_active(void);
void screenshot_shutdown(void);
int  screenshot_snapshot(const wchar_t *png_path);

/* viewer.c and textview.c: lares-view.exe (issue #151) */
void viewer_init(HINSTANCE hInst);
void viewer_open(const wchar_t *file_path);
bool viewer_is_supported_ext(const wchar_t *ext);
void viewer_on_theme_changed(void);
bool viewer_is_open(void);
void viewer_shutdown(void);
HWND viewer_window(void);
int  viewer_snapshot(const wchar_t *png_path);
void textview_init(HINSTANCE hInst);
void textview_open(const wchar_t *file_path);
bool textview_is_supported_ext(const wchar_t *ext);
void textview_on_theme_changed(void);
bool textview_is_open(void);
void textview_shutdown(void);
HWND textview_window(void);
int  textview_snapshot(const wchar_t *png_path);

/* files.c: lares-files.exe (issue #153) */
void files_init(HINSTANCE hInst);
void files_open(const wchar_t *initial_path);
void files_open_ex(const wchar_t *initial_path, const wchar_t *select_name, bool reuse_window);
void files_on_theme_changed(void);
bool files_has_open_windows(void);
int  files_get_window_count(void);
HWND files_get_first_window(void);
void files_shutdown(void);
int  files_snapshot(const wchar_t *png_path);
void files_set_crumb_root(const wchar_t *root);
bool files_loaded(void);

/* hotkeys.c: shortcut ids, as WM_HOTKEY brings them. */
#define LARES_HOTKEY_LAUNCHER     9001
#define LARES_HOTKEY_SNIP_REGION  9002
#define LARES_HOTKEY_SNIP_WINDOW  9003
#define LARES_HOTKEY_SNIP_FULL    9004
#define LARES_HOTKEY_CLIPBOARD    9005
bool hotkeys_init(HWND host_hwnd);
void hotkeys_shutdown(void);

/* popups.c: the bar's popups (issue #147). */
void popups_init(HINSTANCE hInst);
void popups_toggle(PopupKind kind, int anchor_x, int anchor_y);
void popups_close(void);
bool popups_is_open(void);
bool popups_is_kind_open(PopupKind kind);
void popups_refresh_if_open(void);
void popups_on_tailscale_updated(void);
void popups_on_bt_scan_done(void);
void popups_on_airdrop_updated(void);
void popups_shutdown(void);
HWND popups_get_last_fg_window(void);
int  popups_snapshot(const wchar_t *png_path);

/* Things the bar and dock open (actions.c): Lares' own parts once they are
 * on, else Windows' own. */
void action_open_start(void);
void action_open_files(const wchar_t *folder);
void action_open_terminal(void);
void action_open_settings(void);
void action_open_lares(const wchar_t *section);
/* airdrop.c: the bar's AirDrop button and panel on Lares' own sharing code
 * (issue #155). Its updates arrive as LARES_SHARE_UPDATED_MSG. */
#define WM_LARES_SHARE_UPDATED LARES_SHARE_UPDATED_MSG
void airdrop_get(LaresShareState *out);
bool airdrop_active(void);
bool airdrop_pending(void);
void airdrop_panel_opened(void);
void airdrop_panel_closed(void);
void airdrop_on_updated(void);
void airdrop_apply_settings(void);
void airdrop_shutdown(void);
void airdrop_set_mode(int mode);
void airdrop_answer(bool accept);
void airdrop_dismiss(void);
void airdrop_cancel_send(void);
void airdrop_open_lares(void);
void airdrop_show_saved(void);
void airdrop_send_to(const char *key, HWND owner);
void airdrop_set_test_state(const LaresShareState *s);
void action_open_processes(void);
/* 0 a region, 1 the window in front, 2 the whole screen. */
void action_screenshot(int mode);
bool action_open_viewer(int kind, const wchar_t *path);

#endif /* LARES_SHELL_H */
