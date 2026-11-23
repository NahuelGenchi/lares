// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "share_win.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include "config.h"

#include <cwctype>

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

std::wstring widen(const std::string& s)
{
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        uint32_t c = 0xFFFD;
        size_t n = 1;
        if (b < 0x80) {
            c = b;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
            c = ((b & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
            c = ((b & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < s.size()) {
            c = ((b & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            n = 4;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (c >> 10));
            out += static_cast<wchar_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<wchar_t>(c);
        }
        i += n;
    }
    return out;
}

bool drive_letter(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

} // namespace

std::string wsl_path_from_windows(const std::wstring& path)
{
    if (path.size() < 3 || !drive_letter(path[0]) || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) return {};
    std::wstring tail = path.substr(2);
    std::replace(tail.begin(), tail.end(), L'\\', L'/');
    std::string out = "/mnt/";
    out += static_cast<char>(path[0] | 0x20);
    return out + narrow(tail);
}

std::wstring windows_path_from_wsl(const std::string& path)
{
    if (path.size() < 7 || path.compare(0, 5, "/mnt/") != 0 || path[6] != '/') return {};
    if (!drive_letter(static_cast<wchar_t>(path[5]))) return {};
    std::wstring out;
    out += static_cast<wchar_t>(path[5] & ~0x20);
    out += L':';
    std::wstring tail = widen(path.substr(6));
    std::replace(tail.begin(), tail.end(), L'/', L'\\');
    return out + tail;
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

constexpr wchar_t kRunScript[] = L"/opt/lares-wsl/bin/run.sh";

std::wstring reg_string(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

bool file_there(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring wsl_exe()
{
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring path = std::wstring(dir) + L"\\wsl.exe";
    return file_there(path) ? path : std::wstring();
}

/* The distribution AirDrop runs in: the default one, unless that is a
 * container tool's own. */
std::wstring wsl_distro()
{
    wchar_t env[128];
    DWORD got = GetEnvironmentVariableW(L"LARES_WSL_DISTRO", env, 128);
    const wchar_t* lxss = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss";
    auto usable = [](const std::wstring& name) {
        /* wsl.exe takes the name as it's written, quotes and all, so it
         * goes on the command line bare: letters, digits, ".", "-" and "_". */
        for (wchar_t c : name)
            if (!(c < 128 && (iswalnum(c) || c == L'.' || c == L'-' || c == L'_'))) return false;
        return !name.empty() && name.compare(0, 14, L"docker-desktop") != 0 &&
               name.compare(0, 15, L"rancher-desktop") != 0 && name.compare(0, 14, L"podman-machine") != 0;
    };
    if (got > 0 && got < 128) return usable(env) ? std::wstring(env) : std::wstring();
    std::wstring guid = reg_string(HKEY_CURRENT_USER, lxss, L"DefaultDistribution");
    if (!guid.empty()) {
        std::wstring name = reg_string(HKEY_CURRENT_USER, (std::wstring(lxss) + L"\\" + guid).c_str(), L"DistributionName");
        if (usable(name)) return name;
    }
    HKEY key = nullptr;
    std::wstring found;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, lxss, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    for (DWORD i = 0; found.empty(); ++i) {
        wchar_t sub[128];
        DWORD len = 128;
        if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring name = reg_string(key, sub, L"DistributionName");
        if (usable(name)) found = name;
    }
    RegCloseKey(key);
    return found;
}

std::wstring usbipd_exe()
{
    std::wstring dir = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\usbipd-win", L"APPLICATIONFOLDER");
    if (dir.empty()) return {};
    if (dir.back() != L'\\') dir += L'\\';
    std::wstring path = dir + L"usbipd.exe";
    return file_there(path) ? path : std::wstring();
}

/* Whether usbipd-win shares the adapter, and whether WSL has it right now:
 * it keeps what it shares here, with an "Attached" key while it's lent. */
bool adapter_shared(bool* lent = nullptr)
{
    if (lent) *lent = false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\usbipd-win\\Devices", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    bool shared = false;
    for (DWORD i = 0; !shared; ++i) {
        wchar_t sub[128];
        DWORD len = 128;
        if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring id = reg_string(key, sub, L"InstanceId");
        shared = _wcsnicmp(id.c_str(), kShareAdapterId, wcslen(kShareAdapterId)) == 0;
        if (shared && lent) {
            HKEY attached = nullptr;
            std::wstring path = std::wstring(sub) + L"\\Attached";
            if (RegOpenKeyExW(key, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &attached) == ERROR_SUCCESS) {
                *lent = true;
                RegCloseKey(attached);
            }
        }
    }
    RegCloseKey(key);
    return shared;
}

std::wstring exe_dir()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
}

std::string read_small_file(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    char buf[256];
    DWORD n = 0;
    std::string out;
    if (ReadFile(f, buf, sizeof(buf), &n, nullptr)) out.assign(buf, n);
    CloseHandle(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

void write_small_file(const std::wstring& path, const std::string& text)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(f);
}

/* The programs for WSL that came with this Lares, and their version. */
std::wstring kit_dir()
{
    return exe_dir() + L"\\wsl";
}

std::string kit_version()
{
    return read_small_file(kit_dir() + L"\\VERSION");
}

/* The version setup last installed in WSL, and where. */
std::wstring kit_mark_path()
{
    return config_dir_path() + L"\\airdrop-kit.txt";
}

std::string kit_mark(const std::wstring& distro)
{
    return kit_version() + " " + narrow(distro);
}

ShareSetup look()
{
    std::wstring distro = wsl_distro();
    if (wsl_exe().empty() || distro.empty()) return ShareSetup::NoWsl;
    if (!share_adapter_present()) return ShareSetup::NoAdapter;
    if (usbipd_exe().empty() || !adapter_shared()) return ShareSetup::NeedsSetup;
    std::string version = kit_version();
    if (version.empty() || read_small_file(kit_mark_path()) != kit_mark(distro)) return ShareSetup::NeedsSetup;
    return ShareSetup::Ready;
}

/* Runs a program without a window and takes what it writes; -1 if it
 * didn't start, 124 if it ran out of time. */
int run_quiet(const std::wstring& cmdline, std::string* out, DWORD timeout_ms)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    /* The read ends when the program does; a watchdog ends one that hangs. */
    HANDLE proc = pi.hProcess;
    std::atomic<bool> timed_out{ false };
    std::thread watchdog([proc, timeout_ms, &timed_out] {
        if (WaitForSingleObject(proc, timeout_ms) == WAIT_TIMEOUT) {
            timed_out = true;
            TerminateProcess(proc, 124);
        }
    });
    char buf[1024];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
        if (out && out->size() < 64 * 1024) out->append(buf, n);
    watchdog.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return timed_out ? 124 : static_cast<int>(code);
}

/* Gives the adapter back to Windows, whatever state the session left it in. */
void detach_adapter()
{
    std::wstring usbipd = usbipd_exe();
    if (usbipd.empty()) return;
    run_quiet(L"\"" + usbipd + L"\" detach --hardware-id " + kShareAdapterUsbipd, nullptr, 15000);
}

std::wstring downloads_dir()
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)) && path) out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring base_name(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void write_line(HANDLE to, const std::string& line)
{
    DWORD wrote = 0;
    if (to) WriteFile(to, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
}

} // namespace

bool share_adapter_present()
{
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, kShareAdapterId, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) == CR_SUCCESS &&
        len > 1)   // more than the list's own ending
        return true;
    /* While WSL has it, Windows lists usbipd-win's stand-in in its place. */
    bool lent = false;
    adapter_shared(&lent);
    return lent;
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
        if (state == ShareIncomingState::Done) view_.s.incoming.saved = { "/mnt/c/Users/me/Downloads/IMG_0412.HEIC" };
    };
    if (want == L"nowsl") {
        view_.setup = ShareSetup::NoWsl;
    } else if (want == L"noadapter") {
        view_.setup = ShareSetup::NoAdapter;
    } else if (want == L"setup") {
        view_.setup = ShareSetup::NeedsSetup;
    } else if (want == L"setup-failed") {
        view_.setup = ShareSetup::NeedsSetup;
        view_.setup_error = "Couldn't install the packages in WSL. Is this PC online?";
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
        /* Not under a session: what it does to the adapter isn't news. */
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
        if (dry_run_) {
            view_.setup = ShareSetup::Ready;
        } else {
            view_.setup_busy = true;
        }
    }
    notify();
    if (dry_run_) return;
    if (setup_thread_.joinable()) setup_thread_.join();
    setup_thread_ = std::thread([this] { setup_worker(); });
}

void ShareManager::setup_worker()
{
    std::string error;
    std::wstring distro = wsl_distro();

    /* usbipd-win, and the adapter shared with it: both ask for
     * administrator rights, in one prompt. */
    if (usbipd_exe().empty() || !adapter_shared()) {
        std::wstring ps =
            L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \""
            L"$u = Join-Path $env:ProgramFiles 'usbipd-win\\usbipd.exe'; "
            L"if (-not (Test-Path $u)) { winget install --exact --id dorssel.usbipd-win --silent "
            L"--accept-source-agreements --accept-package-agreements --disable-interactivity | Out-Null }; "
            L"if (-not (Test-Path $u)) { exit 2 }; "
            L"& $u bind --hardware-id ";
        ps += kShareAdapterUsbipd;
        ps += L" | Out-Null; exit $LASTEXITCODE\"";
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = ps.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            error = "Setting AirDrop up needs administrator rights.";
        } else {
            WaitForSingleObject(sei.hProcess, 10 * 60 * 1000);
            DWORD code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            if (code == 2 || usbipd_exe().empty())
                error = "Couldn't install usbipd-win. Install it from github.com/dorssel/usbipd-win, then set up again.";
            else if (!adapter_shared())
                error = "Couldn't share the Wi-Fi adapter with WSL.";
        }
    }

    /* The WSL side: packages, the adapter's firmware and Lares' programs. */
    if (error.empty()) {
        std::string kit = wsl_path_from_windows(kit_dir());
        if (kit.empty() || kit_version().empty()) {
            error = "This copy of Lares came without its programs for WSL.";
        } else {
            std::string out;
            std::wstring cmd = L"\"" + wsl_exe() + L"\" -d " + distro + L" -u root -e bash \"" + widen(kit) + L"/setup.sh\"";
            int code = run_quiet(cmd, &out, 30 * 60 * 1000);
            if (out.find("setup ok") != std::string::npos) {
                write_small_file(kit_mark_path(), kit_mark(distro));
            } else if (out.find("setup failed packages") != std::string::npos) {
                error = "Couldn't install the packages in WSL. Is this PC online?";
            } else if (out.find("setup failed firmware") != std::string::npos) {
                error = "Couldn't find the Wi-Fi adapter's firmware in WSL.";
            } else if (out.find("setup failed build") != std::string::npos) {
                error = "Couldn't build AirDrop's programs in WSL.";
            } else if (code == 124) {
                error = "Setting up in WSL took too long.";
            } else {
                error = "Couldn't set AirDrop up in WSL. It needs Ubuntu or Debian there.";
            }
        }
    }

    ShareSetup setup = look();
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
    std::wstring wsl = wsl_exe(), distro = wsl_distro();
    std::string downloads = wsl_path_from_windows(downloads_dir());
    if (wsl.empty() || distro.empty() || downloads.empty()) return false;

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
    std::wstring cmd = L"\"" + wsl + L"\" -d " + distro + L" -u root -e " + kRunScript + L" \"" + widen(downloads) + L"\"";
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
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
             * and the adapter goes back to Windows. */
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
    detach_adapter();
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
            /* It never spoke: its programs aren't in WSL any more. */
            DeleteFileW(kit_mark_path().c_str());
            view_.setup = ShareSetup::NeedsSetup;
            view_.setup_error = "AirDrop's programs are missing in WSL. Set it up again.";
            pending_.clear();
        } else if (!asked) {
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
        view_.error = "Couldn't start AirDrop in WSL.";
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
                view_.error = "Couldn't start AirDrop in WSL.";
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
            /* Closing its input stops it as "quit" does, and it gives the
             * adapter back on its way out. */
            if (!quitting_) write_line(to_child_, "quit\n");
            quitting_ = true;
            CloseHandle(to_child_);
            to_child_ = nullptr;
            /* A handle of its own: the reader closes the session's. */
            DuplicateHandle(GetCurrentProcess(), proc_, GetCurrentProcess(), &proc, 0, FALSE, DUPLICATE_SAME_ACCESS);
        }
    }
    if (!reader_.joinable()) return;
    if (proc && WaitForSingleObject(proc, 2500) == WAIT_TIMEOUT) {
        /* Lares is closing and the session is slow to end: the adapter is
         * taken back from here, and what's left of the session ended. */
        detach_adapter();
        if (WaitForSingleObject(proc, 1500) == WAIT_TIMEOUT) TerminateProcess(proc, 1);
    }
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
    std::string line = "send";
    std::vector<std::wstring> names;
    for (const std::wstring& p : paths) {
        std::string w = wsl_path_from_windows(p);
        if (w.empty()) continue;
        line += "\t" + w;
        names.push_back(base_name(p));
    }
    if (names.empty()) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            view_.error = "Those files aren't on one of this PC's drives, so AirDrop can't reach them.";
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
