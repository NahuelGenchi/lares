// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* AirDrop on Windows (docs/WINDOWS.md). Windows gives no program the raw
 * 802.11 frames AirDrop's link needs, so the link and everything above it
 * run in WSL as they do on Linux: `lares-share` (src/sharecli/main.c) is
 * src/share.c without a window, and usbipd-win lends it the USB Wi-Fi
 * adapter while AirDrop is on. This is the Windows end of that: what's
 * still to set up, the session's process and what it says, and the paths
 * between the two systems. */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <atomic>
#include <mutex>
#include <thread>
#include <windows.h>
#endif

namespace lares {

// ------------------------------------------------- plain data, for the tests

enum class ShareLink { Off, Starting, Up, Stopping, Failed };
enum class ShareDeviceState { Found, Asking, Sending, Sent, Declined, Failed };
enum class ShareIncomingState { Asking, Receiving, Done, Failed };

struct ShareDevice {
    std::string key, name, model, message;
    ShareDeviceState state = ShareDeviceState::Found;
    uint64_t sent = 0, total = 0;
};

struct ShareIncoming {
    unsigned id = 0;
    ShareIncomingState state = ShareIncomingState::Asking;
    bool links = false;
    std::string sender, message;
    std::vector<std::string> names;   // file names, or URLs
    std::vector<std::string> saved;   // where they were saved, as WSL sees it, once done
    uint64_t done = 0, total = 0;
};

/* One block of what lares-share writes. */
struct ShareSession {
    bool ready = false;
    ShareLink link = ShareLink::Off;
    bool receiving = false;
    int left = 0;            // seconds of receiving left
    bool sending = false;
    bool wake = false;       // the Bluetooth wake-up is wanted
    bool quiet = false;      // the link is up and has heard no Apple device
    std::string name, card, error;
    bool has_incoming = false;
    ShareIncoming incoming;
    std::vector<ShareDevice> devices;

    /* Nothing uses the session: it can end, and the adapter go back. */
    bool idle() const { return !receiving && !sending && !wake && !has_incoming && link == ShareLink::Off; }
};

/* The lines of a block up to, not including, its "end"; false if it has
 * no "state" line. */
bool share_parse_block(const std::vector<std::string>& lines, ShareSession& out);

/* "C:\Users\me\a.txt" as WSL mounts it, "/mnt/c/Users/me/a.txt", and back.
 * Empty for a path that has no drive letter, or isn't under /mnt/<letter>/. */
std::string wsl_path_from_windows(const std::wstring& path);
std::wstring windows_path_from_wsl(const std::string& path);

/* "IMG_0412.HEIC and 1 more": what a sender offers, briefly. */
std::string share_offer_text(const ShareIncoming& in);
/* "1.2 MB", as GLib's g_format_size writes it on Linux. */
std::string share_format_size(uint64_t bytes);
/* Whether a received link may be opened: http and https only. */
bool share_link_opens(const std::string& url);

/* The Apple manufacturer data that wakes AirDrop receivers: the type, the
 * length, and src/airdrop/ble.c's payload around `tag`. */
inline constexpr uint16_t kAppleCompanyId = 0x004C;
inline constexpr size_t kAirdropWakeBytes = 20;
void airdrop_wake_data(const uint8_t tag[3], uint8_t out[kAirdropWakeBytes]);

/* The USB Wi-Fi adapter the WSL side knows how to start: Realtek RTL8188EU. */
inline constexpr wchar_t kShareAdapterId[] = L"USB\\VID_0BDA&PID_8179";
inline constexpr wchar_t kShareAdapterUsbipd[] = L"0bda:8179";

#ifdef _WIN32
inline constexpr UINT kMsgShareUpdated = WM_APP + 5;

enum class ShareSetup {
    NoWsl,        // no WSL distribution
    NoAdapter,    // the adapter isn't plugged in
    NeedsSetup,   // usbipd-win, sharing the adapter, or the programs in WSL
    Ready,
};

/* What the Share page shows. */
struct ShareView {
    ShareSetup setup = ShareSetup::NeedsSetup;
    bool setup_busy = false;
    std::string setup_error;
    bool running = false;          // a session is on
    ShareSession s;
    ULONGLONG left_at_ms = 0;      // when `s.left` was said, for the countdown
    std::vector<std::wstring> files;   // what's being sent
    std::string error;             // why a session ended by itself, or files couldn't be sent
};

/* Whether the adapter is plugged in: a cheap check, for the sidebar. */
bool share_adapter_present();

class ShareManager {
public:
    explicit ShareManager(bool dry_run);
    ~ShareManager();
    ShareManager(const ShareManager&) = delete;
    ShareManager& operator=(const ShareManager&) = delete;

    void set_notify_hwnd(HWND hwnd) { notify_hwnd_.store(hwnd); }
    ShareView view() const;

    /* Looks again at what's set up: registry and files only, WSL isn't started. */
    void refresh();
    /* Installs what's missing, on a worker thread: usbipd-win and sharing
     * the adapter (asks for administrator rights), then the WSL side. */
    void run_setup();

    void set_receiving(bool on);
    void answer(bool accept);
    void dismiss_incoming();
    void set_looking(bool on);
    void send(const std::vector<std::wstring>& paths);
    void stop_sending();
    void send_to(const std::string& key);
    void cancel_send();
    /* For the window, on kMsgShareUpdated: starts the session that was
     * asked for while the one before was ending. */
    void resume();

private:
    void command(const std::string& line);
    bool start_session();
    void read_loop(HANDLE from, HANDLE proc);
    void end_session();
    void notify();
    void setup_worker();
    void dry_command(const std::string& line);

    bool dry_run_ = false;
    std::atomic<HWND> notify_hwnd_{ nullptr };

    mutable std::mutex mu_;
    ShareView view_;
    std::vector<std::string> pending_;   // commands for the session that follows one that's ending
    bool armed_ = false;                 // the session has been used: it may end once idle
    bool quitting_ = false;              // the session was told to end
    bool restart_ = false;               // and another is to follow it
    bool closing_ = false;

    HANDLE proc_ = nullptr;
    HANDLE to_child_ = nullptr;
    HANDLE from_child_ = nullptr;
    std::thread reader_;
    std::thread setup_thread_;
};
#endif

} // namespace lares
