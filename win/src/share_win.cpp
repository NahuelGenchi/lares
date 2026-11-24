// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "share_win.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
            s.left = static_cast<int>(std::min<uint64_t>(number(field(line, "left")), 24 * 3600));
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
        } else if (starts(line, "incoming-name ")) {
            s.incoming.names.push_back(rest(line));
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
std::string share_offer_text(const ShareIncoming& in)
{
    size_t n = in.names.size();
    if (in.links) return n == 1 ? "a link" : "links";
    std::string first = n ? in.names[0] : "files";
    if (n <= 1) return first;
    return first + " and " + std::to_string(n - 1) + " more";
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

bool share_adapter_present()
{
    return adapter_driver() != AdapterDriver::Absent;
}

// ------------------------------------------------------------- the manager

ShareManager::ShareManager(bool dry_run) : dry_run_(dry_run)
{
    if (!dry_run_) return;
    /* A dry run shows a scene and pretends every change: LARES_DRY_SHARE
     * picks the scene. */
    wchar_t scene[32] = {};
    GetEnvironmentVariableW(L"LARES_DRY_SHARE", scene, 32);
    std::wstring want = scene;
    view_.setup = ShareSetup::Ready;
    view_.s.ready = true;
    view_.s.name = "this-pc";
    auto receiving = [&] {
        view_.running = true;
        view_.s.link = ShareLink::Up;
        view_.s.receiving = true;
        view_.s.left = 540;
        view_.left_at_ms = GetTickCount64();
    };
    auto incoming = [&](ShareIncomingState state) {
        receiving();
        view_.s.has_incoming = true;
        view_.s.incoming.id = 1;
        view_.s.incoming.state = state;
        view_.s.incoming.sender = "Ana's iPhone";
        view_.s.incoming.names = { "IMG_0412.HEIC", "IMG_0413.HEIC" };
        view_.s.incoming.total = 5200000;
        view_.s.incoming.done = state == ShareIncomingState::Receiving ? 2100000 : 0;
        if (state == ShareIncomingState::Done) view_.s.incoming.saved = { "C:\\Users\\me\\Downloads\\IMG_0412.HEIC" };
    };
    if (want == L"noadapter") {
        view_.setup = ShareSetup::NoAdapter;
    } else if (want == L"setup") {
        view_.setup = ShareSetup::NeedsSetup;
    } else if (want == L"setup-failed") {
        view_.setup = ShareSetup::NeedsSetup;
        view_.setup_error = "Nothing changed: Windows' permission wasn't given.";
    } else if (want == L"receiving") {
        receiving();
    } else if (want == L"quiet") {
        receiving();
        view_.s.quiet = true;
    } else if (want == L"incoming") {
        incoming(ShareIncomingState::Asking);
    } else if (want == L"incoming-receiving") {
        incoming(ShareIncomingState::Receiving);
    } else if (want == L"incoming-done") {
        incoming(ShareIncomingState::Done);
    } else if (want == L"sending") {
        view_.running = true;
        view_.s.link = ShareLink::Up;
        view_.s.sending = true;
        view_.s.wake = true;
        view_.files = { L"Holiday.mov", L"Notes.pdf" };
        ShareDevice a, b, c;
        a.key = "a";
        a.name = "Ana's iPhone";
        a.state = ShareDeviceState::Sending;
        a.sent = 41000000;
        a.total = 96000000;
        b.key = "b";
        b.name = "Living room iPad";
        c.key = "c";
        c.name = "Bruno's MacBook Air";
        c.state = ShareDeviceState::Declined;
        view_.s.devices = { a, b, c };
    } else if (want == L"looking") {
        view_.running = true;
        view_.s.link = ShareLink::Starting;
        view_.s.sending = true;
        view_.s.wake = true;
        view_.files = { L"Notes.pdf" };
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
    HANDLE from = from_child_, proc = proc_;
    reader_ = std::thread([this, from, proc] { read_loop(from, proc); });
    return true;
}

void ShareManager::read_loop(HANDLE from, HANDLE proc)
{
    std::string buf;
    std::vector<std::string> block;
    bool heard = false;
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
            std::lock_guard<std::mutex> lock(mu_);
            view_.s = std::move(s);
            view_.left_at_ms = GetTickCount64();
            if (!view_.s.sending && !view_.s.wake) view_.files.clear();
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
        if (!heard && !closing_) {
            view_.error = "lares-share.exe stopped as it started.";
            pending_.clear();
        } else if (!asked && !closing_) {
            view_.error = "AirDrop stopped.";
            pending_.clear();
        }
        if (pending_.empty()) view_.files.clear();
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
        bool starts_one = line == "receive on" || line == "look on" || line.compare(0, 5, "send\t") == 0;
        if (proc_ && !quitting_) {
            write_line(to_child_, out);
        } else if (proc_) {
            if (starts_one) pending_.push_back(out);
            else pending_.clear();
        } else if (starts_one) {
            if (start_session()) {
                write_line(to_child_, out);
            } else {
                view_.error = "Couldn't start AirDrop.";
                view_.files.clear();
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

void ShareManager::set_receiving(bool on) { command(on ? "receive on" : "receive off"); }
void ShareManager::answer(bool accept) { command(accept ? "accept" : "decline"); }
void ShareManager::dismiss_incoming() { command("dismiss"); }
void ShareManager::set_looking(bool on) { command(on ? "look on" : "look off"); }
void ShareManager::stop_sending() { command("send-stop"); }
void ShareManager::send_to(const std::string& key) { command("to\t" + key); }
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
        view_.error.clear();
    }
    command(line);
}

/* A dry run pretends: enough for the page to answer each control. */
void ShareManager::dry_command(const std::string& line)
{
    std::lock_guard<std::mutex> lock(mu_);
    ShareSession& s = view_.s;
    if (line == "receive on") {
        s.receiving = true;
        s.left = 600;
        s.link = ShareLink::Up;
        view_.left_at_ms = GetTickCount64();
    } else if (line == "receive off") {
        s.receiving = false;
        s.left = 0;
        s.has_incoming = false;
        if (!s.sending) s.link = ShareLink::Off;
    } else if (line == "accept" && s.has_incoming) {
        s.incoming.state = ShareIncomingState::Receiving;
        s.incoming.done = s.incoming.total * 2 / 5;
    } else if (line == "decline" || line == "dismiss") {
        s.has_incoming = false;
        s.incoming = ShareIncoming();
    } else if (line.compare(0, 5, "send\t") == 0) {
        s.sending = true;
        s.wake = true;
        s.link = ShareLink::Up;
        ShareDevice d;
        d.key = "a";
        d.name = "Ana's iPhone";
        s.devices = { d };
    } else if (line == "send-stop" || line == "look off") {
        s.sending = false;
        s.wake = false;
        s.devices.clear();
        view_.files.clear();
        if (!s.receiving) s.link = ShareLink::Off;
    } else if (line.compare(0, 3, "to\t") == 0) {
        for (ShareDevice& d : s.devices)
            if (d.key == line.substr(3)) d.state = ShareDeviceState::Asking;
    } else if (line == "cancel") {
        for (ShareDevice& d : s.devices)
            if (d.state == ShareDeviceState::Asking || d.state == ShareDeviceState::Sending) d.state = ShareDeviceState::Found;
    }
    view_.running = s.receiving || s.sending;
}

#endif // _WIN32

} // namespace lares