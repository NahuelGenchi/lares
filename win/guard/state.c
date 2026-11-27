/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The guard's state file. Plain Win32, no C runtime: the guard links without
 * one to stay small.
 */
#include "state.h"

#define STATE_FILE L"\\guard.state"
#define STATE_TEMP L"\\guard.state.new."
#define STATE_HEADER "lares-guard-state 1"

int lares_guard_state_dir(wchar_t *dir, int capacity)
{
    DWORD n = GetEnvironmentVariableW(L"LARES_GUARD_STATE_DIR", dir, (DWORD)capacity);
    if (n > 0 && n < (DWORD)capacity) return 1;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, (DWORD)capacity);
    if (n == 0 || n + 8 >= (DWORD)capacity) return 0;
    lstrcatW(dir, L"\\Lares");
    return 1;
}

ULONGLONG lares_guard_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static int put_text(char *text, int capacity, int at, const char *s)
{
    while (*s && at < capacity - 1) text[at++] = *s++;
    return at;
}

static int put_number(char *text, int capacity, int at, ULONGLONG value)
{
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n && at < capacity - 1) text[at++] = digits[--n];
    return at;
}

static int put_line(char *text, int capacity, int at, const char *key, ULONGLONG value)
{
    at = put_text(text, capacity, at, key);
    at = put_text(text, capacity, at, " ");
    at = put_number(text, capacity, at, value);
    return put_text(text, capacity, at, "\n");
}

int lares_guard_state_format(const lares_guard_state *s, char *text, int capacity)
{
    int at = put_text(text, capacity, 0, STATE_HEADER "\n");
    at = put_line(text, capacity, at, "running", s->running ? 1 : 0);
    at = put_line(text, capacity, at, "hidden", (ULONGLONG)(s->hidden > 0 ? s->hidden : 0));
    at = put_line(text, capacity, at, "guard", s->guard_pid);
    at = put_line(text, capacity, at, "twin", s->twin_pid);
    at = put_line(text, capacity, at, "shell", s->shell_pid);
    for (int i = 0; i < s->crash_count && i < LARES_GUARD_MAX_CRASHES; ++i)
        at = put_line(text, capacity, at, "crash", s->crashes[i]);
    at = put_text(text, capacity, at, "end\n");
    text[at] = 0;
    return at;
}

static int key_is(const char *line, int length, const char *key)
{
    int i = 0;
    while (key[i] && i < length && line[i] == key[i]) ++i;
    return key[i] == 0 && i < length && line[i] == ' ';
}

static ULONGLONG number_after_key(const char *line, int length)
{
    int i = 0;
    ULONGLONG v = 0;
    while (i < length && line[i] != ' ') ++i;
    while (i < length && line[i] == ' ') ++i;
    while (i < length && line[i] >= '0' && line[i] <= '9') v = v * 10 + (ULONGLONG)(line[i++] - '0');
    return v;
}

void lares_guard_state_parse(const char *text, int length, lares_guard_state *out)
{
    lares_guard_state s;
    ZeroMemory(&s, sizeof s);
    ZeroMemory(out, sizeof *out);

    int line_no = 0, complete = 0;
    for (int at = 0; at < length;) {
        int end = at;
        while (end < length && text[end] != '\n') ++end;
        if (end == length) break; /* a line with no end was cut short */
        const char *line = text + at;
        int n = end - at;
        if (line_no == 0) {
            const char *h = STATE_HEADER;
            int i = 0;
            while (h[i] && i < n && line[i] == h[i]) ++i;
            if (h[i] || i != n) return;
        } else if (n == 3 && line[0] == 'e' && line[1] == 'n' && line[2] == 'd') {
            complete = 1;
            break;
        } else if (key_is(line, n, "running")) {
            s.running = number_after_key(line, n) != 0;
        } else if (key_is(line, n, "hidden")) {
            ULONGLONG v = number_after_key(line, n);
            s.hidden = v > LARES_TASKBAR_GIVING_BACK ? LARES_TASKBAR_HIDDEN : (int)v;
        } else if (key_is(line, n, "guard")) {
            s.guard_pid = (DWORD)number_after_key(line, n);
        } else if (key_is(line, n, "twin")) {
            s.twin_pid = (DWORD)number_after_key(line, n);
        } else if (key_is(line, n, "shell")) {
            s.shell_pid = (DWORD)number_after_key(line, n);
        } else if (key_is(line, n, "crash") && s.crash_count < LARES_GUARD_MAX_CRASHES) {
            s.crashes[s.crash_count++] = number_after_key(line, n);
        }
        ++line_no;
        at = end + 1;
    }
    /* A file without its last line was cut short: believe none of it. */
    if (complete) *out = s;
}

static int state_path(const wchar_t *dir, const wchar_t *name, wchar_t *path, int capacity)
{
    if (lstrlenW(dir) + lstrlenW(name) + 1 > capacity) return 0;
    lstrcpyW(path, dir);
    lstrcatW(path, name);
    return 1;
}

/* Another program (a scanner, a backup) may hold the file for a moment.
 * Reading and replacing it wait for that, a quarter of a second at most. */
#define BUSY_TRIES 50
#define BUSY_WAIT_MS 5

static int busy(void)
{
    DWORD e = GetLastError();
    return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION;
}

void lares_guard_state_load(const wchar_t *dir, lares_guard_state *state)
{
    wchar_t path[MAX_PATH + 32];
    char text[1024];
    HANDLE f = INVALID_HANDLE_VALUE;
    ZeroMemory(state, sizeof *state);
    if (!state_path(dir, STATE_FILE, path, ARRAYSIZE(path))) return;
    for (int i = 0; i < BUSY_TRIES; ++i) {
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE || !busy()) break;
        Sleep(BUSY_WAIT_MS);
    }
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    if (ReadFile(f, text, sizeof text, &got, NULL)) lares_guard_state_parse(text, (int)got, state);
    CloseHandle(f);
}

int lares_guard_state_save(const wchar_t *dir, const lares_guard_state *state)
{
    wchar_t path[MAX_PATH + 48], temp[MAX_PATH + 48], digits[12];
    char text[1024];
    if (!state_path(dir, STATE_FILE, path, ARRAYSIZE(path)) ||
        !state_path(dir, STATE_TEMP, temp, ARRAYSIZE(temp) - 12))
        return 0;
    /* Each process writes a new file of its own, so two never share one. */
    DWORD pid = GetCurrentProcessId();
    int at = lstrlenW(temp), n = 0;
    do {
        digits[n++] = (wchar_t)(L'0' + pid % 10);
        pid /= 10;
    } while (pid);
    while (n) temp[at++] = digits[--n];
    temp[at] = 0;
    CreateDirectoryW(dir, NULL);

    int length = lares_guard_state_format(state, text, sizeof text);
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    BOOL ok = WriteFile(f, text, (DWORD)length, &put, NULL) && put == (DWORD)length && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(temp);
        return 0;
    }
    for (int i = 0; i < BUSY_TRIES; ++i) {
        if (MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
        if (!busy()) break;
        Sleep(BUSY_WAIT_MS);
    }
    DeleteFileW(temp);
    return 0;
}

void lares_guard_state_tidy(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH + 48], path[MAX_PATH + 48 + 16];
    WIN32_FIND_DATAW found;
    if (!state_path(dir, STATE_TEMP L"*", pattern, ARRAYSIZE(pattern))) return;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (lstrlenW(dir) + lstrlenW(found.cFileName) + 2 > ARRAYSIZE(path)) continue;
        lstrcpyW(path, dir);
        lstrcatW(path, L"\\");
        lstrcatW(path, found.cFileName);
        DeleteFileW(path);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

int lares_guard_state_add_crash(lares_guard_state *s, ULONGLONG now)
{
    int kept = 0;
    for (int i = 0; i < s->crash_count && i < LARES_GUARD_MAX_CRASHES; ++i) {
        ULONGLONG t = s->crashes[i];
        if (t <= now && now - t < LARES_GUARD_CRASH_WINDOW) s->crashes[kept++] = t;
    }
    if (kept == LARES_GUARD_MAX_CRASHES) {
        for (int i = 1; i < kept; ++i) s->crashes[i - 1] = s->crashes[i];
        --kept;
    }
    s->crashes[kept++] = now;
    s->crash_count = kept;
    return kept;
}
