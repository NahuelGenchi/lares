/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The change journal (journal.h).
 *
 * The file is text, one record a line, each ending in a checksum:
 *
 *   lares-journal 1
 *   B <id> <time> <feature> <kind> <target> <before> <after> *<crc32>
 *   D <id> *<crc32>          the change was made
 *   U <id> *<crc32>          the change was undone
 *
 * A value is "-" when the setting was not there, otherwise its type, a colon
 * and its bytes in hex. A target is UTF-8 with anything but plain letters,
 * digits and a few marks written as %XX. A line whose checksum does not
 * match, or that has no end, was cut short by a crash or a power cut and is
 * ignored; the next record starts on a line of its own.
 *
 * Every record is appended with one write and flushed (FlushFileBuffers)
 * before the call returns. A named mutex keeps Lares' programs from writing
 * at the same time.
 */
#include "journal.h"

#define LINE_MAX_BYTES 8192
#define FILE_MAX_BYTES (8u * 1024u * 1024u)
#define HEADER "lares-journal 1\n"

/* ---- text ----------------------------------------------------------- */

typedef struct {
    char *text;
    int length, capacity;
} line;

static void put_char(line *l, char ch)
{
    if (l->length < l->capacity) l->text[l->length++] = ch;
}

static void put_text(line *l, const char *s)
{
    while (*s) put_char(l, *s++);
}

static void put_number(line *l, ULONGLONG value)
{
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) put_char(l, digits[--n]);
}

static const char kHex[] = "0123456789abcdef";

static void put_hex_byte(line *l, BYTE b)
{
    put_char(l, kHex[b >> 4]);
    put_char(l, kHex[b & 15]);
}

static int is_plain(char ch)
{
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return 1;
    for (const char *p = "_.:\\/|{}()-"; *p; ++p)
        if (ch == *p) return 1;
    return 0;
}

/* A feature or a kind: lower-case letters, digits, '-' and '_' only. */
static void put_word(line *l, const char *word)
{
    int n = 0;
    for (; *word && n < 31; ++word, ++n) {
        char ch = *word;
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        put_char(l, ok ? ch : '_');
    }
    if (!n) put_char(l, '_');
}

static void put_target(line *l, const wchar_t *target)
{
    char utf8[LARES_JOURNAL_TARGET_MAX * 3 + 1];
    int n = WideCharToMultiByte(CP_UTF8, 0, target, -1, utf8, sizeof utf8, NULL, NULL);
    if (n <= 1) {
        put_char(l, '%');
        put_hex_byte(l, 0);
        return;
    }
    for (int i = 0; i < n - 1; ++i) {
        if (is_plain(utf8[i])) {
            put_char(l, utf8[i]);
        } else {
            put_char(l, '%');
            put_hex_byte(l, (BYTE)utf8[i]);
        }
    }
}

static void put_value(line *l, const lares_journal_value *v)
{
    if (!v->present) {
        put_char(l, '-');
        return;
    }
    put_number(l, v->type);
    put_char(l, ':');
    for (DWORD i = 0; i < v->size && i < LARES_JOURNAL_VALUE_MAX; ++i) put_hex_byte(l, v->data[i]);
}

static DWORD crc32(const char *bytes, int n)
{
    DWORD crc = 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) {
        crc ^= (BYTE)bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put_end(line *l)
{
    DWORD crc = crc32(l->text, l->length);
    put_text(l, " *");
    for (int shift = 24; shift >= 0; shift -= 8) put_hex_byte(l, (BYTE)(crc >> shift));
    put_char(l, '\n');
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* ---- reading -------------------------------------------------------- */

typedef struct {
    const char *at, *end;
} cursor;

/* The next word of a line, up to a space. */
static int next_word(cursor *c, const char **word, int *length)
{
    if (c->at >= c->end) return 0;
    *word = c->at;
    while (c->at < c->end && *c->at != ' ') ++c->at;
    *length = (int)(c->at - *word);
    if (c->at < c->end) ++c->at;
    return *length > 0;
}

static int read_number(cursor *c, ULONGLONG *value)
{
    const char *w;
    int n;
    *value = 0;
    if (!next_word(c, &w, &n)) return 0;
    for (int i = 0; i < n; ++i) {
        if (w[i] < '0' || w[i] > '9') return 0;
        *value = *value * 10 + (ULONGLONG)(w[i] - '0');
    }
    return 1;
}

static int read_word(cursor *c, char *out, int capacity)
{
    const char *w;
    int n;
    if (!next_word(c, &w, &n) || n >= capacity) return 0;
    for (int i = 0; i < n; ++i) out[i] = w[i];
    out[n] = 0;
    return 1;
}

static int read_target(cursor *c, wchar_t *out, int capacity)
{
    char utf8[LARES_JOURNAL_TARGET_MAX * 3 + 1];
    const char *w;
    int n, at = 0;
    if (!next_word(c, &w, &n)) return 0;
    for (int i = 0; i < n && at < (int)sizeof utf8 - 1; ++i) {
        if (w[i] != '%') {
            utf8[at++] = w[i];
        } else {
            if (i + 2 >= n) return 0;
            int hi = hex_digit(w[i + 1]), lo = hex_digit(w[i + 2]);
            if (hi < 0 || lo < 0) return 0;
            utf8[at++] = (char)(hi * 16 + lo);
            i += 2;
        }
    }
    utf8[at] = 0;
    if (!at || (at == 1 && utf8[0] == 0)) {
        out[0] = 0;
        return 1;
    }
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, capacity) > 0;
}

static int read_value(cursor *c, lares_journal_value *v)
{
    const char *w;
    int n, i = 0;
    ULONGLONG type = 0;
    v->present = 0;
    v->type = 0;
    v->size = 0;
    if (!next_word(c, &w, &n)) return 0;
    if (n == 1 && w[0] == '-') return 1;
    for (; i < n && w[i] != ':'; ++i) {
        if (w[i] < '0' || w[i] > '9') return 0;
        type = type * 10 + (ULONGLONG)(w[i] - '0');
    }
    if (i == 0 || i == n || ((n - i - 1) & 1)) return 0;
    for (++i; i + 1 < n; i += 2) {
        int hi = hex_digit(w[i]), lo = hex_digit(w[i + 1]);
        if (hi < 0 || lo < 0 || v->size >= LARES_JOURNAL_VALUE_MAX) return 0;
        v->data[v->size++] = (BYTE)(hi * 16 + lo);
    }
    v->present = 1;
    v->type = (DWORD)type;
    return 1;
}

/* A whole line, without its end: is its checksum right? Then `length`
 * becomes the length of what the checksum covers. */
static int line_is_whole(const char *text, int *length)
{
    int n = *length;
    DWORD stored = 0;
    if (n < 10 || text[n - 10] != ' ' || text[n - 9] != '*') return 0;
    for (int i = n - 8; i < n; ++i) {
        int d = hex_digit(text[i]);
        if (d < 0) return 0;
        stored = (stored << 4) | (DWORD)d;
    }
    if (crc32(text, n - 10) != stored) return 0;
    *length = n - 10;
    return 1;
}

typedef struct {
    lares_journal_entry *entries;
    int count;
    ULONGLONG last_id;
    int torn_tail; /* the file does not end with a line end */
    int exists;
} loaded;

static void unload(loaded *j)
{
    if (j->entries) HeapFree(GetProcessHeap(), 0, j->entries);
    j->entries = NULL;
    j->count = 0;
}

static lares_journal_entry *find_entry(loaded *j, ULONGLONG id)
{
    for (int i = j->count - 1; i >= 0; --i)
        if (j->entries[i].id == id) return &j->entries[i];
    return NULL;
}

static void parse(const char *text, int size, loaded *j)
{
    int begins = 0;
    for (int i = 0; i < size; ++i)
        if (text[i] == 'B' && (i == 0 || text[i - 1] == '\n')) ++begins;
    if (begins) j->entries = (lares_journal_entry *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)begins * sizeof *j->entries);
    j->torn_tail = size > 0 && text[size - 1] != '\n';

    for (int at = 0; at < size;) {
        int end = at;
        while (end < size && text[end] != '\n') ++end;
        if (end == size) break; /* no line end: cut short */
        int n = end - at;
        const char *l = text + at;
        at = end + 1;
        if (!line_is_whole(l, &n) || n < 3 || l[1] != ' ') continue;

        cursor c = { l + 2, l + n };
        ULONGLONG id;
        if (!read_number(&c, &id)) continue;
        if (id > j->last_id) j->last_id = id;
        if (l[0] == 'B' && j->entries && j->count < begins) {
            lares_journal_entry *e = &j->entries[j->count];
            e->id = id;
            e->state = LARES_JOURNAL_PENDING;
            if (read_number(&c, &e->time) && read_word(&c, e->feature, sizeof e->feature) &&
                read_word(&c, e->kind, sizeof e->kind) && read_target(&c, e->target, ARRAYSIZE(e->target)) &&
                read_value(&c, &e->before) && read_value(&c, &e->after))
                ++j->count;
        } else if (l[0] == 'D' || l[0] == 'U') {
            lares_journal_entry *e = find_entry(j, id);
            if (e && e->state != LARES_JOURNAL_UNDONE) e->state = l[0] == 'D' ? LARES_JOURNAL_DONE : LARES_JOURNAL_UNDONE;
        }
    }
}

static int load(const lares_journal *journal, loaded *j)
{
    ZeroMemory(j, sizeof *j);
    HANDLE f = CreateFileW(journal->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    j->exists = 1;
    DWORD size = GetFileSize(f, NULL), got = 0;
    int ok = size != INVALID_FILE_SIZE && size <= FILE_MAX_BYTES;
    char *text = ok && size ? (char *)HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    if (ok && size) ok = text && ReadFile(f, text, size, &got, NULL) && got == size;
    CloseHandle(f);
    if (ok && size) parse(text, (int)size, j);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    return ok;
}

/* ---- writing -------------------------------------------------------- */

static HANDLE lock_journal(void)
{
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\com.nahuelgenchi.Lares.journal");
    if (mutex) WaitForSingleObject(mutex, INFINITE);
    return mutex;
}

static void unlock_journal(HANDLE mutex)
{
    if (!mutex) return;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

static void make_folder(const wchar_t *path)
{
    wchar_t folder[MAX_PATH];
    int n = lstrlenW(path);
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
    if (n <= 1 || n >= MAX_PATH) return;
    lstrcpynW(folder, path, n);
    CreateDirectoryW(folder, NULL);
}

/* One record, written whole and flushed. */
static int append(const lares_journal *journal, const line *record, int torn_tail)
{
    DWORD put = 0;
    LARGE_INTEGER size;
    make_folder(journal->path);
    HANDLE f = CreateFileW(journal->path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int ok = GetFileSizeEx(f, &size);
    if (ok && size.QuadPart == 0) ok = WriteFile(f, HEADER, sizeof HEADER - 1, &put, NULL) && put == sizeof HEADER - 1;
    else if (ok && torn_tail) ok = WriteFile(f, "\n", 1, &put, NULL) && put == 1;
    ok = ok && record->length < record->capacity && WriteFile(f, record->text, (DWORD)record->length, &put, NULL) &&
         put == (DWORD)record->length && FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}

static int append_mark(const lares_journal *journal, char mark, ULONGLONG id, int torn_tail)
{
    char text[64];
    line l = { text, 0, sizeof text };
    put_char(&l, mark);
    put_char(&l, ' ');
    put_number(&l, id);
    put_end(&l);
    return append(journal, &l, torn_tail);
}

static ULONGLONG now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* ---- the interface -------------------------------------------------- */

int lares_journal_open(lares_journal *journal, const wchar_t *path)
{
    ZeroMemory(journal, sizeof *journal);
    if (path) {
        if (lstrlenW(path) >= MAX_PATH) return 0;
        lstrcpyW(journal->path, path);
        return 1;
    }
    DWORD n = GetEnvironmentVariableW(L"LARES_JOURNAL", journal->path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return 1;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", journal->path, MAX_PATH);
    if (n == 0 || n + 20 >= MAX_PATH) return 0;
    lstrcatW(journal->path, L"\\Lares\\journal.log");
    return 1;
}

int lares_journal_add_backend(lares_journal *journal, const lares_journal_backend *backend)
{
    if (journal->backend_count >= LARES_JOURNAL_BACKENDS) return 0;
    journal->backends[journal->backend_count++] = *backend;
    return 1;
}

static const lares_journal_backend *backend_for(const lares_journal *journal, const char *kind)
{
    for (int i = 0; i < journal->backend_count; ++i)
        if (lstrcmpA(journal->backends[i].kind, kind) == 0) return &journal->backends[i];
    return NULL;
}

void lares_journal_value_dword(lares_journal_value *value, DWORD number)
{
    value->present = 1;
    value->type = REG_DWORD;
    value->size = sizeof number;
    for (int i = 0; i < 4; ++i) value->data[i] = (BYTE)(number >> (8 * i));
}

void lares_journal_value_absent(lares_journal_value *value)
{
    value->present = 0;
    value->type = 0;
    value->size = 0;
}

int lares_journal_value_equal(const lares_journal_value *a, const lares_journal_value *b)
{
    if (!a->present || !b->present) return !a->present && !b->present;
    if (a->type != b->type || a->size != b->size) return 0;
    for (DWORD i = 0; i < a->size && i < LARES_JOURNAL_VALUE_MAX; ++i)
        if (a->data[i] != b->data[i]) return 0;
    return 1;
}

int lares_journal_begin(lares_journal *journal, const char *feature, const char *kind, const wchar_t *target,
                        const lares_journal_value *before, const lares_journal_value *after, ULONGLONG *id)
{
    loaded j;
    int ok = 0;
    if (lstrlenW(target) >= LARES_JOURNAL_TARGET_MAX) return 0;
    char *text = (char *)HeapAlloc(GetProcessHeap(), 0, LINE_MAX_BYTES);
    if (!text) return 0;

    HANDLE mutex = lock_journal();
    if (load(journal, &j)) {
        line l = { text, 0, LINE_MAX_BYTES };
        ULONGLONG next = j.last_id + 1;
        put_text(&l, "B ");
        put_number(&l, next);
        put_char(&l, ' ');
        put_number(&l, now());
        put_char(&l, ' ');
        put_word(&l, feature);
        put_char(&l, ' ');
        put_word(&l, kind);
        put_char(&l, ' ');
        put_target(&l, target);
        put_char(&l, ' ');
        put_value(&l, before);
        put_char(&l, ' ');
        put_value(&l, after);
        put_end(&l);
        ok = append(journal, &l, j.torn_tail);
        if (ok && id) *id = next;
    }
    unload(&j);
    unlock_journal(mutex);
    HeapFree(GetProcessHeap(), 0, text);
    return ok;
}

static int mark(lares_journal *journal, char which, ULONGLONG id)
{
    loaded j;
    HANDLE mutex = lock_journal();
    int ok = load(journal, &j) && append_mark(journal, which, id, j.torn_tail);
    unload(&j);
    unlock_journal(mutex);
    return ok;
}

int lares_journal_complete(lares_journal *journal, ULONGLONG id)
{
    return mark(journal, 'D', id);
}

int lares_journal_change(lares_journal *journal, const char *feature, const char *kind, const wchar_t *target,
                         const lares_journal_value *after, ULONGLONG *id)
{
    lares_journal_value before;
    ULONGLONG entry = 0;
    const lares_journal_backend *b = backend_for(journal, kind);
    if (id) *id = 0;
    if (!b || !b->read(target, &before, b->ctx)) return 0;
    if (lares_journal_value_equal(&before, after)) return 1;

    /* Nothing is touched unless the entry is safely on disk. */
    if (!lares_journal_begin(journal, feature, kind, target, &before, after, &entry)) return 0;
    if (!b->write(target, after, b->ctx)) {
        mark(journal, 'U', entry); /* nothing was changed, so nothing is owed */
        return 0;
    }
    lares_journal_complete(journal, entry);
    if (id) *id = entry;
    return 1;
}

/* Undo, and with `forget` its twin that only marks. */
static int undo(lares_journal *journal, const char *feature, ULONGLONG id, int force, int forget,
                lares_journal_undo_result *result)
{
    lares_journal_undo_result r = { 0, 0, 0, 0 };
    lares_journal_value current;
    loaded j;
    int left = 0;

    HANDLE mutex = lock_journal();
    if (!load(journal, &j)) {
        unlock_journal(mutex);
        if (result) *result = r;
        return 0;
    }
    int torn = j.torn_tail;
    for (int i = j.count - 1; i >= 0; --i) {
        lares_journal_entry *e = &j.entries[i];
        if (e->state == LARES_JOURNAL_UNDONE) continue;
        if ((id && e->id != id) || (feature && lstrcmpA(e->feature, feature) != 0)) {
            ++left;
            continue;
        }
        const lares_journal_backend *b = backend_for(journal, e->kind);
        int undone = 0;
        if (forget) {
            undone = 1;
        } else if (!b || !b->read(e->target, &current, b->ctx)) {
            ++r.failed;
        } else if (lares_journal_value_equal(&current, &e->before)) {
            ++r.already;
            undone = 1;
        } else if (!force && !lares_journal_value_equal(&current, &e->after)) {
            ++r.changed;
        } else if (b->write(e->target, &e->before, b->ctx)) {
            ++r.restored;
            undone = 1;
        } else {
            ++r.failed;
        }
        if (undone && append_mark(journal, 'U', e->id, torn)) {
            torn = 0;
            e->state = LARES_JOURNAL_UNDONE;
        } else {
            ++left;
        }
    }
    /* With nothing left to undo the file has done its work. */
    if (j.exists && !left) DeleteFileW(journal->path);
    unload(&j);
    unlock_journal(mutex);
    if (result) *result = r;
    return r.failed == 0;
}

int lares_journal_undo(lares_journal *journal, const char *feature, ULONGLONG id, int force,
                       lares_journal_undo_result *result)
{
    return undo(journal, feature, id, force, 0, result);
}

int lares_journal_forget(lares_journal *journal, const char *feature, ULONGLONG id)
{
    return undo(journal, feature, id, 0, 1, NULL);
}

int lares_journal_list(lares_journal *journal, int (*each)(const lares_journal_entry *entry, void *ctx), void *ctx)
{
    loaded j;
    int count = 0, going = 1;
    HANDLE mutex = lock_journal();
    int ok = load(journal, &j);
    unlock_journal(mutex);
    if (!ok) return 0;
    for (int i = 0; i < j.count; ++i) {
        if (j.entries[i].state == LARES_JOURNAL_UNDONE) continue;
        ++count;
        if (going && each) going = each(&j.entries[i], ctx);
    }
    unload(&j);
    return count;
}
