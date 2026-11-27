/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * lares-crash-test: the crash report as plain text (what it holds, and that
 * private details are replaced before it is written), and a real crash of
 * the stand-in shell with the report it leaves.
 *
 *   lares-crash-test                 all of it
 *   lares-crash-test --keep FILE     also copies the real crash's report to
 *                                    FILE, for the privacy gate to read
 *
 * The made-up private details are put together while the test runs, so that
 * neither this file nor the built program holds a user folder path.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "crash.h"

static int g_failures, g_checks;

static void check(int ok, const char *what)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    printf("  FAILED: %s\n", what);
}

static int has(const char *text, const char *part)
{
    return strstr(text, part) != NULL;
}

/* A user folder for a made-up account, with either slash. */
static void user_folder(char *out, size_t size, const char *drive, char slash, const char *parent, const char *name)
{
    snprintf(out, size, "%s%c%s%c%s", drive, slash, parent, slash, name);
}

static void test_scrub(void)
{
    char folder[128], other[128], unix_like[128], text[512];
    lares_crash_private priv = { folder, "jsmith", "DESKTOP-4KQ7" };
    int n;
    printf("private details are replaced\n");
    user_folder(folder, sizeof folder, "C:", '\\', "Users", "jsmith");
    user_folder(other, sizeof other, "D:", '\\', "Users", "maria.lopez");
    user_folder(unix_like, sizeof unix_like, "", '/', "home", "ana");

    n = snprintf(text, sizeof text, "module %s\\AppData\\Local\\Lares\\x.dll loaded", folder);
    n = lares_crash_scrub(text, n, sizeof text, &priv);
    check(!strcmp(text, "module %USERPROFILE%\\AppData\\Local\\Lares\\x.dll loaded"), "the user's own folder becomes %USERPROFILE%");
    check(n == (int)strlen(text), "the new length is returned");

    n = snprintf(text, sizeof text, "a %s\\Desktop\\notes.txt b %s/.config c", other, unix_like);
    lares_crash_scrub(text, n, sizeof text, &priv);
    check(!has(text, "maria") && !has(text, "ana/") && has(text, "D:\\Users\\<name>\\Desktop") && has(text, "/home/<name>/.config"),
          "anyone else's folder keeps its place and loses its name");

    user_folder(other, sizeof other, "c:", '/', "users", "JSMITH");
    user_folder(unix_like, sizeof unix_like, "C:", '\\', "USERS", "Jsmith");
    n = snprintf(text, sizeof text, "%s/x and %s\\y", other, unix_like);
    lares_crash_scrub(text, n, sizeof text, &priv);
    check(!has(text, "JSMITH") && !has(text, "Jsmith") && !has(text, "jsmith"), "letter case and the kind of slash do not matter");

    n = snprintf(text, sizeof text, "user jsmith on DESKTOP-4KQ7; Jsmith's pc desktop-4kq7.");
    lares_crash_scrub(text, n, sizeof text, &priv);
    check(!strcmp(text, "user <name> on <pc>; <name>'s pc <pc>."), "the account's and the computer's name go, as words");

    n = snprintf(text, sizeof text, "jsmithson.dll and xjsmith stay");
    lares_crash_scrub(text, n, sizeof text, &priv);
    check(!strcmp(text, "jsmithson.dll and xjsmith stay"), "a longer word that only contains the name stays");

    n = snprintf(text, sizeof text, "C:\\Users\\Public\\Desktop and C:\\Windows\\System32\\ntdll.dll");
    lares_crash_scrub(text, n, sizeof text, &priv);
    check(has(text, "Users\\Public\\Desktop") && has(text, "System32\\ntdll.dll"), "what is not private stays");

    /* No room to grow: the text is cut, never overrun, and the name still goes. */
    char tight[24];
    user_folder(other, sizeof other, "", '/', "home", "ab");
    user_folder(unix_like, sizeof unix_like, "", '/', "home", "cd");
    n = snprintf(tight, sizeof tight, "%s/x%s/y", other, unix_like);
    n = lares_crash_scrub(tight, n, sizeof tight, &priv);
    check(n < (int)sizeof tight && tight[n] == 0 && !has(tight, "/ab/") && !has(tight, "/cd/"), "a full buffer is cut, not overrun");

    n = snprintf(text, sizeof text, "nothing to hide here");
    check(lares_crash_scrub(text, n, sizeof text, NULL) == n && !strcmp(text, "nothing to hide here"), "plain text is left alone");
}

static void test_format(void)
{
    char folder[128], text[LARES_CRASH_REPORT_MAX];
    lares_crash_private priv = { folder, "jsmith", "DESKTOP-4KQ7" };
    lares_crash_info info;
    printf("what a report holds\n");
    user_folder(folder, sizeof folder, "C:", '\\', "Users", "jsmith");
    memset(&info, 0, sizeof info);
    info.program = "lares";
    info.version = "dev 1a2b3c4";
    info.windows_major = 10;
    info.windows_build = 26100;
    info.machine = "x64";
    info.when.wYear = 2026;
    info.when.wMonth = 11;
    info.when.wDay = 27;
    info.when.wHour = 9;
    info.when.wMinute = 5;
    info.code = 0xC0000005u;
    info.access = 1;
    info.address = 0x10;
    info.frame_count = 3;
    strcpy(info.frames[0].module, "lares.exe");
    info.frames[0].offset = 0x1234;
    /* A module name that somehow carried its folder must not get through. */
    snprintf(info.frames[1].module, sizeof info.frames[1].module, "%s\\x.dll", folder);
    info.frames[1].offset = 0x20;
    info.frames[2].offset = 0x7ffe1000;

    int n = lares_crash_format(&info, &priv, text, sizeof text);
    check(n > 0 && n == (int)strlen(text), "a report is made");
    check(has(text, "Program: lares\r\n") && has(text, "Version: dev 1a2b3c4\r\n"), "the program and its version");
    check(has(text, "Windows: 10.0.26100 (x64)\r\n"), "the Windows version");
    check(has(text, "When: 2026-11-27 09:05:00 UTC\r\n"), "when");
    check(has(text, "What happened: access violation (0xc0000005), writing address 0x10\r\n"), "what happened");
    check(has(text, "Faulting module: lares.exe+0x1234\r\n"), "the faulting module");
    check(has(text, "  00  lares.exe+0x1234\r\n") && has(text, "  02  0x7ffe1000\r\n"), "the call stack");
    check(has(text, "has not been sent anywhere"), "it says that it stays on the PC");
    check(has(text, "%USERPROFILE%\\x.dll+0x20") && !has(text, "jsmith") && !has(text, "DESKTOP-4KQ7"),
          "a path with a user folder comes out with the placeholder");
}

/* ---- a real crash ---------------------------------------------------- */

static int run_shell_until_it_crashes(DWORD *code)
{
    wchar_t self[MAX_PATH], line[MAX_PATH + 64], *slash;
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi;
    si.cb = sizeof si;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    slash = wcsrchr(self, L'\\');
    if (!slash) return 0;
    *slash = 0;
    swprintf(line, ARRAYSIZE(line), L"\"%ls\\lares-test-shell.exe\" --crash-after 100", self);
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    int ended = WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0;
    if (!ended) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ended;
}

static void lower_into(char *to, size_t size, const wchar_t *from)
{
    WideCharToMultiByte(CP_UTF8, 0, from, -1, to, (int)size, NULL, NULL);
    _strlwr(to);
}

static void test_real_crash(const wchar_t *keep)
{
    wchar_t temp[MAX_PATH], dir[MAX_PATH], path[MAX_PATH + 80], value[MAX_PATH];
    char text[LARES_CRASH_REPORT_MAX], lowered[LARES_CRASH_REPORT_MAX], secret[MAX_PATH * 3];
    DWORD code = 0;
    ULONGLONG started = GetTickCount64();
    printf("a real crash of the stand-in shell\n");
    GetTempPathW(MAX_PATH, temp);
    swprintf(dir, MAX_PATH, L"%lslares-crash-test-%lu", temp, GetCurrentProcessId());
    SetEnvironmentVariableW(L"LARES_CRASH_DIR", dir);

    check(run_shell_until_it_crashes(&code), "the stand-in shell crashes and ends by itself");
    check(code == 0xC0000005u, "it ends with the crash's own code");
    check(GetTickCount64() - started < 5000, "and at once, without waiting on Windows' crash handling");
    check(lares_crash_latest(path, ARRAYSIZE(path)), "a report is on disk");
    int n = lares_crash_read(path, text, sizeof text);
    check(n > 0, "it can be read");
    if (n <= 0) return;

    check(has(text, "Program: lares-test-shell\r\n"), "it names the program");
    check(has(text, "Version: ") && !has(text, "Version: \r\n"), "and its version");
    check(has(text, "Windows: ") && !has(text, "Windows: 0.0.0"), "the Windows version");
    check(has(text, "What happened: access violation (0xc0000005), writing address 0x0\r\n"), "what happened");
    check(has(text, "Faulting module: lares-test-shell.exe+0x"), "the faulting module");
    check(has(text, "  01  ") && (has(text, "USER32.dll+0x") || has(text, "user32.dll+0x")), "a call stack that reaches Windows' own code");

    /* Nothing of this PC's: its user folder, account and computer name. */
    strcpy(lowered, text);
    _strlwr(lowered);
    static const wchar_t *const names[] = { L"USERPROFILE", L"USERNAME", L"COMPUTERNAME" };
    for (int i = 0; i < 3; ++i) {
        if (!GetEnvironmentVariableW(names[i], value, MAX_PATH)) continue;
        lower_into(secret, sizeof secret, value);
        /* "lares" as an account name would be replaced in the report too,
         * but then this plain search could not tell. */
        if (strlen(secret) < 2 || has("lares-test-shell.exe user32.dll kernel32.dll ntdll.dll", secret)) continue;
        check(!has(lowered, secret), "none of this PC's private names is in it");
    }
    check(!has(lowered, ":\\users\\") && !has(lowered, "/users/"), "and no user folder path");

    if (keep) check(CopyFileW(path, keep, FALSE), "the report is copied for the privacy gate");
    printf("%s", text);

    DeleteFileW(path);
    RemoveDirectoryW(dir);
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *keep = argc == 3 && !wcscmp(argv[1], L"--keep") ? argv[2] : NULL;
    test_scrub();
    test_format();
    test_real_crash(keep);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
