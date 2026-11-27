// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Sharing with Apple devices on Windows (issue #109): src/share.c's model
 * on the native link. The adapter is used only while receiving is on or a
 * send is waiting, and the link stays a minute after its last use. The
 * Bluetooth wake-up is lares.exe's (win/src/ble_wake.cpp): the state's
 * `wake` asks for it. */
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "loop.h"

namespace lares {

enum class Setup { NoCard, NeedsSetup, Ready };
enum class LinkState { Off, Starting, Up, Stopping, Failed };
enum class DiscoverMode { Off, ContactsOnly, Everyone };
enum class DeviceState { Found, Asking, Sending, Sent, Declined, Failed };
enum class IncomingState { Asking, Receiving, Done, Failed };

struct Device {
    std::string key;            /* the mDNS instance, or "awdl:<MAC>" for DISC-5 */
    std::string name, model;
    std::string address;        /* "fe80::…%awdl0" */
    uint16_t port = 0;
    unsigned flags = 0;
    DeviceState state = DeviceState::Found;
    uint64_t sent = 0, total = 0;
    std::string message;        /* why a send failed */
};

struct Incoming {
    unsigned id = 0;
    IncomingState state = IncomingState::Asking;
    std::string sender;
    std::string sender_model;
    std::string thumb_path;            /* UTF-8 path to FileIcon preview, if any */
    std::string matched_hash;          /* matched contact SHA-256 hash, if any */
    bool links = false;
    std::vector<std::string> names;    /* file names, or URLs */
    std::vector<std::string> offered_links; /* URLs offered alongside files in mixed /Ask */
    std::vector<std::string> saved;    /* saved paths, once done */
    uint64_t done = 0, total = 0;
    std::string message;
};

struct ShareState {
    Setup setup = Setup::NoCard;
    std::string card_text;
    LinkState link = LinkState::Off;
    std::string link_error;
    bool quiet = false;         /* up a while, and no Apple device heard */
    std::string name;           /* how others see this PC */
    DiscoverMode mode = DiscoverMode::Off;
    bool receiving = false;     /* mode != DiscoverMode::Off */
    bool original_media = false; /* ReceiverMediaCapabilities: false=JPEG/H.264, true=HEIC/HEVC */
    Loop::Clock::time_point receive_until{};
    std::unique_ptr<Incoming> incoming;
    bool browsing = false;      /* continuously finding nearby devices while page is open */
    bool sending = false;
    std::vector<std::wstring> paths;
    std::vector<std::string> send_urls;
    std::vector<Device> devices;
};

class Share {
public:
    explicit Share(Loop& loop);
    ~Share();
    const ShareState& state() const { return st_; }
    /* `fn` runs whenever anything in the state changed. */
    void watch(std::function<void()> fn) { fn_ = std::move(fn); }
    bool looking() const { return looking_; }

    void refresh();
    void set_mode(DiscoverMode mode);
    void set_receiving(bool on);
    void set_original_media(bool original);
    void set_contact_hashes(std::set<std::string> hashes);
    void set_browsing(bool on);
    void answer(bool accept);
    void dismiss_incoming();
    void set_looking(bool on);
    void set_sending(const std::vector<std::wstring>& paths);
    void set_sending_urls(const std::vector<std::string>& urls);
    void send_to(const std::string& key);
    void send_files_to(const std::string& key, const std::vector<std::wstring>& paths);
    void send_urls_to(const std::string& key, const std::vector<std::string>& urls);
    void dismiss_device(const std::string& key);
    void cancel_send();
    /* Ends the link now, rather than after its minute. */
    void stop_link();

    struct Impl;

private:
    Loop& loop_;
    ShareState st_;
    std::function<void()> fn_;
    bool looking_ = false;
    std::unique_ptr<Impl> d_;
    friend struct Impl;
};

} // namespace lares
