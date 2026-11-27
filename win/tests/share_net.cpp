// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The IP layer and TLS of lares-share.exe (issue #109), between two
 * interfaces of one lwIP wired to each other as two AWDL peers would be:
 * link-local TCP, mDNS's multicast group, and Schannel both ways. */
#include "share_check.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

extern "C" {
#include "net.h"
#include "lwip/sockets.h"
}
#include "formats.h"
#include "loop.h"
#include "session.h"
#include "share_cms_test.h"
#include "tls.h"

namespace {

net_iface* side_a;
net_iface* side_b;
std::atomic<unsigned> wire_packets{ 0 };

void to_b(void*, const awdl_mac*, const awdl_mac*, const uint8_t* ip, size_t len)
{
    wire_packets++;
    net_iface_input(side_b, ip, len);
}

void to_a(void*, const awdl_mac*, const awdl_mac*, const uint8_t* ip, size_t len)
{
    wire_packets++;
    net_iface_input(side_a, ip, len);
}

sockaddr_in6 address_of(net_iface* on, net_iface* target, uint16_t port)
{
    sockaddr_in6 sa{};
    sa.sin6_len = sizeof sa;
    sa.sin6_family = AF_INET6;
    sa.sin6_port = lwip_htons(port);
    char text[64];
    net_iface_address(target, text);
    lwip_inet_pton(AF_INET6, text, &sa.sin6_addr);
    sa.sin6_scope_id = net_iface_index(on);
    return sa;
}

int fd_recv(void* ctx, uint8_t* buf, size_t len)
{
    int n = lwip_recv(static_cast<int>(reinterpret_cast<intptr_t>(ctx)), buf, len, 0);
    return n < 0 ? -1 : n;
}

int fd_recv_nb(void* ctx, uint8_t* buf, size_t len)
{
    int n = lwip_recv(static_cast<int>(reinterpret_cast<intptr_t>(ctx)), buf, len, MSG_DONTWAIT);
    if (n >= 0) return n;
    if (errno == EWOULDBLOCK || errno == EAGAIN) return -2;
    return -1;
}

int fd_send(void* ctx, const uint8_t* buf, size_t len)
{
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
    while (len) {
        int n = lwip_send(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return 0;
}

lares::tls::Transport over(int fd)
{
    lares::tls::Transport t;
    t.ctx = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    t.recv = fd_recv;
    t.recv_nonblocking = fd_recv_nb;
    t.send = fd_send;
    return t;
}

void set_timeout(int fd, int ms)
{
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &ms, sizeof ms);
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ms, sizeof ms);
}

void test_wire()
{
    awdl_mac a{ { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 } }, b{ { 0x06, 0x66, 0x77, 0x88, 0x99, 0xaa } };
    side_a = net_iface_add(&a, to_b, nullptr);
    side_b = net_iface_add(&b, to_a, nullptr);
    CHECK(side_a && side_b);
    char text[64];
    net_iface_address(side_a, text);
    /* NET-1: modified EUI-64, as awdl_link_local makes it. */
    uint8_t want[16];
    awdl_link_local(&a, want);
    in6_addr got{};
    lwip_inet_pton(AF_INET6, text, &got);
    CHECK(std::memcmp(&got, want, 16) == 0);
    if (std::memcmp(&got, want, 16) != 0) std::fprintf(stderr, "link-local: %s\n", text);
}

void test_tcp()
{
    int ls = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    sockaddr_in6 here = address_of(side_b, side_b, 8770);
    CHECK(lwip_bind(ls, reinterpret_cast<sockaddr*>(&here), sizeof here) == 0);
    CHECK(lwip_listen(ls, 4) == 0);
    std::vector<uint8_t> got;
    std::thread server([&] {
        int c = lwip_accept(ls, nullptr, nullptr);
        set_timeout(c, 5000);
        uint8_t buf[4096];
        int n;
        while ((n = lwip_recv(c, buf, sizeof buf, 0)) > 0) got.insert(got.end(), buf, buf + n);
        lwip_close(c);
    });
    int s = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    set_timeout(s, 5000);
    sockaddr_in6 there = address_of(side_a, side_b, 8770);
    CHECK(lwip_connect(s, reinterpret_cast<sockaddr*>(&there), sizeof there) == 0);
    std::vector<uint8_t> sent(300000);
    for (size_t i = 0; i < sent.size(); i++) sent[i] = static_cast<uint8_t>(i * 7 + (i >> 9));
    CHECK(fd_send(reinterpret_cast<void*>(static_cast<intptr_t>(s)), sent.data(), sent.size()) == 0);
    lwip_close(s);
    server.join();
    lwip_close(ls);
    CHECK(got == sent);
}

void test_multicast()
{
    int r = lwip_socket(AF_INET6, SOCK_DGRAM, 0);
    int on = 1;
    lwip_setsockopt(r, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in6 any{};
    any.sin6_len = sizeof any;
    any.sin6_family = AF_INET6;
    any.sin6_port = lwip_htons(5353);
    CHECK(lwip_bind(r, reinterpret_cast<sockaddr*>(&any), sizeof any) == 0);
    ipv6_mreq mreq{};
    lwip_inet_pton(AF_INET6, "ff02::fb", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = net_iface_index(side_b);
    CHECK(lwip_setsockopt(r, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof mreq) == 0);
    set_timeout(r, 3000);

    int s = lwip_socket(AF_INET6, SOCK_DGRAM, 0);
    sockaddr_in6 group{};
    group.sin6_len = sizeof group;
    group.sin6_family = AF_INET6;
    group.sin6_port = lwip_htons(5353);
    lwip_inet_pton(AF_INET6, "ff02::fb", &group.sin6_addr);
    group.sin6_scope_id = net_iface_index(side_a);
    const char msg[] = "mdns";
    CHECK(lwip_sendto(s, msg, sizeof msg, 0, reinterpret_cast<sockaddr*>(&group), sizeof group) == sizeof msg);
    char buf[64] = {};
    sockaddr_in6 from{};
    socklen_t fromlen = sizeof from;
    int n = lwip_recvfrom(r, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
    CHECK(n == sizeof msg && std::strcmp(buf, "mdns") == 0);
    lwip_close(s);
    lwip_close(r);
}

void test_tls()
{
    std::string err;
    auto id = lares::tls::identity(err);
    CHECK(id != nullptr);
    if (!id) {
        std::fprintf(stderr, "identity: %s\n", err.c_str());
        return;
    }
    int ls = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    sockaddr_in6 here = address_of(side_b, side_b, 8771);
    CHECK(lwip_bind(ls, reinterpret_cast<sockaddr*>(&here), sizeof here) == 0);
    CHECK(lwip_listen(ls, 4) == 0);
    std::string server_err, heard;
    std::vector<uint8_t> big(200000);
    for (size_t i = 0; i < big.size(); i++) big[i] = static_cast<uint8_t>(i ^ (i >> 8));
    bool try_read_would_block = false;
    bool try_read_fast = false;
    std::thread server([&] {
        int c = lwip_accept(ls, nullptr, nullptr);
        set_timeout(c, 10000);
        auto t = lares::tls::Stream::server(over(c), id, server_err);
        if (t) {
            char buf[64] = {};
            int n = t->read(reinterpret_cast<uint8_t*>(buf), sizeof buf - 1);
            if (n > 0) heard.assign(buf, n);
            /* Non-blocking try_read must return -2 immediately when the peer is silent,
             * never blocking for SO_RCVTIMEO (10 s) on TLS 1.3 post-handshake silence. */
            auto t0 = std::chrono::steady_clock::now();
            int nb = t->try_read(reinterpret_cast<uint8_t*>(buf), sizeof buf);
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            try_read_would_block = (nb == -2);
            try_read_fast = (elapsed_ms < 1000);
            t->write(big.data(), big.size());
            t->close();
        }
        lwip_close(c);
    });
    int s = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    set_timeout(s, 10000);
    sockaddr_in6 there = address_of(side_a, side_b, 8771);
    CHECK(lwip_connect(s, reinterpret_cast<sockaddr*>(&there), sizeof there) == 0);
    std::string client_err;
    auto t = lares::tls::Stream::client(over(s), client_err);
    CHECK(t != nullptr);
    std::vector<uint8_t> got;
    if (t) {
        const char hello[] = "POST /Discover";
        CHECK(t->write(reinterpret_cast<const uint8_t*>(hello), sizeof hello - 1));
        uint8_t buf[8192];
        int n;
        while ((n = t->read(buf, sizeof buf)) > 0) got.insert(got.end(), buf, buf + n);
        CHECK(n == 0);   /* close_notify, not an error */
        std::printf("tls: %s\n", t->version());
    } else {
        std::fprintf(stderr, "tls client: %s\n", client_err.c_str());
    }
    t.reset();
    lwip_close(s);
    server.join();
    lwip_close(ls);
    if (!server_err.empty()) std::fprintf(stderr, "tls server: %s\n", server_err.c_str());
    CHECK(server_err.empty());
    CHECK(try_read_would_block);
    CHECK(try_read_fast);
    CHECK(heard == "POST /Discover");
    CHECK(got == big);
}

/* ------------------------------------------------------------ AirDrop */

std::wstring temp_folder(const wchar_t* leaf)
{
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring p = std::wstring(dir) + leaf + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(p.c_str(), nullptr);
    return p;
}

void write_file(const std::wstring& path, const std::vector<uint8_t>& bytes)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr);
    CloseHandle(f);
}

std::vector<uint8_t> read_file(const std::wstring& path)
{
    std::vector<uint8_t> out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD n = 0;
    if (!out.empty()) ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &n, nullptr);
    CloseHandle(f);
    return out;
}

void remove_all(const std::wstring& path)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring c = path + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_all(c);
            else DeleteFileW(c.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path.c_str());
}

/* What the receiver's callbacks said, for the test's thread. */
struct Heard {
    std::mutex mu;
    std::condition_variable cv;
    unsigned offers = 0;
    std::string sender;
    bool offer_links = false;
    std::vector<std::string> offered, offer_items, saved;
    std::vector<uint8_t> offer_icon;
    std::string failed;
    bool received = false, received_links = false, sent = false, declined = false, discovered = false;
    std::string peer, caps, send_err;
    uint64_t progress = 0;

    template <class F> bool wait(F done, int seconds)
    {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(seconds), done);
    }
};

std::vector<uint8_t> make_test_bmp()
{
    constexpr uint32_t w = 64, h = 64;
    constexpr uint32_t data_sz = w * h * 3;
    constexpr uint32_t file_sz = 54 + data_sz;
    std::vector<uint8_t> b(file_sz, 0);
    auto put16 = [&](size_t at, uint16_t v) {
        b[at] = static_cast<uint8_t>(v & 0xff);
        b[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) b[at + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
    };
    b[0] = 'B';
    b[1] = 'M';
    put32(2, file_sz);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    put16(26, 1);
    put16(28, 24);
    put32(34, data_sz);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t p = 54 + (y * w + x) * 3;
            b[p + 0] = static_cast<uint8_t>(x * 4);
            b[p + 1] = static_cast<uint8_t>(y * 4);
            b[p + 2] = static_cast<uint8_t>((x + y) * 2);
        }
    }
    return b;
}

unsigned count_lar_temp_files()
{
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(dir) + L"lar*.tmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned n = 0;
    do {
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

bool read_http_status(lares::tls::Stream& t, unsigned& status_out)
{
    namespace ad = lares::airdrop;
    bool done = false, interim = false, interim_ended = false;
    unsigned status = 0;
    ad::HttpCallbacks cb;
    cb.head = [&](const std::string&, const std::string&, unsigned st, const ad::HttpHeaders&, ad::Error&) {
        interim = st < 200;
        if (!interim) status = st;
        return true;
    };
    cb.body = [](const uint8_t*, size_t, ad::Error&) { return true; };
    cb.done = [&](ad::Error&) {
        if (interim) interim_ended = true;
        else done = true;
        interim = false;
        return true;
    };
    ad::HttpParser p(ad::HttpKind::Response, cb);
    uint8_t buf[4096];
    while (!done) {
        int n = t.read(buf, sizeof buf);
        if (n < 0) return false;
        if (n == 0) {
            ad::Error e;
            if (!p.eof(&e) || !done) return false;
            break;
        }
        size_t off = 0;
        while (off < static_cast<size_t>(n) && !done) {
            size_t used = 0;
            ad::Error e;
            if (!p.feed(buf + off, static_cast<size_t>(n) - off, &used, &e)) return false;
            off += used;
            if (interim_ended) {
                interim_ended = false;
                p.reset();
            } else if (!done) {
                break;
            }
        }
    }
    status_out = status;
    return true;
}

void test_issue_117_features()
{
    namespace ad = lares::airdrop;
    std::string err;
    auto id = lares::tls::identity(err);
    CHECK(id != nullptr);
    if (!id) return;

    std::wstring in = temp_folder(L"lares-117-in-"), out = temp_folder(L"lares-117-out-");
    std::vector<uint8_t> bmp = make_test_bmp();
    std::vector<uint8_t> blob(140000);
    for (size_t i = 0; i < blob.size(); i++) blob[i] = static_cast<uint8_t>((i * 29) ^ (i >> 6));
    write_file(out + L"\\photo.bmp", bmp);
    write_file(out + L"\\clip.bin", blob);

    lares::Loop loop;
    std::thread runner([&] { loop.run(); });
    Heard h;
    std::atomic<unsigned> lar_tmp_before{ count_lar_temp_files() };
    std::atomic<unsigned> lar_tmp_at_offer{ 0 };
    std::unique_ptr<ad::Receiver> receiver;
    ad::ReceiverCallbacks cb;
    cb.offer = [&](const ad::Offer& o) {
        lar_tmp_at_offer = count_lar_temp_files();
        unsigned oid = o.id;
        {
            std::lock_guard<std::mutex> lock(h.mu);
            h.offers++;
            h.sender = o.sender;
            h.offer_links = o.links;
            h.offered = o.names;
            h.offer_items = o.items;
            h.offer_icon = o.file_icon;
        }
        receiver->answer(oid, true);
    };
    cb.received = [&](unsigned, bool links, const std::vector<std::string>& items) {
        std::lock_guard<std::mutex> lock(h.mu);
        h.received_links = links;
        h.saved = items;
        h.received = true;
        h.cv.notify_all();
    };
    cb.failed = [&](unsigned, const std::string& m) {
        std::lock_guard<std::mutex> lock(h.mu);
        h.failed = m;
        h.cv.notify_all();
    };

    ad::Address here, there;
    char text[64];
    net_iface_address(side_b, text);
    CHECK(ad::parse_address(std::string(text) + "%awdl0", net_iface_index(side_b), here));
    CHECK(ad::parse_address(text, net_iface_index(side_a), there));
    std::promise<void> made;
    loop.post([&] {
        receiver = ad::Receiver::start(loop, here, "Test PC", in, id, cb, err);
        made.set_value();
    });
    made.get_future().wait();
    CHECK(receiver != nullptr);
    if (!receiver) {
        loop.quit();
        runner.join();
        return;
    }

    auto cancel = std::make_shared<ad::Cancel>();
    auto discover_caps = [&]() {
        {
            std::lock_guard<std::mutex> lock(h.mu);
            h.discovered = false;
            h.caps.clear();
        }
        ad::discover(loop, there, receiver->port(), cancel, [&](std::optional<ad::Peer> p, const std::string&) {
            std::lock_guard<std::mutex> lock(h.mu);
            h.discovered = true;
            if (p) h.caps = p->media_capabilities;
            h.cv.notify_all();
        });
        CHECK(h.wait([&] { return h.discovered; }, 20));
        return h.caps;
    };

    /* 1. ReceiverMediaCapabilities: compatible (default) vs original (HEIC/HEVC). */
    std::string caps_compat = discover_caps();
    CHECK(caps_compat == "{\"Version\":1}");
    receiver->set_original_media(true);
    std::string caps_orig = discover_caps();
    CHECK(caps_orig.find("\"hvc1\"") != std::string::npos);
    CHECK(caps_orig.find("\"public.heic\"") != std::string::npos);
    receiver->set_original_media(false);
    CHECK(discover_caps() == "{\"Version\":1}");

    /* 2. Streaming chunked /Upload (DVZip AND gzip), zero %TEMP% spooling, and FileIcon preview. */
    for (int mode = 0; mode < 2; mode++) {
        {
            std::lock_guard<std::mutex> lock(h.mu);
            h.received = h.sent = h.declined = false;
            h.offered.clear();
            h.offer_items.clear();
            h.offer_icon.clear();
            h.saved.clear();
            h.failed.clear();
            h.send_err.clear();
        }
        lar_tmp_before = count_lar_temp_files();
        ad::SendRequest req;
        req.addr = there;
        req.port = receiver->port();
        req.flags = mode == 0 ? (0x002u | 0x004u) : 0x004u; /* DVZip vs gzip */
        req.name = "Sender PC";
        req.id = "0123456789ab";
        req.paths = { out + L"\\photo.bmp", out + L"\\clip.bin" };
        ad::send(
            loop, req, cancel, [](bool, uint64_t, uint64_t) {},
            [&](bool ok, bool declined, const std::string& e) {
                std::lock_guard<std::mutex> lock(h.mu);
                h.sent = ok;
                h.declined = declined;
                h.send_err = e;
                h.cv.notify_all();
            });
        CHECK(h.wait([&] { return h.received && h.sent; }, 60));
        CHECK(lar_tmp_at_offer.load() <= lar_tmp_before.load());
        CHECK(!h.offer_links);
        CHECK(h.offered.size() == 2);
        CHECK(!h.offer_icon.empty() && h.offer_icon.size() <= 64 * 1024);
        if (h.offer_icon.size() >= 4) {
            bool jpeg = h.offer_icon[0] == 0xff && h.offer_icon[1] == 0xd8 && h.offer_icon[2] == 0xff;
            bool png = h.offer_icon[0] == 0x89 && h.offer_icon[1] == 0x50 && h.offer_icon[2] == 0x4e && h.offer_icon[3] == 0x47;
            CHECK(jpeg || png);
        }
        if (mode == 0) {
            CHECK(read_file(in + L"\\photo.bmp") == bmp);
            CHECK(read_file(in + L"\\clip.bin") == blob);
        } else {
            CHECK(read_file(in + L"\\photo (2).bmp") == bmp);
            CHECK(read_file(in + L"\\clip (2).bin") == blob);
        }
    }

    /* 3. Outgoing URL share (Items only: completes on /Ask 200 OK with no /Upload). */
    {
        std::lock_guard<std::mutex> lock(h.mu);
        h.received = h.sent = h.declined = false;
        h.offered.clear();
        h.offer_items.clear();
        h.saved.clear();
        h.send_err.clear();
    }
    {
        ad::SendRequest req;
        req.addr = there;
        req.port = receiver->port();
        req.name = "Sender PC";
        req.id = "0123456789ab";
        req.items = { "https://example.com/lares" };
        ad::send(
            loop, req, cancel, [](bool, uint64_t, uint64_t) {},
            [&](bool ok, bool declined, const std::string& e) {
                std::lock_guard<std::mutex> lock(h.mu);
                h.sent = ok;
                h.declined = declined;
                h.send_err = e;
                h.cv.notify_all();
            });
        CHECK(h.wait([&] { return h.received && h.sent; }, 30));
        CHECK(h.offer_links && h.received_links);
        CHECK(h.offered.size() == 1 && h.offered[0] == "https://example.com/lares");
        CHECK(h.offer_items.size() == 1 && h.offer_items[0] == "https://example.com/lares");
        CHECK(h.saved.size() == 1 && h.saved[0] == "https://example.com/lares");
    }

    /* Unsafe outgoing URL scheme refused locally before connecting. */
    {
        std::lock_guard<std::mutex> lock(h.mu);
        h.sent = true;
        h.send_err.clear();
    }
    bool bad_done = false;
    {
        ad::SendRequest req;
        req.addr = there;
        req.port = receiver->port();
        req.name = "Sender PC";
        req.id = "0123456789ab";
        req.items = { "javascript:alert(1)" };
        ad::send(
            loop, req, cancel, [](bool, uint64_t, uint64_t) {},
            [&](bool ok, bool, const std::string& e) {
                std::lock_guard<std::mutex> lock(h.mu);
                h.sent = ok;
                h.send_err = e;
                bad_done = true;
                h.cv.notify_all();
            });
        CHECK(h.wait([&] { return bad_done; }, 10));
        CHECK(!h.sent && !h.send_err.empty());
    }

    /* 4. Mixed Files + Items /Ask offers: accepted cleanly (HTTP 200), /Upload delivers
     * listed files while preserving Items, UP-7 still rejects unlisted files, and malformed
     * Items still fail with HTTP 400. */
    auto run_raw_mixed = [&](const ad::Value& items_arr, const std::string& listed_name,
                             const std::string& upload_entry, unsigned& ask_st, unsigned& up_st) {
        ask_st = up_st = 0;
        int s = lwip_socket(AF_INET6, SOCK_STREAM, 0);
        set_timeout(s, 10000);
        sockaddr_in6 dst = address_of(side_a, side_b, receiver->port());
        CHECK(lwip_connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof dst) == 0);
        std::string cerr;
        auto t = lares::tls::Stream::client(over(s), cerr);
        CHECK(t != nullptr);
        if (!t) {
            lwip_close(s);
            return;
        }
        ad::Value ask = ad::Value::dict();
        ask.add("BundleID", ad::Value::string("com.apple.finder"));
        ask.add("ConvertMediaFormats", ad::Value::boolean(false));
        ad::Value files = ad::Value::array();
        ad::Value f = ad::Value::dict();
        f.add("FileBomPath", ad::Value::string("./" + listed_name));
        f.add("FileIsDirectory", ad::Value::boolean(false));
        f.add("FileName", ad::Value::string(listed_name));
        f.add("FileSize", ad::Value::integer(5));
        f.add("FileType", ad::Value::string("public.plain-text"));
        files.push(std::move(f));
        ask.add("Files", std::move(files));
        ask.add("Items", items_arr);
        ask.add("SenderComputerName", ad::Value::string("iPhone"));
        ask.add("SenderID", ad::Value::string("aabbccddeeff"));
        ad::Value tid = ad::Value::dict();
        tid.add("id", ad::Value::string("11111111-2222-4333-8444-555555555555"));
        ask.add("TransferID", std::move(tid));

        ad::Bytes body;
        CHECK(ad::bplist_encode(ask, body));
        char head[256];
        int hn = std::snprintf(head, sizeof head,
                               "POST /Ask HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\n"
                               "Content-Type: application/octet-stream\r\nConnection: keep-alive\r\n"
                               "Content-Length: %zu\r\n\r\n",
                               body.size());
        CHECK(t->write(reinterpret_cast<const uint8_t*>(head), static_cast<size_t>(hn)));
        CHECK(t->write(body.data(), body.size()));
        CHECK(read_http_status(*t, ask_st));

        if (ask_st == 200 && !upload_entry.empty()) {
            ad::Bytes raw_cpio, dvz;
            ad::Deflater z(true, [&](const uint8_t* b, size_t n, ad::Error&) {
                dvz.insert(dvz.end(), b, b + n);
                return true;
            });
            ad::CpioWriter cw([&](const uint8_t* b, size_t n, ad::Error& e) { return z.feed(b, n, &e); });
            CHECK(cw.dir("."));
            CHECK(cw.file("./" + upload_entry, 5, 0));
            CHECK(cw.file_data(reinterpret_cast<const uint8_t*>("hello"), 5));
            CHECK(cw.finish());
            CHECK(z.finish());
            char uhead[256];
            int uhn = std::snprintf(uhead, sizeof uhead,
                                    "POST /Upload HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\n"
                                    "TransferID: 11111111-2222-4333-8444-555555555555\r\nTotalBytes: 5\r\n"
                                    "Content-Type: application/x-dvzip\r\nConnection: keep-alive\r\n"
                                    "Transfer-Encoding: chunked\r\n\r\n");
            CHECK(t->write(reinterpret_cast<const uint8_t*>(uhead), static_cast<size_t>(uhn)));
            ad::Bytes c1 = ad::http_chunk(dvz.data(), dvz.size());
            ad::Bytes c0 = ad::http_chunk(nullptr, 0);
            t->write(c1.data(), c1.size());
            t->write(c0.data(), c0.size());
            CHECK(read_http_status(*t, up_st));
        }
        t->close();
        lwip_close(s);
    };

    /* 4a. Mixed Files + string Items -> 200 OK on both /Ask and /Upload. */
    {
        std::lock_guard<std::mutex> lock(h.mu);
        h.received = false;
        h.offered.clear();
        h.offer_items.clear();
        h.saved.clear();
    }
    ad::Value items_str = ad::Value::array();
    items_str.push(ad::Value::string("https://example.com/mixed"));
    unsigned ask_st = 0, up_st = 0;
    run_raw_mixed(items_str, "mix.txt", "mix.txt", ask_st, up_st);
    CHECK(ask_st == 200 && up_st == 200);
    CHECK(h.wait([&] { return h.received; }, 20));
    CHECK(!h.offer_links && !h.received_links);
    CHECK(h.offered.size() == 1 && h.offered[0] == "mix.txt");
    CHECK(h.offer_items.size() == 1 && h.offer_items[0] == "https://example.com/mixed");
    std::vector<uint8_t> want_hello = { 'h', 'e', 'l', 'l', 'o' };
    CHECK(read_file(in + L"\\mix.txt") == want_hello);

    /* 4b. Mixed Files + dict Items ({"URL": ...}) -> 200 OK. */
    {
        std::lock_guard<std::mutex> lock(h.mu);
        h.received = false;
        h.offer_items.clear();
    }
    ad::Value items_dict = ad::Value::array();
    ad::Value entry = ad::Value::dict();
    entry.add("URL", ad::Value::string("https://example.com/from-dict"));
    items_dict.push(std::move(entry));
    run_raw_mixed(items_dict, "mixdict.txt", "mixdict.txt", ask_st, up_st);
    CHECK(ask_st == 200 && up_st == 200);
    CHECK(h.wait([&] { return h.received; }, 20));
    CHECK(h.offer_items.size() == 1 && h.offer_items[0] == "https://example.com/from-dict");
    CHECK(read_file(in + L"\\mixdict.txt") == want_hello);

    /* 4c. UP-7 still enforced on mixed offer: unlisted file in /Upload returns 400. */
    {
        std::lock_guard<std::mutex> lock(h.mu);
        h.failed.clear();
    }
    run_raw_mixed(items_str, "expected.txt", "sneaky.txt", ask_st, up_st);
    CHECK(ask_st == 200 && up_st == 400);
    CHECK(GetFileAttributesW((in + L"\\sneaky.txt").c_str()) == INVALID_FILE_ATTRIBUTES);

    /* 4d. Malformed Items (wrong type / empty string) still rejected with 400. */
    ad::Value items_bad_type = ad::Value::array();
    items_bad_type.push(ad::Value::integer(42));
    run_raw_mixed(items_bad_type, "nope.txt", "", ask_st, up_st);
    CHECK(ask_st == 400);
    ad::Value items_bad_empty = ad::Value::array();
    items_bad_empty.push(ad::Value::string(""));
    run_raw_mixed(items_bad_empty, "nope.txt", "", ask_st, up_st);
    CHECK(ask_st == 400);

    loop.post([&] {
        receiver.reset();
        loop.quit();
    });
    runner.join();
    remove_all(in);
    remove_all(out);
}

void test_session(bool accept)
{
    namespace ad = lares::airdrop;
    std::string err;
    auto id = lares::tls::identity(err);
    if (!id) return;
    std::wstring in = temp_folder(accept ? L"lares-in-" : L"lares-no-"), out = temp_folder(L"lares-out-");
    std::vector<uint8_t> photo(150000), a(2000), b(70000);
    for (size_t i = 0; i < photo.size(); i++) photo[i] = static_cast<uint8_t>(i * 13 + (i >> 7));
    for (size_t i = 0; i < a.size(); i++) a[i] = static_cast<uint8_t>('a' + i % 26);
    for (size_t i = 0; i < b.size(); i++) b[i] = static_cast<uint8_t>(i >> 3);
    write_file(out + L"\\IMG_0412.HEIC", photo);
    CreateDirectoryW((out + L"\\Trip").c_str(), nullptr);
    write_file(out + L"\\Trip\\notes.txt", a);
    write_file(out + L"\\Trip\\map.bin", b);

    lares::Loop loop;
    std::thread runner([&] { loop.run(); });
    Heard h;
    std::unique_ptr<ad::Receiver> receiver;
    ad::ReceiverCallbacks cb;
    cb.offer = [&](const ad::Offer& o) {
        unsigned oid = o.id;
        {
            std::lock_guard<std::mutex> lock(h.mu);
            h.offers++;
            h.sender = o.sender;
            h.offered = o.names;
        }
        if (accept) {
            /* Delay accept across one wait_answer() poll slice (500 ms) so TLS 1.3
             * non-blocking polling is exercised before replying 200 OK. */
            loop.after(700, [&, oid] {
                if (receiver) receiver->answer(oid, true);
            });
        } else {
            receiver->answer(oid, false);
        }
    };
    cb.received = [&](unsigned, bool, const std::vector<std::string>& items) {
        std::lock_guard<std::mutex> lock(h.mu);
        h.saved = items;
        h.received = true;
        h.cv.notify_all();
    };
    cb.failed = [&](unsigned, const std::string& m) {
        std::lock_guard<std::mutex> lock(h.mu);
        h.failed = m;
        h.cv.notify_all();
    };
    ad::Address here, there;
    char text[64];
    net_iface_address(side_b, text);
    CHECK(ad::parse_address(std::string(text) + "%awdl0", net_iface_index(side_b), here));
    CHECK(ad::parse_address(text, net_iface_index(side_a), there));
    std::promise<void> made;
    loop.post([&] {
        receiver = ad::Receiver::start(loop, here, "Test PC", in, id, cb, err);
        made.set_value();
    });
    made.get_future().wait();
    CHECK(receiver != nullptr);
    if (!receiver) {
        loop.quit();
        runner.join();
        return;
    }
    CHECK(receiver->port() == ad::kPort);

    auto cancel = std::make_shared<ad::Cancel>();
    ad::discover(loop, there, receiver->port(), cancel, [&](std::optional<ad::Peer> p, const std::string& e) {
        std::lock_guard<std::mutex> lock(h.mu);
        h.discovered = true;
        h.peer = p ? p->name + "/" + p->model : "none: " + e;
        h.cv.notify_all();
    });
    CHECK(h.wait([&] { return h.discovered; }, 20));
    CHECK(h.peer == "Test PC/Windows");

    /* Contacts Only policy: unmatched /Discover returns 401 (no peer); matched record succeeds. */
    const std::string allowed_hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    receiver->set_policy(true, { allowed_hash });
    {
        std::lock_guard<std::mutex> lock(h.mu);
        h.discovered = false;
        h.peer.clear();
    }
    ad::discover(loop, there, receiver->port(), cancel, [&](std::optional<ad::Peer> p, const std::string& e) {
        std::lock_guard<std::mutex> lock(h.mu);
        h.discovered = true;
        h.peer = p ? p->name + "/" + p->model : "none: " + e;
        h.cv.notify_all();
    });
    CHECK(h.wait([&] { return h.discovered; }, 20));
    CHECK(h.peer == "none: ");

    /* Unauthenticated /Ask against Contacts Only returns HTTP 401 with clear hint (not a decline). */
    ad::SendRequest unauth_req;
    unauth_req.addr = there;
    unauth_req.port = receiver->port();
    unauth_req.name = "Sender PC";
    unauth_req.id = "0123456789ab";
    unauth_req.paths = { out + L"\\IMG_0412.HEIC" };
    bool unauth_done = false, unauth_ok = true, unauth_declined = true;
    std::string unauth_err;
    ad::send(
        loop, unauth_req, cancel, [](bool, uint64_t, uint64_t) {},
        [&](bool ok, bool declined, const std::string& e) {
            std::lock_guard<std::mutex> lock(h.mu);
            unauth_done = true;
            unauth_ok = ok;
            unauth_declined = declined;
            unauth_err = e;
            h.cv.notify_all();
        });
    CHECK(h.wait([&] { return unauth_done; }, 20));
    CHECK(!unauth_ok && !unauth_declined && unauth_err.find("Everyone for 10 Minutes") != std::string::npos);

    std::string xml_rec =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>"
        "<key>ValidatedEmailHashes</key><array><string>" + allowed_hash + "</string></array>"
        "</dict></plist>";
    std::vector<uint8_t> rec_bytes(xml_rec.begin(), xml_rec.end());

    auto discover_with_record = [&](const std::vector<uint8_t>& record) {
        {
            std::lock_guard<std::mutex> lock(h.mu);
            h.discovered = false;
            h.peer.clear();
        }
        ad::discover(
            loop, there, receiver->port(), cancel,
            [&](std::optional<ad::Peer> p, const std::string& e) {
                std::lock_guard<std::mutex> lock(h.mu);
                h.discovered = true;
                h.peer = p ? p->name + "/" + p->model : "none: " + e;
                h.cv.notify_all();
            },
            record);
        CHECK(h.wait([&] { return h.discovered; }, 20));
    };

    /* Unsigned raw plist MUST be rejected (401) by live Contacts Only receiver. */
    discover_with_record(rec_bytes);
    CHECK(h.peer == "none: ");

    /* Self-signed CMS SignedData MUST be rejected (401). */
    ad::test_cms::EphemeralCert self_cert;
    CHECK(ad::test_cms::make_cert({ L"CN=Forged Self-Signed", false, false, -1, 7, nullptr }, nullptr, self_cert));
    ad::Bytes self_cms;
    CHECK(ad::test_cms::sign_cms(self_cert, { &self_cert }, rec_bytes.data(), rec_bytes.size(), self_cms));
    discover_with_record(self_cms);
    CHECK(h.peer == "none: ");

    /* Valid CA-chained CMS SignedData: rejected when CA is not Apple, accepted once CA is trusted for test. */
    ad::test_cms::EphemeralCert test_ca, test_leaf;
    CHECK(ad::test_cms::make_cert({ L"CN=Loopback Test CA", true, false, -1, 30, nullptr }, nullptr, test_ca));
    CHECK(ad::test_cms::make_cert({ L"CN=Loopback Test Leaf", false, false, -1, 7, nullptr }, &test_ca, test_leaf));
    ad::Bytes signed_cms;
    CHECK(ad::test_cms::sign_cms(test_leaf, { &test_leaf }, rec_bytes.data(), rec_bytes.size(), signed_cms));
    discover_with_record(signed_cms);
    CHECK(h.peer == "none: ");

    receiver->set_trusted_roots_for_test({ test_ca.der });
    /* Unsigned and self-signed records stay rejected even with a test root installed. */
    discover_with_record(rec_bytes);
    CHECK(h.peer == "none: ");
    discover_with_record(self_cms);
    CHECK(h.peer == "none: ");
    /* Trusted CA-chained CMS record succeeds and returns the receiver identity. */
    discover_with_record(signed_cms);
    CHECK(h.peer == "Test PC/Windows");

    receiver->set_trusted_roots_for_test({});
    receiver->set_policy(false, {});

    ad::SendRequest req;
    req.addr = there;
    req.port = receiver->port();
    req.name = "Sender PC";
    req.id = "0123456789ab";
    req.paths = { out + L"\\IMG_0412.HEIC", out + L"\\Trip" };
    ad::send(
        loop, req, cancel,
        [&](bool, uint64_t sent, uint64_t) {
            std::lock_guard<std::mutex> lock(h.mu);
            h.progress = sent;
        },
        [&](bool ok, bool declined, const std::string& e) {
            std::lock_guard<std::mutex> lock(h.mu);
            h.sent = ok;
            h.declined = declined;
            h.send_err = e;
            h.cv.notify_all();
        });
    if (accept) {
        CHECK(h.wait([&] { return h.received && h.sent; }, 60));
        CHECK(h.sender == "Sender PC");
        CHECK(h.offered.size() == 2);
        CHECK(h.saved.size() == 2);
        CHECK(read_file(in + L"\\IMG_0412.HEIC") == photo);
        CHECK(read_file(in + L"\\Trip\\notes.txt") == a);
        CHECK(read_file(in + L"\\Trip\\map.bin") == b);
        if (!h.failed.empty()) std::fprintf(stderr, "receiver failed: %s\n", h.failed.c_str());
        if (!h.send_err.empty()) std::fprintf(stderr, "sender: %s\n", h.send_err.c_str());
    } else {
        CHECK(h.wait([&] { return h.declined || (!h.sent && !h.send_err.empty()); }, 60));
        CHECK(h.declined);
        CHECK(GetFileAttributesW((in + L"\\IMG_0412.HEIC").c_str()) == INVALID_FILE_ATTRIBUTES);
    }

    loop.post([&] {
        receiver.reset();
        loop.quit();
    });
    runner.join();
    remove_all(in);
    remove_all(out);
}

} // namespace

void run_net_tests()
{
    test_wire();
    if (!side_a || !side_b) return;
    test_tcp();
    test_multicast();
    test_tls();
    test_session(true);
    test_session(false);
    test_issue_117_features();
    std::printf("net: %u packets crossed the wire\n", wire_packets.load());
}
