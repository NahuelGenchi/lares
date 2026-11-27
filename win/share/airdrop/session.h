// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* AirDrop's exchange over TLS on Windows (issue #109), as src/airdrop/
 * session.c on Linux: the receiver's server, and the sender's /Discover,
 * /Ask and /Upload, over lwIP's sockets and Schannel. Each connection runs
 * in a worker thread with blocking I/O and timeouts; what the Share model
 * needs to know is posted to its loop. */
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "loop.h"
#include "tls.h"

namespace lares::airdrop {

constexpr uint16_t kPort = 8770;            /* MDNS-1's conventional port, and DISC-5's */
constexpr unsigned kPorts = 10;             /* 8770 to 8779 */
constexpr unsigned kReceiverFlags = 0x089;  /* URL sharing, mixed types, Discover (MDNS-5) */

/* A peer's link-local address and the zone (lwIP interface) it's on, from
 * text such as "fe80::…%awdl0" (the zone's name is ignored: there's one). */
struct Address {
    uint8_t ip[16] = {};
    unsigned zone = 0;
};
bool parse_address(const std::string& text, unsigned zone, Address& out);

/* Stops what a worker is doing: its blocking reads and writes look at it
 * every quarter of a second. */
class Cancel {
public:
    void cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }

private:
    std::atomic<bool> cancelled_{ false };
};

// ---------------------------------------------------------------- receiver

struct Offer {
    unsigned id = 0;
    std::string sender;                     /* untrusted: shown escaped (SEC-4) */
    std::string sender_model;               /* SenderModelName, e.g. "iPhone17,1" */
    std::string matched_hash;               /* matched contact SHA-256 hash when SenderRecordData matched */
    bool links = false;                     /* true for link-only offers (no /Upload follows); `names` holds URLs */
    std::vector<std::string> names;         /* file names, or URLs when `links` is true */
    std::vector<std::string> items;         /* offered URLs (populated for link-only and mixed offers) */
    std::vector<uint8_t> file_icon;         /* FileIcon preview bytes from /Ask, bounded */
    uint64_t total = 0;
};

struct ReceiverCallbacks {
    std::function<void(const Offer&)> offer;
    /* The sender gave up, or the prompt timed out. */
    std::function<void(unsigned id)> withdrawn;
    std::function<void(unsigned id, uint64_t done, uint64_t total)> progress;
    /* Saved files' paths (UTF-8), or an accepted link share's URLs. */
    std::function<void(unsigned id, bool links, const std::vector<std::string>& items)> received;
    std::function<void(unsigned id, const std::string& message)> failed;
    /* Formats, sizes and counts for the log; never a name. */
    std::function<void(unsigned id, const std::string& line)> trace;
};

class Receiver {
public:
    /* Listens on `addr` only, on kPort or the next free one of kPorts, else
     * any. Files go to `folder`, through a staging folder inside it, never
     * over an existing file. Callbacks run on `loop`. Null with `err`. */
    static std::unique_ptr<Receiver> start(Loop& loop, const Address& addr, const std::string& name,
                                           const std::wstring& folder, std::shared_ptr<tls::Identity> id,
                                           ReceiverCallbacks cb, std::string& err);
    ~Receiver();   /* stops listening, cuts connections; no callback after */
    uint16_t port() const;
    void answer(unsigned id, bool accept);
    /* Updates Contacts Only filtering and allowed contact SHA-256 hashes live (ID-3). */
    void set_policy(bool contacts_only, std::set<std::string> contact_hashes);
    /* Chooses between compatibility transcoding (false: JPEG/H.264, DISC-3) and
     * original iOS media formats (true: HEIC/HEVC) in ReceiverMediaCapabilities. */
    void set_original_media(bool original);
    /* Overrides trusted CA roots for CMS SenderRecordData verification in
     * loopback unit tests. Empty (the default) uses Apple's embedded Root CAs. */
    void set_trusted_roots_for_test(std::vector<std::vector<uint8_t>> roots_der);

    struct Impl;

private:
    explicit Receiver(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

// ------------------------------------------------------------------ sender

struct Peer {
    std::string name, model;
    std::string media_capabilities;         /* ReceiverMediaCapabilities JSON, when present */
};

/* POST /Discover on a connection of its own (§6.3). `done` runs on `loop`
 * with the receiver's name and model when it lists us (DISC-1), nothing
 * when it doesn't, and an error message when it failed; never once
 * cancelled. `sender_record`, if non-empty, is sent as SenderRecordData. */
void discover(Loop& loop, const Address& addr, uint16_t port, std::shared_ptr<Cancel> cancel,
              std::function<void(std::optional<Peer>, const std::string& err)> done,
              std::vector<uint8_t> sender_record = {});

struct SendRequest {
    Address addr;
    uint16_t port = kPort;
    unsigned flags = 0;                     /* the receiver's TXT flags, 0 when unknown (UP-1b) */
    std::string name;                       /* our SenderComputerName */
    std::string id;                         /* our service ID, as SenderID (MDNS-3) */
    std::vector<std::wstring> paths;        /* files and folders to send */
    std::vector<std::string> items;         /* URLs to send in Items (http/https) */
    std::vector<uint8_t> file_icon;         /* optional explicit FileIcon preview bytes (bounded) */
};

/* /Ask and, once accepted, streamed chunked /Upload on one fresh connection
 * (HTTP-1, UP-1a) with no pre-/Ask disk spooling. Link-only shares complete
 * on /Ask 200 OK (ASK-4). `progress` and `done` run on `loop`, never once
 * cancelled; `declined` when the receiver said no or didn't answer. */
void send(Loop& loop, const SendRequest& req, std::shared_ptr<Cancel> cancel,
          std::function<void(bool accepted, uint64_t sent, uint64_t total)> progress,
          std::function<void(bool ok, bool declined, const std::string& err)> done);

} // namespace lares::airdrop
