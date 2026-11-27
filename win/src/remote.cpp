// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "remote.h"
#include "config.h"
#include "programs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <shellapi.h>
#include <winsvc.h>
#endif

namespace lares {

static const char* const kItemKeys[kRemoteItemCount] = { "screen", "sunshine", "tailscale" };
static const char* const kItemNames[kRemoteItemCount] = { "Screen", "Sunshine", "Tailscale" };
static const char* const kDefaults[kRemoteModeCount][kRemoteItemCount] = {
    { "leave", "off", "off" },
    { "leave", "on",  "on"  },
};

std::string ScreenMode::spec() const
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%dx%d@%d", w, h, rate);
    return buf;
}

std::string ScreenMode::label() const
{
    return describe_screen_mode(w, h, static_cast<double>(rate));
}

RemoteConfig::RemoteConfig()
{
    for (int m = 0; m < kRemoteModeCount; ++m) {
        for (int i = 0; i < kRemoteItemCount; ++i) {
            values[m][i] = kDefaults[m][i];
        }
    }
}

const char* remote_item_name(RemoteItem i)
{
    int idx = static_cast<int>(i);
    if (idx < 0 || idx >= kRemoteItemCount) return "";
    return kItemNames[idx];
}

const char* remote_mode_name(RemoteMode m)
{
    return m == RemoteMode::Home ? "Home" : "Remote";
}

const RemoteItem* remote_apply_order(RemoteMode m)
{
    static const RemoteItem home_order[kRemoteItemCount] = {
        RemoteItem::Sunshine, RemoteItem::Screen, RemoteItem::Tailscale
    };
    static const RemoteItem remote_order[kRemoteItemCount] = {
        RemoteItem::Tailscale, RemoteItem::Screen, RemoteItem::Sunshine
    };
    return m == RemoteMode::Home ? home_order : remote_order;
}

bool parse_screen_spec(const std::string& s, int& w, int& h, double& rate)
{
    if (s.empty()) return false;
    int ww = 0, hh = 0;
    double rr = 60.0;
    int matched = std::sscanf(s.c_str(), "%dx%d@%lf", &ww, &hh, &rr);
    if (matched < 2 || ww <= 0 || hh <= 0 || rr <= 0.0) return false;
    w = ww;
    h = hh;
    rate = rr;
    return true;
}

std::string describe_screen_mode(int w, int h, double rate)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d \xC3\x97 %d \xC2\xB7 %d Hz", w, h, static_cast<int>(std::lround(rate)));
    return buf;
}

static bool is_leave(const std::string& v)
{
    return v.empty() || v == "leave";
}

static bool is_on(const std::string& v)
{
    return v == "on";
}

std::string remote_saved_value(RemoteMode m, RemoteItem i, const std::string& saved)
{
    int mi = static_cast<int>(m);
    int ii = static_cast<int>(i);
    if (saved.empty() || (i != RemoteItem::Screen && is_leave(saved))) {
        return kDefaults[mi][ii];
    }
    return saved;
}

std::string remote_own_size(const RemoteLive& live)
{
    const ScreenMode* own = nullptr;
    for (const auto& m : live.modes) {
        if (m.preferred || (!own && m.current)) own = &m;
        if (m.preferred) break;
    }
    if (own) return own->spec();
    if (live.screen_known && live.w > 0 && live.h > 0) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%dx%d@%d", live.w, live.h, live.rate);
        return buf;
    }
    return {};
}

std::string remote_describe(RemoteItem i, const std::string& v)
{
    if (is_leave(v)) return "Not changed";
    if (i == RemoteItem::Screen) {
        int w = 0, h = 0;
        double r = 60.0;
        return parse_screen_spec(v, w, h, r) ? describe_screen_mode(w, h, r) : v;
    }
    return is_on(v) ? "On" : "Off";
}

std::string remote_describe_live(const RemoteLive& live, RemoteItem i)
{
    if (i == RemoteItem::Screen) {
        if (!live.screen_known) return "Reading the screen...";
        return "Now " + describe_screen_mode(live.w, live.h, static_cast<double>(live.rate));
    }
    int v = (i == RemoteItem::Sunshine) ? live.sunshine : live.tailscale;
    if (v < 0) return "Checking...";
    return v ? "Now on" : "Now off";
}

bool remote_matches(const RemoteLive& live, RemoteItem i, const std::string& v)
{
    if (is_leave(v)) return true;
    if (i == RemoteItem::Screen) {
        int w = 0, h = 0;
        double r = 60.0;
        return live.screen_known && parse_screen_spec(v, w, h, r) &&
               live.w == w && live.h == h && std::fabs(static_cast<double>(live.rate) - r) <= 1.0;
    }
    int now = (i == RemoteItem::Sunshine) ? live.sunshine : live.tailscale;
    return now >= 0 && (now != 0) == is_on(v);
}

bool remote_setup_done(const RemoteLive& live)
{
    return live.tailscale_installed && live.tailscale_signed_in &&
           live.sunshine_installed && live.paired;
}

static std::string trim_str(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

RemoteConfig parse_lares_ini(const std::string& text)
{
    RemoteConfig cfg;
    std::istringstream in(text);
    std::string line;
    std::string group;
    while (std::getline(in, line)) {
        std::string t = trim_str(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            group = trim_str(t.substr(1, t.size() - 2));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_str(t.substr(0, eq));
        std::string val = trim_str(t.substr(eq + 1));

        if (group == "state") {
            if (key == "mode" && (val == "home" || val == "remote")) {
                cfg.mode_known = true;
                cfg.mode = (val == "remote") ? RemoteMode::Remote : RemoteMode::Home;
            } else if (key == "ready") {
                cfg.ready = (val == "true" || val == "1");
            }
        } else if (group == "home" || group == "remote") {
            RemoteMode m = (group == "remote") ? RemoteMode::Remote : RemoteMode::Home;
            for (int i = 0; i < kRemoteItemCount; ++i) {
                if (key == kItemKeys[i]) {
                    cfg.value(m, static_cast<RemoteItem>(i)) =
                        remote_saved_value(m, static_cast<RemoteItem>(i), val);
                }
            }
        }
    }
    return cfg;
}

std::string serialize_lares_ini(const RemoteConfig& cfg)
{
    std::ostringstream out;
    out << "[state]\n";
    if (cfg.mode_known) {
        out << "mode=" << (cfg.mode == RemoteMode::Remote ? "remote" : "home") << "\n";
    }
    out << "ready=" << (cfg.ready ? "true" : "false") << "\n\n";

    const char* mode_keys[kRemoteModeCount] = { "home", "remote" };
    for (int m = 0; m < kRemoteModeCount; ++m) {
        out << "[" << mode_keys[m] << "]\n";
        for (int i = 0; i < kRemoteItemCount; ++i) {
            out << kItemKeys[i] << "=" << cfg.values[m][i] << "\n";
        }
        if (m + 1 < kRemoteModeCount) out << "\n";
    }
    return out.str();
}

bool sunshine_state_has_paired_device(const std::string& json)
{
    size_t pos = json.find("\"named_devices\"");
    if (pos == std::string::npos) return false;
    size_t br = json.find('[', pos);
    if (br == std::string::npos) return false;
    ++br;
    while (br < json.size() && std::isspace(static_cast<unsigned char>(json[br]))) ++br;
    return br < json.size() && json[br] != ']';
}

bool sunshine_log_is_streaming(const std::string& log_tail)
{
    size_t on = log_tail.rfind("CLIENT CONNECTED");
    if (on == std::string::npos) return false;
    size_t off = log_tail.rfind("CLIENT DISCONNECTED");
    return off == std::string::npos || on > off;
}

#ifdef _WIN32

namespace {

std::wstring lares_ini_path(bool dry_run)
{
    return config_dir_path() + (dry_run ? L"\\lares-dry-run.ini" : L"\\lares.ini");
}

bool file_exists_w(const wchar_t* path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring tailscale_cli()
{
    wchar_t buf[MAX_PATH];
    return lares_find_tailscale(buf, MAX_PATH) ? std::wstring(buf) : std::wstring();
}

// Sunshine's install folder, from the program its Windows service runs:
// that program sits in the install folder or in its "tools" folder.
std::wstring sunshine_folder()
{
    std::wstring result;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return result;
    SC_HANDLE svc = OpenServiceW(scm, L"SunshineService", SERVICE_QUERY_CONFIG);
    if (svc) {
        DWORD need = 0;
        QueryServiceConfigW(svc, nullptr, 0, &need);
        if (need > 0 && need < 64 * 1024) {
            std::vector<BYTE> buf(need);
            auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
            wchar_t folder[MAX_PATH];
            if (QueryServiceConfigW(svc, cfg, need, &need) && cfg->lpBinaryPathName &&
                lares_service_program_folder(cfg->lpBinaryPathName, folder, MAX_PATH)) {
                std::wstring dir(folder);
                if (file_exists_w((dir + L"\\sunshine.exe").c_str())) {
                    result = dir;
                } else {
                    size_t slash = dir.find_last_of(L'\\');
                    if (slash != std::wstring::npos &&
                        file_exists_w((dir.substr(0, slash) + L"\\sunshine.exe").c_str())) {
                        result = dir.substr(0, slash);
                    }
                }
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return result;
}

// Runs a process silently without flashing a console window, capturing stdout/stderr.
int run_process_hidden(const std::wstring& cmdline, std::string& out_stdout, DWORD timeout_ms = 15000)
{
    out_stdout.clear();
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    std::wstring mutable_cmd = cmdline;
    BOOL ok = CreateProcessW(
        nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);

    if (!ok) {
        CloseHandle(rd);
        return -1;
    }

    char buf[1024];
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    bool timed_out = false;
    while (true) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buf)));
            DWORD n = 0;
            if (!ReadFile(rd, buf, to_read, &n, nullptr) || n == 0) break;
            out_stdout.append(buf, n);
        }
        DWORD wait_rc = WaitForSingleObject(pi.hProcess, 40);
        if (wait_rc == WAIT_OBJECT_0) {
            // Drain any final buffered output after process exit
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buf)));
                DWORD n = 0;
                if (!ReadFile(rd, buf, to_read, &n, nullptr) || n == 0) break;
                out_stdout.append(buf, n);
            }
            break;
        }
        if (GetTickCount64() >= deadline) {
            timed_out = true;
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 500);
            break;
        }
    }
    CloseHandle(rd);

    DWORD exit_code = 124;
    if (!timed_out) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
}

std::wstring primary_display_device()
{
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) {
            return dd.DeviceName;
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    return {};
}

void read_screen_live(RemoteLive& out)
{
    std::wstring dev = primary_display_device();
    const wchar_t* dev_ptr = dev.empty() ? nullptr : dev.c_str();

    DEVMODEW cur = {};
    cur.dmSize = sizeof(cur);
    if (EnumDisplaySettingsW(dev_ptr, ENUM_CURRENT_SETTINGS, &cur)) {
        out.screen_known = true;
        out.w = static_cast<int>(cur.dmPelsWidth);
        out.h = static_cast<int>(cur.dmPelsHeight);
        out.rate = static_cast<int>(cur.dmDisplayFrequency);
    }

    DEVMODEW reg = {};
    reg.dmSize = sizeof(reg);
    int pref_w = out.w, pref_h = out.h, pref_r = out.rate;
    if (EnumDisplaySettingsW(dev_ptr, ENUM_REGISTRY_SETTINGS, &reg) && reg.dmPelsWidth > 0) {
        pref_w = static_cast<int>(reg.dmPelsWidth);
        pref_h = static_cast<int>(reg.dmPelsHeight);
        pref_r = static_cast<int>(reg.dmDisplayFrequency);
    }

    out.modes.clear();
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    int max_w = 0, max_h = 0;
    for (DWORD i = 0; EnumDisplaySettingsW(dev_ptr, i, &dm); ++i) {
        if (dm.dmBitsPerPel == 32 && dm.dmPelsWidth >= 1024 && dm.dmPelsHeight >= 720) {
            int w = static_cast<int>(dm.dmPelsWidth);
            int h = static_cast<int>(dm.dmPelsHeight);
            int r = static_cast<int>(dm.dmDisplayFrequency);
            if (w * h > max_w * max_h) { max_w = w; max_h = h; }
            bool exists = false;
            for (const auto& existing : out.modes) {
                if (existing.w == w && existing.h == h && existing.rate == r) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                ScreenMode sm;
                sm.w = w;
                sm.h = h;
                sm.rate = r;
                sm.current = (w == out.w && h == out.h && r == out.rate);
                sm.preferred = (w == pref_w && h == pref_h && r == pref_r);
                out.modes.push_back(sm);
            }
        }
        dm = {};
        dm.dmSize = sizeof(dm);
    }

    std::sort(out.modes.begin(), out.modes.end(), [](const ScreenMode& a, const ScreenMode& b) {
        int area_a = a.w * a.h;
        int area_b = b.w * b.h;
        if (area_a != area_b) return area_a > area_b;
        if (a.w != b.w) return a.w > b.w;
        return a.rate > b.rate;
    });
}

void read_sunshine_live(RemoteLive& out, bool dry_stream)
{
    // Sunshine's folder comes from its own service, never a fixed place (#141).
    std::wstring sun_dir = sunshine_folder();
    std::wstring sun_exe = sun_dir.empty() ? std::wstring() : sun_dir + L"\\sunshine.exe";
    std::wstring sun_state = sun_dir.empty() ? std::wstring() : sun_dir + L"\\config\\sunshine_state.json";
    std::wstring sun_log = sun_dir.empty() ? std::wstring() : sun_dir + L"\\config\\sunshine.log";

    out.sunshine_installed = !sun_exe.empty() && file_exists_w(sun_exe.c_str());
    out.sunshine = 0;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"SunshineService", SERVICE_QUERY_STATUS);
        if (svc) {
            out.sunshine_installed = true;
            SERVICE_STATUS_PROCESS ssp = {};
            DWORD bytes = 0;
            if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                     reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes)) {
                out.sunshine = (ssp.dwCurrentState == SERVICE_RUNNING ||
                                ssp.dwCurrentState == SERVICE_START_PENDING) ? 1 : 0;
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }

    // Read sunshine_state.json only to check if named_devices is non-empty, then wipe buffer.
    out.paired = false;
    std::ifstream st_in(sun_state, std::ios::binary);
    if (st_in.is_open()) {
        std::ostringstream ss;
        ss << st_in.rdbuf();
        std::string content = ss.str();
        out.paired = sunshine_state_has_paired_device(content);
        if (!content.empty()) {
            SecureZeroMemory(content.data(), content.size());
        }
    }

    out.streaming = false;
    if (out.sunshine == 1) {
        if (dry_stream) {
            out.streaming = true;
        } else {
            std::ifstream log_in(sun_log, std::ios::binary | std::ios::ate);
            if (log_in.is_open()) {
                std::streamoff sz = log_in.tellg();
                if (sz > 0) {
                    std::streamoff start = sz > 262144 ? (sz - 262144) : 0;
                    log_in.seekg(start, std::ios::beg);
                    std::string tail(static_cast<size_t>(sz - start), '\0');
                    log_in.read(tail.data(), sz - start);
                    out.streaming = sunshine_log_is_streaming(tail);
                }
            }
        }
    }
}

static std::string extract_json_string(const std::string& json, const char* key, size_t start_pos = 0)
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t pos = json.find(pattern, start_pos);
    if (pos == std::string::npos) return {};
    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

void read_tailscale_live(RemoteLive& out)
{
    // Found through Tailscale's own service, else the PATH (issue #141).
    std::wstring ts_exe = tailscale_cli();
    out.tailscale_installed = !ts_exe.empty();
    out.tailscale = 0;
    out.tailscale_signed_in = false;
    out.tailscale_host.clear();
    out.tailscale_ip.clear();

    if (!out.tailscale_installed) return;

    std::string json;
    int rc = run_process_hidden(L"\"" + ts_exe + L"\" status --json", json, 5000);
    if (rc < 0 || json.empty()) return;

    std::string state = extract_json_string(json, "BackendState");
    out.tailscale = (state == "Running") ? 1 : 0;
    out.tailscale_signed_in = (state == "Running" || state == "Stopped");

    size_t self_pos = json.find("\"Self\"");
    if (self_pos != std::string::npos) {
        std::string dns = extract_json_string(json, "DNSName", self_pos);
        if (!dns.empty()) {
            size_t dot = dns.find('.');
            out.tailscale_host = (dot != std::string::npos) ? dns.substr(0, dot) : dns;
        } else {
            out.tailscale_host = extract_json_string(json, "HostName", self_pos);
        }

        size_t ips_pos = json.find("\"TailscaleIPs\"", self_pos);
        if (ips_pos != std::string::npos) {
            size_t br = json.find('[', ips_pos);
            if (br != std::string::npos) {
                size_t q1 = json.find('"', br);
                size_t q2 = (q1 != std::string::npos) ? json.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    out.tailscale_ip = json.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }
}

} // namespace

RemoteController::RemoteController(bool dry_run, bool dry_stream)
    : dry_run_(dry_run), dry_stream_(dry_stream)
{
    ini_path_ = lares_ini_path(dry_run_);
    load_config_from_disk();
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    poll_thread_ = std::thread(&RemoteController::poll_loop, this);
}

RemoteController::~RemoteController()
{
    stop_.store(true);
    if (wake_event_) SetEvent(wake_event_);
    if (poll_thread_.joinable()) poll_thread_.join();
    if (apply_thread_.joinable()) apply_thread_.join();
    if (wake_event_) CloseHandle(wake_event_);
}

void RemoteController::load_config_from_disk()
{
    std::ifstream in(ini_path_, std::ios::binary);
    if (!in.is_open()) {
        cfg_ = RemoteConfig{};
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    cfg_ = parse_lares_ini(ss.str());
}

void RemoteController::save_config_to_disk(const RemoteConfig& cfg)
{
    size_t slash = ini_path_.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        CreateDirectoryW(ini_path_.substr(0, slash).c_str(), nullptr);
    }
    std::wstring tmp = ini_path_ + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        std::string text = serialize_lares_ini(cfg);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out.good()) return;
    }
    if (!MoveFileExW(tmp.c_str(), ini_path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
    }
}

RemoteConfig RemoteController::config() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
}

void RemoteController::set_config(const RemoteConfig& cfg, bool save_to_disk)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = cfg;
    }
    if (save_to_disk) save_config_to_disk(cfg);
}

RemoteLive RemoteController::live() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return live_;
}

RemoteLive RemoteController::read_live_once()
{
    RemoteLive snap;
    bool need_save = false;
    RemoteConfig updated_cfg;

    if (dry_run_) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!dry_seeded_) {
            dry_seeded_ = true;
            live_.screen_known = true;
            live_.w = 1920;
            live_.h = 1080;
            live_.rate = 75;
            live_.modes = {
                { 1920, 1080, 75, true,  true  },
                { 1920, 1080, 60, false, false },
                { 1680, 1050, 75, false, false },
                { 1680, 1050, 60, false, false },
                { 1440, 900,  60, false, false },
                { 1280, 800,  60, false, false },
                { 1280, 720,  60, false, false },
            };
            live_.sunshine_installed = true;
            live_.tailscale_installed = true;
            live_.tailscale_signed_in = true;
            live_.tailscale_host = "lares-pc";
            live_.tailscale_ip = "100.64.0.1";
            live_.paired = true;
            live_.sunshine = (cfg_.mode_known && cfg_.mode == RemoteMode::Remote) ? 1 : 0;
            live_.tailscale = (cfg_.mode_known && cfg_.mode == RemoteMode::Remote) ? 1 : 0;
        }
        live_.streaming = (live_.sunshine == 1) && dry_stream_;
        snap = live_;
    } else {
        read_screen_live(snap);
        read_sunshine_live(snap, dry_stream_);
        read_tailscale_live(snap);
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        live_ = snap;
        std::string own = remote_own_size(live_);
        if (!own.empty()) {
            for (int m = 0; m < kRemoteModeCount; ++m) {
                if (is_leave(cfg_.values[m][static_cast<int>(RemoteItem::Screen)])) {
                    cfg_.values[m][static_cast<int>(RemoteItem::Screen)] = own;
                    need_save = true;
                }
            }
        }
        if (!cfg_.ready && remote_setup_done(live_)) {
            cfg_.ready = true;
            need_save = true;
        }
        if (!cfg_.mode_known && live_.sunshine >= 0 && live_.tailscale >= 0) {
            cfg_.mode_known = true;
            cfg_.mode = (live_.sunshine == 1 && live_.tailscale == 1)
                ? RemoteMode::Remote : RemoteMode::Home;
            need_save = true;
        }
        updated_cfg = cfg_;
    }

    if (need_save) save_config_to_disk(updated_cfg);
    return snap;
}

RemoteStepResult RemoteController::apply_item_sync(RemoteItem item, const std::string& value)
{
    RemoteStepResult res;
    res.item = item;
    res.ok = true;
    res.needs_permission = false;

    if (is_leave(value)) return res;

    if (dry_run_) {
        std::lock_guard<std::mutex> lock(mu_);
        if (item == RemoteItem::Screen) {
            int w = 0, h = 0;
            double r = 60.0;
            if (parse_screen_spec(value, w, h, r)) {
                live_.w = w;
                live_.h = h;
                live_.rate = static_cast<int>(std::lround(r));
                for (auto& m : live_.modes) {
                    m.current = (m.w == live_.w && m.h == live_.h && m.rate == live_.rate);
                }
            }
        } else if (item == RemoteItem::Sunshine) {
            live_.sunshine = is_on(value) ? 1 : 0;
            live_.streaming = (live_.sunshine == 1) && dry_stream_;
        } else if (item == RemoteItem::Tailscale) {
            live_.tailscale = is_on(value) ? 1 : 0;
        }
        return res;
    }

    if (item == RemoteItem::Screen) {
        int w = 0, h = 0;
        double r = 60.0;
        if (!parse_screen_spec(value, w, h, r)) {
            res.ok = false;
            res.message = "Invalid screen mode";
            return res;
        }
        std::wstring dev = primary_display_device();
        const wchar_t* dev_ptr = dev.empty() ? nullptr : dev.c_str();
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmBitsPerPel = 32;
        dm.dmPelsWidth = static_cast<DWORD>(w);
        dm.dmPelsHeight = static_cast<DWORD>(h);
        dm.dmDisplayFrequency = static_cast<DWORD>(std::lround(r));
        dm.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        LONG rc = ChangeDisplaySettingsExW(dev_ptr, &dm, nullptr, 0, nullptr);
        if (rc != DISP_CHANGE_SUCCESSFUL) {
            res.ok = false;
            res.message = "The monitor didn't accept that size";
        }
        return res;
    }

    if (item == RemoteItem::Tailscale) {
        std::wstring ts_exe = tailscale_cli();
        if (ts_exe.empty()) {
            res.ok = false;
            res.message = "Tailscale isn't installed";
            return res;
        }
        std::wstring cmd = L"\"" + ts_exe + (is_on(value) ? L"\" up" : L"\" down");
        std::string out;
        int rc = run_process_hidden(cmd, out, 30000);
        if (rc != 0) {
            res.ok = false;
            if (rc == 124) {
                res.message = "Tailscale didn't connect in 30 seconds";
            } else {
                res.needs_permission = (out.find("Access denied") != std::string::npos ||
                                        out.find("access denied") != std::string::npos);
                res.message = out.empty() ? "Tailscale didn't change" : trim_str(out);
            }
        }
        return res;
    }

    if (item == RemoteItem::Sunshine) {
        bool want_on = is_on(value);
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) {
            res.ok = false;
            res.message = "Couldn't open Windows Service Control Manager";
            return res;
        }
        DWORD access = SERVICE_QUERY_STATUS | (want_on ? SERVICE_START : SERVICE_STOP);
        SC_HANDLE svc = OpenServiceW(scm, L"SunshineService", access);
        if (!svc) {
            DWORD err = GetLastError();
            CloseServiceHandle(scm);
            res.ok = false;
            if (err == ERROR_ACCESS_DENIED) {
                res.needs_permission = true;
                res.message = "Needs permission to start or stop SunshineService";
            } else {
                res.message = "SunshineService isn't installed";
            }
            return res;
        }

        SERVICE_STATUS_PROCESS ssp = {};
        DWORD bytes = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes)) {
            bool already = want_on
                ? (ssp.dwCurrentState == SERVICE_RUNNING)
                : (ssp.dwCurrentState == SERVICE_STOPPED);
            if (already) {
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return res;
            }
        }

        BOOL ok = FALSE;
        if (want_on) {
            ok = StartServiceW(svc, 0, nullptr) || (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING);
        } else {
            SERVICE_STATUS ss = {};
            ok = ControlService(svc, SERVICE_CONTROL_STOP, &ss) || (GetLastError() == ERROR_SERVICE_NOT_ACTIVE);
        }

        if (!ok) {
            res.ok = false;
            res.message = want_on ? "SunshineService couldn't start" : "SunshineService couldn't stop";
        } else {
            DWORD target_state = want_on ? SERVICE_RUNNING : SERVICE_STOPPED;
            for (int tick = 0; tick < 40; ++tick) {
                if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                         reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes) &&
                    ssp.dwCurrentState == target_state) {
                    break;
                }
                Sleep(150);
            }
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return res;
    }

    return res;
}

void RemoteController::apply_mode_async(RemoteMode mode)
{
    if (applying_.exchange(true)) return;
    if (apply_thread_.joinable()) apply_thread_.join();

    RemoteConfig cfg_snap;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_.mode = mode;
        cfg_.mode_known = true;
        cfg_snap = cfg_;
    }
    save_config_to_disk(cfg_snap);
    if (HWND hwnd_now = notify_hwnd_.load(); hwnd_now && !stop_.load()) {
        PostMessageW(hwnd_now, kMsgRemoteLive, 0, 0);
    }

    apply_thread_ = std::thread([this, mode, cfg_snap]() {
        const RemoteItem* order = remote_apply_order(mode);
        for (int s = 0; s < kRemoteItemCount && !stop_.load(); ++s) {
            RemoteItem item = order[s];
            std::string val = cfg_snap.value(mode, item);
            RemoteStepResult r = apply_item_sync(item, val);
            read_live_once();
            HWND hwnd = notify_hwnd_.load();
            if (hwnd && !stop_.load()) {
                auto* heap_res = new RemoteStepResult(r);
                PostMessageW(hwnd, kMsgRemoteStepDone, 0, reinterpret_cast<LPARAM>(heap_res));
            }
            if (!stop_.load() && s + 1 < kRemoteItemCount) Sleep(140);
        }
        applying_.store(false);
        HWND hwnd = notify_hwnd_.load();
        if (hwnd && !stop_.load()) {
            PostMessageW(hwnd, kMsgRemoteLive, 0, 0);
        }
    });
}

void RemoteController::apply_single_async(RemoteItem item, const std::string& value)
{
    if (applying_.exchange(true)) return;
    if (apply_thread_.joinable()) apply_thread_.join();

    RemoteConfig cfg_snap;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_.value(cfg_.mode, item) = value;
        cfg_snap = cfg_;
    }
    save_config_to_disk(cfg_snap);
    if (HWND hwnd_now = notify_hwnd_.load(); hwnd_now && !stop_.load()) {
        PostMessageW(hwnd_now, kMsgRemoteLive, 0, 0);
    }

    apply_thread_ = std::thread([this, item, value]() {
        RemoteStepResult r = apply_item_sync(item, value);
        read_live_once();
        applying_.store(false);
        HWND hwnd = notify_hwnd_.load();
        if (hwnd && !stop_.load()) {
            auto* heap_res = new RemoteStepResult(r);
            PostMessageW(hwnd, kMsgRemoteStepDone, 0, reinterpret_cast<LPARAM>(heap_res));
            PostMessageW(hwnd, kMsgRemoteLive, 0, 0);
        }
    });
}

bool RemoteController::allow_sunshine_service()
{
    if (dry_run_) return true;
    // Appends (A;;RPWP;;;IU) (Interactive Users: start/stop) to SunshineService's DACL once via UAC.
    const wchar_t* ps_cmd =
        L"-NoProfile -ExecutionPolicy Bypass -Command \""
        L"$sd = (sc.exe sdshow SunshineService | Where-Object { $_ -match '^D:' }) -join ''; "
        L"if (-not $sd) { exit 1 }; "
        L"if ($sd -notmatch 'RPWP;;;IU') { "
        L"  if ($sd -match 'S:') { $sd = $sd -replace 'S:', '(A;;RPWP;;;IU)S:' } else { $sd += '(A;;RPWP;;;IU)' }; "
        L"  sc.exe sdset SunshineService $sd | Out-Null; exit $LASTEXITCODE "
        L"}\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = ps_cmd;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, 15000);
    DWORD exit_code = 1;
    GetExitCodeProcess(sei.hProcess, &exit_code);
    CloseHandle(sei.hProcess);
    request_refresh();
    return exit_code == 0;
}

void RemoteController::set_wanted(bool on)
{
    bool prev = wanted_.exchange(on);
    if (on && !prev && wake_event_) {
        SetEvent(wake_event_);
    }
}

void RemoteController::request_refresh()
{
    if (wake_event_) SetEvent(wake_event_);
}

void RemoteController::poll_loop()
{
    while (!stop_.load()) {
        if (!wanted_.load()) {
            WaitForSingleObject(wake_event_, INFINITE);
            continue;
        }
        if (!applying_.load()) {
            read_live_once();
            HWND hwnd = notify_hwnd_.load();
            if (hwnd && !stop_.load()) {
                PostMessageW(hwnd, kMsgRemoteLive, 0, 0);
            }
        }
        WaitForSingleObject(wake_event_, 3000);
    }
}

int run_remote_cli(CliAction action, bool dry_run, bool dry_stream, std::string& out_text)
{
    std::ostringstream out;
    RemoteController ctrl(dry_run, dry_stream);
    RemoteLive live = ctrl.read_live_once();
    RemoteConfig cfg = ctrl.config();

    if (action == CliAction::Status) {
        out << (cfg.mode_known ? remote_mode_name(cfg.mode) : "No mode picked yet") << "\n";
        for (int i = 0; i < kRemoteItemCount; ++i) {
            auto item = static_cast<RemoteItem>(i);
            std::string now = remote_describe_live(live, item);
            bool differs = cfg.mode_known && !remote_matches(live, item, cfg.value(cfg.mode, item));
            char line[128];
            std::snprintf(line, sizeof(line), "  %-10s %s%s\n",
                          remote_item_name(item), now.c_str(), differs ? "  (differs)" : "");
            out << line;
        }
        if (live.streaming) out << "  Moonlight is streaming\n";
        out_text = out.str();
        return 0;
    }

    if (action == CliAction::Home) {
        cfg.mode = RemoteMode::Home;
    } else if (action == CliAction::Remote) {
        cfg.mode = RemoteMode::Remote;
    } else if (action == CliAction::Restore) {
        if (!cfg.mode_known) {
            out_text = "No mode picked yet, nothing to restore.\n";
            return 0;
        }
    }

    cfg.mode_known = true;
    ctrl.set_config(cfg, true);
    out << remote_mode_name(cfg.mode) << "\n";

    int failed = 0;
    const RemoteItem* order = remote_apply_order(cfg.mode);
    for (int s = 0; s < kRemoteItemCount; ++s) {
        RemoteItem item = order[s];
        std::string want = cfg.value(cfg.mode, item);
        std::string desc = remote_describe(item, want);
        RemoteStepResult r = ctrl.apply_item_sync(item, want);
        char line[256];
        if (r.ok) {
            std::snprintf(line, sizeof(line), "  %-10s %s\n", remote_item_name(item), desc.c_str());
            out << line;
        } else {
            std::snprintf(line, sizeof(line), "  %-10s %s didn't happen: %s\n",
                          remote_item_name(item), desc.c_str(), r.message.c_str());
            out << line;
            if (r.needs_permission && item == RemoteItem::Sunshine) {
                out << "             Allow it once from the Remote section in Lares.\n";
            }
            ++failed;
        }
    }

    out_text = out.str();
    return failed ? 1 : 0;
}

#endif // _WIN32

} // namespace lares
