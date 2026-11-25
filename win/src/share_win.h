// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* AirDrop on Windows (docs/WINDOWS.md, issue #109). Windows gives no
 * program the raw 802.11 frames AirDrop's link needs, so lares-share.exe
 * (win/share/) drives a USB Wi-Fi adapter itself, over Windows' own WinUSB
 * driver, and runs AWDL, IPv6, TLS and AirDrop above it. This is lares.exe's
 * end of that: what's still to set up, the session's process and what it
 * says. */
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
enum class ShareDiscoverMode { Off, ContactsOnly, Everyone };
enum class ShareDeviceState { Found, Asking, Sending, Sent, Declined, Failed };
enum class ShareIncomingState { Asking, Receiving, Done, Failed };
enum class ShareDeviceKind { Phone, Tablet, Laptop, Desktop, Watch, Other };

struct ShareDevice {
    std::string key, name, model, message;
    ShareDeviceState state = ShareDeviceState::Found;
    uint64_t sent = 0, total = 0;
};

struct ShareIncoming {
    unsigned id = 0;
    ShareIncomingState state = ShareIncomingState::Asking;
    bool links = false;
    std::string sender, sender_model, thumb_path, matched_hash, message;
    std::vector<std::string> names;   // file names, or URLs
    std::vector<std::string> saved;   // where they were saved (UTF-8), once done
    uint64_t done = 0, total = 0;
};

/* A saved AirDrop contact: matches incoming SenderRecordData hashes in
 * Contacts Only mode, and supplies a display name and profile photo. */
struct ShareContact {
    std::string name;
    std::string device;               // optional device name match (e.g. "Susuin")
    std::string email;                // optional email for SHA-256 Contacts Only match
    std::string phone;                // optional phone for SHA-256 Contacts Only match
    std::string photo;                // optional UTF-8 local image path for avatar
};

/* One block of what lares-share writes. */
struct ShareSession {
    bool ready = false;
    ShareLink link = ShareLink::Off;
    ShareDiscoverMode mode = ShareDiscoverMode::Off;
    bool receiving = false;
    int left = 0;            // seconds of Everyone for 10 Minutes left
    bool browsing = false;   // actively discovering nearby AirDrop devices
    bool sending = false;
    bool wake = false;       // the Bluetooth wake-up is wanted
    bool quiet = false;      // the link is up and has heard no Apple device
    std::string name, card, error;
    bool has_incoming = false;
    ShareIncoming incoming;
    std::vector<ShareDevice> devices;

    /* Nothing uses the session: it can end, and the adapter go back. */
    bool idle() const { return !receiving && !browsing && !sending && !wake && !has_incoming && link == ShareLink::Off; }
};

/* The lines of a block up to, not including, its "end"; false if it has
 * no "state" line. */
bool share_parse_block(const std::vector<std::string>& lines, ShareSession& out);

/* The "send" command for `paths`, in UTF-8 with a tab before each, and the
 * names it sends in `names`; a path with a tab or a line break in it can't
 * travel in a line and is left out. Empty if none is left. */
std::string share_send_line(const std::vector<std::wstring>& paths, std::vector<std::wstring>* names);
/* Direct send to a specific discovered device key on the radar. */
std::string share_send_to_line(const std::string& key, const std::vector<std::wstring>& paths,
                               std::vector<std::wstring>* names);

/* "IMG_0412.HEIC and 1 more": what a sender offers, briefly. */
std::string share_offer_text(const ShareIncoming& in);
/* "1.2 MB", as GLib's g_format_size writes it on Linux. */
std::string share_format_size(uint64_t bytes);
/* "About 5 seconds" / "About 2 minutes" from transfer progress, or empty. */
std::string share_format_eta(uint64_t done, uint64_t total, uint64_t elapsed_ms);
/* Whether a received link may be opened: http and https only. */
bool share_link_opens(const std::string& url);

/* Friendly device subtitle ("iPhone", "MacBook Air", "iPad") and icon family
 * derived from ReceiverModelName / SenderModelName (and name fallback). */
ShareDeviceKind share_device_kind(const std::string& model, const std::string& name);
std::string share_model_label(const std::string& model, const std::string& name);
/* One or two uppercase initials for a person's monogram avatar. */
std::string share_initials(const std::string& name);

/* SHA-256 contact hashing (SPEC §7) and contacts INI persistence. Never logged (SEC-5). */
std::string share_sha256_hex(const std::string& input);
std::string share_hash_email(const std::string& email);
std::vector<std::string> share_hash_phone_variants(const std::string& phone);
std::vector<std::string> share_contact_hashes(const std::vector<ShareContact>& contacts);
std::vector<ShareContact> parse_contacts_ini(const std::string& text);
std::string serialize_contacts_ini(const std::vector<ShareContact>& contacts);
const ShareContact* share_find_contact(const std::vector<ShareContact>& contacts, const std::string& name,
                                       const std::string& matched_hash = {});

/* The Apple manufacturer data that wakes AirDrop receivers: the type, the
 * length, and src/airdrop/ble.c's payload around `tag`. */
inline constexpr uint16_t kAppleCompanyId = 0x004C;
inline constexpr size_t kAirdropWakeBytes = 20;
void airdrop_wake_data(const uint8_t tag[3], uint8_t out[kAirdropWakeBytes]);

/* The USB Wi-Fi adapter lares-share.exe knows how to drive: Realtek RTL8188EU. */
inline constexpr wchar_t kShareAdapterId[] = L"USB\\VID_0BDA&PID_8179";

#ifdef _WIN32
inline constexpr UINT kMsgShareUpdated = WM_APP + 5;

enum class ShareSetup {
    NoAdapter,    // the adapter isn't plugged in
    NeedsSetup,   // the adapter is on Windows' own driver
    Ready,        // the adapter is on WinUSB, for lares-share.exe
};

/* What the Share page shows. */
struct ShareView {
    ShareSetup setup = ShareSetup::NeedsSetup;
    bool setup_busy = false;
    std::string setup_error;
    bool running = false;          // a session is on
    ShareSession s;
    ULONGLONG left_at_ms = 0;      // when `s.left` was said, for the countdown
    ULONGLONG xfer_start_ms = 0;   // when the active transfer began, for ETA
    std::vector<std::wstring> files;       // what's being sent (basenames)
    std::vector<std::wstring> file_paths;  // full local paths for thumbnail preview
    std::vector<ShareContact> contacts;    // saved AirDrop contacts and profile photos
    std::string error;             // why a session ended by itself, or files couldn't be sent
};

std::wstring share_contacts_path(bool dry_run);
bool load_share_contacts(const std::wstring& path, std::vector<ShareContact>& out);
bool save_share_contacts(const std::wstring& path, const std::vector<ShareContact>& contacts);

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

    /* Looks again at what's set up: the adapter's driver, nothing started. */
    void refresh();
    /* Gives the adapter Windows' WinUSB driver, on a worker thread; asks
     * for administrator rights, one time. */
    void run_setup();
    /* Gives it back to Windows' own driver, the same way. */
    void give_back();

    void set_discover_mode(ShareDiscoverMode mode);
    void set_receiving(bool on);
    void set_browsing(bool on);
    void answer(bool accept);
    void dismiss_incoming();
    void set_looking(bool on);
    void send(const std::vector<std::wstring>& paths);
    void send_files_to(const std::string& key, const std::vector<std::wstring>& paths);
    void stop_sending();
    void send_to(const std::string& key);
    void dismiss_device(const std::string& key);
    void cancel_send();
    /* Saves contacts & profile photos and syncs their SHA-256 hashes to lares-share.exe. */
    void save_contacts(const std::vector<ShareContact>& contacts);
    void set_device_photo(const std::string& device_name, const std::wstring& photo_path);
    /* For the window, on kMsgShareUpdated: starts the session that was
     * asked for while the one before was ending. */
    void resume();

private:
    void command(const std::string& line);
    std::string contacts_command_locked() const;
    bool start_session();
    void read_loop(HANDLE from, HANDLE proc);
    void end_session();
    void notify();
    void setup_worker(bool winusb);
    void dry_command(const std::string& line);

    bool dry_run_ = false;
    std::wstring contacts_path_;
    std::atomic<HWND> notify_hwnd_{ nullptr };

    mutable std::mutex mu_;
    ShareView view_;
    ShareDiscoverMode dry_prev_mode_ = ShareDiscoverMode::Off;
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
