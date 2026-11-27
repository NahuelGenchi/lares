// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
// Other programs, found the way Windows finds them (issue #141).
#include "programs.h"

#include <windows.h>
#include <shlwapi.h>

#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace {

bool exists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool put(wchar_t* out, size_t cch, const std::wstring& value)
{
    if (!out || !cch || value.empty() || value.size() >= cch) return false;
    wcsncpy_s(out, cch, value.c_str(), _TRUNCATE);
    return true;
}

void put_name(wchar_t* out, size_t cch, const wchar_t* value)
{
    if (out && cch) wcsncpy_s(out, cch, value, _TRUNCATE);
}

// A program on the PATH or in Windows' own search order, as typing its name
// in a terminal would find it. App aliases of Store apps (wt.exe) are found
// this way too.
std::wstring on_path(const wchar_t* file)
{
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, file, nullptr, MAX_PATH, buf, nullptr);
    if (n == 0 || n >= MAX_PATH) return {};
    return buf;
}

std::wstring service_binary(const wchar_t* service)
{
    std::wstring result;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return result;
    SC_HANDLE svc = OpenServiceW(scm, service, SERVICE_QUERY_CONFIG);
    if (svc) {
        DWORD need = 0;
        QueryServiceConfigW(svc, nullptr, 0, &need);
        if (need > 0 && need < 64 * 1024) {
            std::vector<BYTE> buf(need);
            auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
            if (QueryServiceConfigW(svc, cfg, need, &need) && cfg->lpBinaryPathName) {
                result = cfg->lpBinaryPathName;
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return result;
}

} // namespace

extern "C" bool lares_service_program_folder(const wchar_t* image_path, wchar_t* out, size_t cch)
{
    if (!image_path) return false;
    std::wstring s(image_path);
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    s.erase(0, a);
    std::wstring program;
    if (!s.empty() && s[0] == L'"') {
        size_t q = s.find(L'"', 1);
        if (q == std::wstring::npos) return false;
        program = s.substr(1, q - 1);
    } else {
        // Unquoted: the program ends at ".exe"; without one, at the first space.
        std::wstring lower = s;
        for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
        size_t e = lower.find(L".exe");
        if (e != std::wstring::npos) program = s.substr(0, e + 4);
        else program = s.substr(0, s.find(L' '));
    }
    if (program.rfind(L"\\??\\", 0) == 0) program.erase(0, 4);
    if (program.find(L'%') != std::wstring::npos) {
        wchar_t expanded[MAX_PATH];
        DWORD n = ExpandEnvironmentStringsW(program.c_str(), expanded, MAX_PATH);
        if (n == 0 || n > MAX_PATH) return false;
        program = expanded;
    }
    size_t slash = program.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return false;
    return put(out, cch, program.substr(0, slash));
}

extern "C" bool lares_find_tailscale(wchar_t* out, size_t cch)
{
    std::wstring image = service_binary(L"Tailscale");
    wchar_t folder[MAX_PATH];
    if (!image.empty() && lares_service_program_folder(image.c_str(), folder, MAX_PATH)) {
        std::wstring cli = std::wstring(folder) + L"\\tailscale.exe";
        if (exists(cli)) return put(out, cch, cli);
    }
    std::wstring path = on_path(L"tailscale.exe");
    return !path.empty() && put(out, cch, path);
}

extern "C" bool lares_windows_program(const wchar_t* file, wchar_t* out, size_t cch)
{
    if (!file || !*file) return false;
    wchar_t dir[MAX_PATH];
    UINT n = _wcsicmp(file, L"explorer.exe") == 0 ? GetWindowsDirectoryW(dir, MAX_PATH)
                                                   : GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring path = std::wstring(dir) + L"\\" + file;
    return exists(path) && put(out, cch, path);
}

extern "C" bool lares_find_terminal(wchar_t* exe, size_t cch, wchar_t* name, size_t name_cch)
{
    std::wstring path = on_path(L"wt.exe");
    if (!path.empty()) {
        put_name(name, name_cch, L"Terminal");
        return !exe || put(exe, cch, path);
    }
    path = on_path(L"pwsh.exe");
    if (!path.empty()) {
        put_name(name, name_cch, L"PowerShell");
        return !exe || put(exe, cch, path);
    }
    wchar_t buf[MAX_PATH];
    if (lares_windows_program(L"WindowsPowerShell\\v1.0\\powershell.exe", buf, MAX_PATH)) {
        put_name(name, name_cch, L"Windows PowerShell");
        return !exe || put(exe, cch, buf);
    }
    DWORD n = GetEnvironmentVariableW(L"ComSpec", buf, MAX_PATH);
    if ((n > 0 && n < MAX_PATH && exists(buf)) || lares_windows_program(L"cmd.exe", buf, MAX_PATH)) {
        put_name(name, name_cch, L"Command Prompt");
        return !exe || put(exe, cch, buf);
    }
    return false;
}

extern "C" bool lares_find_browser(wchar_t* exe, size_t cch, wchar_t* name, size_t name_cch)
{
    static const wchar_t* schemes[] = { L"https", L"http" };
    for (const wchar_t* scheme : schemes) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (FAILED(AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, scheme, L"open", path, &n)) ||
            !exists(path)) {
            continue;
        }
        if (name && name_cch) {
            wchar_t friendly[128];
            DWORD fn = 128;
            if (SUCCEEDED(AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_FRIENDLYAPPNAME, scheme, L"open",
                                            friendly, &fn))) {
                put_name(name, name_cch, friendly);
            } else {
                put_name(name, name_cch, L"Browser");
            }
        }
        return !exe || put(exe, cch, path);
    }
    return false;
}
