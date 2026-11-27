/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * lares-journal-test: the change journal as plain data (with a made-up
 * backend), on the real registry under a test key, and across a "power
 * cut": a copy of this program that ends itself between the journal write
 * and the change, and between the change and the completion mark.
 *
 *   lares-journal-test                all of it
 *   lares-journal-test --cut N        used by the test itself
 *   lares-journal-test --make FILE    a journal with two changes, to look
 *                                     at Settings with in a dry run
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "journal.h"
#include "journal_win.h"

static int g_failures, g_checks;
static wchar_t g_file[MAX_PATH];

static void check(int ok, const char *what)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    printf("  FAILED: %s\n", what);
}

/* ---- a made-up backend: a few named numbers in memory ---------------- */

typedef struct {
    wchar_t name[64];
    lares_journal_value value;
} slot;

static slot g_slots[8];
static int g_fail_writes;

static slot *slot_for(const wchar_t *name)
{
    for (int i = 0; i < 8; ++i)
        if (!wcscmp(g_slots[i].name, name)) return &g_slots[i];
    for (int i = 0; i < 8; ++i)
        if (!g_slots[i].name[0]) {
            wcscpy(g_slots[i].name, name);
            return &g_slots[i];
        }
    return NULL;
}

static int fake_read(const wchar_t *target, lares_journal_value *value, void *ctx)
{
    (void)ctx;
    *value = slot_for(target)->value;
    return 1;
}

static int fake_write(const wchar_t *target, const lares_journal_value *value, void *ctx)
{
    (void)ctx;
    if (g_fail_writes) return 0;
    slot_for(target)->value = *value;
    return 1;
}

static const lares_journal_backend kFake = { "fake", fake_read, fake_write, NULL };

static void set_slot(const wchar_t *name, int present, DWORD number)
{
    if (present) lares_journal_value_dword(&slot_for(name)->value, number);
    else lares_journal_value_absent(&slot_for(name)->value);
}

static int slot_is(const wchar_t *name, int present, DWORD number)
{
    lares_journal_value want;
    if (present) lares_journal_value_dword(&want, number);
    else lares_journal_value_absent(&want);
    return lares_journal_value_equal(&slot_for(name)->value, &want);
}

static void fresh(lares_journal *j)
{
    DeleteFileW(g_file);
    memset(g_slots, 0, sizeof g_slots);
    g_fail_writes = 0;
    lares_journal_open(j, g_file);
    lares_journal_add_backend(j, &kFake);
    lares_journal_add_backend(j, &lares_journal_registry);
}

static int change(lares_journal *j, const char *feature, const wchar_t *name, DWORD number, ULONGLONG *id)
{
    lares_journal_value v;
    lares_journal_value_dword(&v, number);
    return lares_journal_change(j, feature, "fake", name, &v, id);
}

static char *read_file(DWORD *size)
{
    HANDLE f = CreateFileW(g_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    *size = GetFileSize(f, NULL);
    char *text = (char *)malloc(*size + 1);
    DWORD got = 0;
    ReadFile(f, text, *size, &got, NULL);
    text[got] = 0;
    CloseHandle(f);
    return text;
}

static void write_file(const char *text, DWORD size)
{
    HANDLE f = CreateFileW(g_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD put = 0;
    WriteFile(f, text, size, &put, NULL);
    CloseHandle(f);
}

static int file_exists(void)
{
    return GetFileAttributesW(g_file) != INVALID_FILE_ATTRIBUTES;
}

typedef struct {
    int count;
    lares_journal_entry last;
} seen;

static int remember(const lares_journal_entry *e, void *ctx)
{
    seen *s = (seen *)ctx;
    ++s->count;
    s->last = *e;
    return 1;
}

/* ---- the cases ------------------------------------------------------ */

static void test_change_and_undo(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    ULONGLONG id = 0;
    seen s = { 0 };
    printf("change, list, undo\n");
    fresh(&j);
    set_slot(L"one", 1, 5);

    check(change(&j, "shell", L"one", 9, &id) && id == 1, "a change is made and gets the first id");
    check(slot_is(L"one", 1, 9), "the setting has the new value");
    check(lares_journal_list(&j, remember, &s) == 1 && s.count == 1, "one entry is listed");
    lares_journal_value five, nine;
    lares_journal_value_dword(&five, 5);
    lares_journal_value_dword(&nine, 9);
    check(s.last.id == 1 && !strcmp(s.last.feature, "shell") && !strcmp(s.last.kind, "fake") &&
              !wcscmp(s.last.target, L"one"),
          "it says which feature, which kind and what");
    check(lares_journal_value_equal(&s.last.before, &five) && lares_journal_value_equal(&s.last.after, &nine),
          "it holds the value before and the value after");
    check(s.last.state == LARES_JOURNAL_DONE && s.last.time != 0, "it is marked done and has a time");

    check(change(&j, "shell", L"one", 9, &id) && id == 0, "asking for the value it already has records nothing");
    check(lares_journal_list(&j, NULL, NULL) == 1, "still one entry");

    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.restored == 1 && r.already + r.changed + r.failed == 0,
          "undo puts it back");
    check(slot_is(L"one", 1, 5), "the setting has its old value");
    check(!file_exists(), "with nothing left to undo, the file is gone");
    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.restored == 0, "undo with no journal does nothing");
}

static void test_order_and_selection(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    ULONGLONG a, b, c;
    printf("several entries: newest first, by feature, by id\n");
    fresh(&j);
    set_slot(L"one", 1, 1);
    set_slot(L"two", 0, 0);

    change(&j, "shell", L"one", 2, &a);
    change(&j, "shell", L"one", 3, &b);
    change(&j, "remote", L"two", 7, &c);
    check(a == 1 && b == 2 && c == 3, "ids rise");

    check(lares_journal_undo(&j, "remote", 0, 0, &r) && r.restored == 1, "undo for one feature");
    check(slot_is(L"two", 0, 0), "a setting that was not there is removed again");
    check(slot_is(L"one", 1, 3), "the other feature's setting is untouched");
    check(lares_journal_list(&j, NULL, NULL) == 2, "two entries left");

    check(lares_journal_undo(&j, NULL, b, 0, &r) && r.restored == 1 && slot_is(L"one", 1, 2), "undo of one entry by id");
    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.restored == 1 && slot_is(L"one", 1, 1), "undo of the rest");

    change(&j, "shell", L"one", 2, &a);
    change(&j, "shell", L"one", 3, &b);
    check(a == 1, "a new file starts its ids again");
    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.restored == 2 && slot_is(L"one", 1, 1),
          "two changes of one setting undo newest first, back to the first value");
}

static void test_changed_by_someone_else(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    printf("a setting someone else changed since\n");
    fresh(&j);
    set_slot(L"one", 1, 1);
    change(&j, "shell", L"one", 2, NULL);
    set_slot(L"one", 1, 40); /* the user changed it in Windows' own settings */

    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.changed == 1 && r.restored == 0, "undo leaves it alone and says so");
    check(slot_is(L"one", 1, 40), "the user's value stays");
    check(lares_journal_list(&j, NULL, NULL) == 1, "the entry stays in the journal");
    check(lares_journal_undo(&j, NULL, 0, 1, &r) && r.restored == 1 && slot_is(L"one", 1, 1), "forced, it goes back");
    check(!file_exists(), "and the file is gone");
}

static void test_failed_write(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    printf("a change that cannot be made\n");
    fresh(&j);
    set_slot(L"one", 1, 1);
    g_fail_writes = 1;
    check(!change(&j, "shell", L"one", 2, NULL), "the change reports failure");
    check(lares_journal_list(&j, NULL, NULL) == 0, "nothing is owed for it");
    g_fail_writes = 0;
    lares_journal_value v;
    lares_journal_value_dword(&v, 2);
    check(!lares_journal_change(&j, "shell", "no-such-kind", L"one", &v, NULL), "an unknown kind changes nothing");

    change(&j, "shell", L"one", 2, NULL);
    g_fail_writes = 1;
    check(!lares_journal_undo(&j, NULL, 0, 0, &r) && r.failed == 1, "an undo that cannot be made says so");
    check(lares_journal_list(&j, NULL, NULL) == 1, "and keeps the entry for later");
}

/* An entry with no completion mark: the program died around the change. */
static void test_no_completion_mark(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    lares_journal_value before, after;
    ULONGLONG id = 0;
    seen s = { 0 };
    printf("entries without a completion mark\n");
    fresh(&j);
    lares_journal_value_dword(&before, 1);
    lares_journal_value_dword(&after, 2);

    set_slot(L"one", 1, 1);
    check(lares_journal_begin(&j, "shell", "fake", L"one", &before, &after, &id) && id == 1, "an entry is written");
    lares_journal_list(&j, remember, &s);
    check(s.count == 1 && s.last.state == LARES_JOURNAL_PENDING, "it is listed as not completed");
    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.already == 1 && r.restored == 0 && slot_is(L"one", 1, 1),
          "died before the change: undo finds the old value and leaves it");

    set_slot(L"one", 1, 2);
    lares_journal_begin(&j, "shell", "fake", L"one", &before, &after, &id);
    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.restored == 1 && slot_is(L"one", 1, 1),
          "died after the change: undo puts the old value back");
}

static void test_torn_and_damaged(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    DWORD size = 0, whole = 0;
    printf("a torn last record, and a damaged one\n");
    fresh(&j);
    set_slot(L"one", 1, 1);
    set_slot(L"two", 1, 10);
    change(&j, "shell", L"one", 2, NULL);
    char *first = read_file(&whole);
    change(&j, "shell", L"two", 20, NULL);
    char *both = read_file(&size);
    check(first && both && size > whole, "two entries are on disk");

    /* Cut the second entry's records at every length: whatever is left of
     * them must never be believed, and the first entry must always undo. */
    int bad = 0;
    for (DWORD cut = whole; cut < size && !bad; ++cut) {
        write_file(both, cut);
        set_slot(L"one", 1, 2);
        set_slot(L"two", 1, 10);
        int open = lares_journal_list(&j, NULL, NULL);
        /* Before its own line end the second entry does not exist; after it
         * the entry exists, with or without its completion mark. */
        const char *line_end = memchr(both + whole, '\n', size - whole);
        int entry_whole = cut > (DWORD)(line_end - both);
        if (open != (entry_whole ? 2 : 1)) bad = 1;
        if (!lares_journal_undo(&j, NULL, 0, 0, &r) || !slot_is(L"one", 1, 1) || !slot_is(L"two", 1, 10)) bad = 1;
        if (file_exists()) bad = 1;
    }
    check(!bad, "cut at every byte: the torn record is ignored and everything before it undoes");

    /* After a torn record the next one starts on its own line. */
    write_file(both, size - 7);
    set_slot(L"one", 1, 2);
    set_slot(L"two", 1, 20);
    set_slot(L"three", 1, 100);
    check(change(&j, "shell", L"three", 300, NULL), "a change after a torn record");
    check(lares_journal_list(&j, NULL, NULL) == 3, "all three entries are read");
    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.restored == 3, "and undone");
    check(slot_is(L"one", 1, 1) && slot_is(L"two", 1, 10) && slot_is(L"three", 1, 100), "every value is back");

    /* One flipped character in the middle: that record alone is lost. */
    char *copy = (char *)malloc(size);
    memcpy(copy, both, size);
    copy[whole + 12] ^= 1;
    write_file(copy, size);
    set_slot(L"one", 1, 2);
    check(lares_journal_list(&j, NULL, NULL) == 1, "a record with a wrong checksum is ignored");
    check(lares_journal_undo(&j, NULL, 0, 0, &r) && slot_is(L"one", 1, 1), "the others still undo");

    write_file("this is not a journal\n", 22);
    check(lares_journal_list(&j, NULL, NULL) == 0, "a file that is no journal lists nothing");
    free(first);
    free(both);
    free(copy);
}

static void test_values_and_names(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    lares_journal_value text, big;
    seen s = { 0 };
    const wchar_t *name = L"Se\u00f1al de prueba 100% \u2603";
    printf("values and names\n");
    fresh(&j);
    set_slot(name, 0, 0);
    check(lares_journal_value_string(&text, L"C:\\Program Files\\Lares\\lares-pad.exe --background"), "a string value");
    check(lares_journal_change(&j, "pad helper", "fake", name, &text, NULL), "a name with spaces, a percent sign and accents");
    lares_journal_list(&j, remember, &s);
    check(!wcscmp(s.last.target, name), "the name reads back exactly");
    check(!strcmp(s.last.feature, "pad_helper"), "a feature is kept to plain letters");
    check(lares_journal_value_equal(&s.last.after, &text) && !s.last.before.present, "the string and the absent value read back");

    memset(&big, 0, sizeof big);
    big.present = 1;
    big.type = REG_BINARY;
    big.size = LARES_JOURNAL_VALUE_MAX;
    for (int i = 0; i < LARES_JOURNAL_VALUE_MAX; ++i) big.data[i] = (BYTE)(i * 7);
    check(lares_journal_change(&j, "shell", "fake", L"big", &big, NULL), "the largest value fits a record");
    memset(&s, 0, sizeof s);
    lares_journal_list(&j, remember, &s);
    check(s.count == 2 && lares_journal_value_equal(&s.last.after, &big), "and reads back");
    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.restored == 2, "both undo");
}

/* ---- the real registry, and the power cut --------------------------- */

static wchar_t g_key[128];

static void target(wchar_t *out, const wchar_t *value)
{
    swprintf(out, LARES_JOURNAL_TARGET_MAX, L"HKCU\\%ls|%ls", g_key, value);
}

static int registry_dword(const wchar_t *value, DWORD *number)
{
    DWORD size = sizeof *number;
    return RegGetValueW(HKEY_CURRENT_USER, g_key, value, RRF_RT_REG_DWORD, NULL, number, &size) == ERROR_SUCCESS;
}

static void test_registry(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    lares_journal_value v;
    wchar_t t[LARES_JOURNAL_TARGET_MAX], text[64];
    DWORD number = 0, size = sizeof text;
    printf("registry values under a test key\n");
    fresh(&j);

    target(t, L"Number");
    lares_journal_value_dword(&v, 7);
    check(lares_journal_change(&j, "test", "registry", t, &v, NULL), "a value that was not there is written");
    check(registry_dword(L"Number", &number) && number == 7, "it is in the registry");
    lares_journal_value_dword(&v, 8);
    check(lares_journal_change(&j, "test", "registry", t, &v, NULL), "and changed");

    target(t, L"Text");
    lares_journal_value_string(&v, L"before");
    RegSetKeyValueW(HKEY_CURRENT_USER, g_key, L"Text", REG_SZ, L"before", 14);
    lares_journal_value_string(&v, L"after");
    check(lares_journal_change(&j, "test", "registry", t, &v, NULL), "a string is changed");
    target(t, L"Gone");
    RegSetKeyValueW(HKEY_CURRENT_USER, g_key, L"Gone", REG_DWORD, &number, sizeof number);
    lares_journal_value_absent(&v);
    check(lares_journal_change(&j, "test", "registry", t, &v, NULL) && !registry_dword(L"Gone", &number),
          "a value is removed");

    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.restored == 4 && r.failed == 0, "undo restores all four");
    check(!registry_dword(L"Number", &number), "the new value is gone again");
    check(RegGetValueW(HKEY_CURRENT_USER, g_key, L"Text", RRF_RT_REG_SZ, NULL, text, &size) == ERROR_SUCCESS &&
              !wcscmp(text, L"before"),
          "the string is back");
    check(registry_dword(L"Gone", &number), "the removed value is back");

    lares_journal_value_dword(&v, 1);
    check(!lares_journal_change(&j, "test", "registry", L"HKXX\\Nowhere|Value", &v, NULL), "an unknown root changes nothing");
}

/* The copy that dies. 1: after the journal write, before the change.
 * 2: after the change, before the completion mark. */
static int cut(int where)
{
    lares_journal j;
    lares_journal_value before, after;
    wchar_t t[LARES_JOURNAL_TARGET_MAX];
    ULONGLONG id;
    lares_journal_open(&j, g_file);
    lares_journal_add_backend(&j, &lares_journal_registry);
    target(t, where == 1 ? L"CutBefore" : L"CutAfter");
    lares_journal_registry.read(t, &before, NULL);
    lares_journal_value_dword(&after, 222);
    if (!lares_journal_begin(&j, "test", "registry", t, &before, &after, &id)) return 3;
    if (where == 1) TerminateProcess(GetCurrentProcess(), 77);
    lares_journal_registry.write(t, &after, NULL);
    TerminateProcess(GetCurrentProcess(), 77);
    return 3;
}

static int run_cut(int where)
{
    wchar_t self[MAX_PATH], line[MAX_PATH + 64];
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    si.cb = sizeof si;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(line, ARRAYSIZE(line), L"\"%ls\" --cut %d", self, where);
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    WaitForSingleObject(pi.hProcess, 20000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 77;
}

static void test_power_cut(void)
{
    lares_journal j;
    lares_journal_undo_result r;
    lares_journal_value v;
    wchar_t t[LARES_JOURNAL_TARGET_MAX];
    DWORD number = 111;
    printf("power cut: a program that dies around the change\n");
    fresh(&j);
    RegSetKeyValueW(HKEY_CURRENT_USER, g_key, L"CutBefore", REG_DWORD, &number, sizeof number);
    RegSetKeyValueW(HKEY_CURRENT_USER, g_key, L"CutAfter", REG_DWORD, &number, sizeof number);
    target(t, L"Finished");
    lares_journal_value_dword(&v, 5);
    lares_journal_change(&j, "test", "registry", t, &v, NULL);

    check(run_cut(1), "a copy dies between the journal write and the change");
    check(registry_dword(L"CutBefore", &number) && number == 111, "its setting was never changed");
    check(run_cut(2), "a copy dies between the change and the completion mark");
    check(registry_dword(L"CutAfter", &number) && number == 222, "its setting was changed");
    check(lares_journal_list(&j, NULL, NULL) == 3, "all three entries are in the journal");

    check(lares_journal_undo(&j, NULL, 0, 0, &r) && r.failed == 0 && r.changed == 0, "undo runs clean");
    check(r.restored == 2 && r.already == 1, "two put back, one found as it was");
    check(registry_dword(L"CutBefore", &number) && number == 111, "the first is as before");
    check(registry_dword(L"CutAfter", &number) && number == 111, "the second is as before");
    check(!registry_dword(L"Finished", &number), "the finished change is undone too");
    check(!file_exists(), "the file is gone");
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t temp[MAX_PATH];
    DWORD owner = GetCurrentProcessId();
    int where = 0;
    if (argc == 3 && !wcscmp(argv[1], L"--cut")) {
        where = _wtoi(argv[2]);
        /* The copy works on its parent's file and key. */
        GetEnvironmentVariableW(L"LARES_JOURNAL_TEST_OWNER", temp, MAX_PATH);
        owner = (DWORD)_wtoi(temp);
    }
    if (argc == 3 && !wcscmp(argv[1], L"--make")) {
        /* For a look at Settings in a dry run: a journal at the given path
         * that holds two changes under a key of its own. Undo removes them. */
        lares_journal j;
        lares_journal_value v;
        wchar_t t[LARES_JOURNAL_TARGET_MAX];
        wcscpy(g_file, argv[2]);
        wcscpy(g_key, L"Software\\Lares\\JournalDemo");
        lares_journal_open(&j, g_file);
        lares_journal_add_backend(&j, &lares_journal_registry);
        target(t, L"Number");
        lares_journal_value_dword(&v, 7);
        lares_journal_change(&j, "demo", "registry", t, &v, NULL);
        target(t, L"Text");
        lares_journal_value_string(&v, L"made for a dry run");
        lares_journal_change(&j, "demo", "registry", t, &v, NULL);
        return lares_journal_list(&j, NULL, NULL) == 2 ? 0 : 1;
    }
    GetTempPathW(MAX_PATH, temp);
    swprintf(g_file, MAX_PATH, L"%lslares-journal-test-%lu.log", temp, owner);
    swprintf(g_key, ARRAYSIZE(g_key), L"Software\\Lares\\JournalTest-%lu", owner);
    if (where) return cut(where);

    swprintf(temp, MAX_PATH, L"%lu", owner);
    SetEnvironmentVariableW(L"LARES_JOURNAL_TEST_OWNER", temp);

    test_change_and_undo();
    test_order_and_selection();
    test_changed_by_someone_else();
    test_failed_write();
    test_no_completion_mark();
    test_torn_and_damaged();
    test_values_and_names();
    test_registry();
    test_power_cut();

    DeleteFileW(g_file);
    RegDeleteTreeW(HKEY_CURRENT_USER, g_key);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
