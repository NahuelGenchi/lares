// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* The files Lares' viewers open (issue #151), shared by lares-view.exe and
 * lares-files.exe, which decides what Enter and Quick Look do with a file. */
#include "shell.h"

bool viewer_is_supported_ext(const wchar_t *ext) {
    if (!ext || !ext[0]) return false;
    return (_wcsicmp(ext, L".png")  == 0 ||
            _wcsicmp(ext, L".jpg")  == 0 ||
            _wcsicmp(ext, L".jpeg") == 0 ||
            _wcsicmp(ext, L".webp") == 0 ||
            _wcsicmp(ext, L".bmp")  == 0 ||
            _wcsicmp(ext, L".gif")  == 0 ||
            _wcsicmp(ext, L".ico")  == 0);
}

bool textview_is_supported_ext(const wchar_t *ext) {
    if (!ext || !ext[0]) return false;
    static const wchar_t *exts[] = {
        L".txt", L".md", L".markdown", L".mkd", L".ini", L".json", L".log", L".bat", L".ps1",
        L".yml", L".yaml", L".toml", L".csv", L".c", L".h", L".js",
        L".ts", L".py", L".css", L".html", L".xml", L".reg"
    };
    for (int i = 0; i < (int)(sizeof(exts) / sizeof(exts[0])); i++) {
        if (_wcsicmp(ext, exts[i]) == 0) return true;
    }
    return false;
}
