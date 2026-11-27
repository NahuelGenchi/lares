// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "changes.h"
#include "cli.h"
#include "crash.h"
#include "lares_version.h"
#include "remote.h"
#include "safe.h"
#include "ui.h"

#include <cstring>
#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <windows.h>

static std::string wide_to_utf8(const wchar_t* ws)
{
    if (!ws || !*ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

static std::string get_env_utf8(const wchar_t* name)
{
    wchar_t buf[256];
    DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return wide_to_utf8(buf);
}

static void write_cli_output(const char* text)
{
    if (!text || !*text) return;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    bool attached = false;
    if (h == nullptr || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            attached = true;
            h = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetFileType(h) == FILE_TYPE_CHAR && GetConsoleMode(h, &mode)) {
            int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
            if (n > 1) {
                std::wstring wide(static_cast<size_t>(n - 1), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), n);
                DWORD written = 0;
                WriteConsoleW(h, wide.c_str(), static_cast<DWORD>(wide.size()), &written, nullptr);
            }
        } else {
            DWORD written = 0;
            WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        }
    }
    if (attached) FreeConsole();
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int cmd_show)
{
    // A crash writes a report on this PC only (win/crash/crash.h).
    lares_crash_install("lares", LARES_VERSION);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> args_storage;
    std::vector<const char*> argv;
    if (wargv) {
        args_storage.reserve(static_cast<size_t>(wargc));
        argv.reserve(static_cast<size_t>(wargc));
        for (int i = 0; i < wargc; ++i) {
            args_storage.push_back(wide_to_utf8(wargv[i]));
            argv.push_back(args_storage.back().c_str());
        }
        LocalFree(wargv);
    }

    std::string env_dry = get_env_utf8(L"LARES_DRY_RUN");
    std::string env_stream = get_env_utf8(L"LARES_DRY_STREAM");
    std::string env_scheme = get_env_utf8(L"LARES_COLOR_SCHEME");
    std::string env_sec = get_env_utf8(L"LARES_SECTION");

    lares::CliOptions opts = lares::parse_cli(
        static_cast<int>(argv.size()), argv.data(),
        env_dry.empty() ? nullptr : env_dry.c_str(),
        env_stream.empty() ? nullptr : env_stream.c_str(),
        env_scheme.empty() ? nullptr : env_scheme.c_str(),
        env_sec.empty() ? nullptr : env_sec.c_str());

    /* Safe mode (win/guard/safe.h): the session is marked, and a guard that
     * is running is asked to end the bar and give the taskbar back. A dry
     * run marks a session of its own and touches nothing. */
    if (opts.safe) {
        lares_safe_mode_mark(opts.dry_run ? L"dry" : L"", 1);
        if (!opts.dry_run) {
            lares::guard_command(L"stop");
            lares::guard_command(L"restore");
        }
    }

    if (opts.action == lares::CliAction::Help) {
        write_cli_output(lares::cli_usage());
        return 0;
    }
    if (opts.action == lares::CliAction::Error) {
        write_cli_output(lares::cli_usage());
        return 2;
    }
    if (opts.action == lares::CliAction::Undo) {
        std::string said;
        bool ok = lares::changes_undo(opts.dry_run, said);
        said += "\n";
        write_cli_output(said.c_str());
        return ok ? 0 : 1;
    }
    if (opts.action != lares::CliAction::Window) {
        HRESULT hr_cli = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::string out_text;
        int rc = lares::run_remote_cli(opts.action, opts.dry_run, opts.dry_stream, out_text);
        if (SUCCEEDED(hr_cli)) CoUninitialize();
        write_cli_output(out_text.c_str());
        return rc;
    }

    // Single-instance check: activate existing window and forward --section if given.
    const wchar_t* mutex_name = opts.dry_run
        ? L"Local\\com.nahuelgenchi.Lares.dry"
        : L"Local\\com.nahuelgenchi.Lares";
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        const wchar_t* cls_name = opts.dry_run ? lares::kWindowClassDry : lares::kWindowClass;
        HWND existing = FindWindowW(cls_name, nullptr);
        if (existing) {
            COPYDATASTRUCT cds = {};
            cds.dwData = lares::kCopyDataSection;
            cds.cbData = static_cast<DWORD>(opts.section.size() + 1);
            cds.lpData = const_cast<char*>(opts.section.c_str());
            SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
        }
        CloseHandle(mutex);
        return 0;
    }

    HRESULT hr_ole = OleInitialize(nullptr);
    int exit_code = 1;
    {
        lares::MainWindow win(hinst, opts);
        if (win.create()) {
            win.show(cmd_show);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exit_code = static_cast<int>(msg.wParam);
        }
    }

    if (SUCCEEDED(hr_ole)) OleUninitialize();
    if (mutex) CloseHandle(mutex);
    return exit_code;
}
