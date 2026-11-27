// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* AirDrop's exchange over TLS on Windows (issue #109): src/airdrop/session.c
 * ported to lwIP's sockets, Schannel and Win32 files, with the receiver's
 * trace of PR #108. What each request carries, the receiver's
 * per-connection slots (HTTP-7), how an upload is checked against its /Ask
 * and unpacked into a staging folder, and the timeouts are Linux's. Rule IDs
 * are those of owly's AirDrop specification. */
#include "session.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <thread>

#include <windows.h>
#include <bcrypt.h>

#include "formats.h"
#include "thumb.h"

extern "C" {
#include "lwip/sockets.h"
}

namespace lares::airdrop {

namespace {

constexpr char kModel[] = "Windows";            /* Receiver/SenderModelName */
constexpr unsigned kIoTimeout = 30;             /* s: a peer that stops reading or writing (SEC-10) */
constexpr unsigned kDiscoverIdleTimeout = 10;   /* s: idle keep-alive /Discover slot releases sooner */
constexpr unsigned kConnectTimeout = 10;        /* s */
/* A /Discover that isn't answered soon is tried again on a new connection
 * rather than waited for: over a fixed radio a phone is there about once a
 * second, and a fresh connection's first SYN goes at once. */
constexpr unsigned kDiscoverConnectTimeout = 3; /* s */
constexpr unsigned kDiscoverTimeout = 6;        /* s */
constexpr auto kPromptTimeout = std::chrono::seconds(40);   /* under the ~45 s an Apple sender waits (ASK-2) */
constexpr auto kPromptSlice = std::chrono::milliseconds(500);
constexpr unsigned kSenderAskTimeout = 45;      /* s, longer than a receiver's 15 s (ASK-2a) */
constexpr unsigned kUploadResponseTimeout = 60; /* s */
constexpr unsigned kDrainTimeout = 2;           /* s, reading what follows a complete archive (UP-11) */
constexpr size_t kMaxMessage = 4 * 1024 * 1024; /* a /Discover or /Ask body (multi-photo icons), and a response */
constexpr size_t kMaxPending = 1024 * 1024;     /* bytes buffered while a prompt is up */
constexpr uint64_t kMaxUpload = 16ull << 30;    /* an upload, unpacked (SEC-3) */
constexpr auto kProgressEvery = std::chrono::milliseconds(250);
constexpr size_t kReadChunk = 64 * 1024;
constexpr int kSliceMs = 250;                   /* how often a blocked socket looks at its Cancel */
constexpr int kMaxConnections = 16;

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------ text and files

std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/* "\\?\" before an absolute path: deep folders from a Mac pass MAX_PATH. */
std::wstring long_path(const std::wstring& p)
{
    if (p.compare(0, 4, L"\\\\?\\") == 0 || p.size() < 3 || p[1] != L':') return p;
    return L"\\\\?\\" + p;
}

std::wstring shown_path(const std::wstring& p)
{
    return p.compare(0, 4, L"\\\\?\\") == 0 ? p.substr(4) : p;
}

/* A name as Windows can hold it: what an iPhone may put in a name and
 * Windows can't (":" would even make an alternate data stream) becomes
 * "_", trailing dots and spaces go, and a device name (CON, NUL, COM1...)
 * gets a "_" in front. Names are compared as sent; only the disk sees this. */
std::wstring windows_name(const std::string& name)
{
    std::wstring w = wide(name);
    for (wchar_t& c : w)
        if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    while (!w.empty() && (w.back() == L'.' || w.back() == L' ')) w.pop_back();
    if (w.empty()) w = L"_";
    std::wstring stem = w.substr(0, w.find(L'.'));
    static const wchar_t* const reserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4",
                                               L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3",
                                               L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9", L"CONIN$", L"CONOUT$" };
    for (const wchar_t* r : reserved)
        if (_wcsicmp(stem.c_str(), r) == 0) return L"_" + w;
    return w;
}

/* Removes what the staging folder holds: everything in it is ours, made by
 * this connection, and no link is followed. */
void remove_tree(const std::wstring& path)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring child = path + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_tree(child);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(child.c_str());
            else
                DeleteFileW(child.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path.c_str());
}

/* `name` in `folder`, or "name (2).ext" and so on, moved without ever
 * replacing anything (SEC-2). The path it got, or empty. */
std::wstring move_unique(const std::wstring& from, const std::wstring& folder, const std::wstring& name)
{
    size_t dot = name.rfind(L'.');
    bool ext = dot != std::wstring::npos && dot != 0;
    std::wstring stem = ext ? name.substr(0, dot) : name, tail = ext ? name.substr(dot) : L"";
    for (int i = 1; i < 1000; i++) {
        std::wstring candidate = i == 1 ? name : stem + L" (" + std::to_wstring(i) + L")" + tail;
        std::wstring to = folder + L"\\" + candidate;
        if (MoveFileExW(from.c_str(), to.c_str(), 0)) return to;
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) break;
    }
    return {};
}

std::string uuid_upper()
{
    uint8_t b[16] = {};
    BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);
    char out[40];
    std::snprintf(out, sizeof out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

// ------------------------------------------------------------ sockets

sockaddr_in6 socket_address(const Address& a, uint16_t port)
{
    sockaddr_in6 sa{};
    sa.sin6_len = sizeof sa;
    sa.sin6_family = AF_INET6;
    sa.sin6_port = lwip_htons(port);
    std::memcpy(&sa.sin6_addr, a.ip, 16);
    sa.sin6_scope_id = a.zone;
    return sa;
}

/* A TCP socket of lwIP's whose blocking calls wake every quarter second to
 * look at their Cancel and their own timeout. */
class Sock {
public:
    Sock(int fd, std::shared_ptr<Cancel> cancel) : fd_(fd), cancel_(std::move(cancel))
    {
        if (fd_ >= 0) slices();
    }
    ~Sock()
    {
        if (fd_ >= 0) lwip_close(fd_);
    }
    Sock(const Sock&) = delete;
    Sock& operator=(const Sock&) = delete;

    int fd() const { return fd_; }
    void set_timeout(unsigned seconds) { timeout_ms_ = static_cast<int>(seconds) * 1000; }
    bool cancelled() const { return cancel_ && cancel_->cancelled(); }

    bool connect(const Address& a, uint16_t port, unsigned timeout_s, std::string& err)
    {
        fd_ = lwip_socket(AF_INET6, SOCK_STREAM, 0);
        if (fd_ < 0) {
            err = "No socket for the connection.";
            return false;
        }
        int on = 1;
        lwip_ioctl(fd_, FIONBIO, &on);
        sockaddr_in6 sa = socket_address(a, port);
        int r = lwip_connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
        if (r < 0 && errno != EINPROGRESS) {
            err = "The connection was refused.";
            return false;
        }
        auto end = Clock::now() + std::chrono::seconds(timeout_s);
        while (r < 0) {
            if (cancelled()) {
                err = "Cancelled.";
                return false;
            }
            if (Clock::now() >= end) {
                err = "The connection timed out.";
                return false;
            }
            pollfd p{ fd_, POLLOUT, 0 };
            if (lwip_poll(&p, 1, kSliceMs) > 0) {
                int soerr = 0;
                socklen_t len = sizeof soerr;
                lwip_getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &len);
                if (soerr) {
                    err = (soerr == ECONNREFUSED || soerr == ECONNRESET) ? "The connection was refused." : "The connection failed.";
                    return false;
                }
                r = 0;
            }
        }
        int off = 0;
        lwip_ioctl(fd_, FIONBIO, &off);
        slices();
        timeout_ms_ = static_cast<int>(timeout_s) * 1000;
        return true;
    }

    /* Bytes read, 0 at the end, -1 on an error, a timeout or a cancel. */
    int recv(uint8_t* buf, size_t len)
    {
        auto end = Clock::now() + std::chrono::milliseconds(timeout_ms_);
        for (;;) {
            int n = lwip_recv(fd_, buf, len, 0);
            if (n >= 0) return n;
            if (errno != EWOULDBLOCK && errno != EAGAIN) return -1;
            if (cancelled() || Clock::now() >= end) return -1;
        }
    }

    bool send_all(const uint8_t* buf, size_t len)
    {
        auto end = Clock::now() + std::chrono::milliseconds(timeout_ms_);
        while (len) {
            int n = lwip_send(fd_, buf, len, 0);
            if (n > 0) {
                buf += n;
                len -= static_cast<size_t>(n);
                end = Clock::now() + std::chrono::milliseconds(timeout_ms_);
                continue;
            }
            if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) return false;
            if (cancelled() || Clock::now() >= end) return false;
        }
        return true;
    }

    /* Non-blocking read: >0 bytes, 0 at the end, -1 on error, -2 would block. */
    int recv_nonblocking(uint8_t* buf, size_t len)
    {
        int n = lwip_recv(fd_, buf, len, MSG_DONTWAIT);
        if (n >= 0) return n;
        if (errno == EWOULDBLOCK || errno == EAGAIN) return -2;
        return -1;
    }

    /* Whether something (bytes or the end) waits to be read. */
    bool readable()
    {
        pollfd p{ fd_, POLLIN, 0 };
        return lwip_poll(&p, 1, 0) > 0;
    }

    tls::Transport transport()
    {
        tls::Transport t;
        t.ctx = this;
        t.recv = [](void* ctx, uint8_t* b, size_t n) { return static_cast<Sock*>(ctx)->recv(b, n); };
        t.recv_nonblocking = [](void* ctx, uint8_t* b, size_t n) {
            return static_cast<Sock*>(ctx)->recv_nonblocking(b, n);
        };
        t.send = [](void* ctx, const uint8_t* b, size_t n) { return static_cast<Sock*>(ctx)->send_all(b, n) ? 0 : -1; };
        return t;
    }

private:
    void slices()
    {
        int ms = kSliceMs;
        lwip_setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &ms, sizeof ms);
        lwip_setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &ms, sizeof ms);
        int on = 1;
        lwip_setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    }

    int fd_ = -1;
    std::shared_ptr<Cancel> cancel_;
    int timeout_ms_ = static_cast<int>(kIoTimeout) * 1000;
};

// ------------------------------------------------------------ HTTP

Bytes response_bytes(unsigned status, const Bytes* body, bool close)
{
    const char* reason = status == 100 ? "Continue" : status == 200 ? "OK" : status == 400 ? "Bad Request"
                       : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 406 ? "Not Acceptable"
                       : status == 413 ? "Content Too Large" : "Internal Server Error";
    size_t len = body ? body->size() : 0;
    char head[256];
    int n = std::snprintf(head, sizeof head, "HTTP/1.1 %u %s\r\n%s%sContent-Length: %zu\r\n\r\n", status, reason,
                          body ? "Content-Type: application/octet-stream\r\n" : "", close ? "Connection: close\r\n" : "",
                          len);
    Bytes out(head, head + n);
    if (body) out.insert(out.end(), body->begin(), body->end());
    return out;
}

/* A plist request with a Content-Length body (HTTP-2, HTTP-2a). */
bool plist_request(const char* path, const Value& v, Bytes& out)
{
    Bytes body;
    if (!bplist_encode(v, body)) return false;
    char head[256];
    int n = std::snprintf(head, sizeof head,
                          "POST %s HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\nContent-Type: application/octet-stream\r\n"
                          "Connection: keep-alive\r\nContent-Length: %zu\r\n\r\n",
                          path, body.size());
    out.assign(head, head + n);
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

std::string lookup_str(const Value& dict, const char* key)
{
    const Value* v = dict.is(Value::Type::Dict) ? dict.find(key, Value::Type::String) : nullptr;
    return v ? v->get_string() : std::string();
}

/* The archive path's components, without "./" and empty ones; false if any
 * component is unsafe. "." alone gives none. */
bool components(const std::string& path, std::vector<std::string>& out)
{
    out.clear();
    size_t at = 0;
    for (;;) {
        size_t slash = path.find('/', at);
        std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (!part.empty() && part != ".") {
            if (!airdrop_safe_name(part)) return false;
            out.push_back(part);
        }
        if (slash == std::string::npos) return true;
        at = slash + 1;
    }
}

std::string ext_of(const std::string& name)
{
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || name.size() - dot > 8) return {};
    std::string e = name.substr(dot);
    for (char& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return e;
}

/* A sender's header value as protocol text only, for the trace. */
std::string plain(const std::string& value)
{
    std::string s;
    for (char c : value) {
        if (s.size() >= 40) break;
        s += isalnum(static_cast<unsigned char>(c)) || std::strchr("/.-+;=", c) ? c : '?';
    }
    return s;
}

std::string hex(const uint8_t* b, size_t n)
{
    std::string s;
    char x[3];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(x, sizeof x, "%02x", b[i]);
        s += x;
    }
    return s;
}

} // namespace

bool parse_address(const std::string& text, unsigned zone, Address& out)
{
    std::string ip = text.substr(0, text.find('%'));
    in6_addr a{};
    if (lwip_inet_pton(AF_INET6, ip.c_str(), &a) != 1) return false;
    std::memcpy(out.ip, &a, 16);
    out.zone = zone;
    return true;
}

// ================================================================ receiver

namespace {

/* An item the accepted /Ask lists, as UP-7 checks archive entries against. */
struct Listed {
    std::string name;       /* FileName: what's saved */
    std::string path;       /* FileBomPath without "./" */
    std::string wrapper;    /* a regular file's one-folder wrapper, or empty (ASK-5) */
    bool dir = false;
};

enum class Route { None, Discover, Ask, Upload, Other, Get };

} // namespace

struct Receiver::Impl {
    Loop& loop;
    ReceiverCallbacks cb;
    std::string name;
    std::wstring folder;
    std::shared_ptr<tls::Identity> id;
    int listen_fd = -1;
    uint16_t port = 0;

    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<Cancel> cancel = std::make_shared<Cancel>();
    std::mutex mu;
    std::condition_variable cv;
    std::map<unsigned, int> answers;    /* 0 waiting, 1 accept, 2 decline */
    unsigned next_id = 0;
    bool contacts_only = false;
    bool original_media = false;
    std::set<std::string> contact_hashes;
    std::vector<Bytes> test_roots;
    std::thread acceptor;
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::list<Worker> workers;          /* acceptor's thread only, until the end */

    Impl(Loop& l) : loop(l) {}

    /* Runs on the loop, unless the receiver is gone by then. */
    template <class F> void note(F fn)
    {
        auto a = alive;
        loop.post([a, fn]() mutable {
            if (*a) fn();
        });
    }

    void accept_loop();
};

namespace {

struct Conn {
    Receiver::Impl* r;
    std::unique_ptr<Sock> sock;
    std::unique_ptr<tls::Stream> tls;
    std::unique_ptr<HttpParser> http;
    Bytes input;            /* read but not yet parsed */
    bool closing = false;

    /* the message being parsed */
    Route route = Route::None;
    bool msg_done = false, too_big = false;
    unsigned fail_status = 0;   /* a status to answer at the message's end */
    Bytes body;

    /* HTTP-7's slots */
    bool slot_discover = false, slot_upload = false, accepted = false;
    unsigned id = 0;
    std::string transfer_id;
    std::vector<Listed> listed;
    bool links = false;
    uint64_t total = 0;

    /* the upload */
    std::unique_ptr<Inflater> inflate;
    std::unique_ptr<CpioReader> cpio;
    std::wstring staging;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool skipping = false, archive_done = false;
    uint64_t written = 0;
    Clock::time_point last_progress{};
    Error upload_error;
    /* what the upload looked like, for the trace: never a name */
    uint64_t raw_in = 0, archive_in = 0;
    uint8_t raw_head[4] = {}, archive_head[6] = {};
    unsigned entries = 0;

    void trace(const char* fmt, ...)
    {
        if (!r->cb.trace) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        std::string line = buf;
        unsigned i = id;
        Receiver::Impl* rr = r;
        r->note([rr, i, line] { rr->cb.trace(i, line); });
    }

    bool send_response(unsigned status, const Bytes* b, bool close)
    {
        Bytes out = response_bytes(status, b, close);
        bool ok = tls->write(out.data(), out.size());
        if (close || !ok) closing = true;
        return ok;
    }

    void fail(unsigned status) { send_response(status, nullptr, true); }

    // ---- upload: archive entries into the staging folder

    static bool refuse(Error& e, const char* why)
    {
        e.assign(ErrorCode::Refused, why);
        return false;
    }

    std::wstring staged(const std::vector<std::string>& parts, size_t n)
    {
        std::wstring p = staging;
        for (size_t i = 0; i < n; i++) p += L"\\" + windows_name(parts[i]);
        return p;
    }

    bool make_dirs(const std::vector<std::string>& parts, size_t n, Error& e)
    {
        std::wstring dir = staging;
        for (size_t i = 0; i < n; i++) {
            dir += L"\\" + windows_name(parts[i]);
            if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                e.assign(ErrorCode::Failed, "Couldn't make a folder.");
                return false;
            }
        }
        return true;
    }

    bool entry_begin(const std::string& path, CpioType type, uint64_t size, Error& e)
    {
        skipping = false;
        std::vector<std::string> parts;
        if (!components(path, parts)) {
            trace("entry %u: unsafe name, refused", ++entries);
            return refuse(e, "An archive entry has an unsafe name.");
        }
        bool ok = place(parts, type, size, e);
        /* The first entries and any refused one, compared with the list by
         * extension and by whether the name is the same: no names. */
        if (++entries <= 8 || !ok) {
            std::string ext = ext_of(parts.empty() ? std::string() : parts.back()), l;
            for (size_t i = 0; i < listed.size() && i < 4; i++) {
                std::string le = ext_of(listed[i].name);
                l += (i ? ", " : "") + std::string(!listed[i].wrapper.empty() ? "wrapped " : listed[i].dir ? "folder " : "") +
                     (le.empty() ? "no extension" : le) + (!parts.empty() && parts.back() == listed[i].name ? " (same name)" : "");
            }
            trace("entry %u: %zu parts, %s, %llu bytes, %s; listed: %s; %s", entries, parts.size(),
                  type == CpioType::Dir ? "folder" : "file", static_cast<unsigned long long>(size),
                  ext.empty() ? "no extension" : ext.c_str(), l.empty() ? "nothing" : l.c_str(), ok ? "taken" : "refused");
        }
        return ok;
    }

    bool place(const std::vector<std::string>& parts, CpioType type, uint64_t size, Error& e)
    {
        size_t n = parts.size();
        if (n == 0) return type == CpioType::Dir || refuse(e, "The archive's root isn't a folder."); /* UP-2a */
        const std::string& last = parts.back();
        /* Which listed item the entry belongs to, and where it goes. */
        const Listed* owner = nullptr;
        std::vector<std::string> target;
        bool has_target = false;
        for (const Listed& l : listed) {
            if (owner) break;
            if (!l.wrapper.empty()) {
                if (n == 1 && parts[0] == l.wrapper && type == CpioType::Dir) return true; /* the wrapper itself */
                if (n == 2 && parts[0] == l.wrapper) {
                    if (last == l.name) owner = &l, target = { l.name }, has_target = true;
                    else if (last.compare(0, 2, "._") == 0) owner = &l;
                }
            } else if (l.dir) {
                if (parts[0] == l.name) owner = &l, target = parts, has_target = true;
                else if (n == 1 && last.compare(0, 2, "._") == 0 && last.substr(2) == l.name) owner = &l;
            } else if (n == 1) {
                if (last == l.name) owner = &l, target = parts, has_target = true;
                else if (last.compare(0, 2, "._") == 0 && last.substr(2) == l.name) owner = &l;
            }
        }
        if (!owner) return refuse(e, "The archive holds something the transfer didn't list."); /* UP-7 */
        /* AppleDouble metadata beside a listed item, or inside a listed
         * folder, is accepted and dropped (UP-7a). */
        if (last.compare(0, 2, "._") == 0) {
            skipping = true;
            return type == CpioType::File || refuse(e, "Bad metadata entry.");
        }
        if (!has_target) return refuse(e, "The archive holds something the transfer didn't list.");
        size_t tn = target.size();
        if (type == CpioType::Dir) {
            if (!owner->dir) return refuse(e, "A listed file arrived as a folder.");
            return make_dirs(target, tn, e);
        }
        if (owner->dir && tn == 1) return refuse(e, "A listed folder arrived as a file.");
        if (!owner->dir && tn != 1) return refuse(e, "Bad file entry.");
        if (size > kMaxUpload || written > kMaxUpload - size) {
            e.assign(ErrorCode::Limit, "The transfer is too large.");
            return false;
        }
        if (tn > 1 && !make_dirs(target, tn - 1, e)) return false;
        /* Never over anything (SEC-1, SEC-2): staging is new and ours. */
        file = CreateFileW(staged(target, tn).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            e.assign(ErrorCode::Failed, "Couldn't write a file.");
            return false;
        }
        return true;
    }

    void upload_progress(bool force)
    {
        auto now = Clock::now();
        if (!force && now - last_progress < kProgressEvery) return;
        last_progress = now;
        unsigned i = id;
        uint64_t done = written, t = total;
        Receiver::Impl* rr = r;
        r->note([rr, i, done, t] {
            if (rr->cb.progress) rr->cb.progress(i, done, t);
        });
    }

    bool entry_data(const uint8_t* buf, size_t len, Error& e)
    {
        if (skipping) return true;
        if (file == INVALID_HANDLE_VALUE) return refuse(e, "Data outside a file.");
        while (len) {
            DWORD n = 0;
            DWORD chunk = len > (1u << 30) ? (1u << 30) : static_cast<DWORD>(len);
            if (!WriteFile(file, buf, chunk, &n, nullptr) || n == 0) {
                e.assign(ErrorCode::Failed, "Couldn't write a file: is the disk full?");
                return false;
            }
            buf += n;
            len -= n;
            written += n;
        }
        upload_progress(false);
        return true;
    }

    bool entry_end(Error& e)
    {
        skipping = false;
        if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            e.assign(ErrorCode::Failed, "Couldn't write a file.");
            return false;
        }
        file = INVALID_HANDLE_VALUE;
        return true;
    }

    bool to_cpio(const uint8_t* buf, size_t len, Error& e)
    {
        for (size_t i = 0; i < len && archive_in + i < sizeof archive_head; i++) archive_head[archive_in + i] = buf[i];
        archive_in += len;
        if (archive_done) return true;
        return cpio->feed(buf, len, &archive_done, &e);
    }

    void upload_reset()
    {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        inflate.reset();
        cpio.reset();
        if (!staging.empty()) remove_tree(staging);
        staging.clear();
    }

    void upload_failed(const std::string& message)
    {
        trace("upload failed: %llu bytes in (starting %s), %llu unpacked (starting %s), %u entries, %llu written, archive %s",
              static_cast<unsigned long long>(raw_in), raw_in ? hex(raw_head, raw_in < 4 ? raw_in : 4).c_str() : "-",
              static_cast<unsigned long long>(archive_in),
              archive_in ? hex(archive_head, archive_in < 6 ? archive_in : 6).c_str() : "-", entries,
              static_cast<unsigned long long>(written), archive_done ? "complete" : "not complete");
        upload_reset();
        unsigned i = id;
        Receiver::Impl* rr = r;
        r->note([rr, i, message] {
            if (rr->cb.failed) rr->cb.failed(i, message);
        });
    }

    /* The archive is complete: everything moves from staging into place. */
    void upload_finish()
    {
        std::vector<std::string> saved;
        bool ok = true;
        for (const Listed& l : listed) {
            if (!ok) break;
            std::wstring name = windows_name(l.name), from = staging + L"\\" + name;
            if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::wstring to = move_unique(from, long_path(r->folder), name);
            if (to.empty()) ok = false;
            else saved.push_back(utf8(shown_path(to)));
        }
        upload_progress(true);
        if (!ok) {
            upload_failed("Couldn't save the files.");
            fail(500);
            return;
        }
        upload_reset();
        send_response(200, nullptr, true); /* UP-5 */
        unsigned i = id;
        Receiver::Impl* rr = r;
        r->note([rr, i, saved] {
            if (rr->cb.received) rr->cb.received(i, false, saved);
        });
    }

    // ---- the three requests

    bool decode_body(Value& v)
    {
        if (body.empty()) {
            v = Value::dict();
            return true;
        }
        return bplist_decode(body.data(), body.size(), v) && v.is(Value::Type::Dict);
    }

    /* Checks SenderRecordData against saved contact hashes (ID-3). Returns the
     * matched lowercase SHA-256 hex digest, or empty if no contact matched.
     * Never logs hashes (SEC-5). */
    std::string match_sender_record(const Value& req, bool& contacts_only_out)
    {
        std::set<std::string> allowed;
        std::vector<Bytes> custom_roots;
        {
            std::lock_guard<std::mutex> lock(r->mu);
            contacts_only_out = r->contacts_only;
            allowed = r->contact_hashes;
            custom_roots = r->test_roots;
        }
        if (allowed.empty()) return {};
        const Value* rec = req.is(Value::Type::Dict) ? req.find("SenderRecordData", Value::Type::Data) : nullptr;
        if (!rec || rec->get_data().empty()) return {};
        std::vector<std::string> hashes;
        bool ok = custom_roots.empty()
                      ? airdrop_extract_record_hashes(rec->get_data().data(), rec->get_data().size(), hashes)
                      : airdrop_extract_record_hashes_with_roots(rec->get_data().data(), rec->get_data().size(),
                                                                 custom_roots, hashes);
        if (!ok) return {};
        for (const std::string& h : hashes) {
            if (allowed.count(h)) return h;
        }
        return {};
    }

    void on_discover()
    {
        /* HTTP-7: once per connection, and only first. */
        if (slot_discover || accepted || slot_upload) return fail(400);
        Value request;
        if (!decode_body(request)) return fail(400);
        slot_discover = true;
        bool contacts_only = false;
        std::string matched = match_sender_record(request, contacts_only);
        if (contacts_only && matched.empty()) {
            /* DISC-2 / ID-3: Contacts Only with no match answers 401 and no name. */
            Value b = Value::dict();
            b.add("StatusCode", Value::integer(401));
            Bytes out;
            if (bplist_encode(b, out)) {
                send_response(200, &out, false);
                sock->set_timeout(kDiscoverIdleTimeout);
            } else {
                fail(500);
            }
            return;
        }
        /* Everyone (100) or matched Contact (200) (ID-3, ID-4). */
        bool original_media = false;
        {
            std::lock_guard<std::mutex> lock(r->mu);
            original_media = r->original_media;
        }
        /* DISC-3: compatible mode leaves out Codecs/ContainerFormats so iOS sends
         * JPEG/H.264; original mode advertises HEVC and HEIF/HEIC support. */
        static const char kCompatCaps[] = "{\"Version\":1}";
        static const char kOrigCaps[] =
            "{\"Version\":1,\"Codecs\":{\"hvc1\":{\"Profiles\":{\"VTPerProfileSupport\":{"
            "\"1\":{\"VTMaxPlaybackLevel\":120},\"2\":{\"VTMaxPlaybackLevel\":120},\"3\":{}},"
            "\"VTSupportedProfiles\":[1,2,3]}}},"
            "\"ContainerFormats\":{\"public.heif-standard\":{\"HeifSubtypes\":[\"public.avci\",\"public.heic\",\"public.heif\"]}}}";
        const char* caps = original_media ? kOrigCaps : kCompatCaps;
        size_t caps_len = original_media ? (sizeof kOrigCaps - 1) : (sizeof kCompatCaps - 1);
        Value b = Value::dict();
        b.add("StatusCode", Value::integer(matched.empty() ? 100 : 200));
        b.add("ReceiverComputerName", Value::string(r->name));
        b.add("ReceiverModelName", Value::string(kModel));
        b.add("ReceiverMediaCapabilities", Value::data(caps, caps_len));
        Bytes out;
        if (bplist_encode(b, out)) {
            send_response(200, &out, false);
            sock->set_timeout(kDiscoverIdleTimeout);
        } else {
            fail(500);
        }
    }

    /* ASK-5: a file's FileBomPath is "./<FileName>", or "./<folder>/<FileName>"
     * for a regular file; a folder's is "./<FileName>". */
    static bool listed_from(const Value& file, Listed& l)
    {
        std::string name = lookup_str(file, "FileName"), bom = lookup_str(file, "FileBomPath");
        const Value* isdir = file.is(Value::Type::Dict) ? file.find("FileIsDirectory", Value::Type::Boolean) : nullptr;
        bool dir = isdir && isdir->get_boolean();
        if (name.empty() || !airdrop_safe_name(name)) return false;
        if (bom.empty()) bom = "./" + name;
        std::vector<std::string> parts;
        if (bom.compare(0, 2, "./") != 0 || !components(bom, parts)) return false;
        if (parts.size() == 1 && parts[0] == name) {
            l.path = parts[0];
        } else if (parts.size() == 2 && !dir && parts[1] == name) {
            l.path = parts[0] + "/" + parts[1];
            l.wrapper = parts[0];
        } else {
            return false;
        }
        l.name = name;
        l.dir = dir;
        return true;
    }

    /* Waits for the user's answer, the prompt's timeout, or the sender
     * giving up. Bytes that arrive meanwhile are kept for the parser, up to
     * a bound. 1 accept, 2 decline, 0 timed out, -1 gone. */
    int wait_answer()
    {
        auto end = Clock::now() + kPromptTimeout;
        int answer = 0;
        /* Cap any mid-flight TLS 1.3 post-handshake renegotiation read so the
         * prompt loop never stalls for kIoTimeout (30 s). */
        sock->set_timeout(2);
        std::unique_lock<std::mutex> lock(r->mu);
        for (;;) {
            answer = r->answers[id];
            if (answer || r->cancel->cancelled()) break;
            auto now = Clock::now();
            if (now >= end) break;
            r->cv.wait_until(lock, now + kPromptSlice < end ? now + kPromptSlice : end);
            answer = r->answers[id];
            if (answer) break;
            lock.unlock();
            bool gone = false;
            if (sock->readable()) {
                uint8_t buf[4096];
                int n = tls->try_read(buf, sizeof buf);
                if (n == -2) {
                    /* TLS 1.3 post-handshake record or partial record; peer is still waiting. */
                } else if (n <= 0 || input.size() + static_cast<size_t>(n) > kMaxPending) {
                    gone = true;
                } else {
                    input.insert(input.end(), buf, buf + n);
                }
            }
            lock.lock();
            if (gone) {
                answer = -1;
                break;
            }
        }
        r->answers.erase(id);
        lock.unlock();
        sock->set_timeout(kIoTimeout);
        return answer;
    }

    /* Extracts one bounded URL/text item from /Ask Items[] safely (SEC-3, SEC-4). */
    static bool extract_item_url(const Value& it, std::string& out)
    {
        if (it.is(Value::Type::String)) {
            out = it.get_string();
        } else if (it.is(Value::Type::Dict)) {
            out = lookup_str(it, "URL");
            if (out.empty()) out = lookup_str(it, "url");
        } else {
            return false;
        }
        if (out.empty() || out.size() > 4096) return false;
        for (unsigned char c : out) {
            if (c < 0x20 || c == 0x7f) return false;
        }
        return true;
    }

    void on_ask()
    {
        sock->set_timeout(kIoTimeout);
        slot_discover = false; /* HTTP-7 */
        if (accepted || slot_upload) return fail(400);
        Value req;
        if (!decode_body(req)) return fail(400);
        if (const Value* tid = req.find("TransferID")) {
            if (tid->is(Value::Type::Dict)) transfer_id = lookup_str(*tid, "id");
            else if (tid->is(Value::Type::String)) transfer_id = tid->get_string();
        }
        const Value* files = req.find("Files", Value::Type::Array);
        const Value* items = req.find("Items", Value::Type::Array);
        size_t nfiles = files ? files->size() : 0, nitems = items ? items->size() : 0;
        Offer offer;
        bool bad = (nfiles == 0 && nitems == 0) || nfiles > CPIO_MAX_ENTRIES || nitems > 64;
        for (size_t i = 0; i < nfiles && !bad; i++) {
            const Value& f = files->at(i);
            Listed l;
            if (!listed_from(f, l)) {
                bad = true;
                break;
            }
            if (const Value* size = f.find("FileSize", Value::Type::Integer); size && size->get_integer() > 0) {
                uint64_t sz = static_cast<uint64_t>(size->get_integer());
                if (sz > UINT64_MAX - total) {
                    bad = true;
                    break;
                }
                total += sz;
            }
            /* Two items saved under one name would collide in staging, and
             * Windows' names differ by more than case. */
            for (const Listed& x : listed)
                if (x.name == l.name || _wcsicmp(windows_name(x.name).c_str(), windows_name(l.name).c_str()) == 0) bad = true;
            if (bad) break;
            offer.names.push_back(l.name);
            listed.push_back(std::move(l));
        }
        for (size_t i = 0; i < nitems && !bad; i++) {
            std::string u;
            if (!extract_item_url(items->at(i), u)) {
                bad = true;
                break;
            }
            offer.items.push_back(u);
            if (nfiles == 0) offer.names.push_back(u);
        }
        std::string sender = lookup_str(req, "SenderComputerName");
        std::string sender_model = lookup_str(req, "SenderModelName");
        bool contacts_only = false;
        std::string matched_hash = match_sender_record(req, contacts_only);
        if (contacts_only && matched_hash.empty()) {
            trace("receiver: /Ask rejected (401, contacts only)");
            return fail(401);
        }
        if (const Value* icon = req.find("FileIcon", Value::Type::Data);
            icon && !icon->get_data().empty() && icon->get_data().size() <= 512 * 1024) {
            offer.file_icon = icon->get_data();
        }
        if (bad) {
            trace("receiver: /Ask rejected (400, %zu files, %zu items)", nfiles, nitems);
            return fail(400);
        }
        /* Pure link shares finish on /Ask 200 OK; mixed Files + Items shares
         * keep the connection open for /Upload of the listed files. */
        links = (nfiles == 0 && nitems > 0);
        {
            std::lock_guard<std::mutex> lock(r->mu);
            id = ++r->next_id;
            r->answers[id] = 0;
        }
        offer.id = id;
        offer.sender = sender.empty() ? "Someone" : sender;
        offer.sender_model = sender_model;
        offer.matched_hash = matched_hash;
        offer.links = links;
        offer.total = total;
        Receiver::Impl* rr = r;
        r->note([rr, offer] {
            if (rr->cb.offer) rr->cb.offer(offer);
        });

        int answer = wait_answer();
        if (answer != 1) {
            trace("receiver: offer %s", answer == 2 ? "declined" : answer == 0 ? "timed out" : "withdrawn by sender");
            unsigned i = id;
            r->note([rr, i] {
                if (rr->cb.withdrawn) rr->cb.withdrawn(i);
            });
            if (answer == -1) closing = true;
            else fail(403); /* ASK-2: answer a decline rather than leave the sender waiting */
            return;
        }
        Value b = Value::dict();
        b.add("ReceiverComputerName", Value::string(r->name));
        b.add("ReceiverModelName", Value::string(kModel));
        Bytes out;
        bplist_encode(b, out);
        send_response(200, &out, links);
        accepted = true;
        if (links) { /* ASK-4: a link-only share is complete here */
            unsigned i = id;
            std::vector<std::string> urls = offer.items;
            r->note([rr, i, urls] {
                if (rr->cb.received) rr->cb.received(i, true, urls);
            });
        }
    }

    bool upload_start(const HttpHeaders& headers)
    {
        sock->set_timeout(kIoTimeout);
        /* UP-6: only after an accepted /Ask on this connection, once. */
        if (!accepted || links || slot_upload) {
            fail_status = 400;
            return true;
        }
        auto get = [&](const char* k) -> std::string {
            auto it = headers.find(k);
            return it == headers.end() ? std::string() : it->second;
        };
        std::string type = get("content-type");
        trace("upload: type %s, transfer-encoding %s, length %s, content-encoding %s, %zu listed", plain(type).c_str(),
              get("transfer-encoding").empty() ? "-" : plain(get("transfer-encoding")).c_str(),
              get("content-length").empty() ? "-" : plain(get("content-length")).c_str(),
              get("content-encoding").empty() ? "-" : plain(get("content-encoding")).c_str(), listed.size());
        bool dvzip = _stricmp(type.c_str(), "application/x-dvzip") == 0;
        if (!dvzip && _stricmp(type.c_str(), "application/x-cpio") != 0) {
            fail_status = 406; /* UP-4 */
            return true;
        }
        std::string tid = get("transferid");
        if (!transfer_id.empty() && _stricmp(tid.c_str(), transfer_id.c_str()) != 0) { /* UP-9 */
            fail_status = 400;
            return true;
        }
        /* A hidden staging folder beside where the files go. */
        for (int i = 0; i < 16 && staging.empty(); i++) {
            uint32_t rnd = 0;
            BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&rnd), sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            wchar_t leaf[32];
            swprintf(leaf, 32, L"\\.lares-airdrop-%08x", rnd);
            std::wstring p = long_path(r->folder) + leaf;
            if (CreateDirectoryW(p.c_str(), nullptr)) {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_HIDDEN);
                staging = p;
            }
        }
        if (staging.empty()) {
            fail_status = 500;
            return true;
        }
        slot_upload = true;
        CpioCallbacks cb;
        cb.begin = [this](const std::string& p, CpioType t, uint64_t s, Error& e) { return entry_begin(p, t, s, e); };
        cb.data = [this](const uint8_t* b, size_t n, Error& e) { return entry_data(b, n, e); };
        cb.end = [this](Error& e) { return entry_end(e); };
        cpio = std::make_unique<CpioReader>(cb);
        inflate = std::make_unique<Inflater>(dvzip, kMaxUpload,
                                             [this](const uint8_t* b, size_t n, Error& e) { return to_cpio(b, n, e); });
        std::string expect = get("expect");
        if (_stricmp(expect.c_str(), "100-continue") == 0) { /* UP-3 */
            static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
            tls->write(reinterpret_cast<const uint8_t*>(kContinue), sizeof kContinue - 1);
        }
        return true;
    }

    HttpCallbacks callbacks()
    {
        HttpCallbacks cb;
        cb.head = [this](const std::string& method, const std::string& path, unsigned, const HttpHeaders& headers, Error&) {
            fail_status = 0;
            too_big = false;
            body.clear();
            if (method == "GET" || method == "HEAD") route = Route::Get; /* HTTP-5 */
            else if (method != "POST") route = Route::Other;
            else if (path == "/Discover") route = Route::Discover;
            else if (path == "/Ask") route = Route::Ask;
            else if (path == "/Upload") route = Route::Upload;
            else route = Route::Other;
            if (route == Route::Upload) return upload_start(headers);
            return true;
        };
        cb.body = [this](const uint8_t* buf, size_t len, Error&) {
            if (route == Route::Upload) {
                for (size_t i = 0; i < len && raw_in + i < sizeof raw_head; i++) raw_head[raw_in + i] = buf[i];
                raw_in += len;
                if (fail_status || !inflate || upload_error.set) return true; /* discarded; answered at its end */
                inflate->feed(buf, len, &upload_error);
                return true;
            }
            if (route == Route::Discover || route == Route::Ask) {
                if (body.size() + len > kMaxMessage) too_big = true;
                else body.insert(body.end(), buf, buf + len);
            }
            return true; /* any other body is read and dropped */
        };
        cb.done = [this](Error&) {
            msg_done = true;
            return true;
        };
        return cb;
    }

    /* A whole message has been read: answer it. */
    void dispatch()
    {
        if (too_big) {
            trace("receiver: request body too large (413)");
            return fail(413);
        }
        switch (route) {
        case Route::Discover: return on_discover();
        case Route::Ask: return on_ask();
        case Route::Get: send_response(200, nullptr, false); return;
        case Route::Other: return fail(400); /* HTTP-5 */
        case Route::Upload:
            if (fail_status) return fail(fail_status);
            if (!upload_error.set && inflate) inflate->finish(&upload_error);
            if (!upload_error.set && !archive_done) upload_error.assign(ErrorCode::Invalid, "The archive ended early.");
            if (upload_error.set) {
                upload_failed(upload_error.message);
                upload_error.clear();
                return fail(400);
            }
            return; /* finished as soon as the archive completed */
        default: return;
        }
    }

    void serve()
    {
        std::string err;
        tls = tls::Stream::server(sock->transport(), r->id, err);
        if (!tls) {
            trace("receiver: TLS handshake failed: %s", err.c_str());
            return;
        }
        http = std::make_unique<HttpParser>(HttpKind::Request, callbacks());
        std::vector<uint8_t> buf(kReadChunk);
        while (!closing && !r->cancel->cancelled()) {
            if (input.empty()) {
                int n = tls->read(buf.data(), buf.size());
                if (n <= 0) break;
                input.insert(input.end(), buf.begin(), buf.begin() + n);
            }
            size_t used = 0;
            Error e;
            bool ok = http->feed(input.data(), input.size(), &used, &e);
            input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(used));
            if (!ok) {
                if (slot_upload && !staging.empty()) upload_failed("The transfer was malformed.");
                fail(400);
                break;
            }
            /* A refused upload is answered at once, not after its whole body. */
            if (route == Route::Upload && fail_status) {
                fail(fail_status);
                break;
            }
            /* UP-11: complete once TRAILER!!! is read, whatever follows.
             * What does follow is read for a moment, so closing doesn't
             * reset the connection under the answer. */
            if (route == Route::Upload && archive_done && !staging.empty() && !upload_error.set) {
                upload_finish();
                sock->set_timeout(kDrainTimeout);
                size_t drained = 0;
                int n;
                while (drained < kMaxPending && (n = tls->read(buf.data(), buf.size())) > 0) drained += static_cast<size_t>(n);
                break;
            }
            if (route == Route::Upload && upload_error.set && !staging.empty()) {
                upload_failed(upload_error.message);
                upload_error.clear();
                fail(400);
                break;
            }
            if (msg_done) {
                msg_done = false;
                dispatch();
                http->reset();
                route = Route::None;
            }
        }
        if (slot_upload && !staging.empty()) upload_failed("The sender stopped.");
        upload_reset();
        tls->close();
    }
};

} // namespace

void Receiver::Impl::accept_loop()
{
    while (!cancel->cancelled()) {
        int fd = lwip_accept(listen_fd, nullptr, nullptr);
        /* Finished connections' threads are gathered as new ones come. */
        for (auto it = workers.begin(); it != workers.end();) {
            if (*it->done) {
                it->thread.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
        if (fd < 0) continue;   /* a quarter second passed: look at the cancel */
        if (cancel->cancelled() || workers.size() >= static_cast<size_t>(kMaxConnections)) {
            if (!cancel->cancelled()) {
                note([this] {
                    if (cb.trace) cb.trace(0, "receiver: connection limit reached, closing incoming socket");
                });
            }
            lwip_close(fd);
            continue;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers.push_back(Worker{ std::thread([this, fd, done] {
                                      Conn c;
                                      c.r = this;
                                      c.sock = std::make_unique<Sock>(fd, cancel);
                                      c.serve();
                                      *done = true;
                                  }),
                                  done });
    }
    for (Worker& w : workers) w.thread.join();
    workers.clear();
}

Receiver::Receiver(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<Receiver> Receiver::start(Loop& loop, const Address& addr, const std::string& name,
                                          const std::wstring& folder, std::shared_ptr<tls::Identity> id,
                                          ReceiverCallbacks cb, std::string& err)
{
    auto impl = std::make_shared<Impl>(loop);
    impl->cb = std::move(cb);
    impl->name = name;
    impl->folder = folder;
    impl->id = std::move(id);
    for (unsigned i = 0; i <= kPorts && impl->listen_fd < 0; i++) {
        int fd = lwip_socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) break;
        /* As GIO's listener: the last receiver's closed connections don't
         * keep its port. */
        int on = 1;
        lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in6 sa = socket_address(addr, i < kPorts ? static_cast<uint16_t>(kPort + i) : 0);
        if (lwip_bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 && lwip_listen(fd, 16) == 0) {
            sockaddr_in6 bound{};
            socklen_t len = sizeof bound;
            lwip_getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
            impl->port = lwip_ntohs(bound.sin6_port);
            impl->listen_fd = fd;
        } else {
            lwip_close(fd);
        }
    }
    if (impl->listen_fd < 0) {
        err = "Couldn't listen for AirDrop on the link.";
        return nullptr;
    }
    int ms = kSliceMs;
    lwip_setsockopt(impl->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &ms, sizeof ms);
    Impl* raw = impl.get();
    impl->acceptor = std::thread([raw] { raw->accept_loop(); });
    return std::unique_ptr<Receiver>(new Receiver(impl));
}

Receiver::~Receiver()
{
    *impl_->alive = false;
    impl_->cancel->cancel();
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->cv.notify_all();
    }
    if (impl_->acceptor.joinable()) impl_->acceptor.join();
    if (impl_->listen_fd >= 0) lwip_close(impl_->listen_fd);
}

uint16_t Receiver::port() const
{
    return impl_->port;
}

void Receiver::answer(unsigned id, bool accept)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->answers.find(id);
    if (it != impl_->answers.end()) it->second = accept ? 1 : 2;
    impl_->cv.notify_all();
}

void Receiver::set_policy(bool contacts_only, std::set<std::string> contact_hashes)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->contacts_only = contacts_only;
    impl_->contact_hashes = std::move(contact_hashes);
}

void Receiver::set_original_media(bool original)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->original_media = original;
}

void Receiver::set_trusted_roots_for_test(std::vector<std::vector<uint8_t>> roots_der)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->test_roots = std::move(roots_der);
}

// ================================================================== sender

namespace {

struct Response {
    unsigned status = 0;
    Bytes body;
    bool done = false, too_big = false;
    bool interim = false, interim_ended = false; /* a 1xx answer: the real one follows */
};

/* Reads one response, skipping any 100 Continue before it. */
bool read_response(tls::Stream& t, Response& resp, std::string& err)
{
    HttpCallbacks cb;
    cb.head = [&](const std::string&, const std::string&, unsigned status, const HttpHeaders&, Error&) {
        resp.interim = status < 200;
        if (!resp.interim) resp.status = status;
        return true;
    };
    cb.body = [&](const uint8_t* buf, size_t len, Error&) {
        if (resp.body.size() + len > kMaxMessage) resp.too_big = true;
        else resp.body.insert(resp.body.end(), buf, buf + len);
        return true;
    };
    cb.done = [&](Error&) {
        if (resp.interim) resp.interim_ended = true;
        else resp.done = true;
        resp.interim = false;
        return true;
    };
    HttpParser p(HttpKind::Response, cb);
    std::vector<uint8_t> buf(kReadChunk);
    while (!resp.done) {
        int n = t.read(buf.data(), buf.size());
        if (n < 0) {
            err = "The receiver stopped answering.";
            return false;
        }
        if (n == 0) {
            Error e;
            if (!p.eof(&e)) {
                err = e.message;
                return false;
            }
            if (!resp.done) {
                err = "The receiver closed the connection.";
                return false;
            }
            continue;
        }
        size_t off = 0;
        while (off < static_cast<size_t>(n) && !resp.done) {
            size_t used = 0;
            Error e;
            if (!p.feed(buf.data() + off, static_cast<size_t>(n) - off, &used, &e)) {
                err = e.message;
                return false;
            }
            off += used;
            if (resp.interim_ended) { /* a 100 Continue ended: the next message follows */
                resp.interim_ended = false;
                p.reset();
            } else if (!resp.done) {
                break; /* the message goes on in the next read */
            }
        }
    }
    if (resp.too_big) {
        err = "The receiver's answer is too long.";
        return false;
    }
    return true;
}

/* Live archive streaming straight into TLS chunked transfer encoding:
 * CpioWriter -> Deflater -> http_chunk -> tls::Stream::write, with no
 * pre-/Ask temporary file on disk (issue #117). */
struct Streamer {
    std::unique_ptr<CpioWriter> cpio;
    std::unique_ptr<Deflater> z;
    std::shared_ptr<Cancel> cancel;
    Loop* loop = nullptr;
    std::function<void(bool, uint64_t, uint64_t)> progress;
    uint64_t sent = 0;
    uint64_t total = 0;
    Clock::time_point last{};
    bool write_failed = false;

    void note_progress(bool force)
    {
        auto now = Clock::now();
        if (!force && now - last < kProgressEvery) return;
        last = now;
        uint64_t done = sent > total ? total : sent;
        uint64_t tot = total;
        auto fn = progress;
        auto c = cancel;
        loop->post([c, fn, done, tot]() mutable {
            if (!c->cancelled() && fn) fn(true, done, tot);
        });
    }
};

struct TopEntry {
    std::wstring path;
    std::string name;
    std::string entry;
    bool isdir = false;
};

int64_t unix_time(const FILETIME& ft)
{
    ULARGE_INTEGER t{ { ft.dwLowDateTime, ft.dwHighDateTime } };
    return static_cast<int64_t>((t.QuadPart - 116444736000000000ull) / 10000000ull);
}

bool valid_send_url(const std::string& url)
{
    if (url.size() > 4096) return false;
    auto has_scheme = [&](const char* scheme) {
        size_t n = std::strlen(scheme);
        if (url.size() <= n) return false;
        for (size_t i = 0; i < n; i++)
            if ((url[i] | 0x20) != scheme[i]) return false;
        return true;
    };
    if (!has_scheme("https://") && !has_scheme("http://")) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f || c == '"') return false;
    }
    return true;
}

/* Metadata-only pre-/Ask folder scan: computes FileSize and max single-file
 * size without reading file contents into %TEMP%. */
bool scan_tree(const std::wstring& path, uint64_t& size, uint64_t& max_file, unsigned& entries,
               const std::shared_ptr<Cancel>& cancel, Error& e)
{
    if (cancel->cancelled()) {
        e.assign(ErrorCode::Failed, "Cancelled.");
        return false;
    }
    if (++entries > CPIO_MAX_ENTRIES) {
        e.assign(ErrorCode::Limit, "Too many files to send at once.");
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((long_path(path) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::string name = utf8(fd.cFileName);
        if (!airdrop_safe_name(name) || name.compare(0, 2, "._") == 0) continue;
        std::wstring child = path + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = scan_tree(child, size, max_file, entries, cancel, e);
        } else {
            if (++entries > CPIO_MAX_ENTRIES) {
                e.assign(ErrorCode::Limit, "Too many files to send at once.");
                ok = false;
                break;
            }
            uint64_t fsz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (fsz > UINT64_MAX - size) {
                e.assign(ErrorCode::Limit, "The files are too large to send.");
                ok = false;
                break;
            }
            size += fsz;
            if (fsz > max_file) max_file = fsz;
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool stream_file(Streamer& s, const std::wstring& path, const std::string& entry, Error& e)
{
    HANDLE f = CreateFileW(long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (f == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(f, &info)) {
        e.assign(ErrorCode::Failed, "Couldn't read " + utf8(path.substr(path.find_last_of(L"\\/") + 1)) + ".");
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return false;
    }
    uint64_t length = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    bool ok = s.cpio->file(entry, length, unix_time(info.ftLastWriteTime), &e);
    std::vector<uint8_t> buf(kReadChunk);
    uint64_t left = length;
    while (ok && left) {
        if (s.cancel->cancelled()) {
            e.assign(ErrorCode::Failed, "Cancelled.");
            ok = false;
            break;
        }
        DWORD want = static_cast<DWORD>(left < kReadChunk ? left : kReadChunk), got = 0;
        if (!ReadFile(f, buf.data(), want, &got, nullptr) || got == 0) {
            e.assign(ErrorCode::Failed, "A file changed while it was read.");
            ok = false;
            break;
        }
        ok = s.cpio->file_data(buf.data(), got, &e);
        left -= got;
        s.sent += got;
        if (ok) s.note_progress(false);
    }
    CloseHandle(f);
    return ok;
}

/* A folder and what it holds: folders and regular files only, no links. */
bool stream_tree(Streamer& s, const std::wstring& path, const std::string& entry, Error& e)
{
    if (!s.cpio->dir(entry, &e)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((long_path(path) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::string name = utf8(fd.cFileName);
        if (!airdrop_safe_name(name) || name.compare(0, 2, "._") == 0) continue;
        std::wstring child = path + L"\\" + fd.cFileName;
        std::string child_entry = entry + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = stream_tree(s, child, child_entry, e);
        else ok = stream_file(s, child, child_entry, e);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* DVZip unless the receiver's flags are known and leave it out (UP-1b). */
bool use_dvzip(unsigned flags)
{
    bool known = (flags & (0x004 | 0x008)) != 0;
    return !known || (flags & 0x002);
}

/* Posts to the loop unless the work was cancelled by then. */
template <class F> void post_unless_cancelled(Loop& loop, const std::shared_ptr<Cancel>& cancel, F fn)
{
    loop.post([cancel, fn]() mutable {
        if (!cancel->cancelled()) fn();
    });
}

} // namespace

void discover(Loop& loop, const Address& addr, uint16_t port, std::shared_ptr<Cancel> cancel,
              std::function<void(std::optional<Peer>, const std::string& err)> done,
              std::vector<uint8_t> sender_record)
{
    std::thread([&loop, addr, port, cancel, done, sender_record = std::move(sender_record)] {
        std::optional<Peer> peer;
        std::string err;
        Sock sock(-1, cancel);
        Bytes req;
        /* An empty dict by default, or with SenderRecordData when supplied (§6.3). */
        Value body = Value::dict();
        if (!sender_record.empty()) body.add("SenderRecordData", Value::data(sender_record));
        plist_request("/Discover", body, req);
        std::unique_ptr<tls::Stream> t;
        Response resp;
        if (sock.connect(addr, port, kDiscoverConnectTimeout, err) &&
            (t = tls::Stream::client(sock.transport(), err)) != nullptr) {
            sock.set_timeout(kDiscoverTimeout);
            if (!t->write(req.data(), req.size())) err = "The receiver stopped answering.";
            else if (read_response(*t, resp, err) && resp.status == 200) {
                Value v;
                if (bplist_decode(resp.body.data(), resp.body.size(), v)) {
                    std::string name = lookup_str(v, "ReceiverComputerName");
                    if (!name.empty()) { /* DISC-1 */
                        Peer found{ name, lookup_str(v, "ReceiverModelName"), {} };
                        if (const Value* caps = v.find("ReceiverMediaCapabilities", Value::Type::Data);
                            caps && caps->get_data().size() <= 8192) {
                            found.media_capabilities.assign(
                                reinterpret_cast<const char*>(caps->get_data().data()), caps->get_data().size());
                        }
                        peer = std::move(found);
                    }
                }
            }
            t->close();
        }
        post_unless_cancelled(loop, cancel, [done, peer, err] { done(peer, err); });
    }).detach();
}

void send(Loop& loop, const SendRequest& request, std::shared_ptr<Cancel> cancel,
          std::function<void(bool accepted, uint64_t sent, uint64_t total)> progress,
          std::function<void(bool ok, bool declined, const std::string& err)> done)
{
    std::thread([&loop, request, cancel, progress, done] {
        Error e;
        std::string err;
        bool declined = false, ok = false;
        std::string tid = uuid_upper();
        bool dvzip = use_dvzip(request.flags);
        auto finish = [&] {
            std::string message = ok ? std::string() : !err.empty() ? err : e.message;
            post_unless_cancelled(loop, cancel, [done, ok, declined, message] { done(ok, declined, message); });
        };

        if (request.paths.empty() && request.items.empty()) {
            err = "Nothing to send.";
            return finish();
        }
        if (request.items.size() > 64) {
            err = "Too many links to send at once.";
            return finish();
        }

        /* Pre-/Ask metadata scan only: zero %TEMP% spooling so the receiver's
         * prompt appears right away even on multi-gigabyte videos. */
        Value files = Value::array();
        Value items = Value::array();
        std::vector<TopEntry> top;
        std::set<std::string> names;
        std::vector<std::wstring> win_names;
        std::wstring icon_source;
        uint64_t total_bytes = 0;
        uint64_t max_file = 0;
        unsigned entries = 1; /* root "." entry */
        bool good = true;

        for (const std::wstring& p : request.paths) {
            if (!good) break;
            std::wstring base = p.substr(p.find_last_of(L"\\/") + 1);
            std::string name = utf8(base);
            std::wstring wname = windows_name(name);
            bool dup = names.count(name) != 0;
            for (const std::wstring& wn : win_names)
                if (_wcsicmp(wn.c_str(), wname.c_str()) == 0) dup = true;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (name.empty() || !airdrop_safe_name(name) || dup) {
                err = "Can't send " + name + " under its name.";
                good = false;
                break;
            }
            if (!GetFileAttributesExW(long_path(p).c_str(), GetFileExInfoStandard, &fad)) {
                err = name + " isn't a file or folder.";
                good = false;
                break;
            }
            bool isdir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::string entry = "./" + name;
            uint64_t size = 0;
            if (isdir) {
                good = scan_tree(p, size, max_file, entries, cancel, e);
            } else {
                if (++entries > CPIO_MAX_ENTRIES) {
                    e.assign(ErrorCode::Limit, "Too many files to send at once.");
                    good = false;
                    break;
                }
                if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    HANDLE f = CreateFileW(long_path(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
                    LARGE_INTEGER fsz{};
                    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &fsz)) {
                        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
                        err = "Couldn't read " + name + ".";
                        good = false;
                        break;
                    }
                    CloseHandle(f);
                    size = static_cast<uint64_t>(fsz.QuadPart);
                } else {
                    size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                }
                if (size > max_file) max_file = size;
                if (icon_source.empty() && file_icon_supported(p)) icon_source = p;
            }
            if (!good) break;
            if (size > UINT64_MAX - total_bytes) {
                err = "The files are too large to send.";
                good = false;
                break;
            }
            total_bytes += size;

            Value f = Value::dict();
            f.add("FileBomPath", Value::string(entry));
            f.add("FileIsDirectory", Value::boolean(isdir));
            f.add("FileName", Value::string(name));
            f.add("FileSize", Value::integer(static_cast<int64_t>(size)));
            f.add("FileType", Value::string(airdrop_uti(name, isdir)));
            f.add("ShouldConvertMediaFormats", Value::boolean(false));
            files.push(std::move(f));
            names.insert(name);
            win_names.push_back(std::move(wname));
            top.push_back(TopEntry{ p, name, entry, isdir });
        }

        for (const std::string& url : request.items) {
            if (!good) break;
            if (!valid_send_url(url)) {
                err = "That link can't be sent.";
                good = false;
                break;
            }
            items.push(Value::string(url));
        }
        if (!good) return finish();

        /* Bounded preview thumbnail for photo/video shares (or explicit caller icon). */
        Bytes icon;
        if (!request.file_icon.empty() && request.file_icon.size() <= 512 * 1024) {
            icon = request.file_icon;
        } else if (!icon_source.empty()) {
            make_file_icon(icon_source, icon);
        }

        bool links_only = top.empty() && !request.items.empty();

        /* ASK-7: the current form, keys in the order Finder's come. */
        Value b = Value::dict();
        b.add("BundleID", Value::string(links_only ? "com.apple.Safari" : "com.apple.finder"));
        b.add("ConvertMediaFormats", Value::boolean(false));
        if (!icon.empty()) b.add("FileIcon", Value::data(icon));
        b.add("Files", std::move(files));
        b.add("Items", std::move(items));
        b.add("SenderComputerName", Value::string(request.name));
        b.add("SenderID", Value::string(request.id));
        b.add("SenderModelName", Value::string(kModel));
        Value idv = Value::dict();
        idv.add("id", Value::string(tid));
        b.add("TransferID", std::move(idv));
        Value tt = Value::dict();
        tt.add(links_only ? "links" : "files", Value::dict());
        b.add("TransferType", std::move(tt));
        Bytes ask;
        if (!plist_request("/Ask", b, ask)) {
            err = "Couldn't describe the transfer.";
            return finish();
        }

        Sock sock(-1, cancel);
        if (!sock.connect(request.addr, request.port, kConnectTimeout, err)) return finish();
        std::unique_ptr<tls::Stream> t = tls::Stream::client(sock.transport(), err);
        if (!t) return finish();
        sock.set_timeout(kIoTimeout);
        if (!t->write(ask.data(), ask.size())) {
            err = "The receiver stopped answering.";
            return finish();
        }
        sock.set_timeout(kSenderAskTimeout);
        Response resp;
        std::string why;
        if (!read_response(*t, resp, why)) {
            err = "Declined, or not answered."; /* ASK-1 */
            declined = true;
            return finish();
        }
        if (resp.status == 401) {
            err = "Set AirDrop on that device to Everyone for 10 Minutes.";
            return finish();
        }
        if (resp.status != 200) {
            err = "Declined, or not answered."; /* ASK-1 */
            declined = true;
            return finish();
        }

        /* ASK-4: link-only shares finish on /Ask 200 OK with no /Upload. */
        if (links_only) {
            post_unless_cancelled(loop, cancel, [progress] {
                if (progress) progress(true, 1, 1);
            });
            t->close();
            ok = true;
            return finish();
        }

        post_unless_cancelled(loop, cancel, [progress, total_bytes] {
            if (progress) progress(true, 0, total_bytes);
        });
        sock.set_timeout(kIoTimeout);

        /* UP-1a, UP-9, UP-10: stream the archive with Transfer-Encoding: chunked
         * directly after /Ask acceptance, keeping memory and temp disk near zero. */
        char head[512];
        int hn = std::snprintf(head, sizeof head,
                               "POST /Upload HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\nTransferID: %s\r\n"
                               "TotalBytes: %llu\r\nContent-Type: %s\r\nConnection: keep-alive\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n",
                               tid.c_str(), static_cast<unsigned long long>(total_bytes),
                               dvzip ? "application/x-dvzip" : "application/x-cpio");
        bool wrote = t->write(reinterpret_cast<const uint8_t*>(head), static_cast<size_t>(hn));

        Streamer s;
        s.cancel = cancel;
        s.loop = &loop;
        s.progress = progress;
        s.total = total_bytes;
        tls::Stream* tls_ptr = t.get();
        s.z = std::make_unique<Deflater>(dvzip, [tls_ptr, &s](const uint8_t* buf, size_t len, Error& er) {
            if (!len) return true;
            if (s.cancel->cancelled()) {
                er.assign(ErrorCode::Failed, "Cancelled.");
                return false;
            }
            Bytes chunk = http_chunk(buf, len);
            if (!tls_ptr->write(chunk.data(), chunk.size())) {
                s.write_failed = true;
                er.assign(ErrorCode::Failed, "The receiver stopped answering.");
                return false;
            }
            return true;
        });
        Deflater* z = s.z.get();
        CpioFormat fmt = max_file > CPIO_ODC_MAX_SIZE ? CpioFormat::AfioLarge : CpioFormat::Odc;
        s.cpio = std::make_unique<CpioWriter>(
            [z](const uint8_t* buf, size_t len, Error& er) { return z->feed(buf, len, &er); }, fmt);

        if (wrote) {
            wrote = s.cpio->dir(".", &e);
            for (const TopEntry& item : top) {
                if (!wrote) break;
                wrote = item.isdir ? stream_tree(s, item.path, item.entry, e)
                                   : stream_file(s, item.path, item.entry, e);
            }
            if (wrote) wrote = s.cpio->finish(&e) && s.z->finish(&e);
            if (wrote) {
                Bytes tail = http_chunk(nullptr, 0);
                if (!t->write(tail.data(), tail.size())) {
                    s.write_failed = true;
                    wrote = false;
                }
            }
        } else {
            s.write_failed = true;
        }
        if (wrote) s.note_progress(true);

        /* If a local read failed or the user cancelled, fail immediately; if
         * the network write failed, check whether the receiver sent an error
         * response before closing. */
        if (!wrote && !s.write_failed) {
            t->close();
            return finish();
        }
        err.clear();
        Response done_resp;
        sock.set_timeout(kUploadResponseTimeout);
        if (!read_response(*t, done_resp, err)) return finish();
        if (done_resp.status != 200) {
            err = "The receiver refused the files (" + std::to_string(done_resp.status) + ").";
            return finish();
        }
        t->close();
        ok = true;
        finish();
    }).detach();
}

} // namespace lares::airdrop
