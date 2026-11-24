// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* mDNS on the AWDL link (issue #109): one UDP socket of lwIP's on port 5353
 * in ff02::fb, read on a thread of its own, and the sans-IO engine of
 * formats.h run on the Share model's loop with its deadline as a timer. */
#include "mdns_io.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <windows.h>

#include "formats.h"

extern "C" {
#include "lwip/sockets.h"
}

namespace lares::airdrop {

namespace {

constexpr int kSliceMs = 250;
constexpr char kZone[] = "awdl0";

} // namespace

struct MdnsIo::Impl {
    Loop& loop;
    unsigned zone = 0;
    int fd = -1;
    std::unique_ptr<Mdns> engine;
    unsigned timer = 0;
    std::thread reader;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> stop{ false };
    Loop::Clock::time_point epoch = Loop::Clock::now();

    explicit Impl(Loop& l) : loop(l) {}

    int64_t now_us() const
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Loop::Clock::now() - epoch).count() + 1;
    }

    /* The engine's next deadline as the loop's timer: after every call. */
    void rearm()
    {
        loop.cancel(timer);
        int64_t d = engine->deadline();
        if (d == MDNS_NEVER) return;
        int64_t wait = d - now_us();
        unsigned ms = wait <= 0 ? 0 : static_cast<unsigned>(wait / 1000 + 1);
        timer = loop.after(ms, [this] {
            timer = 0;
            engine->on_timer(now_us());
            rearm();
        });
    }

    void send(const uint8_t* msg, size_t len, const uint8_t* to, uint16_t port)
    {
        sockaddr_in6 sa{};
        sa.sin6_len = sizeof sa;
        sa.sin6_family = AF_INET6;
        sa.sin6_port = lwip_htons(to ? port : MDNS_PORT);
        if (to) std::memcpy(&sa.sin6_addr, to, 16);
        else lwip_inet_pton(AF_INET6, "ff02::fb", &sa.sin6_addr);
        sa.sin6_scope_id = zone;
        lwip_sendto(fd, msg, len, 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    }

    void read_loop()
    {
        std::vector<uint8_t> buf(MDNS_MAX_MESSAGE + 1);
        while (!stop) {
            sockaddr_in6 from{};
            socklen_t len = sizeof from;
            int n = lwip_recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
            if (n <= 0) continue;   /* a quarter second passed, or nothing */
            std::vector<uint8_t> msg(buf.begin(), buf.begin() + n);
            uint8_t addr[16];
            std::memcpy(addr, &from.sin6_addr, 16);
            uint16_t port = lwip_ntohs(from.sin6_port);
            auto a = alive;
            loop.post([this, a, msg, port, addr]() {
                if (!*a) return;
                engine->on_message(msg.data(), msg.size(), addr, port, now_us());
                rearm();
            });
        }
    }
};

MdnsIo::MdnsIo(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<MdnsIo> MdnsIo::start(Loop& loop, unsigned zone, std::function<void(const Service&)> found,
                                      std::function<void(const std::string&)> lost, std::string& err)
{
    auto d = std::make_unique<Impl>(loop);
    d->zone = zone;
    d->fd = lwip_socket(AF_INET6, SOCK_DGRAM, 0);
    if (d->fd < 0) {
        err = "No socket for mDNS.";
        return nullptr;
    }
    int on = 1;
    lwip_setsockopt(d->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in6 any{};
    any.sin6_len = sizeof any;
    any.sin6_family = AF_INET6;
    any.sin6_port = lwip_htons(MDNS_PORT);
    ipv6_mreq mreq{};
    lwip_inet_pton(AF_INET6, "ff02::fb", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = zone;
    if (lwip_bind(d->fd, reinterpret_cast<sockaddr*>(&any), sizeof any) != 0 ||
        lwip_setsockopt(d->fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof mreq) != 0) {
        lwip_close(d->fd);
        err = "Couldn't listen for mDNS on the link.";
        return nullptr;
    }
    int ms = kSliceMs;
    lwip_setsockopt(d->fd, SOL_SOCKET, SO_RCVTIMEO, &ms, sizeof ms);
    MdnsCallbacks cb;
    cb.found = [found](const MdnsService& s) {
        if (!found) return;
        Service out;
        out.instance = s.instance;
        out.host = s.host;
        out.address = s.address;
        out.port = s.port;
        out.flags = s.flags;
        found(out);
    };
    cb.lost = [lost](const std::string& instance) {
        if (lost) lost(instance);
    };
    Impl* raw = d.get();
    d->engine = std::make_unique<Mdns>(kZone, cb, [raw](const uint8_t* msg, size_t len, const uint8_t* to, uint16_t port) {
        raw->send(msg, len, to, port);
    });
    d->reader = std::thread([raw] { raw->read_loop(); });
    return std::unique_ptr<MdnsIo>(new MdnsIo(std::move(d)));
}

MdnsIo::~MdnsIo()
{
    impl_->engine->close();
    *impl_->alive = false;
    impl_->stop = true;
    if (impl_->reader.joinable()) impl_->reader.join();
    impl_->loop.cancel(impl_->timer);
    lwip_close(impl_->fd);
}

void MdnsIo::publish(const std::string& instance, const std::string& host, const std::string& address, uint16_t port,
                     unsigned flags)
{
    if (instance.empty()) impl_->engine->withdraw(impl_->now_us());
    else impl_->engine->publish(instance, host, address, port, flags, impl_->now_us());
    impl_->rearm();
}

void MdnsIo::browse(bool on)
{
    impl_->engine->browse(on, impl_->now_us());
    impl_->rearm();
}

} // namespace lares::airdrop
