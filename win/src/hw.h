// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lares {

inline constexpr int kGraphSamples = 240;

struct HwInfo {
    std::string cpu_name = "Processor";
    int threads = 1;
    std::string gpu_name;     // empty when no discrete/known GPU is found
    uint64_t vram_total = 0;
    std::string disk_kind;    // "NVMe" for an NVMe system drive, empty otherwise
};

struct HwSample {
    double cpu = -1.0;
    double cpu_temp = NAN;
    double gpu = -1.0;
    double gpu_temp = NAN;
    double gpu_watts = NAN;
    double gpu_fan_rpm = NAN;
    uint64_t vram_used = 0;
    uint64_t vram_total = 0;
    uint64_t mem_used = 0;
    uint64_t mem_total = 0;
    uint64_t disk_used = 0;
    uint64_t disk_total = 0;
    double disk_temp = NAN;
};

struct GraphRing {
    float ring[kGraphSamples] = {};
    int head = 0;
    int len = 0;

    void push(double fraction);
    float latest(int age) const; // age 0 = newest, age len-1 = oldest
};

// Pure helpers shared with unit tests (mirrors src/hardware.c and src/widgets.c).
std::string format_bytes(uint64_t bytes);
std::string clean_cpu_name(const std::string& raw);
std::string clean_gpu_name(const std::string& raw);
int meter_filled_blocks(double fraction, int blocks);
int graph_step_height(float fraction, int steps);

#ifdef _WIN32
inline constexpr UINT kMsgHwSample = WM_APP + 1;

class HardwareSampler {
public:
    explicit HardwareSampler(bool dry_run);
    ~HardwareSampler();

    HardwareSampler(const HardwareSampler&) = delete;
    HardwareSampler& operator=(const HardwareSampler&) = delete;

    void set_notify_hwnd(HWND hwnd) { notify_hwnd_.store(hwnd); }
    void set_wanted(bool want_basic, bool want_details);

    const HwInfo& info() const { return info_; }
    HwSample latest_sample() const;

    // Runs one synchronous sample (for testing or initial read).
    HwSample read_once(bool include_details);

private:
    void worker_loop();
    void init_static_info();
    void init_adl();
    void shutdown_adl();
    void sample_cpu(HwSample& out);
    void sample_memory(HwSample& out);
    void sample_disk(HwSample& out, bool details);
    void sample_gpu(HwSample& out, bool details);

    bool dry_run_ = false;
    HwInfo info_;

    mutable std::mutex sample_mu_;
    HwSample now_;

    std::atomic<HWND> notify_hwnd_{ nullptr };
    std::atomic<bool> want_basic_{ false };
    std::atomic<bool> want_details_{ false };
    std::atomic<bool> fresh_load_{ true };
    std::atomic<bool> stop_{ false };
    HANDLE wake_event_ = nullptr;
    std::thread worker_;

    uint64_t last_cpu_busy_ = 0;
    uint64_t last_cpu_total_ = 0;

    // AMD Display Library (atiadlxx.dll) function pointers, used on worker thread.
    HMODULE adl_dll_ = nullptr;
    int adl_adapter_ = -1;
    void* fn_adl_destroy_ = nullptr;
    void* fn_adl_temp_ = nullptr;
    void* fn_adl_activity_ = nullptr;
    void* fn_adl_fan_ = nullptr;

    // PDH query for GPU dedicated VRAM usage when details are shown.
    void* pdh_query_ = nullptr;
    void* pdh_vram_counter_ = nullptr;
    void* pdh_gpu_load_counter_ = nullptr;
    int dry_tick_ = 0;
};
#endif

} // namespace lares
