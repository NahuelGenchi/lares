/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Local crash reports (crash.h).
 *
 * From Microsoft's documentation:
 *   SetUnhandledExceptionFilter and EXCEPTION_POINTERS:
 *     https://learn.microsoft.com/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter
 *   RtlLookupFunctionEntry and RtlVirtualUnwind (walking the stack without
 *   loading anything):
 *     https://learn.microsoft.com/windows/win32/api/winnt/nf-winnt-rtlvirtualunwind
 *   RtlGetVersion (the real Windows version):
 *     https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-rtlgetversion
 */
#include "crash.h"

/* ---- text ----------------------------------------------------------- */

typedef struct {
    char *text;
    int length, capacity;
} out;

static void put_char(out *o, char ch)
{
    if (o->length < o->capacity - 1) o->text[o->length++] = ch;
}

static void put(out *o, const char *s)
{
    while (s && *s) put_char(o, *s++);
}

static void put_number(out *o, ULONGLONG value, int width)
{
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n < width) digits[n++] = '0';
    while (n) put_char(o, digits[--n]);
}

static void put_hex(out *o, ULONGLONG value)
{
    char digits[16];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while (value);
    put(o, "0x");
    while (n) put_char(o, digits[--n]);
}

static char lower(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
}

static int is_word_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
           (unsigned char)ch >= 0x80;
}

static int is_slash(char ch)
{
    return ch == '\\' || ch == '/';
}

static int text_length(const char *s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

/* ---- scrubbing ------------------------------------------------------ */

/* Puts `with` in place of text[at .. at+count). Returns the new length. */
static int splice(char *text, int length, int capacity, int at, int count, const char *with)
{
    int n = text_length(with);
    int tail = length - (at + count);
    if (at + n + tail > capacity - 1) tail = capacity - 1 - at - n;
    if (tail < 0) {
        n += tail;
        tail = 0;
    }
    if (n < 0) n = 0;
    if (n > count)
        for (int i = tail - 1; i >= 0; --i) text[at + n + i] = text[at + count + i];
    else if (n < count)
        for (int i = 0; i < tail; ++i) text[at + n + i] = text[at + count + i];
    for (int i = 0; i < n; ++i) text[at + i] = with[i];
    text[at + n + tail] = 0;
    return at + n + tail;
}

/* Does `needle` start at text[at]? Letter case and the kind of slash do
 * not matter. */
static int starts(const char *text, int length, int at, const char *needle, int n)
{
    if (at + n > length) return 0;
    for (int i = 0; i < n; ++i) {
        char a = text[at + i], b = needle[i];
        if (is_slash(a) && is_slash(b)) continue;
        if (lower(a) != lower(b)) return 0;
    }
    return 1;
}

static int replace_all(char *text, int length, int capacity, const char *needle, const char *with, int whole_words)
{
    int n = text_length(needle), w = text_length(with);
    if (n < 2) return length;
    for (int at = 0; at + n <= length;) {
        int hit = starts(text, length, at, needle, n);
        if (hit && whole_words) {
            if (at > 0 && is_word_char(text[at - 1])) hit = 0;
            if (at + n < length && is_word_char(text[at + n])) hit = 0;
        }
        if (!hit) {
            ++at;
            continue;
        }
        length = splice(text, length, capacity, at, n, with);
        at += w;
    }
    return length;
}

static int ends_folder_name(char ch)
{
    const char *stops = "\\/ \t\r\n\"'`<>|:*?,;()[]{}";
    for (; *stops; ++stops)
        if (ch == *stops) return 1;
    return ch == 0;
}

/* Anyone's folder: a slash, one of the folders that hold users' folders, a
 * slash, and a name. Only the name goes. */
static int scrub_user_folders(char *text, int length, int capacity)
{
    static const char *const parents[] = { "users", "home", "documents and settings" };
    for (int at = 0; at < length; ++at) {
        if (!is_slash(text[at])) continue;
        for (int p = 0; p < 3; ++p) {
            int n = text_length(parents[p]);
            int name = at + 1 + n;
            if (!starts(text, length, at + 1, parents[p], n) || name >= length || !is_slash(text[name])) continue;
            ++name;
            if (name < length && is_slash(text[name])) ++name;
            int end = name;
            while (end < length && !ends_folder_name(text[end])) ++end;
            if (end == name) continue;
            if (end - name == 6 && starts(text, length, name, "public", 6)) continue;
            length = splice(text, length, capacity, name, end - name, "<name>");
            break;
        }
    }
    return length;
}

int lares_crash_scrub(char *text, int length, int capacity, const lares_crash_private *priv)
{
    if (capacity <= 0) return 0;
    if (length > capacity - 1) length = capacity - 1;
    text[length] = 0;
    if (priv && priv->user_folder) length = replace_all(text, length, capacity, priv->user_folder, "%USERPROFILE%", 0);
    length = scrub_user_folders(text, length, capacity);
    if (priv && priv->account) length = replace_all(text, length, capacity, priv->account, "<name>", 1);
    if (priv && priv->computer) length = replace_all(text, length, capacity, priv->computer, "<pc>", 1);
    return length;
}

/* ---- the report's text ---------------------------------------------- */

static const char *what_happened(DWORD code)
{
    switch (code) {
    case 0xC0000005u: return "access violation";
    case 0xC00000FDu: return "stack overflow";
    case 0xC000001Du: return "illegal instruction";
    case 0xC0000094u: return "integer division by zero";
    case 0xC0000096u: return "privileged instruction";
    case 0xC0000006u: return "a page could not be read from disk";
    case 0x80000003u: return "breakpoint";
    case 0xE06D7363u: return "C++ exception that nothing caught";
    default: return "exception";
    }
}

static void put_frame(out *o, const lares_crash_frame *f)
{
    if (f->module[0]) {
        put(o, f->module);
        put_char(o, '+');
    }
    put_hex(o, f->offset);
}

int lares_crash_format(const lares_crash_info *info, const lares_crash_private *priv, char *text, int capacity)
{
    out o = { text, 0, capacity };
    put(&o, "Lares crash report\r\n");
    put(&o, "This file was written on this PC. It has not been sent anywhere.\r\n\r\n");
    put(&o, "Program: ");
    put(&o, info->program);
    put(&o, "\r\nVersion: ");
    put(&o, info->version);
    put(&o, "\r\nWindows: ");
    put_number(&o, info->windows_major, 0);
    put_char(&o, '.');
    put_number(&o, info->windows_minor, 0);
    put_char(&o, '.');
    put_number(&o, info->windows_build, 0);
    put(&o, " (");
    put(&o, info->machine);
    put(&o, ")\r\nWhen: ");
    put_number(&o, info->when.wYear, 4);
    put_char(&o, '-');
    put_number(&o, info->when.wMonth, 2);
    put_char(&o, '-');
    put_number(&o, info->when.wDay, 2);
    put_char(&o, ' ');
    put_number(&o, info->when.wHour, 2);
    put_char(&o, ':');
    put_number(&o, info->when.wMinute, 2);
    put_char(&o, ':');
    put_number(&o, info->when.wSecond, 2);
    put(&o, " UTC\r\nWhat happened: ");
    put(&o, what_happened(info->code));
    put(&o, " (");
    put_hex(&o, info->code);
    put_char(&o, ')');
    if (info->access >= 0) {
        put(&o, info->access == 1 ? ", writing address " : (info->access == 8 ? ", running address " : ", reading address "));
        put_hex(&o, info->address);
    }
    put(&o, "\r\nFaulting module: ");
    if (info->frame_count > 0) put_frame(&o, &info->frames[0]);
    else put(&o, "unknown");
    put(&o, "\r\nCall stack:\r\n");
    for (int i = 0; i < info->frame_count && i < LARES_CRASH_FRAMES; ++i) {
        put(&o, "  ");
        put_number(&o, (ULONGLONG)i, 2);
        put(&o, "  ");
        put_frame(&o, &info->frames[i]);
        put(&o, "\r\n");
    }
    o.text[o.length] = 0;
    return lares_crash_scrub(text, o.length, capacity, priv);
}

/* ---- where reports live --------------------------------------------- */

int lares_crash_dir(wchar_t *dir, int capacity)
{
    DWORD n = GetEnvironmentVariableW(L"LARES_CRASH_DIR", dir, (DWORD)capacity);
    if (n > 0 && n < (DWORD)capacity) return 1;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, (DWORD)capacity);
    if (n == 0 || n + 16 >= (DWORD)capacity) return 0;
    lstrcatW(dir, L"\\Lares\\crash");
    return 1;
}

/* The newest (or the oldest) report in the folder, and how many there are. */
static int pick_report(const wchar_t *dir, int oldest, wchar_t *path, int capacity)
{
    wchar_t pattern[MAX_PATH + 8];
    WIN32_FIND_DATAW found, best;
    int count = 0;
    ZeroMemory(&best, sizeof best);
    if (lstrlenW(dir) + 8 > MAX_PATH) return 0;
    lstrcpyW(pattern, dir);
    lstrcatW(pattern, L"\\*.txt");
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        LONG order = count ? CompareFileTime(&found.ftLastWriteTime, &best.ftLastWriteTime) : 0;
        if (!count || (oldest ? order < 0 : order > 0)) best = found;
        ++count;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (count && path) {
        if (lstrlenW(dir) + lstrlenW(best.cFileName) + 2 > capacity) return 0;
        lstrcpyW(path, dir);
        lstrcatW(path, L"\\");
        lstrcatW(path, best.cFileName);
    }
    return count;
}

int lares_crash_latest(wchar_t *path, int capacity)
{
    wchar_t dir[MAX_PATH];
    return lares_crash_dir(dir, MAX_PATH) && pick_report(dir, 0, path, capacity) > 0;
}

int lares_crash_read(const wchar_t *path, char *text, int capacity)
{
    DWORD got = 0;
    if (capacity <= 0) return 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(f, text, (DWORD)capacity - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    text[got] = 0;
    return (int)got;
}

/* ---- the handler ---------------------------------------------------- */

static struct {
    char program[40], version[48];
    char user_folder[MAX_PATH * 3], account[128], computer[128];
    wchar_t dir[MAX_PATH];
    DWORD major, minor, build;
    LONG crashed;
} c;

static void copy_text(char *to, int capacity, const char *from)
{
    int i = 0;
    for (; from && from[i] && i < capacity - 1; ++i) to[i] = from[i];
    to[i] = 0;
}

static void environment(const wchar_t *name, char *to, int capacity)
{
    wchar_t wide[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, wide, MAX_PATH);
    to[0] = 0;
    if (n > 0 && n < MAX_PATH) WideCharToMultiByte(CP_UTF8, 0, wide, -1, to, capacity, NULL, NULL);
}

static void frame_for(ULONGLONG address, lares_crash_frame *frame)
{
    wchar_t path[MAX_PATH];
    HMODULE module = NULL;
    frame->module[0] = 0;
    frame->offset = address;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const wchar_t *)(ULONG_PTR)address, &module) ||
        !module)
        return;
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    /* The module's own file name, never the folder it is in. */
    const wchar_t *name = path + n;
    while (name > path && name[-1] != L'\\' && name[-1] != L'/') --name;
    int i = 0;
    for (; name[i] && i < (int)sizeof frame->module - 1; ++i)
        frame->module[i] = name[i] < 0x80 && name[i] >= 0x20 ? (char)name[i] : '?';
    frame->module[i] = 0;
    frame->offset = address - (ULONGLONG)(ULONG_PTR)module;
}

static void walk(const CONTEXT *start, lares_crash_info *info)
{
#if defined(_M_X64) || defined(_M_ARM64)
    CONTEXT context = *start;
    while (info->frame_count < LARES_CRASH_FRAMES) {
#if defined(_M_X64)
        DWORD64 pc = context.Rip;
#else
        DWORD64 pc = context.Pc;
#endif
        if (!pc) break;
        frame_for(pc, &info->frames[info->frame_count++]);

        DWORD64 base = 0, established = 0;
        void *handler_data = NULL;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &base, NULL);
        if (function) {
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, function, &context, &handler_data, &established, NULL);
        } else {
            /* A leaf: the return address is where the stack points. */
#if defined(_M_X64)
            DWORD64 back = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (const void *)context.Rsp, &back, sizeof back, &got) ||
                got != sizeof back)
                break;
            context.Rip = back;
            context.Rsp += 8;
#else
            if (context.Pc == context.Lr) break;
            context.Pc = context.Lr;
#endif
        }
    }
#else
    frame_for((ULONGLONG)(ULONG_PTR)start->Eip, &info->frames[info->frame_count++]);
#endif
}

static void write_report(const wchar_t *dir, const SYSTEMTIME *when, const char *text, int length)
{
    wchar_t path[MAX_PATH + 80], stamp[40];
    wchar_t program[40];
    int at = 0;
    if (lstrlenW(dir) >= MAX_PATH) return;
    /* The folder and the one above it. */
    lstrcpyW(path, dir);
    for (int i = lstrlenW(path) - 1; i > 0; --i) {
        if (path[i] != L'\\') continue;
        path[i] = 0;
        CreateDirectoryW(path, NULL);
        path[i] = L'\\';
        break;
    }
    CreateDirectoryW(dir, NULL);

    for (; c.program[at] && at < 39; ++at) program[at] = (wchar_t)c.program[at];
    program[at] = 0;
    const WORD parts[] = { when->wYear, when->wMonth, when->wDay, when->wHour, when->wMinute, when->wSecond };
    const int widths[] = { 4, 2, 2, 2, 2, 2 };
    at = 0;
    for (int p = 0; p < 6; ++p) {
        WORD v = parts[p];
        if (p == 3) stamp[at++] = L'-';
        for (int d = widths[p] - 1; d >= 0; --d) {
            stamp[at + d] = (wchar_t)(L'0' + v % 10);
            v /= 10;
        }
        at += widths[p];
    }
    stamp[at] = 0;
    lstrcatW(path, L"\\");
    lstrcatW(path, program);
    lstrcatW(path, L"-");
    lstrcatW(path, stamp);
    lstrcatW(path, L".txt");

    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put_bytes = 0;
    WriteFile(f, text, (DWORD)length, &put_bytes, NULL);
    FlushFileBuffers(f);
    CloseHandle(f);
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *crash)
{
    DWORD code = crash && crash->ExceptionRecord ? crash->ExceptionRecord->ExceptionCode : 0xE0000000u;
    /* A crash while writing a report, or on a second thread: just end. */
    if (InterlockedExchange(&c.crashed, 1)) TerminateProcess(GetCurrentProcess(), code);

    /* Fresh memory from Windows, not from a heap that may be broken. */
    char *memory = (char *)VirtualAlloc(NULL, sizeof(lares_crash_info) + LARES_CRASH_REPORT_MAX,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (memory && crash && crash->ExceptionRecord && crash->ContextRecord) {
        lares_crash_info *info = (lares_crash_info *)memory;
        char *text = memory + sizeof *info;
        const EXCEPTION_RECORD *record = crash->ExceptionRecord;
        lares_crash_private priv = { c.user_folder[0] ? c.user_folder : NULL, c.account[0] ? c.account : NULL,
                                     c.computer[0] ? c.computer : NULL };
        info->program = c.program;
        info->version = c.version;
        info->windows_major = c.major;
        info->windows_minor = c.minor;
        info->windows_build = c.build;
#if defined(_M_X64)
        info->machine = "x64";
#elif defined(_M_ARM64)
        info->machine = "arm64";
#else
        info->machine = "x86";
#endif
        GetSystemTime(&info->when);
        info->code = code;
        info->access = -1;
        if (code == 0xC0000005u && record->NumberParameters >= 2) {
            info->access = (int)record->ExceptionInformation[0];
            info->address = record->ExceptionInformation[1];
        }
        walk(crash->ContextRecord, info);
        int length = lares_crash_format(info, &priv, text, LARES_CRASH_REPORT_MAX);
        write_report(c.dir, &info->when, text, length);
    }
    /* End now: Windows' own crash handling would keep the dead process
     * around, and whoever waits on it (the guard) waiting. */
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

typedef LONG (WINAPI *get_version_fn)(OSVERSIONINFOW *);

void lares_crash_install(const char *program, const char *version)
{
    wchar_t oldest[MAX_PATH + 80];
    copy_text(c.program, sizeof c.program, program);
    copy_text(c.version, sizeof c.version, version);
    environment(L"USERPROFILE", c.user_folder, sizeof c.user_folder);
    environment(L"USERNAME", c.account, sizeof c.account);
    environment(L"COMPUTERNAME", c.computer, sizeof c.computer);
    if (!lares_crash_dir(c.dir, MAX_PATH)) c.dir[0] = 0;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    get_version_fn get_version = ntdll ? (get_version_fn)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v;
    ZeroMemory(&v, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (get_version && get_version(&v) == 0) {
        c.major = v.dwMajorVersion;
        c.minor = v.dwMinorVersion;
        c.build = v.dwBuildNumber;
    }

    for (int i = 0; i < 64 && c.dir[0] && pick_report(c.dir, 1, oldest, ARRAYSIZE(oldest)) > LARES_CRASH_KEEP; ++i)
        DeleteFileW(oldest);

    if (c.dir[0]) SetUnhandledExceptionFilter(on_crash);
}
