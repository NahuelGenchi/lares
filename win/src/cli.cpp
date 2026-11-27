// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cli.h"
#include <cstring>
#include <vector>

namespace lares {

static bool env_truthy(const char* v)
{
    return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

CliOptions parse_cli(int argc, const char* const* argv,
                     const char* env_dry_run,
                     const char* env_dry_stream,
                     const char* env_color_scheme,
                     const char* env_section)
{
    CliOptions opts;
    opts.dry_run = env_truthy(env_dry_run);
    opts.dry_stream = env_truthy(env_dry_stream);
    if (env_color_scheme && (*env_color_scheme != '\0')) {
        if (std::strcmp(env_color_scheme, "dark") == 0) opts.color_scheme = "dark";
        else if (std::strcmp(env_color_scheme, "light") == 0) opts.color_scheme = "light";
    }
    if (env_section && (*env_section != '\0')) {
        opts.section = env_section;
    }

    // --safe may stand anywhere and goes with anything else.
    std::vector<const char*> rest;
    rest.reserve(static_cast<size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && argv[i] && std::strcmp(argv[i], "--safe") == 0) {
            opts.safe = true;
            continue;
        }
        rest.push_back(argv[i]);
    }
    argc = static_cast<int>(rest.size());
    argv = rest.data();

    if (argc <= 1) return opts;

    const char* first = argv[1];
    if (std::strcmp(first, "--section") == 0) {
        if (argc >= 3 && argv[2] && argv[2][0] != '\0') {
            opts.section = argv[2];
            opts.action = CliAction::Window;
            return opts;
        }
        opts.action = CliAction::Error;
        return opts;
    }
    if (std::strcmp(first, "home") == 0) {
        opts.action = CliAction::Home;
        return opts;
    }
    if (std::strcmp(first, "remote") == 0) {
        opts.action = CliAction::Remote;
        return opts;
    }
    if (std::strcmp(first, "restore") == 0) {
        opts.action = CliAction::Restore;
        return opts;
    }
    if (std::strcmp(first, "status") == 0) {
        opts.action = CliAction::Status;
        return opts;
    }
    if (std::strcmp(first, "undo") == 0) {
        opts.action = CliAction::Undo;
        return opts;
    }
    if (std::strcmp(first, "--help") == 0 || std::strcmp(first, "-h") == 0) {
        opts.action = CliAction::Help;
        return opts;
    }

    opts.action = CliAction::Error;
    return opts;
}

const char* cli_usage()
{
    return "Usage: lares [--safe] [home | remote | restore | status | undo | --section NAME]\n"
           "With no word, Lares opens its window.\n"
           "undo puts back every Windows setting Lares has changed, newest first.\n"
           "--safe starts Lares in safe mode: its bar and dock stay off and Windows'\n"
           "own taskbar is left alone, until you sign out or turn safe mode off in Settings.\n";
}

} // namespace lares
