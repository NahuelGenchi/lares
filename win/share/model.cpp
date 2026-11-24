// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* src/share.c on Windows (issue #109), with what PR #108 added there: the
 * same link lifetime, receive window, quiet hint, DISC-5 retries and
 * device list, on the native link, lwIP and Schannel. Linux's card search,
 * Allow step and dry run belong to lares.exe here. */
#include "model.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include "mdns_io.h"
#include "session.h"
#include "tls.h"

extern "C" {
#include "link.h"
#include "net.h"
#include "rtl_usb.h"
}

namespace lares {

namespace {

constexpr unsigned kReceiveSeconds = 600;   /* as an iPhone's Everyone for 10 minutes */
constexpr unsigned kDiscoverRetryMs = 1000; /* DISC-5: a phone is there about once a second */
constexpr unsigned kLingerSeconds = 60;     /* the link waits this long for a next use */
constexpr unsigned kQuietSeconds = 20;      /* no Apple device heard in this long: say so */
constexpr uint8_t kChannel = 6;             /* the adapter is 2.4 GHz only */
constexpr int kFirmwareResource = 101;      /* firmware.rc */
constexpr char kZone[] = "awdl0";           /* the zone written in addresses, as on Linux */

std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string mac_text(const awdl_mac& m)
{
    char out[18];
    awdl_mac_format(&m, out);
    return out;
}

/* A peer's link-local address from its MAC (modified EUI-64, NET-1). */
std::string link_local(const awdl_mac& m)
{
    uint8_t a[16];
    awdl_link_local(&m, a);
    char buf[64];
    std::snprintf(buf, sizeof buf, "fe80::%x:%x:%x:%x%%%s", (a[8] << 8) | a[9], (a[10] << 8) | a[11],
                  (a[12] << 8) | a[13], (a[14] << 8) | a[15], kZone);
    return buf;
}

bool mac_from_text(const std::string& s, awdl_mac& m)
{
    unsigned b[6];
    if (std::sscanf(s.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; i++) m.a[i] = static_cast<uint8_t>(b[i]);
    return true;
}

/* How others see this PC: its name, as Windows' Settings shows it. */
std::string computer_name()
{
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n) && n) return utf8(std::wstring(buf, n));
    return "Windows PC";
}

std::wstring download_folder()
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

std::string random_id()
{
    uint32_t r[2] = {};
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(r), sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%06x%06x", r[0] & 0xffffff, r[1] & 0xffffff);
    return buf;
}

std::vector<uint8_t> firmware()
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kFirmwareResource), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(nullptr, res) : nullptr;
    const auto* p = h ? static_cast<const uint8_t*>(LockResource(h)) : nullptr;
    DWORD n = res ? SizeofResource(nullptr, res) : 0;
    return p && n ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>();
}

struct Candidate {
    std::string key, address;
    uint16_t port = 0;
    unsigned flags = 0;
    bool trying = false, listed = false;
};

} // namespace

struct Share::Impl {
    Share& s;
    std::string service_id;     /* MDNS-3: our instance, and SenderID */
    std::string label;          /* "lares-<service ID>": AWDL's ARPA TLV and mDNS host */
    std::vector<uint8_t> fw;

    /* The link and the interface on it. */
    unsigned gen = 0;           /* which link the callbacks are about */
    struct link* link = nullptr;
    std::mutex live_mu;         /* lwIP's thread sends through `live` */
    struct link* live = nullptr;
    net_iface* iface = nullptr;
    awdl_mac addr{};
    std::string address;        /* ours, "fe80::…%awdl0" */
    std::thread stopper;

    std::shared_ptr<tls::Identity> cert;
    std::unique_ptr<airdrop::MdnsIo> mdns;
    std::unique_ptr<airdrop::Receiver> receiver;
    unsigned receive_timer = 0, retry = 0, quiet_timer = 0, linger = 0;

    std::map<std::string, Candidate> candidates;
    std::shared_ptr<airdrop::Cancel> discover_cancel, send_cancel;
    std::string send_key;
    std::set<std::string> awdl_peers;   /* MACs the link counts as valid now */
    bool heard = false;

    FILE* log = nullptr;
    Loop::Clock::time_point log_start;

    explicit Impl(Share& share) : s(share) {}

    ShareState& st() { return s.st_; }
    Loop& loop() { return s.loop_; }
    void changed()
    {
        if (s.fn_) s.fn_();
    }
    bool finding() { return st().sending || s.looking_; }

    // ------------------------------------------------------------ the log

    /* When each step happened, in seconds from the link's start, on this PC
     * only: %LOCALAPPDATA%\lares\share.log. No names or addresses. */
    void share_log(const char* fmt, ...)
    {
        if (!log) return;
        double t = std::chrono::duration<double>(Loop::Clock::now() - log_start).count();
        std::fprintf(log, "%7.2f  ", t);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(log, fmt, ap);
        va_end(ap);
        std::fputc('\n', log);
        std::fflush(log);
    }

    void log_open()
    {
        if (log) std::fclose(log);
        log = nullptr;
        wchar_t dir[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
        if (n && n < MAX_PATH - 32) {
            std::wstring d = std::wstring(dir) + L"\\lares";
            CreateDirectoryW(d.c_str(), nullptr);
            log = _wfopen((d + L"\\share.log").c_str(), L"w");
        }
        log_start = Loop::Clock::now();
    }

    void log_session(const char* what)
    {
        if (!log || st().link == LinkState::Off || st().link == LinkState::Failed) log_open();
        share_log("%s", what);
        quiet_restart();
    }

    // ------------------------------------------------------------ quiet

    void quiet_over()
    {
        quiet_timer = 0;
        if (!heard && st().link == LinkState::Up && (st().receiving || finding())) {
            st().quiet = true;
            changed();
        }
    }

    void quiet_watch()
    {
        bool waiting = st().link == LinkState::Up && (st().receiving || finding()) && !heard;
        if (waiting && !quiet_timer && !st().quiet) quiet_timer = loop().after(kQuietSeconds * 1000, [this] { quiet_over(); });
        if (!waiting) {
            loop().cancel(quiet_timer);
            if (st().quiet) {
                st().quiet = false;
                changed();
            }
        }
    }

    void quiet_restart()
    {
        loop().cancel(quiet_timer);
        heard = !awdl_peers.empty();
        st().quiet = false;
    }

    // ------------------------------------------------------------ devices

    Device* device_find(const std::string& key)
    {
        for (Device& d : st().devices)
            if (d.key == key) return &d;
        return nullptr;
    }

    Device* device_at(const std::string& at)
    {
        for (Device& d : st().devices)
            if (!d.address.empty() && _stricmp(d.address.c_str(), at.c_str()) == 0) return &d;
        return nullptr;
    }

    void try_discover(Candidate& c)
    {
        if (c.trying || c.listed || !discover_cancel) return;
        airdrop::Address a;
        if (!airdrop::parse_address(c.address, net_iface_index(iface), a)) return;
        c.trying = true;
        share_log("asking a %s", c.key.compare(0, 5, "awdl:") == 0 ? "nearby AWDL device directly" : "service found over mDNS");
        std::string key = c.key;
        auto cancel = discover_cancel;
        airdrop::discover(loop(), a, c.port, cancel,
                          [this, key, cancel](std::optional<airdrop::Peer> peer, const std::string& err) {
                              discovered(key, cancel, std::move(peer), err);
                          });
    }

    void discovered(const std::string& key, const std::shared_ptr<airdrop::Cancel>& cancel,
                    std::optional<airdrop::Peer> peer, const std::string& err)
    {
        if (cancel != discover_cancel) return;
        auto it = candidates.find(key);
        if (it == candidates.end()) return;
        Candidate& c = it->second;
        c.trying = false;
        if (!peer) {
            share_log("not listed yet: %s", err.empty() ? "it didn't list us" : err.c_str());
            return;
        }
        share_log("listed a device");
        c.listed = true;
        Device* d = device_find(key);
        Device* same = device_at(c.address);
        if (!d && same) {
            d = same;   /* found both over mDNS and on AWDL alone: one device */
            if (c.flags) d->flags = c.flags;
        }
        if (!d) {
            Device nd;
            nd.key = key;
            nd.address = c.address;
            nd.port = c.port;
            nd.flags = c.flags;
            st().devices.push_back(std::move(nd));
            d = &st().devices.back();
        }
        d->name = peer->name;
        d->model = peer->model;
        changed();
    }

    /* Receivers refuse until a Bluetooth advert has woken them: keep asking
     * while looking (DISC-5). */
    void retry_discover()
    {
        retry = loop().after(kDiscoverRetryMs, [this] { retry_discover(); });
        for (auto& [key, c] : candidates) try_discover(c);
    }

    void add_candidate(const std::string& key, const std::string& where, uint16_t port, unsigned flags)
    {
        Candidate& c = candidates[key];
        c.key = key;
        if (c.address.empty() || c.address != where || c.port != port) {
            c.address = where;
            c.port = port;
            c.listed = false;
        }
        if (flags) c.flags = flags;
        if (Device* d = device_find(key); d && flags) d->flags = flags;
        try_discover(c);
    }

    /* DISC-5: asks an AWDL peer directly, at its link-local address. */
    void ask_peer(const std::string& mac)
    {
        awdl_mac m;
        if (mac_from_text(mac, m)) add_candidate("awdl:" + mac, link_local(m), airdrop::kPort, 0);
    }

    void drop_device(const std::string& key)
    {
        candidates.erase(key);
        for (auto it = st().devices.begin(); it != st().devices.end(); ++it) {
            if (it->key != key) continue;
            if (it->state != DeviceState::Asking && it->state != DeviceState::Sending) {
                st().devices.erase(it);
                changed();
            }
            return;
        }
    }

    // ------------------------------------------------------------ receiver

    Incoming* incoming_for(unsigned id)
    {
        return st().incoming && st().incoming->id == id ? st().incoming.get() : nullptr;
    }

    airdrop::ReceiverCallbacks receiver_callbacks()
    {
        airdrop::ReceiverCallbacks cb;
        cb.offer = [this](const airdrop::Offer& o) {
            Incoming* cur = st().incoming.get();
            /* One at a time: a second sender is declined while the first is on. */
            if (cur && (cur->state == IncomingState::Asking || cur->state == IncomingState::Receiving)) {
                if (receiver) receiver->answer(o.id, false);
                return;
            }
            share_log("a device asks to send");
            auto in = std::make_unique<Incoming>();
            in->id = o.id;
            in->sender = o.sender;
            in->links = o.links;
            in->names = o.names;
            in->total = o.total;
            st().incoming = std::move(in);
            changed();
        };
        cb.withdrawn = [this](unsigned id) {
            Incoming* in = incoming_for(id);
            if (!in || in->state != IncomingState::Asking) return;
            st().incoming.reset();
            changed();
            update_link();
        };
        cb.progress = [this](unsigned id, uint64_t done, uint64_t total) {
            Incoming* in = incoming_for(id);
            if (!in) return;
            in->done = done;
            if (total) in->total = total;
            changed();
        };
        cb.received = [this](unsigned id, bool, const std::vector<std::string>& items) {
            Incoming* in = incoming_for(id);
            if (!in) return;
            in->state = IncomingState::Done;
            share_log("received");
            in->saved = items;
            changed();
            update_link();
        };
        cb.failed = [this](unsigned id, const std::string& message) {
            Incoming* in = incoming_for(id);
            if (!in) return;
            in->state = IncomingState::Failed;
            /* The receiver's messages are its own fixed texts: no names. */
            share_log("receiving failed: %s", message.c_str());
            in->message = message;
            changed();
            update_link();
        };
        cb.trace = [this](unsigned, const std::string& line) { share_log("%s", line.c_str()); };
        return cb;
    }

    // ------------------------------------------------------------ the parts

    /* What runs on the link follows what's on: the receiver and its
     * announcement while receiving, browsing while sending. */
    void parts_apply()
    {
        if (st().link != LinkState::Up) return;
        unsigned zone = net_iface_index(iface);
        if (!mdns) {
            std::string err;
            mdns = airdrop::MdnsIo::start(
                loop(), zone,
                [this](const airdrop::Service& svc) {
                    if (!finding() || svc.instance == service_id) return;
                    share_log("service found over mDNS");
                    add_candidate(svc.instance, svc.address, svc.port, svc.flags);
                },
                [this](const std::string& instance) { drop_device(instance); }, err);
            if (!mdns) st().link_error = "mDNS: " + err;
        }
        bool incoming_on = st().incoming && (st().incoming->state == IncomingState::Asking ||
                                             st().incoming->state == IncomingState::Receiving);
        if ((st().receiving || incoming_on) && !receiver && cert) {
            airdrop::Address a;
            std::string err;
            if (airdrop::parse_address(address, zone, a))
                receiver = airdrop::Receiver::start(loop(), a, st().name, download_folder(), cert, receiver_callbacks(), err);
            else
                err = "Bad address " + address;
            if (!receiver) st().link_error = err;
        } else if (!st().receiving && !incoming_on && receiver) {
            receiver.reset();
        }
        if (mdns) {
            if (st().receiving && receiver)
                mdns->publish(service_id, label, address, receiver->port(), airdrop::kReceiverFlags);
            else
                mdns->publish({}, {}, {}, 0, 0);
            mdns->browse(finding());
        }
        if (finding()) {
            if (!discover_cancel) discover_cancel = std::make_shared<airdrop::Cancel>();
            if (!retry) retry = loop().after(kDiscoverRetryMs, [this] { retry_discover(); });
            /* Peers already met while only receiving are asked too (#107). */
            for (const std::string& mac : std::set<std::string>(awdl_peers)) ask_peer(mac);
        } else {
            if (discover_cancel) discover_cancel->cancel();
            discover_cancel.reset();
            loop().cancel(retry);
            candidates.clear();
        }
        changed();
    }

    void parts_stop()
    {
        receiver.reset();
        if (mdns) mdns->publish({}, {}, {}, 0, 0);
        mdns.reset();
        if (discover_cancel) discover_cancel->cancel();
        discover_cancel.reset();
        loop().cancel(retry);
        candidates.clear();
    }

    // ------------------------------------------------------------ the link

    bool need_link()
    {
        Incoming* in = st().incoming.get();
        return st().setup == Setup::Ready &&
               (st().receiving || finding() ||
                (in && (in->state == IncomingState::Asking || in->state == IncomingState::Receiving)));
    }

    static void link_output(void* ctx, const awdl_mac* dst, const awdl_mac* src, const uint8_t* ip, size_t len)
    {
        auto* d = static_cast<Impl*>(ctx);
        std::lock_guard<std::mutex> lock(d->live_mu);
        if (d->live) link_send(d->live, dst, src, ip, len);
    }

    void on_ready(uint8_t channel)
    {
        char text[64];
        net_iface_address(iface, text);
        address = std::string(text) + "%" + kZone;
        st().link = LinkState::Up;
        share_log("link up on channel %u", channel);
        st().link_error.clear();
        awdl_peers.clear();
        heard = false;
        parts_apply();
        quiet_watch();
    }

    void on_peer(const std::string& mac, bool up)
    {
        share_log("AWDL peer %s", up ? "valid" : "gone");
        if (up) {
            awdl_peers.insert(mac);
            heard = true;
            quiet_watch();
            if (st().link == LinkState::Up && finding()) ask_peer(mac);
        } else {
            awdl_peers.erase(mac);
            if (st().link != LinkState::Up || !finding()) return;
            drop_device("awdl:" + mac);
        }
    }

    void link_start()
    {
        if (!cert) {
            std::string err;
            cert = tls::identity(err);
            if (!cert) {
                st().link = LinkState::Failed;
                st().link_error = err;
                changed();
                return;
            }
        }
        if (fw.empty()) fw = firmware();
        st().link = LinkState::Starting;
        st().link_error.clear();
        changed();
        unsigned g = ++gen;
        link_session_address(&addr);
        net_init();
        iface = net_iface_add(&addr, link_output, this);
        if (!iface || fw.empty()) {
            if (iface) net_iface_remove(iface);
            iface = nullptr;
            st().link = LinkState::Failed;
            st().link_error = fw.empty() ? "This copy of Lares came without the adapter's firmware." : "AirDrop's network didn't start.";
            changed();
            return;
        }
        link_config cfg{};
        cfg.channel = kChannel;
        cfg.addr = addr;
        std::snprintf(cfg.name, sizeof cfg.name, "%s", label.c_str());
        cfg.firmware = fw.data();
        cfg.firmware_len = fw.size();
        link_callbacks cb{};
        cb.ctx = this;
        cb.ready = [](void* ctx, uint8_t channel, const awdl_mac*) {
            auto* d = static_cast<Impl*>(ctx);
            unsigned g2 = d->gen;
            d->loop().post([d, g2, channel] {
                if (g2 == d->gen && d->st().link == LinkState::Starting) d->on_ready(channel);
            });
        };
        cb.failed = [](void* ctx, const char* message) {
            auto* d = static_cast<Impl*>(ctx);
            unsigned g2 = d->gen;
            std::string m = message ? message : "";
            d->loop().post([d, g2, m] {
                if (g2 == d->gen) d->link_failed(m);
            });
        };
        cb.peer = [](void* ctx, const awdl_mac* mac, const uint8_t*, size_t, bool up) {
            auto* d = static_cast<Impl*>(ctx);
            unsigned g2 = d->gen;
            std::string m = mac_text(*mac);
            d->loop().post([d, g2, m, up] {
                if (g2 == d->gen) d->on_peer(m, up);
            });
        };
        cb.deliver = [](void* ctx, const awdl_mac*, const awdl_mac*, const uint8_t* ip, size_t len) {
            auto* d = static_cast<Impl*>(ctx);
            if (d->iface) net_iface_input(d->iface, ip, len);
        };
        link = ::link_start(&cfg, &cb);
        if (!link) {
            net_iface_remove(iface);
            iface = nullptr;
            st().link = LinkState::Failed;
            st().link_error = "The AirDrop link didn't start.";
            changed();
            return;
        }
        std::lock_guard<std::mutex> lock(live_mu);
        live = link;
        (void)g;
    }

    /* Stops the link's thread off the loop (it powers the chip down), then
     * says so on it. */
    void link_end(bool failed, const std::string& why)
    {
        unsigned g = ++gen;     /* nothing more from this link counts */
        struct link* l = link;
        link = nullptr;
        {
            std::lock_guard<std::mutex> lock(live_mu);
            live = nullptr;
        }
        if (stopper.joinable()) stopper.join();
        stopper = std::thread([this, l, g, failed, why] {
            ::link_stop(l);
            loop().post([this, g, failed, why] {
                if (g != gen) return;
                link_ended(failed, why);
            });
        });
    }

    void link_ended(bool failed, const std::string& why)
    {
        if (iface) net_iface_remove(iface);
        iface = nullptr;
        address.clear();
        awdl_peers.clear();
        st().link = failed ? LinkState::Failed : LinkState::Off;
        quiet_watch();
        if (failed) {
            if (st().link_error.empty()) st().link_error = why.empty() ? "The AirDrop link stopped." : why;
            /* Nothing can work without the link: turn receiving off. */
            st().receiving = false;
            loop().cancel(receive_timer);
        }
        changed();
        if (!failed) update_link();
    }

    void link_failed(const std::string& message)
    {
        share_log("link failed");
        parts_stop();
        st().link_error = message;
        st().link = LinkState::Stopping;
        changed();
        link_end(true, message);
    }

    void link_stop()
    {
        share_log("link stopping");
        parts_stop();
        if (!link) {
            st().link = LinkState::Off;
            changed();
            return;
        }
        st().link = LinkState::Stopping;
        changed();
        link_end(false, {});
    }

    void linger_over()
    {
        linger = 0;
        if (!need_link() && (st().link == LinkState::Up || st().link == LinkState::Starting)) link_stop();
    }

    void update_link()
    {
        bool need = need_link();
        if (need) loop().cancel(linger);
        if (need && (st().link == LinkState::Off || st().link == LinkState::Failed)) {
            if (st().link == LinkState::Failed && !st().receiving && !finding()) return;
            link_start();
        } else if (!need && (st().link == LinkState::Up || st().link == LinkState::Starting)) {
            /* What used it stops at once; the link itself waits a minute. */
            if (st().link == LinkState::Up) parts_apply();
            if (!linger) linger = loop().after(kLingerSeconds * 1000, [this] { linger_over(); });
        } else if (st().link == LinkState::Up) {
            parts_apply();
        }
        quiet_watch();
    }

    void check_setup()
    {
        /* While the link has it, the adapter is ours and ready. */
        if (link) return;
        switch (rtl_usb_find()) {
        case RTL_USB_WINUSB: st().setup = Setup::Ready; break;
        case RTL_USB_OTHER_DRIVER: st().setup = Setup::NeedsSetup; break;
        default: st().setup = Setup::NoCard; break;
        }
        st().card_text = st().setup == Setup::NoCard ? std::string() : "USB Wi-Fi adapter, 2.4 GHz only";
    }
};

// ------------------------------------------------------------------ API

Share::Share(Loop& loop) : loop_(loop), d_(std::make_unique<Impl>(*this))
{
    st_.name = computer_name();
    d_->service_id = random_id();
    /* Not the machine's name, which would tell strangers nearby: as owly
     * does, a label from the session's service ID. */
    d_->label = "lares-" + d_->service_id;
    d_->check_setup();
}

Share::~Share()
{
    d_->parts_stop();
    if (d_->link) d_->link_end(false, {});
    if (d_->stopper.joinable()) d_->stopper.join();
    if (d_->iface) net_iface_remove(d_->iface);
    if (d_->log) std::fclose(d_->log);
}

void Share::refresh()
{
    if (st_.link != LinkState::Off && st_.link != LinkState::Failed) return;
    d_->check_setup();
    d_->changed();
}

void Share::set_receiving(bool on)
{
    if (on && st_.setup != Setup::Ready) return;
    st_.receiving = on;
    loop_.cancel(d_->receive_timer);
    if (on) {
        d_->log_session("receiving on");
        st_.receive_until = Loop::Clock::now() + std::chrono::seconds(kReceiveSeconds);
        d_->receive_timer = loop_.after(kReceiveSeconds * 1000, [this] {
            d_->receive_timer = 0;
            set_receiving(false);
        });
        if (st_.link == LinkState::Failed) st_.link = LinkState::Off;
    }
    d_->changed();
    d_->update_link();
}

void Share::answer(bool accept)
{
    Incoming* in = st_.incoming.get();
    if (!in || in->state != IncomingState::Asking) return;
    if (d_->receiver) d_->receiver->answer(in->id, accept);
    if (accept) in->state = IncomingState::Receiving;
    else st_.incoming.reset();
    d_->changed();
    d_->update_link();
}

void Share::dismiss_incoming()
{
    Incoming* in = st_.incoming.get();
    if (!in || in->state == IncomingState::Asking || in->state == IncomingState::Receiving) return;
    st_.incoming.reset();
    d_->changed();
}

void Share::set_looking(bool on)
{
    if (on == looking_ || (on && st_.setup != Setup::Ready)) return;
    looking_ = on;
    if (on) {
        d_->log_session("looking while files are picked");
        if (st_.link == LinkState::Failed) st_.link = LinkState::Off;
    } else if (!st_.sending) {
        st_.devices.clear();
        d_->candidates.clear();
    }
    d_->changed();
    d_->update_link();
}

void Share::set_sending(const std::vector<std::wstring>& paths)
{
    bool on = !paths.empty();
    if (on && st_.setup != Setup::Ready) return;
    if (!on) cancel_send();
    st_.paths = paths;
    /* What was found while the files were picked stays. */
    bool keep = on && looking_;
    looking_ = false;
    st_.sending = on;
    if (on) d_->log_session(keep ? "files picked" : "sending");
    if (!keep) {
        st_.devices.clear();
        d_->candidates.clear();
    }
    if (on && st_.link == LinkState::Failed) st_.link = LinkState::Off;
    d_->changed();
    d_->update_link();
}

void Share::send_to(const std::string& key)
{
    Device* d = d_->device_find(key);
    if (!d || st_.paths.empty() || !d_->send_key.empty()) return;
    airdrop::SendRequest req;
    if (!airdrop::parse_address(d->address, d_->iface ? net_iface_index(d_->iface) : 0, req.addr)) return;
    d->state = DeviceState::Asking;
    d_->share_log("asking to send");
    d->sent = d->total = 0;
    d->message.clear();
    d_->send_key = key;
    d_->changed();
    auto cancel = std::make_shared<airdrop::Cancel>();
    d_->send_cancel = cancel;
    req.port = d->port;
    req.flags = d->flags;
    req.name = st_.name;
    req.id = d_->service_id;
    req.paths = st_.paths;
    Impl* impl = d_.get();
    airdrop::send(
        loop_, req, cancel,
        [impl, key](bool accepted, uint64_t sent, uint64_t total) {
            Device* dev = impl->device_find(key);
            if (!dev) return;
            if (accepted && dev->state == DeviceState::Asking) {
                dev->state = DeviceState::Sending;
                impl->share_log("they accepted");
            }
            dev->sent = sent;
            dev->total = total;
            impl->changed();
        },
        [impl, key, cancel](bool ok, bool declined, const std::string& err) {
            Device* dev = impl->device_find(key);
            impl->share_log(ok ? "sent" : "send ended without sending");
            if (dev) {
                dev->state = ok ? DeviceState::Sent : declined ? DeviceState::Declined : DeviceState::Failed;
                dev->message = err;
            }
            if (impl->send_cancel == cancel) {
                impl->send_cancel.reset();
                impl->send_key.clear();
            }
            impl->changed();
        });
}

void Share::cancel_send()
{
    if (d_->send_cancel) d_->send_cancel->cancel();
    d_->send_cancel.reset();
    d_->send_key.clear();
    for (Device& d : st_.devices)
        if (d.state == DeviceState::Asking || d.state == DeviceState::Sending) d.state = DeviceState::Found;
    d_->changed();
}

void Share::stop_link()
{
    loop_.cancel(d_->linger);
    if (st_.link == LinkState::Up || st_.link == LinkState::Starting) d_->link_stop();
}

} // namespace lares
