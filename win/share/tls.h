// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TLS for AirDrop on Windows (issue #109), on Schannel: encryption only,
 * never peer authentication (TLS-2), as src/airdrop/session.c does with
 * GIO. Blocking, over any byte transport (lwIP sockets in Lares). */
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lares::tls {

/* The bytes under TLS. recv: bytes read, 0 at the end, -1 on an error or a
 * timeout. recv_nonblocking (optional): bytes read, 0 at the end, -1 on an
 * error, -2 when nothing is ready yet. send: 0 once all of it went, -1 if not. */
struct Transport {
    void* ctx = nullptr;
    int (*recv)(void* ctx, uint8_t* buf, size_t len) = nullptr;
    int (*recv_nonblocking)(void* ctx, uint8_t* buf, size_t len) = nullptr;
    int (*send)(void* ctx, const uint8_t* buf, size_t len) = nullptr;
};

/* Our identity: a P-256 key kept in the user's own key store under "Lares
 * AirDrop", and a self-signed certificate for it, CN=Lares, as Linux's
 * (nothing in it says who the user is). Made the first time it's asked
 * for. Null with `err` if Windows refused. */
struct Identity;
std::shared_ptr<Identity> identity(std::string& err);

class Stream {
public:
    /* The handshake, as a receiver (asking for the sender's certificate,
     * which nothing depends on, TLS-3) or a sender. Null with `err`. */
    static std::unique_ptr<Stream> server(const Transport& t, const std::shared_ptr<Identity>& id, std::string& err);
    static std::unique_ptr<Stream> client(const Transport& t, std::string& err);
    ~Stream();

    /* Up to `len` decrypted bytes: how many, 0 when the peer closed, -1 on
     * an error. */
    int read(uint8_t* buf, size_t len);
    /* Non-blocking decrypted read for polling an idle connection (e.g. while an
     * /Ask prompt is up): >0 bytes, 0 when the peer closed, -1 on an error, -2
     * when no plaintext is ready yet (including after TLS 1.3 control records). */
    int try_read(uint8_t* buf, size_t len);
    /* All of it, encrypted: false on an error. */
    bool write(const uint8_t* buf, size_t len);
    /* Sends close_notify; reads and writes fail after it. */
    void close();
    /* Which version was agreed, for the log ("TLS 1.3"). */
    const char* version() const;

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

private:
    struct Impl;
    explicit Stream(Impl* impl);
    Impl* impl_;
};

} // namespace lares::tls
