// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "share_win.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <cfgmgr32.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace lares {

// ------------------------------------------------------------- plain data

namespace {

/* "key=value" among a line's space-separated words; empty if it isn't there. */
std::string field(const std::string& line, const char* key)
{
    std::string want = std::string(" ") + key + "=";
    size_t at = line.find(want);
    if (at == std::string::npos) return {};
    at += want.size();
    size_t end = line.find_first_of(" \t", at);
    return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

uint64_t number(const std::string& s)
{
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}

/* The text after a line's first word. */
std::string rest(const std::string& line)
{
    size_t sp = line.find(' ');
    return sp == std::string::npos ? std::string() : line.substr(sp + 1);
}

bool starts(const std::string& s, const char* prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

ShareLink link_from(const std::string& s)
{
    if (s == "starting") return ShareLink::Starting;
    if (s == "up") return ShareLink::Up;
    if (s == "stopping") return ShareLink::Stopping;
    if (s == "failed") return ShareLink::Failed;
    return ShareLink::Off;
}

ShareDiscoverMode mode_from(const std::string& s, bool receiving_fallback)
{
    if (s == "contacts") return ShareDiscoverMode::ContactsOnly;
    if (s == "everyone") return ShareDiscoverMode::Everyone;
    if (s == "off") return ShareDiscoverMode::Off;
    return receiving_fallback ? ShareDiscoverMode::Everyone : ShareDiscoverMode::Off;
}

ShareDeviceState device_from(const std::string& s)
{
    if (s == "asking") return ShareDeviceState::Asking;
    if (s == "sending") return ShareDeviceState::Sending;
    if (s == "sent") return ShareDeviceState::Sent;
    if (s == "declined") return ShareDeviceState::Declined;
    if (s == "failed") return ShareDeviceState::Failed;
    return ShareDeviceState::Found;
}

ShareIncomingState incoming_from(const std::string& s)
{
    if (s == "receiving") return ShareIncomingState::Receiving;
    if (s == "done") return ShareIncomingState::Done;
    if (s == "failed") return ShareIncomingState::Failed;
    return ShareIncomingState::Asking;
}

} // namespace

bool share_parse_block(const std::vector<std::string>& lines, ShareSession& out)
{
    ShareSession s;
    bool have_state = false;
    for (const std::string& line : lines) {
        if (starts(line, "state ")) {
            have_state = true;
            s.ready = field(line, "setup") == "ready";
            s.link = link_from(field(line, "link"));
            s.receiving = field(line, "receiving") == "1";
            s.mode = mode_from(field(line, "mode"), s.receiving);
            if (s.mode != ShareDiscoverMode::Off) s.receiving = true;
            s.original_media = field(line, "media") == "original";
            s.left = static_cast<int>(std::min<uint64_t>(number(field(line, "left")), 24 * 3600));
            s.browsing = field(line, "browsing") == "1";
            s.sending = field(line, "sending") == "1";
            s.wake = field(line, "wake") == "1";
            s.quiet = field(line, "quiet") == "1";
        } else if (starts(line, "name ")) {
            s.name = rest(line);
        } else if (starts(line, "card ")) {
            s.card = rest(line);
        } else if (starts(line, "error ")) {
            s.error = rest(line);
        } else if (starts(line, "incoming ")) {
            s.has_incoming = true;
            s.incoming.id = static_cast<unsigned>(number(field(line, "id")));
            s.incoming.state = incoming_from(field(line, "state"));
            s.incoming.done = number(field(line, "done"));
            s.incoming.total = number(field(line, "total"));
            s.incoming.links = field(line, "links") == "1";
        } else if (starts(line, "incoming-sender ")) {
            s.incoming.sender = rest(line);
        } else if (starts(line, "incoming-model ")) {
            s.incoming.sender_model = rest(line);
        } else if (starts(line, "incoming-thumb ")) {
            s.incoming.thumb_path = rest(line);
        } else if (starts(line, "incoming-contact ")) {
            s.incoming.matched_hash = rest(line);
        } else if (starts(line, "incoming-name ")) {
            s.incoming.names.push_back(rest(line));
        } else if (starts(line, "incoming-link ")) {
            s.incoming.offered_links.push_back(rest(line));
        } else if (starts(line, "incoming-saved ")) {
            s.incoming.saved.push_back(rest(line));
        } else if (starts(line, "incoming-message ")) {
            s.incoming.message = rest(line);
        } else if (starts(line, "device ")) {
            /* Its words, then a tab before each of key, name, model and message. */
            ShareDevice d;
            size_t tab = line.find('\t');
            std::string head = line.substr(0, tab);
            d.state = device_from(field(head, "state"));
            d.sent = number(field(head, "sent"));
            d.total = number(field(head, "total"));
            std::string* parts[4] = { &d.key, &d.name, &d.model, &d.message };
            for (int i = 0; i < 4 && tab != std::string::npos; ++i) {
                size_t next = line.find('\t', tab + 1);
                *parts[i] = line.substr(tab + 1, next == std::string::npos ? std::string::npos : next - tab - 1);
                tab = next;
            }
            if (!d.key.empty()) s.devices.push_back(std::move(d));
        }
    }
    if (!have_state) return false;
    out = std::move(s);
    return true;
}

namespace {

/* UTF-16 to UTF-8 and back without Win32, so the tests run anywhere. */
void put_utf8(std::string& out, uint32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

std::string narrow(const std::wstring& w)
{
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t c = static_cast<uint16_t>(w[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) {
            uint32_t lo = static_cast<uint16_t>(w[i + 1]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        put_utf8(out, c);
    }
    return out;
}

std::wstring base_name(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

} // namespace

std::string share_send_line(const std::vector<std::wstring>& paths, std::vector<std::wstring>* names)
{
    std::string line = "send";
    bool any = false;
    for (const std::wstring& p : paths) {
        if (p.empty() || p.find_first_of(L"\t\r\n") != std::wstring::npos) continue;
        line += "\t" + narrow(p);
        if (names) names->push_back(base_name(p));
        any = true;
    }
    return any ? line : std::string();
}

std::string share_send_to_line(const std::string& key, const std::vector<std::wstring>& paths,
                               std::vector<std::wstring>* names)
{
    if (key.empty() || key.find_first_of("\t\r\n") != std::string::npos) return {};
    std::string line = "send-to\t" + key;
    bool any = false;
    for (const std::wstring& p : paths) {
        if (p.empty() || p.find_first_of(L"\t\r\n") != std::wstring::npos) continue;
        line += "\t" + narrow(p);
        if (names) names->push_back(base_name(p));
        any = true;
    }
    return any ? line : std::string();
}

std::string share_send_url_line(const std::vector<std::string>& urls, std::vector<std::string>* accepted)
{
    std::string line = "send-url";
    bool any = false;
    for (const std::string& u : urls) {
        if (u.empty() || u.size() > 2048 || !share_link_opens(u) ||
            u.find_first_of("\t\r\n") != std::string::npos)
            continue;
        line += "\t" + u;
        if (accepted) accepted->push_back(u);
        any = true;
    }
    return any ? line : std::string();
}

std::string share_send_url_to_line(const std::string& key, const std::vector<std::string>& urls,
                                   std::vector<std::string>* accepted)
{
    if (key.empty() || key.find_first_of("\t\r\n") != std::string::npos) return {};
    std::string line = "send-url-to\t" + key;
    bool any = false;
    for (const std::string& u : urls) {
        if (u.empty() || u.size() > 2048 || !share_link_opens(u) ||
            u.find_first_of("\t\r\n") != std::string::npos)
            continue;
        line += "\t" + u;
        if (accepted) accepted->push_back(u);
        any = true;
    }
    return any ? line : std::string();
}

std::string share_offer_text(const ShareIncoming& in)
{
    size_t n = in.names.size();
    if (in.links) return n == 1 ? "a link" : "links";
    std::string first = n ? in.names[0] : "files";
    size_t extra = (n > 1 ? n - 1 : 0) + in.offered_links.size();
    if (extra == 0) return first;
    if (n == 1 && in.offered_links.size() == 1) return first + " and 1 link";
    return first + " and " + std::to_string(extra) + " more";
}

std::string share_format_size(uint64_t bytes)
{
    char buf[48];
    if (bytes < 1000) {
        std::snprintf(buf, sizeof(buf), "%u bytes", static_cast<unsigned>(bytes));
    } else {
        const char* units[] = { "kB", "MB", "GB", "TB" };
        double v = static_cast<double>(bytes) / 1000.0;
        int u = 0;
        while (v >= 1000.0 && u < 3) {
            v /= 1000.0;
            ++u;
        }
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    }
    return buf;
}

std::string share_format_eta(uint64_t done, uint64_t total, uint64_t elapsed_ms)
{
    if (!total || done == 0 || elapsed_ms < 400) return {};
    if (done >= total) return "Less than 5 seconds";
    double rate = static_cast<double>(done) / (static_cast<double>(elapsed_ms) / 1000.0);
    if (rate <= 1.0) return {};
    double left_s = static_cast<double>(total - done) / rate;
    if (left_s < 5.0) return "About 5 seconds";
    if (left_s < 60.0) {
        int s = static_cast<int>((left_s + 2.5) / 5.0) * 5;
        if (s < 5) s = 5;
        return "About " + std::to_string(s) + " seconds";
    }
    int mins = static_cast<int>((left_s + 30.0) / 60.0);
    if (mins <= 1) return "About 1 minute";
    return "About " + std::to_string(mins) + " minutes";
}

bool share_link_opens(const std::string& url)
{
    auto is = [&](const char* scheme) {
        size_t n = std::strlen(scheme);
        if (url.size() <= n) return false;
        for (size_t i = 0; i < n; ++i)
            if ((url[i] | 0x20) != scheme[i]) return false;
        return true;
    };
    if (!is("https://") && !is("http://")) return false;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '"') return false;
    return true;
}

namespace {

std::string ascii_lower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool contains_ci(const std::string& hay, const char* needle)
{
    return ascii_lower(hay).find(needle) != std::string::npos;
}

} // namespace

ShareDeviceKind share_device_kind(const std::string& model, const std::string& name)
{
    auto check = [](const std::string& s) -> ShareDeviceKind {
        if (s.empty()) return ShareDeviceKind::Other;
        std::string lo = ascii_lower(s);
        if (lo.find("iphone") != std::string::npos || lo.find("ipod") != std::string::npos) return ShareDeviceKind::Phone;
        if (lo.find("ipad") != std::string::npos) return ShareDeviceKind::Tablet;
        if (lo.find("macbook") != std::string::npos) return ShareDeviceKind::Laptop;
        if (lo.find("watch") != std::string::npos) return ShareDeviceKind::Watch;
        if (lo.find("imac") != std::string::npos || lo.find("macmini") != std::string::npos ||
            lo.find("mac mini") != std::string::npos || lo.find("macstudio") != std::string::npos ||
            lo.find("mac studio") != std::string::npos || lo.find("macpro") != std::string::npos ||
            lo.find("mac pro") != std::string::npos || lo.find("windows") != std::string::npos ||
            lo.find("linux") != std::string::npos)
            return ShareDeviceKind::Desktop;
        if (lo.compare(0, 3, "mac") == 0) return ShareDeviceKind::Laptop;
        return ShareDeviceKind::Other;
    };
    ShareDeviceKind k = check(model);
    if (k != ShareDeviceKind::Other) return k;
    return check(name);
}

std::string share_model_label(const std::string& model, const std::string& name)
{
    auto from_str = [](const std::string& s) -> std::string {
        if (s.empty()) return {};
        std::string lo = ascii_lower(s);
        if (lo.find("iphone") != std::string::npos) return "iPhone";
        if (lo.find("ipad") != std::string::npos) return "iPad";
        if (lo.find("macbookair") != std::string::npos || lo.find("macbook air") != std::string::npos) return "MacBook Air";
        if (lo.find("macbookpro") != std::string::npos || lo.find("macbook pro") != std::string::npos) return "MacBook Pro";
        if (lo.find("macbook") != std::string::npos) return "MacBook";
        if (lo.find("imac") != std::string::npos) return "iMac";
        if (lo.find("macmini") != std::string::npos || lo.find("mac mini") != std::string::npos) return "Mac mini";
        if (lo.find("macstudio") != std::string::npos || lo.find("mac studio") != std::string::npos) return "Mac Studio";
        if (lo.find("macpro") != std::string::npos || lo.find("mac pro") != std::string::npos) return "Mac Pro";
        if (lo.find("watch") != std::string::npos) return "Apple Watch";
        if (lo.find("realitydevice") != std::string::npos || lo.find("vision") != std::string::npos) return "Apple Vision Pro";
        if (lo == "windows" || lo.find("windows") != std::string::npos) return "Windows";
        if (lo == "linux" || lo.find("linux") != std::string::npos) return "Linux";
        if (lo.compare(0, 3, "mac") == 0) return "Mac";
        return {};
    };
    std::string lbl = from_str(model);
    if (!lbl.empty()) return lbl;
    lbl = from_str(name);
    if (!lbl.empty()) return lbl;
    return "Apple Device";
}

std::string share_initials(const std::string& name)
{
    /* Strip trailing "'s iPhone", "'s MacBook Air", etc., so "Ana's iPhone" -> "A". */
    std::string base = name;
    size_t pos = base.find("'s ");
    if (pos == std::string::npos) pos = base.find("\xE2\x80\x99s ");
    if (pos != std::string::npos && pos > 0) base = base.substr(0, pos);
    // Skip generic device names without a person's name
    if (contains_ci(base, "iphone") || contains_ci(base, "ipad") || contains_ci(base, "macbook") ||
        contains_ci(base, "imac") || contains_ci(base, "apple device"))
        return {};

    std::string out;
    bool in_word = false;
    for (size_t i = 0; i < base.size() && out.size() < 2; ++i) {
        unsigned char c = static_cast<unsigned char>(base[i]);
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum && !in_word) {
            out.push_back(static_cast<char>(std::toupper(c)));
            in_word = true;
        } else if (c == ' ' || c == '-' || c == '_') {
            in_word = false;
        }
    }
    return out;
}

/* Self-contained SHA-256 (FIPS 180-4) so contact hashing works identically
 * in unit tests and runtime without external libraries. */
std::string share_sha256_hex(const std::string& input)
{
    static const uint32_t kK[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    auto rotr = [](uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8ull;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64] = {};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char hex[65];
    for (int i = 0; i < 8; ++i) std::snprintf(hex + i * 8, 9, "%08x", h[i]);
    hex[64] = '\0';
    return hex;
}

std::string share_hash_email(const std::string& email)
{
    size_t a = 0, b = email.size();
    while (a < b && std::isspace(static_cast<unsigned char>(email[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(email[b - 1]))) --b;
    if (a >= b) return {};
    std::string norm = ascii_lower(email.substr(a, b - a));
    if (norm.find('@') == std::string::npos) return {};
    return share_sha256_hex(norm);
}

std::vector<std::string> share_hash_phone_variants(const std::string& phone)
{
    std::string digits;
    for (char c : phone) {
        if (c >= '0' && c <= '9') digits.push_back(c);
    }
    if (digits.size() < 4) return {};
    std::vector<std::string> out;
    out.push_back(share_sha256_hex("+" + digits));
    out.push_back(share_sha256_hex(digits));
    return out;
}

std::vector<std::string> share_contact_hashes(const std::vector<ShareContact>& contacts)
{
    std::vector<std::string> out;
    auto push = [&](const std::string& h) {
        if (h.size() == 64 && std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
    };
    for (const ShareContact& c : contacts) {
        if (!c.email.empty()) push(share_hash_email(c.email));
        if (!c.phone.empty()) {
            for (const std::string& h : share_hash_phone_variants(c.phone)) push(h);
        }
    }
    return out;
}

static std::string trim_copy(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

static std::string clean_ini_value(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '\r' || c == '\n') continue;
        out.push_back(c);
    }
    return trim_copy(out);
}

std::vector<ShareContact> parse_contacts_ini(const std::string& text)
{
    std::vector<ShareContact> list;
    ShareContact cur;
    bool in_section = false;
    auto flush = [&] {
        if (in_section && (!cur.name.empty() || !cur.device.empty() || !cur.email.empty() || !cur.phone.empty())) {
            list.push_back(cur);
        }
        cur = ShareContact{};
    };
    std::istringstream ss(text);
    std::string line;
    bool first_line = true;
    while (std::getline(ss, line)) {
        if (first_line) {
            first_line = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        std::string t = trim_copy(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            flush();
            in_section = true;
            continue;
        }
        if (!in_section) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = ascii_lower(trim_copy(t.substr(0, eq)));
        std::string v = trim_copy(t.substr(eq + 1));
        if (k == "name") cur.name = v;
        else if (k == "device") cur.device = v;
        else if (k == "email") cur.email = v;
        else if (k == "phone") cur.phone = v;
        else if (k == "photo") cur.photo = v;
    }
    flush();
    return list;
}

std::string serialize_contacts_ini(const std::vector<ShareContact>& contacts)
{
    std::ostringstream ss;
    int idx = 1;
    for (const ShareContact& c : contacts) {
        if (c.name.empty() && c.device.empty() && c.email.empty() && c.phone.empty()) continue;
        ss << "[contact-" << idx++ << "]\n";
        if (!c.name.empty()) ss << "name=" << clean_ini_value(c.name) << "\n";
        if (!c.device.empty()) ss << "device=" << clean_ini_value(c.device) << "\n";
        if (!c.email.empty()) ss << "email=" << clean_ini_value(c.email) << "\n";
        if (!c.phone.empty()) ss << "phone=" << clean_ini_value(c.phone) << "\n";
        if (!c.photo.empty()) ss << "photo=" << clean_ini_value(c.photo) << "\n";
        ss << "\n";
    }
    return ss.str();
}

static std::string normalize_contact_name(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        // Normalize UTF-8 curly apostrophes (U+2018 / U+2019: E2 80 98 / E2 80 99) to ASCII '\''
        if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) == 0x98 || static_cast<unsigned char>(s[i + 2]) == 0x99)) {
            out.push_back('\'');
            i += 2;
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return trim_copy(out);
}

static std::string owner_prefix_norm(const std::string& norm)
{
    size_t p = norm.find("'s ");
    if (p != std::string::npos && p >= 2) return trim_copy(norm.substr(0, p));
    return {};
}

const ShareContact* share_find_contact(const std::vector<ShareContact>& contacts, const std::string& name,
                                       const std::string& matched_hash)
{
    if (!matched_hash.empty()) {
        std::string want = ascii_lower(matched_hash);
        for (const ShareContact& c : contacts) {
            if (!c.email.empty() && share_hash_email(c.email) == want) return &c;
            if (!c.phone.empty()) {
                for (const std::string& h : share_hash_phone_variants(c.phone))
                    if (h == want) return &c;
            }
        }
    }
    if (name.empty()) return nullptr;
    std::string lo = normalize_contact_name(name);
    for (const ShareContact& c : contacts) {
        if (!c.device.empty() && normalize_contact_name(c.device) == lo) return &c;
        if (!c.name.empty() && normalize_contact_name(c.name) == lo) return &c;
    }
    std::string owner = owner_prefix_norm(lo);
    if (!owner.empty()) {
        for (const ShareContact& c : contacts) {
            if (!c.name.empty() && normalize_contact_name(c.name) == owner) return &c;
        }
    }
    return nullptr;
}

void airdrop_wake_data(const uint8_t tag[3], uint8_t out[kAirdropWakeBytes])
{
    std::memset(out, 0, kAirdropWakeBytes);
    out[0] = 0x05;   // AirDrop
    out[1] = 18;     // its payload's length
    out[2] = 0x40;
    std::memcpy(out + 3, tag, 3);
    out[10] = 0x03;  // the version
}

#ifdef _WIN32

// ------------------------------------------------------------ what's set up

namespace {

std::wstring exe_dir()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
}

/* lares-share.exe, which comes beside lares.exe. */
std::wstring share_exe()
{
    return exe_dir() + L"\\lares-share.exe";
}

enum class AdapterDriver { Absent, Other, WinUsb };

/* The adapter's device, if it's plugged in, and whether Windows gives it
 * WinUSB (Set up's doing) or its own driver. */
AdapterDriver adapter_driver()
{
    const ULONG filter = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, kShareAdapterId, filter) != CR_SUCCESS || len <= 1) return AdapterDriver::Absent;
    std::vector<wchar_t> list(len);
    if (CM_Get_Device_ID_ListW(kShareAdapterId, list.data(), len, filter) != CR_SUCCESS) return AdapterDriver::Absent;
    for (const wchar_t* id = list.data(); *id; id += wcslen(id) + 1) {
        DEVINST inst = 0;
        if (CM_Locate_DevNodeW(&inst, const_cast<wchar_t*>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        wchar_t service[64] = L"";
        ULONG size = sizeof(service) - sizeof(wchar_t);
        if (CM_Get_DevNode_Registry_PropertyW(inst, CM_DRP_SERVICE, nullptr, service, &size, 0) == CR_SUCCESS &&
            _wcsicmp(service, L"WinUSB") == 0)
            return AdapterDriver::WinUsb;
        return AdapterDriver::Other;
    }
    return AdapterDriver::Absent;
}

ShareSetup look()
{
    switch (adapter_driver()) {
    case AdapterDriver::WinUsb: return ShareSetup::Ready;
    case AdapterDriver::Other: return ShareSetup::NeedsSetup;
    default: return ShareSetup::NoAdapter;
    }
}

std::string read_small_file(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    char buf[512];
    DWORD n = 0;
    std::string out;
    if (ReadFile(f, buf, sizeof(buf), &n, nullptr)) out.assign(buf, n);
    CloseHandle(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

void write_line(HANDLE to, const std::string& line)
{
    DWORD wrote = 0;
    if (to) WriteFile(to, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
}

} // namespace

std::wstring share_contacts_path(bool dry_run)
{
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appdata)) && appdata) {
        dir = std::wstring(appdata) + L"\\lares";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    CoTaskMemFree(appdata);
    if (dir.empty()) dir = L".";
    return dir + (dry_run ? L"\\airdrop-contacts-dry.ini" : L"\\airdrop-contacts.ini");
}

bool load_share_contacts(const std::wstring& path, std::vector<ShareContact>& out)
{
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    std::string text;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 512 * 1024) {
        text.resize(static_cast<size_t>(sz.QuadPart));
        DWORD n = 0;
        if (ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &n, nullptr)) text.resize(n);
        else text.clear();
    }
    CloseHandle(f);
    out = parse_contacts_ini(text);
    return true;
}

bool save_share_contacts(const std::wstring& path, const std::vector<ShareContact>& contacts)
{
    std::string text = serialize_contacts_ini(contacts);
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = text.empty() ? TRUE : WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(f);
    if (!ok || wrote != text.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool share_adapter_present()
{
    return adapter_driver() != AdapterDriver::Absent;
}

// ------------------------------------------------------------- the manager

ShareManager::ShareManager(bool dry_run) : dry_run_(dry_run)
{
    contacts_path_ = share_contacts_path(dry_run_);
    load_share_contacts(contacts_path_, view_.contacts);
    if (!dry_run_) return;
    /* A dry run shows a scene and pretends every change: LARES_DRY_SHARE
     * picks the scene. */
    wchar_t scene[48] = {};
    GetEnvironmentVariableW(L"LARES_DRY_SHARE", scene, 48);
    std::wstring want = scene;
    view_.setup = ShareSetup::Ready;
    view_.s.ready = true;
    view_.s.name = "This PC";
    auto add_sample_devices = [&] {
        ShareDevice a, b, c;
        a.key = "susuin";
        a.name = "Susuin";
        a.model = "iPhone17,1";
        b.key = "ana";
        b.name = "Ana's iPhone";
        b.model = "iPhone16,2";
        c.key = "bruno";
        c.name = "Bruno's MacBook Air";
        c.model = "MacBookAir10,1";
        view_.s.devices = { a, b, c };
    };
    auto set_everyone = [&] {
        view_.running = true;
        view_.s.link = ShareLink::Up;
        view_.s.mode = ShareDiscoverMode::Everyone;
        view_.s.receiving = true;
        view_.s.browsing = true;
        view_.s.wake = true;
        view_.s.left = 540;
        view_.left_at_ms = GetTickCount64();
    };
    auto set_incoming = [&](ShareIncomingState state) {
        set_everyone();
        view_.s.has_incoming = true;
        view_.s.incoming.id = 1;
        view_.s.incoming.state = state;
        view_.s.incoming.sender = "Susuin";
        view_.s.incoming.sender_model = "iPhone17,1";
        view_.s.incoming.names = { "61541ab060fa3.PNG" };
        wchar_t thumb_env[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LARES_DRY_THUMB", thumb_env, MAX_PATH) > 0) {
            view_.s.incoming.thumb_path = narrow(thumb_env);
        }
        view_.s.incoming.total = 13200000;
        view_.s.incoming.done = state == ShareIncomingState::Receiving ? 9900000
                              : state == ShareIncomingState::Done ? 13200000 : 0;
        view_.xfer_start_ms = GetTickCount64() - 12000;
        if (state == ShareIncomingState::Done)
            view_.s.incoming.saved = { "C:\\Users\\me\\Downloads\\61541ab060fa3.PNG" };
        if (state == ShareIncomingState::Failed)
            view_.s.incoming.message = "Susuin canceled the copy.";
        ShareDevice s;
        s.key = "susuin";
        s.name = "Susuin";
        s.model = "iPhone17,1";
        view_.s.devices = { s };
    };

    if (want == L"noadapter") {
        view_.setup = ShareSetup::NoAdapter;
    } else if (want == L"setup") {
        view_.setup = ShareSetup::NeedsSetup;
    } else if (want == L"setup-failed") {
        view_.setup = ShareSetup::NeedsSetup;
        view_.setup_error = "Nothing changed: Windows' permission wasn't given.";
    } else if (want == L"off") {
        view_.running = true;
        view_.s.link = ShareLink::Up;
        view_.s.mode = ShareDiscoverMode::Off;
        view_.s.browsing = true;
        view_.s.wake = true;
        add_sample_devices();
    } else if (want == L"contacts") {
        view_.running = true;
        view_.s.link = ShareLink::Up;
        view_.s.mode = ShareDiscoverMode::ContactsOnly;
        view_.s.receiving = true;
        view_.s.browsing = true;
        view_.s.wake = true;
        add_sample_devices();
    } else if (want == L"receiving") {
        set_everyone();
        add_sample_devices();
    } else if (want == L"quiet") {
        set_everyone();
        view_.s.quiet = true;
    } else if (want == L"incoming") {
        set_incoming(ShareIncomingState::Asking);
    } else if (want == L"incoming-receiving") {
        set_incoming(ShareIncomingState::Receiving);
    } else if (want == L"incoming-done") {
        set_incoming(ShareIncomingState::Done);
    } else if (want == L"incoming-failed") {
        set_incoming(ShareIncomingState::Failed);
    } else if (want == L"sending") {
        set_everyone();
        view_.s.sending = true;
        view_.files = { L"Holiday.mov", L"Notes.pdf" };
        view_.xfer_start_ms = GetTickCount64() - 8000;
        ShareDevice a, b, c;
        a.key = "a";
        a.name = "Ana's iPhone";
        a.model = "iPhone16,2";
        a.state = ShareDeviceState::Sending;
        a.sent = 41000000;
        a.total = 96000000;
        b.key = "b";
        b.name = "Living room iPad";
        b.model = "iPad14,5";
        c.key = "c";
        c.name = "Bruno's MacBook Air";
        c.model = "MacBookAir10,1";
        c.state = ShareDeviceState::Declined;
        view_.s.devices = { a, b, c };
    } else if (want == L"looking") {
        view_.running = true;
        view_.s.link = ShareLink::Starting;
        view_.s.browsing = true;
        view_.s.wake = true;
    } else {
        /* Default dry-run scene: AirDrop open, Everyone for 10 Minutes,
         * showing nearby Apple devices right on the radar canvas. */
        set_everyone();
        add_sample_devices();
    }
}

ShareManager::~ShareManager()
{
    if (setup_thread_.joinable()) setup_thread_.join();
    end_session();
}

ShareView ShareManager::view() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return view_;
}

void ShareManager::notify()
{
    HWND hwnd = notify_hwnd_.load();
    if (hwnd) PostMessageW(hwnd, kMsgShareUpdated, 0, 0);
}

void ShareManager::refresh()
{
    if (dry_run_) return;
    ShareSetup setup = look();
    {
        std::lock_guard<std::mutex> lock(mu_);
        /* Not under a session: the adapter is lares-share.exe's then. */
        if (view_.setup_busy || proc_ || view_.setup == setup) return;
        view_.setup = setup;
        if (setup == ShareSetup::Ready) view_.setup_error.clear();
    }
    notify();
}

// ------------------------------------------------------------------ set-up

void ShareManager::run_setup()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (view_.setup_busy) return;
        view_.setup_error.clear();
        if (dry_run_) view_.setup = ShareSetup::Ready;
        else view_.setup_busy = true;
    }
    notify();
    if (dry_run_) return;
    if (setup_thread_.joinable()) setup_thread_.join();
    setup_thread_ = std::thread([this] { setup_worker(true); });
}

void ShareManager::give_back()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (view_.setup_busy || proc_) return;
        view_.setup_error.clear();
        if (dry_run_) view_.setup = ShareSetup::NeedsSetup;
        else view_.setup_busy = true;
    }
    notify();
    if (dry_run_) return;
    if (setup_thread_.joinable()) setup_thread_.join();
    setup_thread_ = std::thread([this] { setup_worker(false); });
}

/* lares-share.exe switches the adapter's driver, as administrator: one
 * prompt, and what it did in a file, since its console can't be read. */
void ShareManager::setup_worker(bool winusb)
{
    std::string error;
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring log = (n && n < MAX_PATH - 40 ? std::wstring(tmp) : exe_dir() + L"\\") + L"lares-share-setup.txt";
    DeleteFileW(log.c_str());
    std::wstring exe = share_exe();
    std::wstring params = std::wstring(winusb ? L"--use-winusb" : L"--use-windows-driver") + L" --log \"" + log + L"\"";
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "This copy of Lares came without lares-share.exe.";
    } else if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        error = GetLastError() == ERROR_CANCELLED ? "Nothing changed: Windows' permission wasn't given."
                                                  : "Couldn't ask Windows for permission.";
    } else {
        WaitForSingleObject(sei.hProcess, 2 * 60 * 1000);
        CloseHandle(sei.hProcess);
        std::string said = read_small_file(log);
        DeleteFileW(log.c_str());
        if (said.compare(0, 9, "driver ok") != 0) {
            const char kFailed[] = "driver failed ";
            error = said.compare(0, sizeof(kFailed) - 1, kFailed) == 0 ? said.substr(sizeof(kFailed) - 1)
                    : winusb ? "Couldn't give the adapter the WinUSB driver."
                             : "Couldn't give the adapter back to Windows.";
        }
    }
    /* Windows takes a moment to start the device on its new driver. */
    ShareSetup setup = look();
    for (int i = 0; i < 20 && error.empty() && (setup == ShareSetup::Ready) != winusb; ++i) {
        Sleep(250);
        setup = look();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        view_.setup_busy = false;
        view_.setup = setup;
        view_.setup_error = error;
    }
    notify();
}

// ------------------------------------------------------------- the session

std::string ShareManager::contacts_command_locked() const
{
    std::string line = "contacts";
    for (const std::string& h : share_contact_hashes(view_.contacts)) line += "\t" + h;
    return line + "\n";
}

bool ShareManager::start_session()
{
    /* mu_ is held, and no session is on. */
    if (reader_.joinable()) reader_.join();   // the session before, which has ended
    std::wstring exe = share_exe();
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE in_rd = nullptr, in_wr = nullptr, out_rd = nullptr, out_wr = nullptr;
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) return false;
    if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
        CloseHandle(in_rd);
        CloseHandle(in_wr);
        return false;
    }
    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_rd;
    si.hStdOutput = out_wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_rd);
    CloseHandle(out_wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(in_wr);
        CloseHandle(out_rd);
        return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    to_child_ = in_wr;
    from_child_ = out_rd;
    armed_ = false;
    quitting_ = false;
    view_.running = true;
    view_.error.clear();
    view_.s = ShareSession();
    view_.s.link = ShareLink::Starting;
    view_.s.original_media = original_media_;
    /* Hand lares-share.exe the current saved contact hashes and media format right away. */
    write_line(to_child_, contacts_command_locked());
    write_line(to_child_, original_media_ ? "media-format original\n" : "media-format compatible\n");
    HANDLE from = from_child_, proc = proc_;
    reader_ = std::thread([this, from, proc] { read_loop(from, proc); });
    return true;
}

void ShareManager::read_loop(HANDLE from, HANDLE proc)
{
    std::string buf;
    std::vector<std::string> block;
    bool heard = false;
    std::string last_error;   /* the last block's, kept for when it ends by itself */
    char chunk[2048];
    DWORD n = 0;
    while (ReadFile(from, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
        buf.append(chunk, n);
        size_t nl;
        bool changed = false;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line != "end") {
                if (block.size() < 512) block.push_back(std::move(line));
                continue;
            }
            ShareSession s;
            bool ok = share_parse_block(block, s);
            block.clear();
            if (!ok) continue;
            heard = true;
            last_error = s.error;
            std::lock_guard<std::mutex> lock(mu_);
            bool was_in_rx = view_.s.has_incoming && view_.s.incoming.state == ShareIncomingState::Receiving;
            bool now_in_rx = s.has_incoming && s.incoming.state == ShareIncomingState::Receiving;
            bool was_tx = false, now_tx = false;
            for (const auto& d : view_.s.devices)
                if (d.state == ShareDeviceState::Sending) was_tx = true;
            for (const auto& d : s.devices)
                if (d.state == ShareDeviceState::Sending) now_tx = true;
            if ((!was_in_rx && now_in_rx) || (!was_tx && now_tx)) view_.xfer_start_ms = GetTickCount64();

            view_.s = std::move(s);
            view_.left_at_ms = GetTickCount64();
            if (!view_.s.sending) {
                view_.files.clear();
                view_.file_paths.clear();
                view_.send_urls.clear();
            }
            if (!view_.s.idle()) armed_ = true;
            /* Used and idle again, with nothing waiting: the session ends,
             * and the adapter powers down. */
            if (armed_ && view_.s.idle() && pending_.empty() && !quitting_) {
                write_line(to_child_, "quit\n");
                quitting_ = true;
            }
            changed = true;
        }
        if (changed) notify();
    }

    /* It ended: by being asked to, or by itself. */
    WaitForSingleObject(proc, 30000);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (to_child_) CloseHandle(to_child_);
        CloseHandle(from_child_);
        CloseHandle(proc_);
        to_child_ = from_child_ = proc_ = nullptr;
        bool asked = quitting_;
        quitting_ = false;
        view_.running = false;
        view_.s = ShareSession();
        view_.s.original_media = original_media_;
        if (!heard && !closing_) {
            view_.error = "lares-share.exe stopped as it started.";
            pending_.clear();
        } else if (!asked && !closing_) {
            view_.error = last_error.empty() ? "AirDrop stopped." : last_error;
            pending_.clear();
        }
        if (pending_.empty()) {
            view_.files.clear();
            view_.file_paths.clear();
            view_.send_urls.clear();
        }
        /* The window starts the session that what's waiting asks for: this
         * thread is the one that session would have to join. */
        restart_ = !pending_.empty();
    }
    notify();
}

void ShareManager::resume()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (!restart_ || proc_) return;
    restart_ = false;
    std::vector<std::string> lines;
    lines.swap(pending_);
    if (lines.empty()) return;
    if (!start_session()) {
        view_.error = "Couldn't start AirDrop.";
        view_.files.clear();
        view_.file_paths.clear();
        return;
    }
    for (const std::string& l : lines) write_line(to_child_, l);
}

void ShareManager::command(const std::string& line)
{
    if (dry_run_) {
        dry_command(line);
        notify();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::string out = line + "\n";
        /* Only what turns something on starts a session, or waits for the next. */
        bool starts_one = line == "receive on" || line == "mode contacts" || line == "mode everyone" ||
                          line == "browse on" || line == "look on" ||
                          line.compare(0, 5, "send\t") == 0 || line.compare(0, 8, "send-to\t") == 0 ||
                          line.compare(0, 9, "send-url\t") == 0 || line.compare(0, 12, "send-url-to\t") == 0;
        if (proc_ && !quitting_) {
            write_line(to_child_, out);
        } else if (proc_) {
            if (starts_one) pending_.push_back(out);
            else if (line == "browse off" || line == "mode off" || line == "receive off") pending_.clear();
        } else if (starts_one) {
            if (view_.setup != ShareSetup::Ready) return;
            if (start_session()) {
                write_line(to_child_, out);
            } else {
                view_.error = "Couldn't start AirDrop.";
                view_.files.clear();
                view_.file_paths.clear();
            }
        }
    }
    notify();
}

void ShareManager::end_session()
{
    HANDLE proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        closing_ = true;
        pending_.clear();
        if (proc_ && to_child_) {
            /* Closing its input stops it as "quit" does, and it powers the
             * adapter down on its way out. */
            if (!quitting_) write_line(to_child_, "quit\n");
            quitting_ = true;
            CloseHandle(to_child_);
            to_child_ = nullptr;
            /* A handle of its own: the reader closes the session's. */
            DuplicateHandle(GetCurrentProcess(), proc_, GetCurrentProcess(), &proc, 0, FALSE, DUPLICATE_SAME_ACCESS);
        }
    }
    if (!reader_.joinable()) return;
    if (proc && WaitForSingleObject(proc, 5000) == WAIT_TIMEOUT) TerminateProcess(proc, 1);
    if (proc) CloseHandle(proc);
    reader_.join();
}

void ShareManager::set_discover_mode(ShareDiscoverMode mode)
{
    switch (mode) {
    case ShareDiscoverMode::ContactsOnly: command("mode contacts"); break;
    case ShareDiscoverMode::Everyone: command("mode everyone"); break;
    default: command("mode off"); break;
    }
}

void ShareManager::set_receiving(bool on) { set_discover_mode(on ? ShareDiscoverMode::Everyone : ShareDiscoverMode::Off); }

void ShareManager::set_original_media(bool original)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        original_media_ = original;
        view_.s.original_media = original;
        if (!dry_run_ && proc_ && !quitting_) {
            write_line(to_child_, original ? "media-format original\n" : "media-format compatible\n");
        }
    }
    notify();
}

void ShareManager::set_browsing(bool on) { command(on ? "browse on" : "browse off"); }
void ShareManager::answer(bool accept) { command(accept ? "accept" : "decline"); }
void ShareManager::dismiss_incoming() { command("dismiss"); }
void ShareManager::set_looking(bool on) { command(on ? "look on" : "look off"); }
void ShareManager::stop_sending() { command("send-stop"); }
void ShareManager::send_to(const std::string& key) { command("to\t" + key); }
void ShareManager::dismiss_device(const std::string& key) { command("device-dismiss\t" + key); }
void ShareManager::cancel_send() { command("cancel"); }

void ShareManager::send(const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> names;
    std::string line = share_send_line(paths, &names);
    if (line.empty()) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            view_.error = "Those files' names can't be sent.";
        }
        command("look off");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        view_.files = names;
        view_.file_paths = paths;
        view_.send_urls.clear();
        view_.error.clear();
    }
    command(line);
}

void ShareManager::send_files_to(const std::string& key, const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> names;
    std::string line = share_send_to_line(key, paths, &names);
    if (line.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        view_.error = "Those files' names can't be sent.";
        notify();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        view_.files = names;
        view_.file_paths = paths;
        view_.send_urls.clear();
        view_.xfer_start_ms = GetTickCount64();
        view_.error.clear();
    }
    command(line);
}

void ShareManager::send_urls(const std::vector<std::string>& urls)
{
    std::vector<std::string> ok_urls;
    std::string line = share_send_url_line(urls, &ok_urls);
    if (line.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        view_.error = "Enter a valid http:// or https:// link.";
        notify();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        view_.files.clear();
        view_.file_paths.clear();
        view_.send_urls = std::move(ok_urls);
        view_.error.clear();
    }
    command(line);
}

void ShareManager::send_urls_to(const std::string& key, const std::vector<std::string>& urls)
{
    std::vector<std::string> ok_urls;
    std::string line = share_send_url_to_line(key, urls, &ok_urls);
    if (line.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        view_.error = "Enter a valid http:// or https:// link.";
        notify();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        view_.files.clear();
        view_.file_paths.clear();
        view_.send_urls = std::move(ok_urls);
        view_.xfer_start_ms = GetTickCount64();
        view_.error.clear();
    }
    command(line);
}

void ShareManager::save_contacts(const std::vector<ShareContact>& contacts)
{
    save_share_contacts(contacts_path_, contacts);
    std::string cmd;
    {
        std::lock_guard<std::mutex> lock(mu_);
        view_.contacts = contacts;
        if (proc_ && !quitting_) cmd = contacts_command_locked();
    }
    if (!cmd.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        if (proc_ && !quitting_) write_line(to_child_, cmd);
    }
    notify();
}

void ShareManager::set_device_photo(const std::string& device_name, const std::wstring& photo_path)
{
    std::vector<ShareContact> list;
    {
        std::lock_guard<std::mutex> lock(mu_);
        list = view_.contacts;
    }
    std::string photo_u8 = narrow(photo_path);
    std::string lo = normalize_contact_name(device_name);
    std::string owner = owner_prefix_norm(lo);
    int match = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        if ((!list[i].device.empty() && normalize_contact_name(list[i].device) == lo) ||
            (!list[i].name.empty() && normalize_contact_name(list[i].name) == lo)) {
            match = static_cast<int>(i);
            break;
        }
    }
    if (match < 0 && !owner.empty()) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (!list[i].name.empty() && normalize_contact_name(list[i].name) == owner) {
                match = static_cast<int>(i);
                break;
            }
        }
    }
    if (match >= 0) {
        list[static_cast<size_t>(match)].photo = photo_u8;
        if (list[static_cast<size_t>(match)].device.empty()) list[static_cast<size_t>(match)].device = device_name;
    } else if (!photo_u8.empty()) {
        ShareContact c;
        c.name = device_name;
        c.device = device_name;
        c.photo = photo_u8;
        list.push_back(std::move(c));
    }
    save_contacts(list);
}

/* A dry run pretends: enough for the page to answer each control. */
void ShareManager::dry_command(const std::string& line)
{
    std::lock_guard<std::mutex> lock(mu_);
    ShareSession& s = view_.s;
    if (line == "mode everyone" || line == "receive on") {
        if (s.mode != ShareDiscoverMode::Everyone) dry_prev_mode_ = s.mode;
        s.mode = ShareDiscoverMode::Everyone;
        s.receiving = true;
        s.left = 600;
        s.link = ShareLink::Up;
        view_.left_at_ms = GetTickCount64();
    } else if (line == "mode contacts") {
        s.mode = ShareDiscoverMode::ContactsOnly;
        s.receiving = true;
        s.left = 0;
        s.link = ShareLink::Up;
    } else if (line == "mode off" || line == "receive off") {
        s.mode = ShareDiscoverMode::Off;
        s.receiving = false;
        s.left = 0;
        s.has_incoming = false;
        if (!s.sending && !s.browsing) s.link = ShareLink::Off;
    } else if (line == "browse on") {
        if (view_.setup == ShareSetup::Ready) {
            s.browsing = true;
            s.wake = true;
            if (s.link == ShareLink::Off) s.link = ShareLink::Up;
        }
    } else if (line == "browse off") {
        s.browsing = false;
        if (!s.sending) s.wake = false;
    } else if (line == "accept" && s.has_incoming) {
        s.incoming.state = ShareIncomingState::Receiving;
        s.incoming.done = s.incoming.total * 3 / 4;
        view_.xfer_start_ms = GetTickCount64() - 8000;
    } else if (line == "decline" || line == "dismiss") {
        s.has_incoming = false;
        s.incoming = ShareIncoming();
    } else if (line.compare(0, 5, "send\t") == 0) {
        s.sending = true;
        s.wake = true;
        s.link = ShareLink::Up;
        if (s.devices.empty()) {
            ShareDevice d;
            d.key = "a";
            d.name = "Ana's iPhone";
            d.model = "iPhone16,2";
            s.devices = { d };
        }
    } else if (line.compare(0, 8, "send-to\t") == 0) {
        size_t tab = line.find('\t', 8);
        std::string key = line.substr(8, tab == std::string::npos ? std::string::npos : tab - 8);
        s.sending = true;
        s.wake = true;
        s.link = ShareLink::Up;
        view_.xfer_start_ms = GetTickCount64() - 4000;
        for (ShareDevice& d : s.devices) {
            if (d.key == key) {
                d.state = ShareDeviceState::Sending;
                d.sent = 4200000;
                d.total = 9600000;
                d.message.clear();
            }
        }
    } else if (line.compare(0, 9, "send-url\t") == 0) {
        s.sending = true;
        s.wake = true;
        s.link = ShareLink::Up;
        if (s.devices.empty()) {
            ShareDevice d;
            d.key = "a";
            d.name = "Ana's iPhone";
            d.model = "iPhone16,2";
            s.devices = { d };
        }
    } else if (line.compare(0, 12, "send-url-to\t") == 0) {
        size_t tab = line.find('\t', 12);
        std::string key = line.substr(12, tab == std::string::npos ? std::string::npos : tab - 12);
        s.sending = true;
        s.wake = true;
        s.link = ShareLink::Up;
        for (ShareDevice& d : s.devices) {
            if (d.key == key) {
                d.state = ShareDeviceState::Sent;
                d.sent = 1;
                d.total = 1;
                d.message.clear();
            }
        }
    } else if (line == "send-stop" || line == "look off") {
        s.sending = false;
        view_.files.clear();
        view_.file_paths.clear();
        view_.send_urls.clear();
        if (!s.browsing) {
            s.wake = false;
            s.devices.clear();
        }
        if (!s.receiving && !s.browsing) s.link = ShareLink::Off;
    } else if (line.compare(0, 3, "to\t") == 0) {
        for (ShareDevice& d : s.devices)
            if (d.key == line.substr(3)) {
                d.state = view_.send_urls.empty() ? ShareDeviceState::Sending : ShareDeviceState::Sent;
                d.sent = view_.send_urls.empty() ? 4200000 : 1;
                d.total = view_.send_urls.empty() ? 9600000 : 1;
            }
    } else if (line.compare(0, 15, "device-dismiss\t") == 0) {
        std::string key = line.substr(15);
        for (ShareDevice& d : s.devices)
            if (d.key == key) {
                d.state = ShareDeviceState::Found;
                d.sent = d.total = 0;
                d.message.clear();
            }
        s.sending = false;
        view_.files.clear();
        view_.file_paths.clear();
        view_.send_urls.clear();
    } else if (line == "cancel") {
        for (ShareDevice& d : s.devices)
            if (d.state == ShareDeviceState::Asking || d.state == ShareDeviceState::Sending) d.state = ShareDeviceState::Found;
    }
    view_.running = s.receiving || s.sending || s.browsing;
}

#endif // _WIN32

} // namespace lares