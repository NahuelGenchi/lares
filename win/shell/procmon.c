// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The process list (issue #149), opened from the bar's readings, the
 * launcher and the dock: processor, graphics and memory at a glance, a list
 * to search and sort, and End Task behind a question that protects Windows'
 * own processes. In Files' colours, with its own title bar. Its readings run
 * only while it is open, and its memory goes back when it closes.
 */

#define PROCMON_CLASS   L"LaresProcessesWnd"
#define ID_PM_SEARCH    5001
#define TIMER_PM_TICK   5101
#define MAX_PROCS       1024

typedef struct {
    DWORD     pid;
    wchar_t   name[128];
    wchar_t   name_lower[128];
    DWORD     threads;
    uint64_t  mem_bytes;     /* PrivateUsage (fallback WorkingSetSize) */
    uint64_t  cpu_time_100ns;
    float     cpu_percent;
    bool      is_critical;
} ProcEntry;

typedef struct {
    DWORD    pid;
    uint64_t cpu_time_100ns;
} PrevCpuEntry;

static HWND     g_hPmWnd = NULL;
static HWND     g_hSearchEdit = NULL;
static WNDPROC  g_orig_search_proc = NULL;
static HFONT    g_hEditFont = NULL;
static HBRUSH   g_hEditBrush = NULL;
static bool     g_pm_destroying = false;

static ProcEntry    *g_procs = NULL;
static int           g_proc_count = 0;
static int          *g_visible = NULL;
static int           g_visible_count = 0;
static PrevCpuEntry *g_prev_cpu = NULL;
static int           g_prev_cpu_count = 0;
static ULONGLONG     g_prev_tick_ms = 0;
static int           g_num_cpus = 1;

static ProcMonSortCol g_sort_col = PROCMON_SORT_MEM;
static bool           g_sort_asc = false;
static wchar_t        g_filter[128] = {0};
static DWORD          g_selected_pid = 0;
static int            g_hover_row = -1;
static int            g_hover_cap = 0;   /* 1=min, 2=max, 3=close */
static int            g_hover_btn = 0;   /* 1=end task, 2=refresh */
static bool           g_tracking = false;
static int            g_scroll_y = 0;
static int            g_content_h = 0;
static uint64_t       g_own_priv_bytes = 0;

/* Layout rects */
static RECT g_rc_cap[3];
static RECT g_rc_search;
static RECT g_rc_end_btn;
static RECT g_rc_ref_btn;
static RECT g_rc_cols[5];
static RECT g_rc_list;

static ARGB pm_argb(BYTE a, COLORREF c) {
    return MAKE_ARGB(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void pm_str_lower(const wchar_t *src, wchar_t *dst, size_t max_len) {
    size_t i = 0;
    for (; src[i] && i + 1 < max_len; i++) dst[i] = (wchar_t)towlower(src[i]);
    dst[i] = L'\0';
}

static void pm_format_mem(uint64_t bytes, wchar_t *out, size_t len) {
    if (bytes == 0) {
        wcsncpy_s(out, len, L"--", _TRUNCATE);
    } else if (bytes < 1024ULL * 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%.0f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        _snwprintf_s(out, len, _TRUNCATE, L"%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static bool is_critical_process(DWORD pid, const wchar_t *name) {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) return true;
    static const wchar_t *crit[] = {
        L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"winlogon.exe", L"services.exe", L"lsass.exe", L"dwm.exe", L"Memory Compression"
    };
    for (int i = 0; i < (int)(sizeof(crit) / sizeof(crit[0])); i++) {
        if (_wcsicmp(name, crit[i]) == 0) return true;
    }
    return false;
}

static uint64_t find_prev_cpu(DWORD pid) {
    for (int i = 0; i < g_prev_cpu_count; i++) {
        if (g_prev_cpu[i].pid == pid) return g_prev_cpu[i].cpu_time_100ns;
    }
    return 0;
}

static int cmp_proc_vis(const void *pa, const void *pb) {
    const ProcEntry *a = &g_procs[*(const int *)pa];
    const ProcEntry *b = &g_procs[*(const int *)pb];
    int cmp = 0;
    switch (g_sort_col) {
    case PROCMON_SORT_NAME:
        cmp = _wcsicmp(a->name, b->name);
        break;
    case PROCMON_SORT_PID:
        cmp = (a->pid < b->pid) ? -1 : (a->pid > b->pid ? 1 : 0);
        break;
    case PROCMON_SORT_CPU:
        cmp = (a->cpu_percent < b->cpu_percent) ? -1 : (a->cpu_percent > b->cpu_percent ? 1 : 0);
        if (cmp == 0) cmp = (a->mem_bytes < b->mem_bytes) ? -1 : (a->mem_bytes > b->mem_bytes ? 1 : 0);
        break;
    case PROCMON_SORT_THR:
        cmp = (a->threads < b->threads) ? -1 : (a->threads > b->threads ? 1 : 0);
        break;
    case PROCMON_SORT_MEM:
    default:
        cmp = (a->mem_bytes < b->mem_bytes) ? -1 : (a->mem_bytes > b->mem_bytes ? 1 : 0);
        if (cmp == 0) cmp = _wcsicmp(a->name, b->name);
        break;
    }
    return g_sort_asc ? cmp : -cmp;
}

static void pm_rebuild_visible(void) {
    if (!g_procs || !g_visible) { g_visible_count = 0; return; }
    wchar_t qlow[128];
    pm_str_lower(g_filter, qlow, 128);
    g_visible_count = 0;

    for (int i = 0; i < g_proc_count; i++) {
        if (qlow[0]) {
            wchar_t pid_s[24];
            _snwprintf_s(pid_s, 24, _TRUNCATE, L"%lu", (unsigned long)g_procs[i].pid);
            if (!wcsstr(g_procs[i].name_lower, qlow) && !wcsstr(pid_s, qlow)) continue;
        }
        g_visible[g_visible_count++] = i;
    }

    if (g_visible_count > 1) {
        qsort(g_visible, (size_t)g_visible_count, sizeof(int), cmp_proc_vis);
    }
}

static void pm_sample_processes(void) {
    if (!g_procs) {
        g_procs = (ProcEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_PROCS * sizeof(ProcEntry));
    }
    if (!g_visible) {
        g_visible = (int *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_PROCS * sizeof(int));
    }
    if (!g_procs || !g_visible) return;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    ULONGLONG now_ms = GetTickCount64();
    ULONGLONG dt_100ns = (g_prev_tick_ms > 0 && now_ms > g_prev_tick_ms)
                       ? (now_ms - g_prev_tick_ms) * 10000ULL : 0;

    int cnt = 0;
    DWORD own_pid = GetCurrentProcessId();
    g_own_priv_bytes = 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (cnt >= MAX_PROCS) break;
            if (pe.th32ProcessID == 0) continue; /* Skip System Idle Process */

            ProcEntry *e = &g_procs[cnt++];
            ZeroMemory(e, sizeof(*e));
            e->pid = pe.th32ProcessID;
            e->threads = pe.cntThreads;
            wcsncpy_s(e->name, 128, pe.szExeFile, _TRUNCATE);
            pm_str_lower(e->name, e->name_lower, 128);
            e->is_critical = is_critical_process(e->pid, e->name);

            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e->pid);
            if (hp) {
                PROCESS_MEMORY_COUNTERS_EX pmc;
                ZeroMemory(&pmc, sizeof(pmc));
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hp, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
                    e->mem_bytes = pmc.PrivateUsage ? (uint64_t)pmc.PrivateUsage : (uint64_t)pmc.WorkingSetSize;
                    if (e->pid == own_pid) g_own_priv_bytes = e->mem_bytes;
                }
                FILETIME ft_c, ft_e, ft_k, ft_u;
                if (GetProcessTimes(hp, &ft_c, &ft_e, &ft_k, &ft_u)) {
                    ULARGE_INTEGER k, u;
                    k.LowPart = ft_k.dwLowDateTime; k.HighPart = ft_k.dwHighDateTime;
                    u.LowPart = ft_u.dwLowDateTime; u.HighPart = ft_u.dwHighDateTime;
                    e->cpu_time_100ns = k.QuadPart + u.QuadPart;
                    if (dt_100ns > 0) {
                        uint64_t prev = find_prev_cpu(e->pid);
                        if (prev > 0 && e->cpu_time_100ns >= prev) {
                            double pct = ((double)(e->cpu_time_100ns - prev) * 100.0) /
                                         ((double)dt_100ns * (double)g_num_cpus);
                            if (pct > 100.0) pct = 100.0;
                            if (pct < 0.0) pct = 0.0;
                            e->cpu_percent = (float)pct;
                        }
                    }
                }
                CloseHandle(hp);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    g_proc_count = cnt;

    /* Save CPU times for next delta */
    if (!g_prev_cpu) {
        g_prev_cpu = (PrevCpuEntry *)HeapAlloc(GetProcessHeap(), 0, MAX_PROCS * sizeof(PrevCpuEntry));
    }
    if (g_prev_cpu) {
        for (int i = 0; i < cnt; i++) {
            g_prev_cpu[i].pid = g_procs[i].pid;
            g_prev_cpu[i].cpu_time_100ns = g_procs[i].cpu_time_100ns;
        }
        g_prev_cpu_count = cnt;
    }
    g_prev_tick_ms = now_ms;

    pm_rebuild_visible();
}

static const ProcEntry *find_selected_proc(void) {
    if (g_selected_pid == 0 || !g_procs) return NULL;
    for (int i = 0; i < g_proc_count; i++) {
        if (g_procs[i].pid == g_selected_pid) return &g_procs[i];
    }
    return NULL;
}

static void pm_try_end_task(HWND hwnd) {
    const ProcEntry *sel = find_selected_proc();
    if (!sel || sel->is_critical) return;

    DWORD pid = sel->pid;
    wchar_t name_copy[128];
    wcsncpy_s(name_copy, 128, sel->name, _TRUNCATE);

    wchar_t prompt[256];
    _snwprintf_s(prompt, 256, _TRUNCATE,
                 L"End process \"%ls\" (PID %lu)?\n\nAny unsaved work in this app may be lost.",
                 name_copy, (unsigned long)pid);

    int ans = MessageBoxW(hwnd, prompt, L"End Task", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (ans != IDYES) return;

    HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hp) {
        MessageBoxW(hwnd,
                    L"Could not terminate this process. It may have already exited or require administrator permissions.",
                    L"End Task", MB_OK | MB_ICONINFORMATION);
        return;
    }
    BOOL ok = TerminateProcess(hp, 1);
    CloseHandle(hp);
    if (!ok) {
        MessageBoxW(hwnd, L"Windows declined to terminate this process.", L"End Task", MB_OK | MB_ICONWARNING);
    } else {
        g_selected_pid = 0;
        pm_sample_processes();
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static void pm_update_layout(int w, int h) {
    int cap_w = 46;
    for (int i = 0; i < 3; i++) {
        int cx = w - (3 - i) * cap_w;
        g_rc_cap[i].left = cx; g_rc_cap[i].top = 0;
        g_rc_cap[i].right = cx + cap_w; g_rc_cap[i].bottom = 38;
    }

    int act_y = 38 + 86;
    g_rc_end_btn.left = w - 110; g_rc_end_btn.top = act_y + 7;
    g_rc_end_btn.right = w - 14; g_rc_end_btn.bottom = act_y + 37;

    g_rc_ref_btn.left = g_rc_end_btn.left - 38; g_rc_ref_btn.top = act_y + 7;
    g_rc_ref_btn.right = g_rc_end_btn.left - 8; g_rc_ref_btn.bottom = act_y + 37;

    g_rc_search.left = 14; g_rc_search.top = act_y + 7;
    g_rc_search.right = g_rc_ref_btn.left - 12; g_rc_search.bottom = act_y + 37;
    if (g_rc_search.right < g_rc_search.left + 120) g_rc_search.right = g_rc_search.left + 120;

    if (g_hSearchEdit) {
        MoveWindow(g_hSearchEdit, g_rc_search.left + 28, g_rc_search.top + 6,
                   (g_rc_search.right - g_rc_search.left) - 38, 18, TRUE);
    }

    int hdr_y = act_y + 44;
    int stat_h = 26;
    g_rc_list.left = 0; g_rc_list.top = hdr_y;
    g_rc_list.right = w; g_rc_list.bottom = h - stat_h;

    int c_pid = 80, c_cpu = 90, c_mem = 122, c_thr = 84;
    int c_name = w - c_pid - c_cpu - c_mem - c_thr - 18;
    if (c_name < 160) c_name = 160;
    int cw[5] = { c_name, c_pid, c_cpu, c_mem, c_thr };
    int cx = 0;
    for (int i = 0; i < 5; i++) {
        g_rc_cols[i].left = cx; g_rc_cols[i].top = hdr_y;
        g_rc_cols[i].right = cx + cw[i]; g_rc_cols[i].bottom = hdr_y + 28;
        cx += cw[i];
    }

    g_content_h = g_visible_count * 28 + 12;
    int view_h = (g_rc_list.bottom - g_rc_list.top) - 28;
    int max_s = max(0, g_content_h - view_h);
    if (g_scroll_y > max_s) g_scroll_y = max_s;
    if (g_scroll_y < 0) g_scroll_y = 0;
}

static void draw_stat_card(GpGraphics *g, int x, int y, int w, int h, wchar_t glyph,
                           const wchar_t *title, const wchar_t *big_val, const wchar_t *sub,
                           float pct, ARGB bar_col, ARGB card_bg, ARGB bdr, ARGB txt, ARGB muted, ARGB track_bg) {
    draw_round_rect_fill(g, (float)x, (float)y, (float)w, (float)h, 10.0f, card_bg);
    draw_round_rect_stroke(g, (float)x + 0.5f, (float)y + 0.5f, (float)w - 1.0f, (float)h - 1.0f, 10.0f, bdr, 1.0f);

    draw_icon_glyph(g, glyph, 14.0f, bar_col, (float)(x + 12), (float)(y + 8), 20.0f, 20.0f);
    draw_text(g, title, L"Segoe UI", 11.5f, GP_FONT_BOLD, muted,
              (float)(x + 34), (float)(y + 8), 90.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
    draw_text(g, big_val, L"Segoe UI", 16.0f, GP_FONT_BOLD, txt,
              (float)(x + w - 120), (float)(y + 6), 108.0f, 24.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, false);

    draw_text(g, sub, L"Segoe UI", 11.0f, GP_FONT_REGULAR, muted,
              (float)(x + 12), (float)(y + 32), (float)(w - 24), 16.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

    float bw = (float)(w - 24);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    draw_round_rect_fill(g, (float)(x + 12), (float)(y + h - 14), bw, 5.0f, 2.5f, track_bg);
    if (pct > 0.01f) {
        draw_round_rect_fill(g, (float)(x + 12), (float)(y + h - 14), bw * pct, 5.0f, 2.5f, bar_col);
    }
}

/* The graphics card's name as Windows shows it, read once. */
static const wchar_t *pm_gpu_name(void) {
    static wchar_t name[128];
    static bool read = false;
    if (!read) {
        read = true;
        wcsncpy_s(name, 128, L"Graphics card", _TRUNCATE);
        DISPLAY_DEVICEW dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); ++i) {
            if ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) && dd.DeviceString[0]) {
                wcsncpy_s(name, 128, dd.DeviceString, _TRUNCATE);
                break;
            }
            ZeroMemory(&dd, sizeof(dd));
            dd.cb = sizeof(dd);
        }
    }
    return name;
}

static void pm_draw(LayerSurface *fb, int w, int h) {
    pm_update_layout(w, h);
    {
        GpGraphics *g = fb->g;
        GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
        GdipSetTextRenderingHint(g, GP_TEXT_ANTIALIAS);

        ARGB c_sb    = pm_argb(255, g_theme.files_sidebar);
        ARGB c_tb    = pm_argb(255, g_theme.files_toolbar);
        ARGB c_addr  = pm_argb(255, g_theme.files_addrbar);
        ARGB c_bg    = pm_argb(255, g_theme.files_bg);
        ARGB c_alt   = pm_argb(255, g_theme.files_alt);
        ARGB c_bdr   = pm_argb(255, g_theme.files_border);
        ARGB c_txt   = pm_argb(255, g_theme.files_text);
        ARGB c_muted = pm_argb(255, g_theme.files_text_muted);
        ARGB c_sel   = pm_argb(255, g_theme.files_sel_bg);
        ARGB c_hov   = pm_argb(255, g_theme.files_hover_bg);

        /* 1. A 38 px title bar of its own */
        draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, 38.0f, 0.0f, c_sb);
        draw_line(g, 0.0f, 37.5f, (float)w, 37.5f, c_bdr, 1.0f);
        draw_icon_glyph(g, 0xE9D9, 15.0f, g_theme.accent, 14.0f, 0.0f, 22.0f, 38.0f);
        draw_text(g, L"Processes", L"Segoe UI", 13.0f, GP_FONT_BOLD, c_txt,
                  40.0f, 0.0f, 240.0f, 38.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        bool zoomed = g_hPmWnd && IsZoomed(g_hPmWnd) != 0;
        wchar_t cglyphs[3] = { 0xE921, zoomed ? 0xE923 : 0xE922, 0xE8BB };
        for (int c = 0; c < 3; c++) {
            const RECT *r = &g_rc_cap[c];
            bool hov = (g_hover_cap == c + 1);
            if (hov) {
                draw_round_rect_fill(g, (float)r->left, (float)r->top,
                                     (float)(r->right - r->left), (float)(r->bottom - r->top), 0.0f,
                                     (c == 2) ? MAKE_ARGB(255, 232, 17, 35) : c_hov);
            }
            draw_icon_glyph(g, cglyphs[c], 10.0f, (hov && c == 2) ? MAKE_ARGB(255, 255, 255, 255) : c_txt,
                            (float)r->left, (float)r->top, (float)(r->right - r->left), (float)(r->bottom - r->top));
        }

        /* 2. Summary Stats Strip (86px) */
        draw_round_rect_fill(g, 0.0f, 38.0f, (float)w, 86.0f, 0.0f, c_tb);
        int gap = 12;
        int cw = (w - 28 - gap * 2) / 3;
        int cy = 48;
        int ch = 66;

        wchar_t cpu_v[24], cpu_s[64], gpu_v[24], ram_v[32], ram_s[64];
        _snwprintf_s(cpu_v, 24, _TRUNCATE, L"%d%%", g_telemetry.cpu_percent);
        _snwprintf_s(cpu_s, 64, _TRUNCATE, L"%d running processes (%d logical cores)", g_proc_count, g_num_cpus);
        _snwprintf_s(gpu_v, 24, _TRUNCATE, L"%d%%", g_telemetry.gpu_percent);

        double ugb = (double)g_telemetry.ram_used_mb / 1024.0;
        double tgb = (double)max(1ULL, g_telemetry.ram_total_mb) / 1024.0;
        float rpct = (float)(ugb / tgb);
        _snwprintf_s(ram_v, 32, _TRUNCATE, L"%.1f / %.0f GB", ugb, tgb);
        wchar_t own_s[32];
        pm_format_mem(g_own_priv_bytes, own_s, 32);
        _snwprintf_s(ram_s, 64, _TRUNCATE, L"%.0f%% used  |  Lares' shell: %ls", rpct * 100.0f, own_s);

        draw_stat_card(g, 14, cy, cw, ch, 0xE950, L"CPU", cpu_v, cpu_s,
                       (float)g_telemetry.cpu_percent / 100.0f, g_theme.accent, c_bg, c_bdr, c_txt, c_muted, c_addr);
        draw_stat_card(g, 14 + cw + gap, cy, cw, ch, 0xE7F4, L"GPU (3D)", gpu_v, pm_gpu_name(),
                       (float)g_telemetry.gpu_percent / 100.0f, MAKE_ARGB(255, 168, 120, 245), c_bg, c_bdr, c_txt, c_muted, c_addr);
        draw_stat_card(g, 14 + (cw + gap) * 2, cy, cw, ch, 0xE964, L"Memory", ram_v, ram_s,
                       rpct, rpct > 0.85f ? g_theme.danger : g_theme.success, c_bg, c_bdr, c_txt, c_muted, c_addr);

        /* 3. Search & Action Bar (44px) */
        int act_y = 124;
        draw_round_rect_fill(g, 0.0f, (float)act_y, (float)w, 44.0f, 0.0f, c_tb);
        draw_line(g, 0.0f, (float)(act_y + 43.5f), (float)w, (float)(act_y + 43.5f), c_bdr, 1.0f);

        draw_round_rect_fill(g, (float)g_rc_search.left, (float)g_rc_search.top,
                             (float)(g_rc_search.right - g_rc_search.left), 30.0f, 8.0f, c_addr);
        draw_round_rect_stroke(g, (float)g_rc_search.left + 0.5f, (float)g_rc_search.top + 0.5f,
                               (float)(g_rc_search.right - g_rc_search.left) - 1.0f, 29.0f, 8.0f, c_bdr, 1.0f);
        draw_icon_glyph(g, 0xE721, 12.0f, c_muted, (float)(g_rc_search.left + 7), (float)g_rc_search.top, 18.0f, 30.0f);

        /* Refresh button */
        draw_round_rect_fill(g, (float)g_rc_ref_btn.left, (float)g_rc_ref_btn.top, 30.0f, 30.0f, 7.0f,
                             g_hover_btn == 2 ? c_hov : c_addr);
        draw_icon_glyph(g, 0xE72C, 13.0f, c_txt, (float)g_rc_ref_btn.left, (float)g_rc_ref_btn.top, 30.0f, 30.0f);

        /* Safe End Task button */
        const ProcEntry *sel = find_selected_proc();
        bool can_end = (sel != NULL && !sel->is_critical);
        ARGB end_bg = can_end ? (g_hover_btn == 1 ? MAKE_ARGB(255, 224, 62, 62) : g_theme.danger) : c_addr;
        ARGB end_fg = can_end ? MAKE_ARGB(255, 255, 255, 255) : c_muted;
        draw_round_rect_fill(g, (float)g_rc_end_btn.left, (float)g_rc_end_btn.top,
                             (float)(g_rc_end_btn.right - g_rc_end_btn.left), 30.0f, 7.0f, end_bg);
        draw_text(g, L"End Task", L"Segoe UI", 12.0f, GP_FONT_BOLD, end_fg,
                  (float)g_rc_end_btn.left, (float)g_rc_end_btn.top,
                  (float)(g_rc_end_btn.right - g_rc_end_btn.left), 30.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

        /* 4. Process Table Header & Virtualized Rows */
        int list_y = g_rc_list.top;
        int list_h = g_rc_list.bottom - g_rc_list.top;
        draw_round_rect_fill(g, 0.0f, (float)list_y, (float)w, (float)list_h, 0.0f, c_bg);
        draw_round_rect_fill(g, 0.0f, (float)list_y, (float)w, 28.0f, 0.0f, c_alt);
        draw_line(g, 0.0f, (float)(list_y + 27.5f), (float)w, (float)(list_y + 27.5f), c_bdr, 1.0f);

        static const wchar_t *col_lbls[5] = { L"Process Name", L"PID", L"CPU", L"Memory", L"Threads" };
        for (int c = 0; c < 5; c++) {
            const RECT *hr = &g_rc_cols[c];
            bool sorted = (g_sort_col == (ProcMonSortCol)c);
            int align = (c == 0) ? GP_ALIGN_NEAR : GP_ALIGN_FAR;
            draw_text(g, col_lbls[c], L"Segoe UI", 11.5f, GP_FONT_BOLD, sorted ? c_txt : c_muted,
                      (float)(hr->left + 12), (float)hr->top, (float)(hr->right - hr->left - 24), 28.0f,
                      align, GP_ALIGN_CENTER, false);
            if (sorted) {
                float tw = measure_text_width(g, col_lbls[c], L"Segoe UI", 11.5f, GP_FONT_BOLD);
                float chx = (c == 0) ? (float)(hr->left + 14 + (int)tw) : (float)(hr->right - 14);
                draw_icon_glyph(g, g_sort_asc ? 0xE70E : 0xE70D, 8.5f, c_muted, chx, (float)hr->top, 14.0f, 28.0f);
            }
        }

        int rows_top = list_y + 28;
        int rows_h = g_rc_list.bottom - rows_top;
        GdipSetClipRect(g, 0.0f, (float)rows_top, (float)w, (float)rows_h, GP_COMBINE_REPLACE);

        int row_h = 28;
        int first = max(0, g_scroll_y / row_h);
        int vis_n = (rows_h / row_h) + 2;
        int last  = min(g_visible_count, first + vis_n);

        for (int v = first; v < last; v++) {
            const ProcEntry *e = &g_procs[g_visible[v]];
            int ry = rows_top + v * row_h - g_scroll_y;
            bool is_sel = (e->pid == g_selected_pid);

            if (is_sel) {
                draw_round_rect_fill(g, 6.0f, (float)(ry + 1), (float)(w - 18), (float)(row_h - 2), 6.0f, c_sel);
            } else if (v == g_hover_row) {
                draw_round_rect_fill(g, 6.0f, (float)(ry + 1), (float)(w - 18), (float)(row_h - 2), 6.0f, c_hov);
            } else if ((v & 1) == 1) {
                draw_round_rect_fill(g, 6.0f, (float)ry, (float)(w - 18), (float)row_h, 0.0f, c_alt);
            }

            draw_icon_glyph(g, e->is_critical ? 0xE7EF : 0xE756, 12.5f,
                            e->is_critical ? c_muted : g_theme.accent,
                            14.0f, (float)ry, 18.0f, (float)row_h);
            draw_text(g, e->name, L"Segoe UI", 12.0f, is_sel ? GP_FONT_BOLD : GP_FONT_REGULAR, c_txt,
                      36.0f, (float)ry, (float)(g_rc_cols[0].right - 44), (float)row_h,
                      GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

            wchar_t pid_s[24], rcpu_s[24], mem_s[32], thr_s[24];
            _snwprintf_s(pid_s, 24, _TRUNCATE, L"%lu", (unsigned long)e->pid);
            _snwprintf_s(rcpu_s, 24, _TRUNCATE, L"%.1f%%", (double)e->cpu_percent);
            pm_format_mem(e->mem_bytes, mem_s, 32);
            _snwprintf_s(thr_s, 24, _TRUNCATE, L"%lu", (unsigned long)e->threads);

            draw_text(g, pid_s, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                      (float)g_rc_cols[1].left, (float)ry, (float)(g_rc_cols[1].right - g_rc_cols[1].left - 12),
                      (float)row_h, GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
            ARGB cpu_c = (e->cpu_percent >= 15.0f) ? g_theme.danger : (e->cpu_percent >= 2.0f ? c_txt : c_muted);
            draw_text(g, rcpu_s, L"Segoe UI", 11.5f, e->cpu_percent >= 2.0f ? GP_FONT_BOLD : GP_FONT_REGULAR, cpu_c,
                      (float)g_rc_cols[2].left, (float)ry, (float)(g_rc_cols[2].right - g_rc_cols[2].left - 12),
                      (float)row_h, GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
            ARGB mem_c = (e->mem_bytes >= 300ULL * 1024ULL * 1024ULL) ? g_theme.danger : c_txt;
            draw_text(g, mem_s, L"Segoe UI", 11.5f, GP_FONT_REGULAR, mem_c,
                      (float)g_rc_cols[3].left, (float)ry, (float)(g_rc_cols[3].right - g_rc_cols[3].left - 12),
                      (float)row_h, GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
            draw_text(g, thr_s, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                      (float)g_rc_cols[4].left, (float)ry, (float)(g_rc_cols[4].right - g_rc_cols[4].left - 14),
                      (float)row_h, GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
        }

        /* Slim scrollbar */
        if (g_content_h > rows_h && rows_h > 20) {
            float ratio = (float)rows_h / (float)g_content_h;
            float th = max(28.0f, (float)rows_h * ratio);
            float max_s = (float)(g_content_h - rows_h);
            float ty = (float)rows_top + ((float)g_scroll_y / max_s) * ((float)rows_h - th);
            draw_round_rect_fill(g, (float)(w - 8), ty, 5.0f, th, 2.5f, c_bdr);
        }
        GdipResetClip(g);

        /* 5. Status Bar */
        int sty = h - 26;
        draw_round_rect_fill(g, 0.0f, (float)sty, (float)w, 26.0f, 0.0f, c_tb);
        draw_line(g, 0.0f, (float)sty + 0.5f, (float)w, (float)sty + 0.5f, c_bdr, 1.0f);
        wchar_t st[160];
        if (sel) {
            wchar_t ms[32]; pm_format_mem(sel->mem_bytes, ms, 32);
            _snwprintf_s(st, 160, _TRUNCATE, L"%d processes shown  |  Selected: %ls (PID %lu, %ls)%ls",
                         g_visible_count, sel->name, (unsigned long)sel->pid, ms,
                         sel->is_critical ? L" [Protected]" : L"");
        } else {
            _snwprintf_s(st, 160, _TRUNCATE, L"%d processes shown  |  Click a column header to sort  |  Del to end task",
                         g_visible_count);
        }
        draw_text(g, st, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                  14.0f, (float)sty, (float)(w - 28), 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
    }
}

static void pm_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        pm_draw(&fb, cr.right, cr.bottom);
        opaque_frame_end(hdc, &fb);
    }
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK pm_search_sub_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_pm_destroying || !g_orig_search_proc) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        SetWindowTextW(hwnd, L"");
        if (g_hPmWnd) SetFocus(g_hPmWnd);
        return 0;
    }
    return CallWindowProcW(g_orig_search_proc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK procmon_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            NCCALCSIZE_PARAMS *np = (NCCALCSIZE_PARAMS *)lParam;
            if (IsZoomed(hwnd)) {
                int pad = GetSystemMetrics(SM_CXPADDEDBORDER);
                int fx = GetSystemMetrics(SM_CXSIZEFRAME) + pad;
                int fy = GetSystemMetrics(SM_CYSIZEFRAME) + pad;
                np->rgrc[0].left += fx; np->rgrc[0].top += fy;
                np->rgrc[0].right -= fx; np->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        return 0;

    case WM_NCHITTEST: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        RECT wr;
        GetWindowRect(hwnd, &wr);
        if (!IsZoomed(hwnd)) {
            const int b = 6;
            bool l = pt.x < wr.left + b, r = pt.x >= wr.right - b;
            bool t = pt.y < wr.top + b, bt = pt.y >= wr.bottom - b;
            if (t && l) return HTTOPLEFT;
            if (t && r) return HTTOPRIGHT;
            if (bt && l) return HTBOTTOMLEFT;
            if (bt && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (t) return HTTOP;
            if (bt) return HTBOTTOM;
        }
        POINT cl = pt;
        ScreenToClient(hwnd, &cl);
        if (cl.y >= 0 && cl.y < 38) {
            for (int c = 0; c < 3; c++) {
                if (PtInRect(&g_rc_cap[c], cl)) return HTCLIENT;
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_TIMER:
        if (wParam == TIMER_PM_TICK) {
            pm_sample_processes();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_PM_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            GetWindowTextW(g_hSearchEdit, g_filter, 128);
            pm_rebuild_visible();
            g_scroll_y = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_theme.files_text);
        SetBkColor(hdc, g_theme.files_addrbar);
        return (LRESULT)g_hEditBrush;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        short d = GET_WHEEL_DELTA_WPARAM(wParam);
        g_scroll_y -= (d / 120) * 56;
        if (g_scroll_y < 0) g_scroll_y = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT pt = { mx, my };

        int ncap = 0;
        for (int c = 0; c < 3; c++) if (PtInRect(&g_rc_cap[c], pt)) { ncap = c + 1; break; }
        int nbtn = 0;
        if (PtInRect(&g_rc_end_btn, pt)) nbtn = 1;
        else if (PtInRect(&g_rc_ref_btn, pt)) nbtn = 2;

        int nrow = -1;
        int rows_top = g_rc_list.top + 28;
        if (my >= rows_top && my < g_rc_list.bottom) {
            int idx = (my - rows_top + g_scroll_y) / 28;
            if (idx >= 0 && idx < g_visible_count) nrow = idx;
        }

        if (ncap != g_hover_cap || nbtn != g_hover_btn || nrow != g_hover_row) {
            g_hover_cap = ncap; g_hover_btn = nbtn; g_hover_row = nrow;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = false;
        g_hover_cap = 0; g_hover_btn = 0; g_hover_row = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT pt = { mx, my };
        SetFocus(hwnd);

        for (int c = 0; c < 3; c++) {
            if (PtInRect(&g_rc_cap[c], pt)) {
                if (c == 0) ShowWindow(hwnd, SW_MINIMIZE);
                else if (c == 1) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                else PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
        }
        if (PtInRect(&g_rc_end_btn, pt)) {
            pm_try_end_task(hwnd);
            return 0;
        }
        if (PtInRect(&g_rc_ref_btn, pt)) {
            pm_sample_processes();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        for (int c = 0; c < 5; c++) {
            if (PtInRect(&g_rc_cols[c], pt)) {
                if (g_sort_col == (ProcMonSortCol)c) g_sort_asc = !g_sort_asc;
                else {
                    g_sort_col = (ProcMonSortCol)c;
                    g_sort_asc = (c == PROCMON_SORT_NAME || c == PROCMON_SORT_PID);
                }
                pm_rebuild_visible();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }

        int rows_top = g_rc_list.top + 28;
        if (my >= rows_top && my < g_rc_list.bottom) {
            int idx = (my - rows_top + g_scroll_y) / 28;
            if (idx >= 0 && idx < g_visible_count) {
                g_selected_pid = g_procs[g_visible[idx]].pid;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (wParam == VK_F5) { pm_sample_processes(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wParam == VK_DELETE) { pm_try_end_task(hwnd); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'F') {
            if (g_hSearchEdit) SetFocus(g_hSearchEdit);
            return 0;
        }
        if ((wParam == VK_DOWN || wParam == VK_UP) && g_visible_count > 0) {
            int cur = -1;
            for (int v = 0; v < g_visible_count; v++) {
                if (g_procs[g_visible[v]].pid == g_selected_pid) { cur = v; break; }
            }
            cur = (wParam == VK_DOWN) ? min(g_visible_count - 1, cur + 1) : max(0, cur - 1);
            g_selected_pid = g_procs[g_visible[cur]].pid;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        pm_paint(hwnd);
        return 0;

    case WM_DESTROY:
        g_pm_destroying = true;
        KillTimer(hwnd, TIMER_PM_TICK);
        if (g_hSearchEdit && g_orig_search_proc) {
            SetWindowLongPtrW(g_hSearchEdit, GWLP_WNDPROC, (LONG_PTR)g_orig_search_proc);
            g_orig_search_proc = NULL;
        }
        g_hSearchEdit = NULL;
        g_hPmWnd = NULL;
        if (g_procs)    { HeapFree(GetProcessHeap(), 0, g_procs);    g_procs = NULL; }
        if (g_visible)  { HeapFree(GetProcessHeap(), 0, g_visible);  g_visible = NULL; }
        if (g_prev_cpu) { HeapFree(GetProcessHeap(), 0, g_prev_cpu); g_prev_cpu = NULL; }
        g_proc_count = 0; g_visible_count = 0; g_prev_cpu_count = 0;
        g_prev_tick_ms = 0; g_selected_pid = 0; g_filter[0] = L'\0';
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
        if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_WINDOWS_CHANGED, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void refresh_pm_brush(void) {
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
    g_hEditBrush = CreateSolidBrush(g_theme.files_addrbar);
}

void procmon_init(HINSTANCE hInst) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_num_cpus = max(1, (int)si.dwNumberOfProcessors);

    g_hEditFont = render_create_ui_font(13, false);
    refresh_pm_brush();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = procmon_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = PROCMON_CLASS;
    RegisterClassExW(&wc);
}

void procmon_open(ProcMonSortCol initial_sort) {
    g_sort_col = initial_sort;
    g_sort_asc = (initial_sort == PROCMON_SORT_NAME || initial_sort == PROCMON_SORT_PID);

    if (g_hPmWnd && IsWindow(g_hPmWnd)) {
        if (IsIconic(g_hPmWnd)) ShowWindow(g_hPmWnd, SW_RESTORE);
        SetForegroundWindow(g_hPmWnd);
        pm_rebuild_visible();
        InvalidateRect(g_hPmWnd, NULL, FALSE);
        return;
    }

    g_pm_destroying = false;
    g_filter[0] = L'\0';
    g_scroll_y = 0;
    g_hover_row = -1; g_hover_cap = 0; g_hover_btn = 0;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = min(880, sw - 80);
    int wh = min(600, sh - 100);

    g_hPmWnd = CreateWindowExW(
        WS_EX_APPWINDOW, PROCMON_CLASS, L"Processes",
        WS_OVERLAPPEDWINDOW,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        NULL, NULL, g_hInst, NULL);
    if (!g_hPmWnd) return;

    BOOL dark = g_theme.is_dark ? TRUE : FALSE;
    DWORD corner = 2;
    MARGINS m = { 0, 0, 1, 0 };
    DwmSetWindowAttribute(g_hPmWnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_hPmWnd, 33, &corner, sizeof(corner));
    DwmExtendFrameIntoClientArea(g_hPmWnd, &m);
    SetWindowPos(g_hPmWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    g_hSearchEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    0, 0, 200, 18, g_hPmWnd, (HMENU)(INT_PTR)ID_PM_SEARCH, g_hInst, NULL);
    SendMessageW(g_hSearchEdit, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);
    SendMessageW(g_hSearchEdit, 0x1501 /* EM_SETCUEBANNER */, (WPARAM)TRUE,
                 (LPARAM)L"Filter processes by name or PID...");
    g_orig_search_proc = (WNDPROC)SetWindowLongPtrW(g_hSearchEdit, GWLP_WNDPROC, (LONG_PTR)pm_search_sub_proc);

    pm_sample_processes();
    /* A test copy draws the list and shows nothing. */
    if (g_cfg.isolated) return;
    SetTimer(g_hPmWnd, TIMER_PM_TICK, 1500, NULL);

    ShowWindow(g_hPmWnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hPmWnd);
    if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_WINDOWS_CHANGED, 0, 0);
}

void procmon_on_theme_changed(void) {
    refresh_pm_brush();
    if (g_hPmWnd && IsWindow(g_hPmWnd)) {
        BOOL dark = g_theme.is_dark ? TRUE : FALSE;
        DwmSetWindowAttribute(g_hPmWnd, 20, &dark, sizeof(dark));
        InvalidateRect(g_hPmWnd, NULL, TRUE);
    }
}

int procmon_snapshot(const wchar_t *png_path) {
    if (!g_hPmWnd) return 0;
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    int ok = 0;
    if (opaque_frame_begin(&fb, 880, 600)) {
        pm_draw(&fb, 880, 600);
        ok = surface_save_png(&fb, png_path);
        surface_destroy(&fb);
    }
    return ok;
}

bool procmon_is_open(void) {
    return (g_hPmWnd != NULL && IsWindow(g_hPmWnd));
}

void procmon_shutdown(void) {
    if (g_hPmWnd && IsWindow(g_hPmWnd)) {
        DestroyWindow(g_hPmWnd);
        g_hPmWnd = NULL;
    }
    if (g_hEditFont)  { DeleteObject(g_hEditFont);  g_hEditFont = NULL; }
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
}
