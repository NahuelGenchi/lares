// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "hw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>
#endif

namespace lares {

void GraphRing::push(double fraction)
{
    float v = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    ring[head] = v;
    head = (head + 1) % kGraphSamples;
    if (len < kGraphSamples) ++len;
}

float GraphRing::latest(int age) const
{
    if (age < 0 || age >= len) return 0.0f;
    return ring[(head - 1 - age + kGraphSamples) % kGraphSamples];
}

std::string format_bytes(uint64_t bytes)
{
    double gb = static_cast<double>(bytes) / 1073741824.0;
    char buf[32];
    if (gb >= 1000.0) std::snprintf(buf, sizeof(buf), "%.1f TB", gb / 1024.0);
    else if (gb >= 10.0) std::snprintf(buf, sizeof(buf), "%.0f GB", gb);
    else std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
    return buf;
}

static void erase_all(std::string& s, const char* needle)
{
    size_t nlen = std::strlen(needle);
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        s.erase(pos, nlen);
    }
}

static std::string collapse_spaces(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool in_space = true;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!in_space) {
                out.push_back(' ');
                in_space = true;
            }
        } else {
            out.push_back(c);
            in_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string clean_cpu_name(const std::string& raw)
{
    std::string s = raw;
    erase_all(s, "(R)");
    erase_all(s, "(TM)");
    erase_all(s, " CPU");
    size_t at = s.find(" @");
    if (at != std::string::npos) s.erase(at);
    // Strip trailing N-Core Processor suffix common on AMD strings if present
    s = collapse_spaces(s);
    return s.empty() ? "Processor" : s;
}

std::string clean_gpu_name(const std::string& raw)
{
    std::string s = raw;
    erase_all(s, "(R)");
    erase_all(s, "(TM)");
    erase_all(s, " Series");
    s = collapse_spaces(s);
    return s;
}

int meter_filled_blocks(double fraction, int blocks)
{
    if (blocks <= 0 || fraction < 0.0 || std::isnan(fraction)) return 0;
    double clamped = std::clamp(fraction, 0.0, 1.0);
    int filled = static_cast<int>(std::lround(clamped * blocks));
    if (fraction > 0.0 && filled == 0) filled = 1;
    return filled;
}

int graph_step_height(float fraction, int steps)
{
    if (steps <= 0 || fraction <= 0.0f || std::isnan(fraction)) return 0;
    float clamped = std::clamp(fraction, 0.0f, 1.0f);
    int h = static_cast<int>(std::lround(clamped * static_cast<float>(steps)));
    if (fraction > 0.0f && h == 0) h = 1;
    return h;
}

#ifdef _WIN32

namespace {

using AdlMallocCb = void* (__cdecl*)(int);
using AdlCreateFn = int (__cdecl*)(AdlMallocCb, int);
using AdlDestroyFn = int (__cdecl*)();
using AdlNumAdaptersFn = int (__cdecl*)(int*);

struct AdlTemperature {
    int size;
    int temperature; // millidegrees C
};

struct AdlPmActivity {
    int size;
    int engine_clock;
    int memory_clock;
    int vddc;
    int activity_percent;
    int current_perf_level;
    int current_bus_speed;
    int current_bus_lanes;
    int max_bus_lanes;
    int reserved;
};

struct AdlFanSpeedValue {
    int size;
    int speed_type;
    int fan_speed;
    int flags;
};

using AdlTempGetFn = int (__cdecl*)(int, int, AdlTemperature*);
using AdlActGetFn = int (__cdecl*)(int, AdlPmActivity*);
using AdlFanGetFn = int (__cdecl*)(int, int, AdlFanSpeedValue*);

void* __cdecl adl_malloc(int size)
{
    return std::malloc(static_cast<size_t>(size));
}

std::string wide_to_utf8_str(const wchar_t* ws)
{
    if (!ws || !*ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring system_drive_root()
{
    wchar_t win_dir[MAX_PATH] = {};
    UINT n = GetSystemWindowsDirectoryW(win_dir, MAX_PATH);
    if (n >= 2 && win_dir[1] == L':') {
        std::wstring root;
        root.push_back(win_dir[0]);
        root += L":\\";
        return root;
    }
    return L"C:\\";
}

std::wstring system_physical_drive_path()
{
    std::wstring root = system_drive_root();
    std::wstring vol = L"\\\\.\\";
    vol.push_back(root[0]);
    vol.push_back(L':');
    HANDLE hv = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (hv != INVALID_HANDLE_VALUE) {
        VOLUME_DISK_EXTENTS vde = {};
        DWORD bytes = 0;
        if (DeviceIoControl(hv, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                            &vde, sizeof(vde), &bytes, nullptr) &&
            vde.NumberOfDiskExtents > 0) {
            CloseHandle(hv);
            wchar_t path[64];
            std::swprintf(path, std::size(path), L"\\\\.\\PhysicalDrive%lu", vde.Extents[0].DiskNumber);
            return path;
        }
        CloseHandle(hv);
    }
    return L"\\\\.\\PhysicalDrive0";
}

} // namespace

HardwareSampler::HardwareSampler(bool dry_run)
    : dry_run_(dry_run)
{
    init_static_info();
    if (!dry_run_) {
        init_adl();
    }
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker_ = std::thread(&HardwareSampler::worker_loop, this);
}

HardwareSampler::~HardwareSampler()
{
    stop_.store(true);
    if (wake_event_) SetEvent(wake_event_);
    if (worker_.joinable()) worker_.join();
    if (wake_event_) CloseHandle(wake_event_);
    shutdown_adl();
    if (pdh_query_) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(pdh_query_));
        pdh_query_ = nullptr;
    }
}

void HardwareSampler::init_static_info()
{
    if (dry_run_) {
        info_.cpu_name = "Sample 8-Core Processor";
        info_.threads = 16;
        info_.gpu_name = "Sample Radeon Graphics";
        info_.vram_total = 4ULL * 1024ULL * 1024ULL * 1024ULL;
        info_.disk_kind = "NVMe";
        return;
    }

    info_.threads = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    if (info_.threads <= 0) info_.threads = 1;

    wchar_t cpu_buf[256] = {};
    DWORD sz = sizeof(cpu_buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu_buf, &sz) == ERROR_SUCCESS) {
        info_.cpu_name = clean_cpu_name(wide_to_utf8_str(cpu_buf));
    }

    // Read discrete/primary GPU name and total VRAM from the display class registry key
    // without loading any 3D GPU user-mode driver DLL into the UI process.
    for (int idx = 0; idx < 4; ++idx) {
        wchar_t subkey[128];
        std::swprintf(subkey, std::size(subkey),
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\%04d", idx);
        wchar_t desc[256] = {};
        DWORD dsz = sizeof(desc);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, L"DriverDesc",
                         RRF_RT_REG_SZ, nullptr, desc, &dsz) == ERROR_SUCCESS && desc[0] != L'\0') {
            std::string cleaned = clean_gpu_name(wide_to_utf8_str(desc));
            if (!cleaned.empty() && cleaned.find("Basic Display") == std::string::npos) {
                info_.gpu_name = cleaned;
                uint64_t qw_mem = 0;
                DWORD msz = sizeof(qw_mem);
                if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, L"HardwareInformation.qwMemorySize",
                                 RRF_RT_REG_QWORD | RRF_RT_REG_BINARY, nullptr, &qw_mem, &msz) == ERROR_SUCCESS && qw_mem > 0) {
                    info_.vram_total = qw_mem;
                } else {
                    DWORD dw_mem = 0;
                    msz = sizeof(dw_mem);
                    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, L"HardwareInformation.MemorySize",
                                     RRF_RT_REG_DWORD | RRF_RT_REG_BINARY, nullptr, &dw_mem, &msz) == ERROR_SUCCESS && dw_mem > 0) {
                        info_.vram_total = static_cast<uint64_t>(dw_mem);
                    }
                }
                break;
            }
        }
    }

    // Check if the system physical drive is NVMe using unprivileged metadata handle (dwDesiredAccess = 0).
    std::wstring phys_drive = system_physical_drive_path();
    HANDLE hd = CreateFileW(phys_drive.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (hd != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q = {};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        uint8_t buf[512] = {};
        DWORD bytes = 0;
        if (DeviceIoControl(hd, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                            buf, sizeof(buf), &bytes, nullptr) &&
            bytes >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf);
            if (d->BusType == BusTypeNvme) {
                info_.disk_kind = "NVMe";
            }
        }
        CloseHandle(hd);
    }
}

void HardwareSampler::init_adl()
{
    adl_dll_ = LoadLibraryW(L"atiadlxx.dll");
    if (adl_dll_) {
        auto create = reinterpret_cast<AdlCreateFn>(GetProcAddress(adl_dll_, "ADL_Main_Control_Create"));
        auto destroy = reinterpret_cast<AdlDestroyFn>(GetProcAddress(adl_dll_, "ADL_Main_Control_Destroy"));
        auto num_fn = reinterpret_cast<AdlNumAdaptersFn>(GetProcAddress(adl_dll_, "ADL_Adapter_NumberOfAdapters_Get"));
        auto temp_fn = reinterpret_cast<AdlTempGetFn>(GetProcAddress(adl_dll_, "ADL_Overdrive5_Temperature_Get"));
        auto act_fn = reinterpret_cast<AdlActGetFn>(GetProcAddress(adl_dll_, "ADL_Overdrive5_CurrentActivity_Get"));
        auto fan_fn = reinterpret_cast<AdlFanGetFn>(GetProcAddress(adl_dll_, "ADL_Overdrive5_FanSpeed_Get"));

        if (create && destroy && num_fn && temp_fn && act_fn && create(adl_malloc, 1) == 0) {
            fn_adl_destroy_ = reinterpret_cast<void*>(destroy);
            fn_adl_temp_ = reinterpret_cast<void*>(temp_fn);
            fn_adl_activity_ = reinterpret_cast<void*>(act_fn);
            fn_adl_fan_ = reinterpret_cast<void*>(fan_fn);

            int n = 0;
            num_fn(&n);
            for (int i = 0; i < n; ++i) {
                AdlPmActivity a = {};
                a.size = sizeof(a);
                if (act_fn(i, &a) == 0) {
                    adl_adapter_ = i;
                    break;
                }
            }
        }
    }

    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS && q) {
        pdh_query_ = q;
        PDH_HCOUNTER c_vram = nullptr;
        if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &c_vram) == ERROR_SUCCESS) {
            pdh_vram_counter_ = c_vram;
        }
        if (adl_adapter_ < 0) {
            PDH_HCOUNTER c_load = nullptr;
            if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c_load) == ERROR_SUCCESS) {
                pdh_gpu_load_counter_ = c_load;
            }
        }
        PdhCollectQueryData(q);
    }
}

void HardwareSampler::shutdown_adl()
{
    if (fn_adl_destroy_) {
        reinterpret_cast<AdlDestroyFn>(fn_adl_destroy_)();
        fn_adl_destroy_ = nullptr;
    }
    if (adl_dll_) {
        FreeLibrary(adl_dll_);
        adl_dll_ = nullptr;
    }
}

static uint64_t filetime_to_u64(const FILETIME& ft)
{
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void HardwareSampler::sample_cpu(HwSample& out)
{
    if (fresh_load_.exchange(false)) {
        last_cpu_total_ = 0;
        out.cpu = -1.0;
    }
    FILETIME idle_ft = {}, kernel_ft = {}, user_ft = {};
    if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        out.cpu = -1.0;
        return;
    }
    uint64_t idle = filetime_to_u64(idle_ft);
    uint64_t total = filetime_to_u64(kernel_ft) + filetime_to_u64(user_ft);
    uint64_t busy = total > idle ? (total - idle) : 0;

    if (last_cpu_total_ > 0 && total > last_cpu_total_) {
        uint64_t dt = total - last_cpu_total_;
        uint64_t db = busy >= last_cpu_busy_ ? (busy - last_cpu_busy_) : 0;
        out.cpu = std::clamp(static_cast<double>(db) / static_cast<double>(dt), 0.0, 1.0);
    }
    last_cpu_busy_ = busy;
    last_cpu_total_ = total;
    out.cpu_temp = NAN; // Windows has no unprivileged user-mode CPU package temp API
}

void HardwareSampler::sample_memory(HwSample& out)
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out.mem_total = ms.ullTotalPhys;
        out.mem_used = ms.ullTotalPhys > ms.ullAvailPhys ? (ms.ullTotalPhys - ms.ullAvailPhys) : 0;
    }
}

void HardwareSampler::sample_disk(HwSample& out, bool details)
{
    ULARGE_INTEGER avail = {}, total = {}, free_bytes = {};
    std::wstring root = system_drive_root();
    if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &free_bytes)) {
        out.disk_total = total.QuadPart;
        out.disk_used = total.QuadPart > avail.QuadPart ? (total.QuadPart - avail.QuadPart) : 0;
    }

    if (!details) return;

    std::wstring phys_drive = system_physical_drive_path();
    HANDLE hd = CreateFileW(phys_drive.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (hd != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q = {};
        q.PropertyId = StorageDeviceTemperatureProperty;
        q.QueryType = PropertyStandardQuery;
        uint8_t buf[512] = {};
        DWORD bytes = 0;
        if (DeviceIoControl(hd, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                            buf, sizeof(buf), &bytes, nullptr) &&
            bytes >= sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR)) {
            const auto* d = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf);
            if (d->InfoCount > 0) {
                short t = d->TemperatureInfo[0].Temperature;
                if (t > -50 && t < 150) {
                    out.disk_temp = static_cast<double>(t);
                }
            }
        }
        CloseHandle(hd);
    }
}

void HardwareSampler::sample_gpu(HwSample& out, bool details)
{
    if (info_.gpu_name.empty()) {
        out.gpu = -1.0;
        return;
    }
    out.vram_total = info_.vram_total;

    if (adl_adapter_ >= 0 && fn_adl_activity_ && fn_adl_temp_) {
        AdlPmActivity act = {};
        act.size = sizeof(act);
        if (reinterpret_cast<AdlActGetFn>(fn_adl_activity_)(adl_adapter_, &act) == 0) {
            out.gpu = std::clamp(static_cast<double>(act.activity_percent) / 100.0, 0.0, 1.0);
        }
        AdlTemperature temp = {};
        temp.size = sizeof(temp);
        if (reinterpret_cast<AdlTempGetFn>(fn_adl_temp_)(adl_adapter_, 0, &temp) == 0) {
            out.gpu_temp = static_cast<double>(temp.temperature) / 1000.0;
        }
        if (details && fn_adl_fan_) {
            AdlFanSpeedValue fan = {};
            fan.size = sizeof(fan);
            fan.speed_type = 2; // ADL_DL_FANCTRL_SPEED_TYPE_RPM
            if (reinterpret_cast<AdlFanGetFn>(fn_adl_fan_)(adl_adapter_, 0, &fan) == 0 && fan.fan_speed >= 0) {
                out.gpu_fan_rpm = static_cast<double>(fan.fan_speed);
            }
        }
    }

    if (pdh_query_ && (details || adl_adapter_ < 0)) {
        auto q = static_cast<PDH_HQUERY>(pdh_query_);
        if (PdhCollectQueryData(q) == ERROR_SUCCESS) {
            if (details && pdh_vram_counter_) {
                DWORD buf_sz = 0, item_cnt = 0;
                auto c = static_cast<PDH_HCOUNTER>(pdh_vram_counter_);
                if (PdhGetFormattedCounterArrayW(c, PDH_FMT_LARGE, &buf_sz, &item_cnt, nullptr) == static_cast<PDH_STATUS>(PDH_MORE_DATA) && buf_sz > 0) {
                    std::vector<uint8_t> raw(buf_sz);
                    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(raw.data());
                    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_LARGE, &buf_sz, &item_cnt, items) == ERROR_SUCCESS) {
                        uint64_t best = 0;
                        for (DWORD i = 0; i < item_cnt; ++i) {
                            if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.largeValue > 0) {
                                best = std::max(best, static_cast<uint64_t>(items[i].FmtValue.largeValue));
                            }
                        }
                        if (best > 0) out.vram_used = best;
                    }
                }
            }
            if (adl_adapter_ < 0 && pdh_gpu_load_counter_) {
                DWORD buf_sz = 0, item_cnt = 0;
                auto c = static_cast<PDH_HCOUNTER>(pdh_gpu_load_counter_);
                if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &buf_sz, &item_cnt, nullptr) == static_cast<PDH_STATUS>(PDH_MORE_DATA) && buf_sz > 0) {
                    std::vector<uint8_t> raw(buf_sz);
                    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(raw.data());
                    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &buf_sz, &item_cnt, items) == ERROR_SUCCESS) {
                        double sum = 0.0;
                        for (DWORD i = 0; i < item_cnt; ++i) {
                            if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.doubleValue > 0.0) {
                                sum += items[i].FmtValue.doubleValue;
                            }
                        }
                        out.gpu = std::clamp(sum / 100.0, 0.0, 1.0);
                    }
                }
            }
        }
    }
}

HwSample HardwareSampler::read_once(bool include_details)
{
    HwSample s = latest_sample();
    if (dry_run_) {
        ++dry_tick_;
        double phase = static_cast<double>(dry_tick_) * 0.35;
        s.cpu = 0.14 + 0.08 * std::sin(phase);
        s.cpu_temp = NAN;
        s.gpu = 0.18 + 0.10 * std::cos(phase * 0.8);
        s.gpu_temp = 49.0 + 2.0 * std::sin(phase * 0.5);
        s.mem_total = 16ULL * 1024ULL * 1024ULL * 1024ULL;
        s.mem_used = 7ULL * 1024ULL * 1024ULL * 1024ULL + 400ULL * 1024ULL * 1024ULL;
        s.disk_total = 451ULL * 1024ULL * 1024ULL * 1024ULL;
        s.disk_used = 344ULL * 1024ULL * 1024ULL * 1024ULL;
        if (include_details) {
            s.vram_total = info_.vram_total;
            s.vram_used = 920ULL * 1024ULL * 1024ULL;
            s.gpu_fan_rpm = 1090.0;
            s.disk_temp = 36.0;
        }
        return s;
    }

    sample_cpu(s);
    if (s.cpu < 0.0) {
        // First delta after starting: take a brief 60 ms window on the worker thread
        // so the initial reading shows a real load percentage immediately.
        Sleep(60);
        sample_cpu(s);
    }
    sample_memory(s);
    sample_gpu(s, include_details);
    sample_disk(s, include_details);
    return s;
}

void HardwareSampler::set_wanted(bool want_basic, bool want_details)
{
    bool prev = want_basic_.exchange(want_basic);
    bool prev_det = want_details_.exchange(want_details);
    if (!want_basic) {
        fresh_load_.store(true);
    } else if (!prev || (want_details && !prev_det)) {
        if (wake_event_) SetEvent(wake_event_);
    }
}

HwSample HardwareSampler::latest_sample() const
{
    std::lock_guard<std::mutex> lock(sample_mu_);
    return now_;
}

void HardwareSampler::worker_loop()
{
    while (!stop_.load()) {
        bool basic = want_basic_.load();
        if (!basic) {
            WaitForSingleObject(wake_event_, INFINITE);
            continue;
        }
        bool details = want_details_.load();
        HwSample s = read_once(details);
        {
            std::lock_guard<std::mutex> lock(sample_mu_);
            now_ = s;
        }
        HWND target = notify_hwnd_.load();
        if (target && !stop_.load()) {
            PostMessageW(target, kMsgHwSample, 0, 0);
        }
        WaitForSingleObject(wake_event_, 1000);
    }
}

#endif // _WIN32

} // namespace lares
