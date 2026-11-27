// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "crash_view.h"

#include <cstring>
#include <vector>

extern "C" {
#include "crash.h"
}

namespace lares {

std::wstring crash_program_from_name(const std::wstring& file_name)
{
    // "-YYYYMMDD-HHMMSS.txt" is 20 characters.
    static const size_t kTail = 20;
    if (file_name.size() <= kTail) return L"a Lares program";
    std::wstring tail = file_name.substr(file_name.size() - kTail);
    if (tail[0] != L'-' || tail[9] != L'-' || tail.compare(16, 4, L".txt") != 0) return L"a Lares program";
    for (size_t i : { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u, 12u, 13u, 14u, 15u })
        if (tail[i] < L'0' || tail[i] > L'9') return L"a Lares program";
    return file_name.substr(0, file_name.size() - kTail);
}

static std::wstring local_when(const FILETIME& written)
{
    static const wchar_t* const kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                              L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&written, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    wchar_t text[48];
    swprintf(text, 48, L"%u %ls %u, %02u:%02u", local.wDay, kMonths[(local.wMonth + 11) % 12], local.wYear,
             local.wHour, local.wMinute);
    return text;
}

CrashReportInfo crash_latest()
{
    CrashReportInfo info;
    wchar_t path[MAX_PATH + 80];
    if (!lares_crash_latest(path, static_cast<int>(std::size(path)))) return info;
    info.path = path;
    size_t slash = info.path.find_last_of(L"\\/");
    info.program = crash_program_from_name(slash == std::wstring::npos ? info.path : info.path.substr(slash + 1));
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &data)) info.when = local_when(data.ftLastWriteTime);
    return info;
}

bool crash_copy(HWND owner, const std::wstring& path)
{
    if (path.empty()) return false;
    std::vector<char> text(LARES_CRASH_REPORT_MAX);
    int n = lares_crash_read(path.c_str(), text.data(), static_cast<int>(text.size()));
    if (n <= 0) return false;
    int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), n, nullptr, 0);
    if (wide <= 0) return false;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(wide) + 1) * sizeof(wchar_t));
    if (!memory) return false;
    auto* out = static_cast<wchar_t*>(GlobalLock(memory));
    if (!out) {
        GlobalFree(memory);
        return false;
    }
    MultiByteToWideChar(CP_UTF8, 0, text.data(), n, out, wide);
    out[wide] = 0;
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(memory); // on success the clipboard owns it
    return ok;
}

} // namespace lares
