// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "cli.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lares {

enum class RemoteItem : int {
    Screen = 0,
    Sunshine = 1,
    Tailscale = 2,
};
inline constexpr int kRemoteItemCount = 3;

enum class RemoteMode : int {
    Home = 0,
    Remote = 1,
};
inline constexpr int kRemoteModeCount = 2;

struct ScreenMode {
    int w = 0;
    int h = 0;
    int rate = 60;
    bool current = false;
    bool preferred = false;

    std::string spec() const;
    std::string label() const;
};

struct RemoteLive {
    bool screen_known = false;
    int w = 0;
    int h = 0;
    int rate = 60;
    std::vector<ScreenMode> modes;
    int sunshine = -1;   // -1 unknown, 0 off, 1 on
    int tailscale = -1;  // -1 unknown, 0 off, 1 on
    bool streaming = false;
    bool tailscale_installed = false;
    bool tailscale_signed_in = false;
    std::string tailscale_host;
    std::string tailscale_ip;
    bool sunshine_installed = false;
    bool paired = false;
};

struct RemoteConfig {
    RemoteMode mode = RemoteMode::Home;
    bool mode_known = false;
    bool ready = false;
    std::string values[kRemoteModeCount][kRemoteItemCount];

    RemoteConfig();
    const std::string& value(RemoteMode m, RemoteItem i) const
    {
        return values[static_cast<int>(m)][static_cast<int>(i)];
    }
    std::string& value(RemoteMode m, RemoteItem i)
    {
        return values[static_cast<int>(m)][static_cast<int>(i)];
    }
};

// Pure functions shared with unit tests (mirrors src/remote_model.c).
const char* remote_item_name(RemoteItem i);
const char* remote_mode_name(RemoteMode m);
const RemoteItem* remote_apply_order(RemoteMode m);
bool parse_screen_spec(const std::string& s, int& w, int& h, double& rate);
std::string describe_screen_mode(int w, int h, double rate);
std::string remote_saved_value(RemoteMode m, RemoteItem i, const std::string& saved);
std::string remote_own_size(const RemoteLive& live);
std::string remote_describe(RemoteItem i, const std::string& v);
std::string remote_describe_live(const RemoteLive& live, RemoteItem i);
bool remote_matches(const RemoteLive& live, RemoteItem i, const std::string& v);
bool remote_setup_done(const RemoteLive& live);

RemoteConfig parse_lares_ini(const std::string& text);
std::string serialize_lares_ini(const RemoteConfig& cfg);
bool sunshine_state_has_paired_device(const std::string& json);
bool sunshine_log_is_streaming(const std::string& log_tail);

#ifdef _WIN32
inline constexpr UINT kMsgRemoteLive = WM_APP + 2;
inline constexpr UINT kMsgRemoteStepDone = WM_APP + 3;

struct RemoteStepResult {
    RemoteItem item = RemoteItem::Screen;
    bool ok = true;
    bool needs_permission = false;
    std::string message;
};

class RemoteController {
public:
    RemoteController(bool dry_run, bool dry_stream);
    ~RemoteController();

    RemoteController(const RemoteController&) = delete;
    RemoteController& operator=(const RemoteController&) = delete;

    void set_notify_hwnd(HWND hwnd) { notify_hwnd_.store(hwnd); }
    void set_wanted(bool on);
    void request_refresh();

    RemoteConfig config() const;
    void set_config(const RemoteConfig& cfg, bool save_to_disk = true);
    RemoteLive live() const;

    // Reads live state synchronously once (used by CLI and worker thread).
    RemoteLive read_live_once();

    // Applies a single item synchronously (used by CLI and async apply worker).
    RemoteStepResult apply_item_sync(RemoteItem item, const std::string& value);

    // Starts an asynchronous mode or single-item apply on a worker thread,
    // posting kMsgRemoteStepDone to notify_hwnd_ for each item.
    void apply_mode_async(RemoteMode mode);
    void apply_single_async(RemoteItem item, const std::string& value);
    bool is_applying() const { return applying_.load(); }

    // One-time UAC helper granting interactive users start/stop rights on SunshineService.
    bool allow_sunshine_service();

private:
    void poll_loop();
    void load_config_from_disk();
    void save_config_to_disk(const RemoteConfig& cfg);

    bool dry_run_ = false;
    bool dry_stream_ = false;
    std::wstring ini_path_;

    mutable std::mutex mu_;
    RemoteConfig cfg_;
    RemoteLive live_;
    bool dry_seeded_ = false;

    std::atomic<HWND> notify_hwnd_{ nullptr };
    std::atomic<bool> wanted_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> applying_{ false };
    HANDLE wake_event_ = nullptr;
    std::thread poll_thread_;
    std::thread apply_thread_;
};

int run_remote_cli(CliAction action, bool dry_run, bool dry_stream, std::string& out_text);
#endif

} // namespace lares
