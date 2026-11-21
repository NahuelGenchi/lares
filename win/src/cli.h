// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace lares {

enum class CliAction {
    Window,
    Home,
    Remote,
    Restore,
    Status,
    Help,
    Error,
};

struct CliOptions {
    CliAction action = CliAction::Window;
    std::string section;
    bool dry_run = false;
    bool dry_stream = false;
    std::string color_scheme; // "", "dark", or "light"
};

CliOptions parse_cli(int argc, const char* const* argv,
                     const char* env_dry_run = nullptr,
                     const char* env_dry_stream = nullptr,
                     const char* env_color_scheme = nullptr,
                     const char* env_section = nullptr);

const char* cli_usage();

} // namespace lares
