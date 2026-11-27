// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The bar's readings (issue #145): processor, graphics and memory load,
 * volume, network, Bluetooth, keyboard layout, Tailscale. Nothing scans:
 * Bluetooth and Wi-Fi are read from what Windows already knows. */
#include "shell.h"
#include "programs.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <bluetoothapis.h>
#pragma warning(pop)
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winsvc.h>
#include <shellapi.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>

#define TS_SERVICE_NAME L"Tailscale"

/* CoreAudio GUIDs defined cleanly for pure C */
static const GUID LARES_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID LARES_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID LARES_IID_IAudioEndpointVolume =
    { 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };

static HWND g_tel_notify = NULL;
static ULARGE_INTEGER g_prev_idle = {0};
static ULARGE_INTEGER g_prev_kern = {0};
static ULARGE_INTEGER g_prev_user = {0};
static bool g_cpu_primed = false;

static PDH_HQUERY g_gpu_query = NULL;
static PDH_HCOUNTER g_gpu_counter = NULL;
static bool g_gpu_ready = false;

static IMMDeviceEnumerator *g_audio_enum = NULL;
static IAudioEndpointVolume *g_audio_vol = NULL;
static CRITICAL_SECTION g_ts_cs;
static volatile LONG g_ts_busy = 0;

/* Shared Tailscale peer/exit-node list for Network popup */
typedef struct {
    wchar_t name[96];
    wchar_t ip[64];
    bool    online;
    bool    can_exit_node;
    bool    is_active_exit_node;
} TailscalePeer;

#define LARES_MAX_TS_PEERS 32
static TailscalePeer g_ts_peers[LARES_MAX_TS_PEERS];
static int g_ts_peer_list_count = 0;

int telemetry_get_ts_peers(void *out_buf, int max_peers) {
    EnterCriticalSection(&g_ts_cs);
    int n = g_ts_peer_list_count;
    if (n > max_peers) n = max_peers;
    if (n > 0 && out_buf) {
        memcpy(out_buf, g_ts_peers, (size_t)n * sizeof(TailscalePeer));
    }
    LeaveCriticalSection(&g_ts_cs);
    return n;
}

static void audio_ensure(void) {
    if (g_audio_vol) return;
    if (!g_audio_enum) {
        CoCreateInstance(&LARES_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                         &LARES_IID_IMMDeviceEnumerator, (void **)&g_audio_enum);
    }
    if (g_audio_enum) {
        IMMDevice *dev = NULL;
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_audio_enum, eRender, eConsole, &dev)) && dev) {
            IMMDevice_Activate(dev, &LARES_IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&g_audio_vol);
            IMMDevice_Release(dev);
        }
    }
}

static void audio_poll(void) {
    audio_ensure();
    if (!g_audio_vol) return;
    float vol = 0.0f;
    BOOL muted = FALSE;
    if (SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(g_audio_vol, &vol))) {
        int pct = (int)(vol * 100.0f + 0.5f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        g_telemetry.volume_percent = pct;
    } else {
        /* Audio endpoint may have changed (e.g., Bluetooth headphones switched) */
        IAudioEndpointVolume_Release(g_audio_vol);
        g_audio_vol = NULL;
    }
    if (g_audio_vol && SUCCEEDED(IAudioEndpointVolume_GetMute(g_audio_vol, &muted))) {
        g_telemetry.volume_muted = (muted != FALSE);
    }
}

void telemetry_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    audio_ensure();
    if (g_audio_vol) {
        IAudioEndpointVolume_SetMasterVolumeLevelScalar(g_audio_vol, (float)percent / 100.0f, NULL);
        if (percent > 0 && g_telemetry.volume_muted) {
            IAudioEndpointVolume_SetMute(g_audio_vol, FALSE, NULL);
        }
    }
    audio_poll();
    if (g_tel_notify) PostMessageW(g_tel_notify, WM_LARES_TELEMETRY_UPDATED, 0, 0);
}

void telemetry_toggle_mute(void) {
    audio_ensure();
    if (g_audio_vol) {
        BOOL muted = FALSE;
        IAudioEndpointVolume_GetMute(g_audio_vol, &muted);
        IAudioEndpointVolume_SetMute(g_audio_vol, !muted, NULL);
    }
    audio_poll();
    if (g_tel_notify) PostMessageW(g_tel_notify, WM_LARES_TELEMETRY_UPDATED, 0, 0);
}

void telemetry_step_volume(int delta) {
    telemetry_set_volume(g_telemetry.volume_percent + delta);
}

static void poll_cpu_ram(void) {
    FILETIME ft_idle, ft_kern, ft_user;
    if (GetSystemTimes(&ft_idle, &ft_kern, &ft_user)) {
        ULARGE_INTEGER i, k, u;
        i.LowPart = ft_idle.dwLowDateTime; i.HighPart = ft_idle.dwHighDateTime;
        k.LowPart = ft_kern.dwLowDateTime; k.HighPart = ft_kern.dwHighDateTime;
        u.LowPart = ft_user.dwLowDateTime; u.HighPart = ft_user.dwHighDateTime;
        if (g_cpu_primed) {
            ULONGLONG di = i.QuadPart - g_prev_idle.QuadPart;
            ULONGLONG dk = k.QuadPart - g_prev_kern.QuadPart;
            ULONGLONG du = u.QuadPart - g_prev_user.QuadPart;
            ULONGLONG tot = dk + du;
            if (tot > 0) {
                int pct = (int)(((tot - di) * 100ULL) / tot);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                g_telemetry.cpu_percent = pct;
            }
        }
        g_prev_idle = i; g_prev_kern = k; g_prev_user = u;
        g_cpu_primed = true;
    }

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        g_telemetry.ram_total_mb = (uint64_t)(ms.ullTotalPhys / (1024ULL * 1024ULL));
        g_telemetry.ram_used_mb  = (uint64_t)((ms.ullTotalPhys - ms.ullAvailPhys) / (1024ULL * 1024ULL));
    }
}

static void poll_gpu(void) {
    if (!g_gpu_ready) {
        if (PdhOpenQueryW(NULL, 0, &g_gpu_query) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(g_gpu_query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                                      0, &g_gpu_counter) == ERROR_SUCCESS) {
                PdhCollectQueryData(g_gpu_query);
                g_gpu_ready = true;
            } else {
                PdhCloseQuery(g_gpu_query);
                g_gpu_query = NULL;
            }
        }
        return;
    }
    if (PdhCollectQueryData(g_gpu_query) != ERROR_SUCCESS) return;

    DWORD buf_sz = 0, item_cnt = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(g_gpu_counter, PDH_FMT_DOUBLE, &buf_sz, &item_cnt, NULL);
    if (st == (PDH_STATUS)PDH_MORE_DATA && buf_sz > 0 && buf_sz < 512 * 1024) {
        PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)HeapAlloc(GetProcessHeap(), 0, buf_sz);
        if (items) {
            if (PdhGetFormattedCounterArrayW(g_gpu_counter, PDH_FMT_DOUBLE, &buf_sz, &item_cnt, items) == ERROR_SUCCESS) {
                double sum = 0.0;
                for (DWORD i = 0; i < item_cnt; i++) {
                    if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.doubleValue > 0.0) {
                        sum += items[i].FmtValue.doubleValue;
                    }
                }
                int pct = (int)(sum + 0.5);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                g_telemetry.gpu_percent = pct;
            }
            HeapFree(GetProcessHeap(), 0, items);
        }
    }
}

static void poll_keyboard(void) {
    HWND fg = GetForegroundWindow();
    DWORD tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    HKL hkl = GetKeyboardLayout(tid);
    g_telemetry.kbd_hkl = hkl;

    LANGID lang = LOWORD((UINT_PTR)hkl);
    LCID lcid = MAKELCID(lang, SORT_DEFAULT);

    wchar_t iso[16] = {0};
    if (GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, iso, 16) > 0) {
        for (int i = 0; iso[i]; i++) iso[i] = (wchar_t)towupper(iso[i]);
        wcsncpy_s(g_telemetry.kbd_short, 16, iso, _TRUNCATE);
    } else {
        wcsncpy_s(g_telemetry.kbd_short, 16, L"EN", _TRUNCATE);
    }
    if (GetLocaleInfoW(lcid, LOCALE_SLOCALIZEDDISPLAYNAME, g_telemetry.kbd_full, 128) == 0) {
        wcsncpy_s(g_telemetry.kbd_full, 128, g_telemetry.kbd_short, _TRUNCATE);
    }
}

static void poll_network(void) {
    g_telemetry.net_connected = false;
    g_telemetry.net_is_wifi = false;
    g_telemetry.wifi_signal = 0;
    g_telemetry.net_name[0] = L'\0';
    g_telemetry.net_ipv4[0] = L'\0';

    ULONG sz = 32768;
    IP_ADAPTER_ADDRESSES *buf = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, sz);
    if (!buf) return;

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, buf, &sz) == ERROR_SUCCESS) {
        for (IP_ADAPTER_ADDRESSES *a = buf; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
            if (a->FriendlyName && (wcsstr(a->FriendlyName, L"vEthernet") || wcsstr(a->FriendlyName, L"Tailscale"))) {
                continue;
            }
            if (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211) {
                g_telemetry.net_connected = true;
                g_telemetry.net_is_wifi = (a->IfType == IF_TYPE_IEEE80211);
                if (a->FriendlyName) {
                    wcsncpy_s(g_telemetry.net_name, 128, a->FriendlyName, _TRUNCATE);
                }
                for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                    if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                        struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
                        InetNtopW(AF_INET, &sin->sin_addr, g_telemetry.net_ipv4, 64);
                        break;
                    }
                }
                if (!g_telemetry.net_is_wifi) break; /* Prefer active wired Ethernet if both up */
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);

    /* If Wi-Fi is active, grab signal strength cleanly (zero scan) */
    if (g_telemetry.net_is_wifi) {
        HANDLE hWlan = NULL;
        DWORD ver = 0;
        if (WlanOpenHandle(2, NULL, &ver, &hWlan) == ERROR_SUCCESS && hWlan) {
            PWLAN_INTERFACE_INFO_LIST list = NULL;
            if (WlanEnumInterfaces(hWlan, NULL, &list) == ERROR_SUCCESS && list) {
                for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
                    if (list->InterfaceInfo[i].isState == wlan_interface_state_connected) {
                        PWLAN_CONNECTION_ATTRIBUTES attr = NULL;
                        DWORD cbsz = sizeof(WLAN_CONNECTION_ATTRIBUTES);
                        if (WlanQueryInterface(hWlan, &list->InterfaceInfo[i].InterfaceGuid,
                                               wlan_intf_opcode_current_connection, NULL, &cbsz,
                                               (PVOID *)&attr, NULL) == ERROR_SUCCESS && attr) {
                            g_telemetry.wifi_signal = (int)attr->wlanAssociationAttributes.wlanSignalQuality;
                            if (attr->wlanAssociationAttributes.dot11Ssid.uSSIDLength > 0) {
                                MultiByteToWideChar(CP_UTF8, 0,
                                    (const char *)attr->wlanAssociationAttributes.dot11Ssid.ucSSID,
                                    (int)attr->wlanAssociationAttributes.dot11Ssid.uSSIDLength,
                                    g_telemetry.net_name, 127);
                                g_telemetry.net_name[attr->wlanAssociationAttributes.dot11Ssid.uSSIDLength] = L'\0';
                            }
                            WlanFreeMemory(attr);
                        }
                        break;
                    }
                }
                WlanFreeMemory(list);
            }
            WlanCloseHandle(hWlan, NULL);
        }
    }
}

/*
 * Bluetooth from what Windows already knows (fIssueInquiry = FALSE): never
 * an active scan, which can make Bluetooth audio stutter.
 */
static void poll_bluetooth_cached(void) {
    BLUETOOTH_FIND_RADIO_PARAMS rp;
    rp.dwSize = sizeof(rp);
    HANDLE hRadio = NULL;
    HBLUETOOTH_RADIO_FIND hFind = BluetoothFindFirstRadio(&rp, &hRadio);
    if (!hFind) {
        g_telemetry.bt_present = false;
        g_telemetry.bt_enabled = false;
        g_telemetry.bt_connected_count = 0;
        return;
    }
    g_telemetry.bt_present = true;
    g_telemetry.bt_enabled = true;
    CloseHandle(hRadio);
    BluetoothFindRadioClose(hFind);

    int connected = 0;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp;
    ZeroMemory(&sp, sizeof(sp));
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE; /* CRITICAL: zero active scan */
    sp.cTimeoutMultiplier = 0;

    BLUETOOTH_DEVICE_INFO_STRUCT di;
    ZeroMemory(&di, sizeof(di));
    di.dwSize = sizeof(di);
    HBLUETOOTH_DEVICE_FIND hDev = BluetoothFindFirstDevice(&sp, &di);
    if (hDev) {
        do {
            if (di.fConnected) connected++;
        } while (BluetoothFindNextDevice(hDev, &di));
        BluetoothFindDeviceClose(hDev);
    }
    g_telemetry.bt_connected_count = connected;
}

/* Minimal JSON string field extractor (avoids any external JSON dependency) */
static bool json_extract_str(const char *json, const char *key, char *out, size_t out_len) {
    if (!json || !key || !out || out_len == 0) return false;
    out[0] = '\0';
    char pat[96];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '\"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '\"' && i + 1 < out_len) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static bool run_tailscale_cmd(const wchar_t *args, char *out_buf, size_t out_cap) {
    if (out_buf && out_cap > 0) out_buf[0] = '\0';
    wchar_t ts_exe[MAX_PATH];
    if (!lares_find_tailscale(ts_exe, MAX_PATH)) return false;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    wchar_t cmd[512];
    _snwprintf_s(cmd, 512, _TRUNCATE, L"\"%ls\" %ls", ts_exe, args);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWrite);

    if (!ok) {
        CloseHandle(hRead);
        return false;
    }

    if (out_buf && out_cap > 1) {
        size_t total = 0;
        DWORD nread = 0;
        while (total + 1 < out_cap &&
               ReadFile(hRead, out_buf + total, (DWORD)(out_cap - 1 - total), &nread, NULL) && nread > 0) {
            total += nread;
        }
        out_buf[total] = '\0';
    }
    WaitForSingleObject(pi.hProcess, 4000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return true;
}

static void parse_tailscale_status_json(const char *json) {
    char state[32] = {0}, host[128] = {0}, ip[64] = {0};
    json_extract_str(json, "BackendState", state, sizeof(state));

    const char *self_pos = strstr(json, "\"Self\"");
    if (self_pos) {
        json_extract_str(self_pos, "HostName", host, sizeof(host));
    }

    const char *ips_pos = strstr(json, "\"TailscaleIPs\"");
    if (ips_pos) {
        const char *q1 = strchr(ips_pos + 14, '\"');
        if (q1) {
            q1++;
            const char *q2 = strchr(q1, '\"');
            if (q2 && (size_t)(q2 - q1) < sizeof(ip)) {
                memcpy(ip, q1, (size_t)(q2 - q1));
                ip[q2 - q1] = '\0';
            }
        }
    }

    /* Parse Peer map for peer count and exit node options */
    TailscalePeer peers[LARES_MAX_TS_PEERS];
    int pcount = 0;
    wchar_t active_exit[128] = {0};

    const char *peer_sec = strstr(json, "\"Peer\"");
    if (peer_sec) {
        const char *scan = peer_sec;
        while (pcount < LARES_MAX_TS_PEERS) {
            const char *hn = strstr(scan, "\"HostName\"");
            if (!hn) break;
            /* Stop if we left the Peer section into User/MagicDNSSuffix */
            const char *next_hn = strstr(hn + 10, "\"HostName\"");
            char pname[96] = {0};
            json_extract_str(hn, "HostName", pname, sizeof(pname));

            /* Find IP & flags within this peer chunk */
            size_t chunk_len = next_hn ? (size_t)(next_hn - hn) : strlen(hn);
            if (chunk_len > 2048) chunk_len = 2048;
            char chunk[2049];
            memcpy(chunk, hn, chunk_len);
            chunk[chunk_len] = '\0';

            char pip[64] = {0};
            const char *pips = strstr(chunk, "\"TailscaleIPs\"");
            if (pips) {
                const char *q1 = strchr(pips + 14, '\"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '\"');
                    if (q2 && (size_t)(q2 - q1) < sizeof(pip)) {
                        memcpy(pip, q1, (size_t)(q2 - q1));
                        pip[q2 - q1] = '\0';
                    }
                }
            }

            TailscalePeer *tp = &peers[pcount];
            ZeroMemory(tp, sizeof(*tp));
            MultiByteToWideChar(CP_UTF8, 0, pname, -1, tp->name, 96);
            MultiByteToWideChar(CP_UTF8, 0, pip, -1, tp->ip, 64);
            tp->online = (strstr(chunk, "\"Online\": true") != NULL);
            tp->can_exit_node = (strstr(chunk, "\"ExitNodeOption\": true") != NULL);
            tp->is_active_exit_node = (strstr(chunk, "\"ExitNode\": true") != NULL);
            if (tp->is_active_exit_node) {
                wcsncpy_s(active_exit, 128, tp->name[0] ? tp->name : tp->ip, _TRUNCATE);
            }
            pcount++;
            scan = hn + 10;
        }
    }

    EnterCriticalSection(&g_ts_cs);
    g_telemetry.ts_available = true;
    MultiByteToWideChar(CP_UTF8, 0, state, -1, g_telemetry.ts_state, 32);
    MultiByteToWideChar(CP_UTF8, 0, host, -1, g_telemetry.ts_hostname, 128);
    MultiByteToWideChar(CP_UTF8, 0, ip, -1, g_telemetry.ts_ip, 64);
    wcsncpy_s(g_telemetry.ts_exit_node, 128, active_exit, _TRUNCATE);
    g_telemetry.ts_running = (_stricmp(state, "Running") == 0);
    g_telemetry.ts_peer_count = pcount;
    g_ts_peer_list_count = pcount;
    if (pcount > 0) memcpy(g_ts_peers, peers, (size_t)pcount * sizeof(TailscalePeer));
    LeaveCriticalSection(&g_ts_cs);
}

typedef struct {
    int action; /* 0=status, 1=up, 2=down, 3=exit-node */
    wchar_t arg[128];
} TsTask;

/* ---------- Tailscale Windows service (read-only query needs no elevation) ---------- */

/* Private bytes of every tailscaled.exe. A standard user cannot open SYSTEM service processes for
 * memory counters, so read the same system-wide snapshot Task Manager uses (no process handles). */
typedef LONG (NTAPI *PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);

typedef struct {
    ULONG          NextEntryOffset;
    ULONG          NumberOfThreads;
    BYTE           Reserved1[48];
    USHORT         NameLength;          /* UNICODE_STRING ImageName */
    USHORT         NameMaxLength;
    PWSTR          NameBuffer;
    LONG           BasePriority;
    HANDLE         UniqueProcessId;
    PVOID          Reserved2;
    ULONG          HandleCount;
    ULONG          SessionId;
    PVOID          Reserved3;
    SIZE_T         PeakVirtualSize;
    SIZE_T         VirtualSize;
    ULONG          Reserved4;
    SIZE_T         PeakWorkingSetSize;
    SIZE_T         WorkingSetSize;
    PVOID          Reserved5;
    SIZE_T         QuotaPagedPoolUsage;
    PVOID          Reserved6;
    SIZE_T         QuotaNonPagedPoolUsage;
    SIZE_T         PagefileUsage;       /* private commit (Task Manager "Commit size") */
    SIZE_T         PeakPagefileUsage;
    SIZE_T         PrivatePageCount;
} SnSysProcInfo;

static int ts_sum_tailscaled_private_mb(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NtQuerySystemInformation qsi = ntdll ? (PFN_NtQuerySystemInformation)(void *)GetProcAddress(ntdll, "NtQuerySystemInformation") : NULL;
    if (!qsi) return 0;
    ULONG cap = 512 * 1024;
    BYTE *buf = NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        buf = (BYTE *)VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) return 0;
        ULONG need = 0;
        LONG st = qsi(5 /* SystemProcessInformation */, buf, cap, &need);
        if (st >= 0) break;
        VirtualFree(buf, 0, MEM_RELEASE);
        buf = NULL;
        if (st != (LONG)0xC0000004L /* STATUS_INFO_LENGTH_MISMATCH */) return 0;
        cap = max(cap * 2, need + 64 * 1024);
    }
    if (!buf) return 0;
    uint64_t total = 0;
    const BYTE *p = buf;
    for (;;) {
        const SnSysProcInfo *pi = (const SnSysProcInfo *)p;
        if (pi->NameBuffer && pi->NameLength == 28 && _wcsnicmp(pi->NameBuffer, L"tailscaled.exe", 14) == 0) {
            total += (uint64_t)(pi->PrivatePageCount ? pi->PrivatePageCount : pi->PagefileUsage);
        }
        if (pi->NextEntryOffset == 0) break;
        p += pi->NextEntryOffset;
    }
    VirtualFree(buf, 0, MEM_RELEASE); /* returned to Windows immediately */
    return (int)(total / (1024ULL * 1024ULL));
}

static void ts_query_service(int *state, int *start_type) {
    *state = 0;
    *start_type = 0;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, TS_SERVICE_NAME, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (svc) {
        SERVICE_STATUS_PROCESS ssp;
        DWORD need = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &need)) {
            *state = (int)ssp.dwCurrentState;
        }
        ULONGLONG cfg_buf[512]; /* 8-byte aligned QUERY_SERVICE_CONFIGW storage */
        QUERY_SERVICE_CONFIGW *qc = (QUERY_SERVICE_CONFIGW *)cfg_buf;
        if (QueryServiceConfigW(svc, qc, (DWORD)sizeof(cfg_buf), &need)) {
            *start_type = (int)qc->dwStartType;
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

static void ts_publish_service_state(void) {
    int st = 0, start = 0;
    ts_query_service(&st, &start);
    /* Memory figure is only shown in the Network popup, so skip the snapshot while it is closed */
    int mem = (st == SERVICE_RUNNING && popups_is_kind_open(POPUP_NETWORK)) ? ts_sum_tailscaled_private_mb() : 0;
    EnterCriticalSection(&g_ts_cs);
    g_telemetry.ts_svc_state = st;
    g_telemetry.ts_svc_start_type = start;
    g_telemetry.ts_svc_mem_mb = mem;
    LeaveCriticalSection(&g_ts_cs);
}

/* Performs the SCM change in-process. Returns 0 on success or a Win32 error code
 * (ERROR_ACCESS_DENIED means the caller has to ask for elevation). */
static DWORD ts_service_control_local(bool start, int startup_type) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    DWORD access = SERVICE_QUERY_STATUS | (start ? SERVICE_START : SERVICE_STOP);
    if (startup_type) access |= SERVICE_CHANGE_CONFIG;
    SC_HANDLE svc = OpenServiceW(scm, TS_SERVICE_NAME, access);
    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        return e;
    }

    DWORD err = 0;
    if (startup_type == SERVICE_AUTO_START || startup_type == SERVICE_DEMAND_START) {
        if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, (DWORD)startup_type, SERVICE_NO_CHANGE,
                                  NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
            err = GetLastError();
        }
    }
    if (!err) {
        if (start) {
            if (!StartServiceW(svc, 0, NULL)) {
                DWORD e = GetLastError();
                if (e != ERROR_SERVICE_ALREADY_RUNNING) err = e;
            }
        } else {
            SERVICE_STATUS ss;
            if (!ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
                DWORD e = GetLastError();
                if (e != ERROR_SERVICE_NOT_ACTIVE) err = e;
            }
        }
    }
    if (!err) {
        DWORD target = start ? SERVICE_RUNNING : SERVICE_STOPPED;
        SERVICE_STATUS_PROCESS ssp;
        ZeroMemory(&ssp, sizeof(ssp));
        DWORD need = 0;
        ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 30000ULL) {
            if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &need)) break;
            if (ssp.dwCurrentState == target) break;
            Sleep(200);
        }
        if (ssp.dwCurrentState != target) err = ERROR_SERVICE_REQUEST_TIMEOUT;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return err;
}

/* Re-runs this same executable elevated for one fixed SCM action (UAC prompt only here). */
static DWORD ts_service_control_elevated(bool start, int startup_type) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return GetLastError();
    wchar_t params[96];
    _snwprintf_s(params, 96, _TRUNCATE, L"--ts-service %ls%ls", start ? L"start" : L"stop",
                 startup_type == SERVICE_DEMAND_START ? L" --ts-startup manual"
               : startup_type == SERVICE_AUTO_START   ? L" --ts-startup auto" : L"");

    HRESULT hr_init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_UNICODE | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g_hTopBarWnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    DWORD err = 0;
    if (!ShellExecuteExW(&sei)) {
        err = GetLastError();
        if (err == 0) err = ERROR_CANCELLED;
    } else if (sei.hProcess) {
        if (WaitForSingleObject(sei.hProcess, 60000) != WAIT_OBJECT_0) {
            err = ERROR_SERVICE_REQUEST_TIMEOUT;
        } else {
            DWORD code = 0;
            if (GetExitCodeProcess(sei.hProcess, &code)) err = code;
        }
        CloseHandle(sei.hProcess);
    }
    if (SUCCEEDED(hr_init)) CoUninitialize();
    return err;
}

int telemetry_ts_service_helper(bool start, int startup_type) {
    return (int)ts_service_control_local(start, startup_type);
}

/* Refreshes Tailscale state; skips spawning tailscale.exe entirely while the service is stopped. */
static void ts_refresh_status_inline(char *json, size_t cap) {
    ts_publish_service_state();
    int svc_state = g_telemetry.ts_svc_state;
    wchar_t ts_cli[MAX_PATH];
    bool cli_present = lares_find_tailscale(ts_cli, MAX_PATH);
    if (svc_state != 0 && svc_state != SERVICE_RUNNING) {
        EnterCriticalSection(&g_ts_cs);
        g_telemetry.ts_available = cli_present;
        g_telemetry.ts_running = false;
        wcsncpy_s(g_telemetry.ts_state, 32,
                  svc_state == SERVICE_START_PENDING ? L"Starting service" :
                  svc_state == SERVICE_STOP_PENDING  ? L"Stopping service" : L"Service stopped", _TRUNCATE);
        g_telemetry.ts_ip[0] = L'\0';
        g_telemetry.ts_exit_node[0] = L'\0';
        g_telemetry.ts_peer_count = 0;
        g_ts_peer_list_count = 0;
        LeaveCriticalSection(&g_ts_cs);
        return;
    }
    if (run_tailscale_cmd(L"status --json", json, cap) && json[0] == '{') {
        parse_tailscale_status_json(json);
    } else {
        EnterCriticalSection(&g_ts_cs);
        g_telemetry.ts_available = cli_present;
        g_telemetry.ts_running = false;
        wcsncpy_s(g_telemetry.ts_state, 32, L"Stopped", _TRUNCATE);
        LeaveCriticalSection(&g_ts_cs);
    }
}

static DWORD WINAPI ts_worker_thread(LPVOID lp) {
    TsTask *t = (TsTask *)lp;
    char *json = (char *)HeapAlloc(GetProcessHeap(), 0, 131072);
    if (json) {
        if (t->action == 1) {
            run_tailscale_cmd(L"up", json, 4096);
        } else if (t->action == 2) {
            run_tailscale_cmd(L"down", json, 4096);
        } else if (t->action == 3) {
            wchar_t cmd[200];
            _snwprintf_s(cmd, 200, _TRUNCATE, L"set --exit-node=%ls", t->arg);
            run_tailscale_cmd(cmd, json, 4096);
        }
        ts_refresh_status_inline(json, 131072);
        HeapFree(GetProcessHeap(), 0, json);
    }
    HeapFree(GetProcessHeap(), 0, t);
    InterlockedExchange(&g_ts_busy, 0);
    if (g_tel_notify) PostMessageW(g_tel_notify, WM_LARES_TAILSCALE_UPDATED, 0, 0);
    return 0;
}

typedef struct {
    bool start;
    int  startup_type;
} TsSvcTask;

static volatile LONG g_ts_svc_busy = 0;

static DWORD WINAPI ts_service_thread(LPVOID lp) {
    TsSvcTask *t = (TsSvcTask *)lp;
    DWORD err = ts_service_control_local(t->start, t->startup_type);
    if (err == ERROR_ACCESS_DENIED) {
        err = ts_service_control_elevated(t->start, t->startup_type);
    }

    wchar_t msg[96] = {0};
    if (err == 0) {
        msg[0] = L'\0';
    } else if (err == ERROR_CANCELLED) {
        wcsncpy_s(msg, 96, L"Cancelled: administrator approval is needed", _TRUNCATE);
    } else if (err == ERROR_SERVICE_REQUEST_TIMEOUT) {
        wcsncpy_s(msg, 96, L"Service did not respond in time", _TRUNCATE);
    } else if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
        wcsncpy_s(msg, 96, L"Tailscale service is not installed", _TRUNCATE);
    } else {
        _snwprintf_s(msg, 96, _TRUNCATE, L"Service change failed (error %lu)", (unsigned long)err);
    }

    /* Give tailscaled a moment to open its local API before asking for status */
    if (err == 0 && t->start) Sleep(1500);
    char *json = (char *)HeapAlloc(GetProcessHeap(), 0, 131072);
    if (json) {
        ts_refresh_status_inline(json, 131072);
        HeapFree(GetProcessHeap(), 0, json);
    } else {
        ts_publish_service_state();
    }

    EnterCriticalSection(&g_ts_cs);
    wcsncpy_s(g_telemetry.ts_svc_msg, 96, msg, _TRUNCATE);
    g_telemetry.ts_svc_msg_tick = msg[0] ? GetTickCount() : 0;
    g_telemetry.ts_svc_busy = false;
    LeaveCriticalSection(&g_ts_cs);

    HeapFree(GetProcessHeap(), 0, t);
    InterlockedExchange(&g_ts_svc_busy, 0);
    if (g_tel_notify) PostMessageW(g_tel_notify, WM_LARES_TAILSCALE_UPDATED, 0, 0);
    return 0;
}

void telemetry_tailscale_service_async(bool start, int startup_type) {
    if (InterlockedCompareExchange(&g_ts_svc_busy, 1, 0) != 0) return;
    TsSvcTask *t = (TsSvcTask *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TsSvcTask));
    if (!t) { InterlockedExchange(&g_ts_svc_busy, 0); return; }
    t->start = start;
    t->startup_type = startup_type;
    EnterCriticalSection(&g_ts_cs);
    g_telemetry.ts_svc_busy = true;
    g_telemetry.ts_svc_msg[0] = L'\0';
    g_telemetry.ts_svc_msg_tick = 0;
    LeaveCriticalSection(&g_ts_cs);
    HANDLE h = CreateThread(NULL, 0, ts_service_thread, t, 0, NULL);
    if (h) {
        CloseHandle(h);
    } else {
        HeapFree(GetProcessHeap(), 0, t);
        EnterCriticalSection(&g_ts_cs);
        g_telemetry.ts_svc_busy = false;
        LeaveCriticalSection(&g_ts_cs);
        InterlockedExchange(&g_ts_svc_busy, 0);
    }
    if (g_tel_notify) PostMessageW(g_tel_notify, WM_LARES_TAILSCALE_UPDATED, 0, 0);
}

static void launch_ts_task(int action, const wchar_t *arg) {
    if (InterlockedCompareExchange(&g_ts_busy, 1, 0) != 0) return;
    TsTask *t = (TsTask *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TsTask));
    if (!t) { InterlockedExchange(&g_ts_busy, 0); return; }
    t->action = action;
    if (arg) wcsncpy_s(t->arg, 128, arg, _TRUNCATE);
    HANDLE h = CreateThread(NULL, 0, ts_worker_thread, t, 0, NULL);
    if (h) CloseHandle(h);
    else { HeapFree(GetProcessHeap(), 0, t); InterlockedExchange(&g_ts_busy, 0); }
}

void telemetry_refresh_tailscale_async(void) {
    launch_ts_task(0, NULL);
}

void telemetry_tailscale_toggle_async(bool connect) {
    launch_ts_task(connect ? 1 : 2, NULL);
}

void telemetry_tailscale_set_exit_node_async(const wchar_t *node_ip) {
    launch_ts_task(3, node_ip ? node_ip : L"");
}

void telemetry_init(HWND notify_hwnd) {
    InitializeCriticalSection(&g_ts_cs);
    g_tel_notify = notify_hwnd;
    ZeroMemory(&g_telemetry, sizeof(g_telemetry));
    wcsncpy_s(g_telemetry.kbd_short, 16, L"EN", _TRUNCATE);
    telemetry_update(true);
    telemetry_refresh_tailscale_async();
}

void telemetry_update(bool poll_gpu_flag) {
    poll_cpu_ram();
    if (poll_gpu_flag) poll_gpu();
    poll_keyboard();
    poll_network();
    poll_bluetooth_cached();
    audio_poll();
}

void telemetry_shutdown(void) {
    if (g_gpu_query) {
        PdhCloseQuery(g_gpu_query);
        g_gpu_query = NULL;
        g_gpu_counter = NULL;
        g_gpu_ready = false;
    }
    if (g_audio_vol) {
        IAudioEndpointVolume_Release(g_audio_vol);
        g_audio_vol = NULL;
    }
    if (g_audio_enum) {
        IMMDeviceEnumerator_Release(g_audio_enum);
        g_audio_enum = NULL;
    }
    DeleteCriticalSection(&g_ts_cs);
}
