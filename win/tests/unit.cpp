// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
extern "C" {
#include "art.h"
}
#include "cli.h"
#include "config.h"
#include "theme.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static void test_cli()
{
    const char* a0[] = { "lares" };
    auto o = lares::parse_cli(1, a0, "1", nullptr, "dark", "settings");
    CHECK(o.action == lares::CliAction::Window);
    CHECK(o.dry_run == true);
    CHECK(o.dry_stream == false);
    CHECK(o.color_scheme == "dark");
    CHECK(o.section == "settings");

    const char* a1[] = { "lares", "--section", "hardware" };
    o = lares::parse_cli(3, a1, nullptr, nullptr, nullptr, "overview");
    CHECK(o.action == lares::CliAction::Window);
    CHECK(o.section == "hardware");

    const char* a2[] = { "lares", "home" };
    CHECK(lares::parse_cli(2, a2).action == lares::CliAction::Home);

    const char* a3[] = { "lares", "remote" };
    CHECK(lares::parse_cli(2, a3).action == lares::CliAction::Remote);

    const char* a4[] = { "lares", "status" };
    CHECK(lares::parse_cli(2, a4).action == lares::CliAction::Status);

    const char* a5[] = { "lares", "restore" };
    CHECK(lares::parse_cli(2, a5).action == lares::CliAction::Restore);

    const char* a6[] = { "lares", "--help" };
    CHECK(lares::parse_cli(2, a6).action == lares::CliAction::Help);

    const char* a7[] = { "lares", "--section" };
    CHECK(lares::parse_cli(2, a7).action == lares::CliAction::Error);

    const char* a8[] = { "lares", "bogus" };
    CHECK(lares::parse_cli(2, a8).action == lares::CliAction::Error);
}

static void test_theme()
{
    auto home_l = lares::palette_for(false, false);
    auto rem_l = lares::palette_for(false, true);
    auto home_d = lares::palette_for(true, false);
    auto rem_d = lares::palette_for(true, true);

    // Home is warm (red > blue), Remote is blue (blue > red).
    CHECK(home_l.accent.r > home_l.accent.b);
    CHECK(rem_l.accent.b > rem_l.accent.r);
    CHECK(home_d.accent.r > home_d.accent.b);
    CHECK(rem_d.accent.b > rem_d.accent.r);

    // Lerp endpoints match exactly.
    auto p0 = lares::palette_lerp(home_l, rem_l, 0.0f);
    auto p1 = lares::palette_lerp(home_l, rem_l, 1.0f);
    CHECK(std::fabs(p0.bg.r - home_l.bg.r) < 1e-5f);
    CHECK(std::fabs(p1.bg.b - rem_l.bg.b) < 1e-5f);

    // Easing bounds and monotonicity.
    CHECK(lares::ease_out_cubic_css(0.0f) == 0.0f);
    CHECK(lares::ease_out_cubic_css(1.0f) == 1.0f);
    float mid = lares::ease_out_cubic_css(0.5f);
    CHECK(mid > 0.5f && mid < 1.0f);

    // COLORREF is 0x00BBGGRR.
    uint32_t cr = lares::rgba_to_colorref({ 1.0f, 0.5f, 0.0f, 1.0f });
    CHECK((cr & 0xff) == 0xff);
    CHECK(((cr >> 16) & 0xff) == 0x00);
}

static void test_config()
{
    // Defaults match Linux src/pad/padconf.c
    lares::PadPrefs def = lares::parse_pad_ini("");
    CHECK(def.low_level == 20);
    CHECK(def.alerts == true);
    CHECK(def.notices == true);
    CHECK(def.keep_awake == false);
    CHECK(def.idle_disconnect == false);
    CHECK(def.idle_minutes == 15);
    CHECK(def.topbar == false);
    CHECK(def.topbar_level == false);
    CHECK(def.topbar_hide == false);
    CHECK(def.background == true);
    CHECK(def.key_light == "own");

    // Round-trip custom settings + clamping
    lares::PadPrefs custom;
    custom.low_level = 35;
    custom.alerts = false;
    custom.notices = true;
    custom.keep_awake = true;
    custom.idle_disconnect = true;
    custom.idle_minutes = 30;
    custom.topbar = true;
    custom.topbar_level = true;
    custom.topbar_hide = true;
    custom.background = false;
    custom.key_light = "breathing";
    custom.key_rgb[0] = 0x12;
    custom.key_rgb[1] = 0xab;
    custom.key_rgb[2] = 0xef;

    std::string ini = lares::serialize_pad_ini(custom);
    lares::PadPrefs back = lares::parse_pad_ini(ini);
    CHECK(back.low_level == 35);
    CHECK(back.alerts == false);
    CHECK(back.notices == true);
    CHECK(back.keep_awake == true);
    CHECK(back.idle_disconnect == true);
    CHECK(back.idle_minutes == 30);
    CHECK(back.topbar == true);
    CHECK(back.topbar_level == true);
    CHECK(back.topbar_hide == true);
    CHECK(back.background == false);
    CHECK(back.key_light == "breathing");
    CHECK(back.key_rgb[0] == 0x12 && back.key_rgb[1] == 0xab && back.key_rgb[2] == 0xef);

    // Out-of-range integers clamp safely
    lares::PadPrefs clamped = lares::parse_pad_ini("[pad]\nlow-battery-level=999\nidle-disconnect-minutes=0\n");
    CHECK(clamped.low_level == 50);
    CHECK(clamped.idle_minutes == 1);
}

static void test_shared_art()
{
    CHECK(ART_OVERVIEW[0] != nullptr);
    CHECK(ART_CHIP[0] != nullptr);
    CHECK(ART_GAMEPAD[0] != nullptr);
    CHECK(ART_HEADPHONES[0] != nullptr);
    CHECK(ART_KEYBOARD[0] != nullptr);
    CHECK(ART_MOUSE[0] != nullptr);
    CHECK(ART_LAPTOP[0] != nullptr);
    CHECK(ART_SLIDERS[0] != nullptr);
}

int main()
{
    test_cli();
    test_theme();
    test_config();
    test_shared_art();
    if (failures == 0) {
        std::printf("lares-win-unit: all passed\n");
        return 0;
    }
    std::fprintf(stderr, "lares-win-unit: %d failure(s)\n", failures);
    return 1;
}
