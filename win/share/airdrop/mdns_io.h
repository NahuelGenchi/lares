// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* mDNS on the AWDL link (issue #109): the sans-IO engine of mdns.cpp on a
 * UDP socket of lwIP's, port 5353 and group ff02::fb, driven from the Share
 * model's loop. Windows' own mDNS responder never sees this traffic: it
 * isn't on Windows' network at all. */
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "loop.h"

namespace lares::airdrop {

struct Service {
    std::string instance;   /* the service ID, 12 hex digits */
    std::string host;       /* SRV target, "name.local." */
    std::string address;    /* "fe80::…%awdl0" */
    uint16_t port = 0;
    unsigned flags = 0;     /* TXT flags (MDNS-4), 0 when there's no TXT yet */
};

class MdnsIo {
public:
    /* On lwIP interface `zone`; `found` and `lost` run on `loop`. Null with
     * `err`. */
    static std::unique_ptr<MdnsIo> start(Loop& loop, unsigned zone, std::function<void(const Service&)> found,
                                         std::function<void(const std::string& instance)> lost, std::string& err);
    ~MdnsIo();
    /* Announce this receiver (MDNS-1), or withdraw it with a goodbye when
     * `instance` is empty. Loop's thread. */
    void publish(const std::string& instance, const std::string& host, const std::string& address, uint16_t port,
                 unsigned flags);
    /* Ask for `_airdrop._tcp` services, repeatedly while on. Loop's thread. */
    void browse(bool on);

    struct Impl;

private:
    explicit MdnsIo(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace lares::airdrop
