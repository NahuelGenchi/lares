// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Lares offered for Markdown and pictures (issue #159): filetypes_reg.h. */
#include "filetypes_reg.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

extern "C" {
#include "journal.h"
#include "journal_win.h"
}

namespace lares {

namespace {

struct Kind {
    const wchar_t* progid;
    const wchar_t* name;
    const wchar_t* verb_args;
    const wchar_t* const* exts;
    int ext_count;
};

const wchar_t* const kMarkdownExts[] = { L".md", L".markdown" };
const wchar_t* const kImageExts[] = { L".png", L".jpg", L".jpeg", L".webp", L".bmp", L".gif" };

const Kind kKinds[] = {
    { L"Lares.Markdown", L"Markdown document", L"--text \"%1\"", kMarkdownExts, 2 },
    { L"Lares.Image", L"Picture", L"--view \"%1\"", kImageExts, 6 },
};

constexpr wchar_t kCapabilities[] = L"Software\\Lares\\Capabilities";

std::wstring exe_dir()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
}

std::wstring settings_root()
{
    wchar_t root[LARES_JOURNAL_TARGET_MAX];
    DWORD n = GetEnvironmentVariableW(L"LARES_FILETYPES_ROOT", root, LARES_JOURNAL_TARGET_MAX);
    if (n > 0 && n < LARES_JOURNAL_TARGET_MAX) return root;
    return L"HKCU";
}

bool open_journal(const wchar_t* path, lares_journal& j)
{
    if (!lares_journal_open(&j, path)) return false;
    return lares_journal_add_backend(&j, &lares_journal_registry) &&
           lares_journal_add_backend(&j, &lares_journal_registry_key);
}

/* A value's text, read the way the journal reads it; empty when absent. */
bool read_value(const std::wstring& target, std::wstring& out)
{
    lares_journal_value v;
    out.clear();
    if (!lares_journal_registry.read(target.c_str(), &v, nullptr) || !v.present || v.type != REG_SZ) return false;
    out.assign(reinterpret_cast<const wchar_t*>(v.data), v.size / sizeof(wchar_t));
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return true;
}

} // namespace

std::vector<FileTypeChange> filetypes_changes(const std::wstring& root, const std::wstring& view_exe)
{
    std::vector<FileTypeChange> out;
    auto key = [&](const std::wstring& k) { out.push_back({ true, root + L"\\" + k, {} }); };
    auto value = [&](const std::wstring& k, const std::wstring& name, const std::wstring& text) {
        out.push_back({ false, root + L"\\" + k + L"|" + name, text });
    };
    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring quoted = L"\"" + view_exe + L"\"";

    for (const Kind& kind : kKinds) {
        std::wstring p = classes + kind.progid;
        key(p);
        value(p, L"", kind.name);
        key(p + L"\\DefaultIcon");
        value(p + L"\\DefaultIcon", L"", quoted + L",0");
        key(p + L"\\shell");
        key(p + L"\\shell\\open");
        key(p + L"\\shell\\open\\command");
        value(p + L"\\shell\\open\\command", L"", quoted + L" " + kind.verb_args);
        for (int i = 0; i < kind.ext_count; ++i) {
            std::wstring e = classes + kind.exts[i];
            key(e);
            key(e + L"\\OpenWithProgids");
            value(e + L"\\OpenWithProgids", kind.progid, L"");
        }
    }

    key(L"Software\\Lares");
    key(kCapabilities);
    value(kCapabilities, L"ApplicationName", L"Lares");
    value(kCapabilities, L"ApplicationDescription", L"Lares' image viewer and its text and Markdown editor.");
    std::wstring assoc = std::wstring(kCapabilities) + L"\\FileAssociations";
    key(assoc);
    for (const Kind& kind : kKinds)
        for (int i = 0; i < kind.ext_count; ++i) value(assoc, kind.exts[i], kind.progid);
    key(L"Software\\RegisteredApplications");
    value(L"Software\\RegisteredApplications", L"Lares", kCapabilities);
    return out;
}

bool filetypes_offered(const std::wstring& root)
{
    std::wstring text;
    return read_value(root + L"\\Software\\RegisteredApplications|Lares", text) &&
           read_value(root + L"\\Software\\Classes\\Lares.Markdown\\shell\\open\\command|", text);
}

bool filetypes_offer(const wchar_t* journal_path, const std::wstring& root, const std::wstring& view_exe,
                     std::string& said)
{
    lares_journal j;
    if (!open_journal(journal_path, j)) {
        said = "The list of changes could not be opened, so nothing was changed.";
        return false;
    }
    for (const FileTypeChange& c : filetypes_changes(root, view_exe)) {
        lares_journal_value v;
        bool made = false;
        if (c.key) {
            lares_journal_value_dword(&v, 1);
            made = lares_journal_change(&j, kFileTypesFeature, "registry-key", c.target.c_str(), &v, nullptr) != 0;
        } else {
            made = lares_journal_value_string(&v, c.text.c_str()) &&
                   lares_journal_change(&j, kFileTypesFeature, "registry", c.target.c_str(), &v, nullptr) != 0;
        }
        if (!made) {
            lares_journal_undo(&j, kFileTypesFeature, 0, 0, nullptr);
            said = "Windows did not take the change, so what was made is taken back.";
            return false;
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    said.clear();
    return true;
}

bool filetypes_withdraw(const wchar_t* journal_path, std::string& said)
{
    lares_journal j;
    lares_journal_undo_result r = {};
    if (!open_journal(journal_path, j)) {
        said = "The list of changes could not be opened.";
        return false;
    }
    bool ok = lares_journal_undo(&j, kFileTypesFeature, 0, 0, &r) != 0;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    said.clear();
    if (r.changed) said = "Some of it was changed by another program since, and is left alone.";
    else if (!ok) said = "Some of it could not be taken back.";
    return ok;
}

bool filetypes_offered_now()
{
    return filetypes_offered(settings_root());
}

bool filetypes_set(bool on, bool dry_run, std::string& said)
{
    if (dry_run) {
        said = on ? "Dry run: Lares would be offered for Markdown and pictures."
                  : "Dry run: Lares would leave Open with.";
        return true;
    }
    if (!on) return filetypes_withdraw(nullptr, said);
    std::wstring view = exe_dir() + L"\\lares-view.exe";
    if (GetFileAttributesW(view.c_str()) == INVALID_FILE_ATTRIBUTES) {
        said = "This copy of Lares came without lares-view.exe.";
        return false;
    }
    return filetypes_offer(nullptr, settings_root(), view, said);
}

void filetypes_open_default_apps()
{
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Lares", nullptr, nullptr,
                  SW_SHOWNORMAL);
}

} // namespace lares
