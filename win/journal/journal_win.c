/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The journal's backend for registry values (journal_win.h).
 */
#include "journal_win.h"

typedef struct {
    HKEY root;
    wchar_t key[LARES_JOURNAL_TARGET_MAX];
    const wchar_t *name;
} registry_target;

static int split_target(const wchar_t *target, registry_target *out)
{
    int n = lstrlenW(target), bar = n;
    if (n >= LARES_JOURNAL_TARGET_MAX || n < 6) return 0;
    while (bar > 0 && target[bar] != L'|') --bar;
    if (bar < 6 || target[4] != L'\\') return 0;
    if (target[0] != L'H' || target[1] != L'K') return 0;
    if (target[2] == L'C' && target[3] == L'U') out->root = HKEY_CURRENT_USER;
    else if (target[2] == L'L' && target[3] == L'M') out->root = HKEY_LOCAL_MACHINE;
    else return 0;
    lstrcpynW(out->key, target + 5, bar - 5 + 1);
    out->name = target + bar + 1;
    return 1;
}

static int registry_read(const wchar_t *target, lares_journal_value *value, void *ctx)
{
    registry_target t;
    HKEY key;
    DWORD size = LARES_JOURNAL_VALUE_MAX;
    (void)ctx;
    lares_journal_value_absent(value);
    if (!split_target(target, &t)) return 0;
    LSTATUS s = RegOpenKeyExW(t.root, t.key, 0, KEY_QUERY_VALUE, &key);
    if (s == ERROR_FILE_NOT_FOUND || s == ERROR_PATH_NOT_FOUND) return 1;
    if (s != ERROR_SUCCESS) return 0;
    s = RegQueryValueExW(key, t.name, NULL, &value->type, value->data, &size);
    RegCloseKey(key);
    if (s == ERROR_FILE_NOT_FOUND) return 1;
    if (s != ERROR_SUCCESS) return 0;
    value->present = 1;
    value->size = size;
    return 1;
}

static int registry_write(const wchar_t *target, const lares_journal_value *value, void *ctx)
{
    registry_target t;
    HKEY key;
    LSTATUS s;
    (void)ctx;
    if (!split_target(target, &t)) return 0;
    if (!value->present) {
        s = RegOpenKeyExW(t.root, t.key, 0, KEY_SET_VALUE, &key);
        if (s == ERROR_FILE_NOT_FOUND || s == ERROR_PATH_NOT_FOUND) return 1;
        if (s != ERROR_SUCCESS) return 0;
        s = RegDeleteValueW(key, t.name);
        RegCloseKey(key);
        return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
    }
    s = RegCreateKeyExW(t.root, t.key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (s != ERROR_SUCCESS) return 0;
    s = RegSetValueExW(key, t.name, 0, value->type, value->data, value->size);
    RegCloseKey(key);
    return s == ERROR_SUCCESS;
}

const lares_journal_backend lares_journal_registry = { "registry", registry_read, registry_write, NULL };

int lares_journal_value_string(lares_journal_value *value, const wchar_t *text)
{
    DWORD bytes = (DWORD)(lstrlenW(text) + 1) * sizeof(wchar_t);
    if (bytes > LARES_JOURNAL_VALUE_MAX) return 0;
    value->present = 1;
    value->type = REG_SZ;
    value->size = bytes;
    for (DWORD i = 0; i < bytes; ++i) value->data[i] = ((const BYTE *)text)[i];
    return 1;
}
