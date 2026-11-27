// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* lares-share.exe: AirDrop on Windows (issue #109), src/share.c's model
 * without a window. lares.exe starts it while sharing is on and talks to it
 * in lines (win/src/share_win.cpp): one command a line on stdin, and the
 * whole state, a block of lines ending in "end", on stdout whenever it
 * changes:
 *
 *   state setup=ready link=up receiving=1 left=540 sending=0 wake=0 quiet=0
 *   name <how others see this PC>
 *   card <what the card is>
 *   error <why the link failed>
 *   incoming id=3 state=asking done=0 total=0 links=0
 *   incoming-sender <name>
 *   incoming-name <a file's name>            (one a file)
 *   incoming-saved <path>                    (one a file, once done)
 *   incoming-message <why it failed>
 *   device state=found sent=0 total=0 <TAB> key <TAB> name <TAB> model <TAB> message
 *   end
 *
 * Commands: receive on|off, accept, decline, dismiss, look on|off,
 * send <TAB> path <TAB> path..., send-stop, to <TAB> key, cancel, refresh,
 * quit. Paths are Windows paths in UTF-8. It ends when stdin does, which
 * stops the link and powers the adapter down.
 *
 * Other modes, for Set up and for checks: --use-winusb, --use-windows-driver
 * (administrator), and, reading only or sending what AWDL sends, --driver-probe,
 * --peek, --listen [s], --link [s] and --probe-test [s]. */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "driver_setup.h"
#include "loop.h"
#include "model.h"

extern "C" {
#include "link.h"
#include "net.h"
#include "rtl_usb.h"
}

namespace {

using lares::DeviceState;
using lares::DiscoverMode;
using lares::IncomingState;
using lares::LinkState;
using lares::Setup;

std::wstring wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/* One line: no tabs or newlines from a name someone else chose. */
std::string one_line(const std::string& text)
{
    std::string t = text;
    for (char& c : t)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    return t;
}

void put(std::string& out, const char* key, const std::string& text)
{
    if (text.empty()) return;
    out += key;
    out += ' ';
    out += one_line(text);
    out += '\n';
}

const char* setup_name(Setup s)
{
    return s == Setup::Ready ? "ready" : s == Setup::NeedsSetup ? "needs-allow" : "no-card";
}

const char* link_name(LinkState l)
{
    switch (l) {
    case LinkState::Starting: return "starting";
    case LinkState::Up: return "up";
    case LinkState::Stopping: return "stopping";
    case LinkState::Failed: return "failed";
    default: return "off";
    }
}

const char* mode_name(DiscoverMode m)
{
    switch (m) {
    case DiscoverMode::ContactsOnly: return "contacts";
    case DiscoverMode::Everyone: return "everyone";
    default: return "off";
    }
}

const char* device_name(DeviceState d)
{
    switch (d) {
    case DeviceState::Asking: return "asking";
    case DeviceState::Sending: return "sending";
    case DeviceState::Sent: return "sent";
    case DeviceState::Declined: return "declined";
    case DeviceState::Failed: return "failed";
    default: return "found";
    }
}

const char* incoming_name(IncomingState i)
{
    switch (i) {
    case IncomingState::Receiving: return "receiving";
    case IncomingState::Done: return "done";
    case IncomingState::Failed: return "failed";
    default: return "asking";
    }
}

struct Session {
    lares::Loop loop;
    lares::Share share{ loop };
    std::string last;   /* the block written last, so the same one isn't written twice */

    void write_state()
    {
        const lares::ShareState& st = share.state();
        long long left = 0;
        if (st.mode == DiscoverMode::Everyone) {
            auto d = std::chrono::duration_cast<std::chrono::seconds>(st.receive_until - lares::Loop::Clock::now());
            left = d.count() > 0 ? d.count() : 0;
        }
        char head[360];
        std::snprintf(head, sizeof head,
                      "state setup=%s link=%s mode=%s media=%s receiving=%d left=%lld browsing=%d sending=%d wake=%d quiet=%d\n",
                      setup_name(st.setup), link_name(st.link), mode_name(st.mode),
                      st.original_media ? "original" : "compatible", st.receiving ? 1 : 0, left,
                      st.browsing ? 1 : 0, st.sending ? 1 : 0,
                      st.browsing || st.sending || share.looking() ? 1 : 0, st.quiet ? 1 : 0);
        std::string out = head;
        put(out, "name", st.name);
        put(out, "card", st.card_text);
        put(out, "error", st.link_error);
        if (st.incoming) {
            const lares::Incoming& in = *st.incoming;
            char line[160];
            std::snprintf(line, sizeof line, "incoming id=%u state=%s done=%llu total=%llu links=%d\n", in.id,
                          incoming_name(in.state), static_cast<unsigned long long>(in.done),
                          static_cast<unsigned long long>(in.total), in.links ? 1 : 0);
            out += line;
            put(out, "incoming-sender", in.sender);
            put(out, "incoming-model", in.sender_model);
            put(out, "incoming-thumb", in.thumb_path);
            put(out, "incoming-contact", in.matched_hash);
            for (const std::string& n : in.names) put(out, "incoming-name", n);
            for (const std::string& u : in.offered_links) put(out, "incoming-link", u);
            for (const std::string& n : in.saved) put(out, "incoming-saved", n);
            put(out, "incoming-message", in.message);
        }
        for (const lares::Device& d : st.devices) {
            char line[128];
            std::snprintf(line, sizeof line, "device state=%s sent=%llu total=%llu", device_name(d.state),
                          static_cast<unsigned long long>(d.sent), static_cast<unsigned long long>(d.total));
            out += line;
            out += "\t" + one_line(d.key) + "\t" + one_line(d.name) + "\t" + one_line(d.model) + "\t" +
                   one_line(d.message) + "\n";
        }
        out += "end\n";
        if (out == last) return;
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
        last = std::move(out);
    }

    void command(const std::string& line)
    {
        std::vector<std::string> a;
        size_t at = 0;
        for (;;) {
            size_t tab = line.find('\t', at);
            a.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        const std::string& c = a[0];
        if (c == "mode off") share.set_mode(DiscoverMode::Off);
        else if (c == "mode contacts") share.set_mode(DiscoverMode::ContactsOnly);
        else if (c == "mode everyone") share.set_mode(DiscoverMode::Everyone);
        else if (c == "media-format original") share.set_original_media(true);
        else if (c == "media-format compatible") share.set_original_media(false);
        else if (c == "receive on") share.set_receiving(true);
        else if (c == "receive off") share.set_receiving(false);
        else if (c == "browse on" || c == "browse off") share.set_browsing(c == "browse on");
        else if (c == "contacts") {
            std::set<std::string> hashes;
            for (size_t i = 1; i < a.size(); i++)
                if (a[i].size() == 64) hashes.insert(a[i]);
            share.set_contact_hashes(std::move(hashes));
        } else if (c == "accept") share.answer(true);
        else if (c == "decline") share.answer(false);
        else if (c == "dismiss") share.dismiss_incoming();
        else if (c == "look on" || c == "look off") share.set_looking(c == "look on");
        else if (c == "send" && a.size() > 1) {
            std::vector<std::wstring> paths;
            for (size_t i = 1; i < a.size(); i++)
                if (!a[i].empty()) paths.push_back(wide(a[i]));
            share.set_sending(paths);
        } else if (c == "send-to" && a.size() > 2) {
            std::vector<std::wstring> paths;
            for (size_t i = 2; i < a.size(); i++)
                if (!a[i].empty()) paths.push_back(wide(a[i]));
            share.send_files_to(a[1], paths);
        } else if (c == "send-url" && a.size() > 1) {
            std::vector<std::string> urls;
            for (size_t i = 1; i < a.size(); i++)
                if (!a[i].empty()) urls.push_back(a[i]);
            share.set_sending_urls(urls);
        } else if (c == "send-url-to" && a.size() > 2) {
            std::vector<std::string> urls;
            for (size_t i = 2; i < a.size(); i++)
                if (!a[i].empty()) urls.push_back(a[i]);
            share.send_urls_to(a[1], urls);
        } else if (c == "send-stop") share.set_sending({});
        else if (c == "to" && a.size() > 1) share.send_to(a[1]);
        else if (c == "device-dismiss" && a.size() > 1) share.dismiss_device(a[1]);
        else if (c == "cancel") share.cancel_send();
        else if (c == "refresh") share.refresh();
        else if (c == "quit") loop.quit();
        write_state();
    }

    void tick()
    {
        write_state();
        loop.after(5000, [this] { tick(); });
    }

    int run()
    {
        _setmode(_fileno(stdout), _O_BINARY);
        share.watch([this] { write_state(); });
        share.refresh();
        write_state();
        loop.after(5000, [this] { tick(); });
        /* stdin's lines arrive on a thread of their own; its end means
         * lares.exe is gone. */
        std::thread reader([this] {
            HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            std::string buf;
            char chunk[1024];
            DWORD n = 0;
            while (ReadFile(in, chunk, sizeof chunk, &n, nullptr) && n > 0) {
                buf.append(chunk, n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);   /* a writer's UTF-8 mark */
                    loop.post([this, line] { command(line); });
                }
            }
            loop.quit();
        });
        reader.detach();
        loop.run();

        /* Stop what uses the link and wait for it to power the adapter down. */
        share.set_browsing(false);
        share.set_sending({});
        share.set_looking(false);
        share.set_mode(DiscoverMode::Off);
        share.stop_link();
        for (int i = 0; i < 250 && share.state().link != LinkState::Off && share.state().link != LinkState::Failed; i++) {
            loop.run_pending();
            Sleep(100);
        }
        return 0;
    }
};

/* Elevated runs have no console Lares can read: they say how it went in a
 * file as well. */
void report(bool log, const std::string& line)
{
    std::printf("%s\n", line.c_str());
    if (!log) return;
    /* The path as Windows gave it, whatever the user's name is written in. */
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 3) {
        if (FILE* f = _wfopen(argv[3], L"w")) {
            std::fprintf(f, "%s\n", line.c_str());
            std::fclose(f);
        }
    }
    LocalFree(argv);
}

int peek()
{
    char err[256];
    rtl_usb* u = rtl_usb_open(err, sizeof err);
    if (!u) {
        std::printf("open failed: %s\n", err);
        return 1;
    }
    std::printf("in ep %02x, %u out:", rtl_usb_in_ep(u), rtl_usb_out_count(u));
    for (unsigned i = 0; i < rtl_usb_out_count(u); i++) std::printf(" %02x", rtl_usb_out_ep(u, i));
    std::printf(", packet %u\n", rtl_usb_packet_size(u));
    const uint16_t regs[] = { 0x0000, 0x0004, 0x001c, 0x001f, 0x0020, 0x0024, 0x0040, 0x0044, 0x0048, 0x004c, 0x0060, 0x0064, 0x0068, 0x006c, 0x00f0, 0x0100, 0x042a, 0x0610 };
    for (uint16_t r : regs) {
        uint32_t v = 0;
        int ok = rtl_usb_read(u, r, &v, 4);
        std::printf("reg %04x: %s %08x\n", r, ok == 0 ? "" : "(failed)", v);
    }
    rtl_usb_close(u);
    return 0;
}

std::vector<uint8_t> load_firmware()
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    HGLOBAL h = res ? LoadResource(nullptr, res) : nullptr;
    const auto* p = h ? static_cast<const uint8_t*>(LockResource(h)) : nullptr;
    DWORD n = res ? SizeofResource(nullptr, res) : 0;
    return p && n ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>();
}

/* --listen and --link: the link alone, with what it hears each second. */
struct Probe {
    std::atomic<bool> ready{ false }, failed{ false };
    std::string why;
    std::atomic<unsigned> peers{ 0 }, delivered{ 0 };
};

int run_link(bool listen_only, unsigned seconds)
{
    std::vector<uint8_t> fw = load_firmware();
    Probe probe;
    link_config cfg{};
    cfg.channel = 6;
    link_session_address(&cfg.addr);
    std::snprintf(cfg.name, sizeof cfg.name, "lares-probe");
    cfg.firmware = fw.data();
    cfg.firmware_len = fw.size();
    cfg.listen_only = listen_only;
    link_callbacks cb{};
    cb.ctx = &probe;
    cb.ready = [](void* ctx, uint8_t, const awdl_mac*) { static_cast<Probe*>(ctx)->ready = true; };
    cb.failed = [](void* ctx, const char* m) {
        auto* p = static_cast<Probe*>(ctx);
        p->why = m ? m : "";
        p->failed = true;
    };
    cb.peer = [](void* ctx, const awdl_mac*, const uint8_t*, size_t, bool up) {
        auto* p = static_cast<Probe*>(ctx);
        if (up) p->peers++;
        else if (p->peers) p->peers--;
        std::printf("peer %s\n", up ? "valid" : "gone");
    };
    cb.deliver = [](void* ctx, const awdl_mac*, const awdl_mac*, const uint8_t*, size_t) {
        static_cast<Probe*>(ctx)->delivered++;
    };
    struct link* l = link_start(&cfg, &cb);
    if (!l) {
        std::printf("link didn't start\n");
        return 1;
    }
    for (unsigned t = 0; t < seconds && !probe.failed; t++) {
        Sleep(1000);
        struct link_counts c;
        link_counts(l, &c);
        std::printf("%3u s %s xfers %llu frames %llu (ofdm %llu) awdl action %llu data %llu bad %llu rssi %d tsf %s sent %llu failed %llu peers %u ip %u\n",
                    t + 1, probe.ready ? "up" : "starting", static_cast<unsigned long long>(c.rx_xfers),
                    static_cast<unsigned long long>(c.frames), static_cast<unsigned long long>(c.ofdm),
                    static_cast<unsigned long long>(c.awdl_action), static_cast<unsigned long long>(c.awdl_data),
                    static_cast<unsigned long long>(c.bad), c.last_rssi, c.tsf_running ? "on" : "off",
                    static_cast<unsigned long long>(c.tx_sent), static_cast<unsigned long long>(c.tx_failed),
                    probe.peers.load(), probe.delivered.load());
        std::fflush(stdout);
    }
    if (probe.failed) std::printf("failed: %s\n", probe.why.c_str());
    link_dump(l, stdout);
    link_stop(l);
    std::printf("stopped\n");
    return probe.failed ? 1 : 0;
}

/* --probe-test: broadcast probe requests from our address, as #107 checked
 * sending with: access points that answer prove the frames went out right
 * and that frames to our address come in; answers without the retry bit
 * prove the chip ACKed them. Only counts: no network names or addresses. */
int probe_test(unsigned seconds)
{
    std::vector<uint8_t> fw = load_firmware();
    link_config cfg{};
    cfg.channel = 6;
    link_session_address(&cfg.addr);
    std::snprintf(cfg.name, sizeof cfg.name, "lares-probe");
    cfg.firmware = fw.data();
    cfg.firmware_len = fw.size();
    cfg.listen_only = true;
    std::atomic<bool> ready{ false };
    link_callbacks cb{};
    cb.ctx = &ready;
    cb.ready = [](void* ctx, uint8_t, const awdl_mac*) { *static_cast<std::atomic<bool>*>(ctx) = true; };
    struct link* l = link_start(&cfg, &cb);
    for (int i = 0; l && i < 100 && !ready; i++) Sleep(100);
    if (!l || !ready) {
        std::printf("link didn't come up\n");
        if (l) link_stop(l);
        return 1;
    }
    /* A wildcard probe request: no SSID, 802.11g's rates. */
    static const uint8_t body[] = { 0x00, 0x00, 0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
                                    0x32, 0x04, 0x30, 0x48, 0x60, 0x6c };
    uint16_t seq = 0;
    for (unsigned t = 0; t < seconds * 5; t++) {
        uint8_t f[24 + sizeof body] = { 0x40, 0x00, 0x00, 0x00 };
        std::memset(f + 4, 0xff, 6);
        std::memcpy(f + 10, cfg.addr.a, 6);
        std::memset(f + 16, 0xff, 6);
        f[22] = static_cast<uint8_t>(seq << 4);
        f[23] = static_cast<uint8_t>(seq >> 4);
        seq = static_cast<uint16_t>((seq + 1) & 0x0fff);
        std::memcpy(f + 24, body, sizeof body);
        link_raw(l, f, sizeof f);
        Sleep(200);
    }
    Sleep(500);
    struct link_counts c;
    link_counts(l, &c);
    std::printf("probe requests sent %llu, failed %llu; probe responses to us %llu, %llu of them retries; frames heard %llu\n",
                static_cast<unsigned long long>(c.raw_sent), static_cast<unsigned long long>(c.tx_failed),
                static_cast<unsigned long long>(c.probe_resp), static_cast<unsigned long long>(c.probe_resp_retry),
                static_cast<unsigned long long>(c.frames));
    link_stop(l);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    bool log = argc > 3 && std::strcmp(argv[2], "--log") == 0;
    if (argc > 1 && std::strcmp(argv[1], "--driver-probe") == 0) {
        std::fputs(lares::adapter_driver_probe().c_str(), stdout);
        return 0;
    }
    if (argc > 1 && (std::strcmp(argv[1], "--use-winusb") == 0 || std::strcmp(argv[1], "--use-windows-driver") == 0)) {
        std::string err;
        bool ok = std::strcmp(argv[1], "--use-winusb") == 0 ? lares::adapter_use_winusb(err)
                                                             : lares::adapter_use_windows_driver(err);
        report(log, ok ? "driver ok" : "driver failed " + err);
        return ok ? 0 : 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--peek") == 0) return peek();
    if (argc > 1 && std::strcmp(argv[1], "--probe-test") == 0)
        return probe_test(argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 10);
    if (argc > 1 && (std::strcmp(argv[1], "--listen") == 0 || std::strcmp(argv[1], "--link") == 0)) {
        unsigned seconds = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 10;
        return run_link(std::strcmp(argv[1], "--listen") == 0, seconds ? seconds : 10);
    }
    /* Out at once once the link is off: a worker still waiting on a peer
     * has nothing left to tell. */
    static Session s;
    int code = s.run();
    std::fflush(stdout);
    ExitProcess(static_cast<UINT>(code));
}
