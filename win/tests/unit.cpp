// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
extern "C" {
#include "art.h"
}
#include "cli.h"
#include "config.h"
#include "devices.h"
#include "helper.h"
#include "hid_win.h"
#include "hw.h"
#include "livelight.h"
#include "pad.h"
#include "remote.h"
#include "share_win.h"
#include "theme.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

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

    // Settings > Accessibility > Size: 100% unless saved, kept only when it differs
    CHECK(def.size == 100);
    CHECK(lares::serialize_pad_ini(def).find("[window]") == std::string::npos);
    lares::PadPrefs larger;
    larger.size = 130;
    std::string with_size = lares::serialize_pad_ini(larger);
    CHECK(with_size.find("[window]\nsize=130\n") != std::string::npos);
    CHECK(lares::parse_pad_ini(with_size).size == 130);
    CHECK(lares::parse_pad_ini("[window]\nsize=20\n").size == lares::kSizeMin);
    CHECK(lares::parse_pad_ini("[window]\nsize=400\n").size == lares::kSizeMax);
    CHECK(lares::parse_pad_ini("[window]\nsize=big\n").size == 100);
    CHECK(lares::parse_pad_ini("[pad]\nsize=120\n").size == 100);
    for (int i = 0; i < lares::kSizeChoices; ++i) {
        CHECK(lares::kSizePercents[i] >= lares::kSizeMin && lares::kSizePercents[i] <= lares::kSizeMax);
    }
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

static void test_hw_helpers()
{
    constexpr uint64_t GiB = 1073741824ULL;
    CHECK(lares::format_bytes(0) == "0.0 GB");
    CHECK(lares::format_bytes(2ULL * GiB) == "2.0 GB");
    CHECK(lares::format_bytes(16ULL * GiB) == "16 GB");
    CHECK(lares::format_bytes(1536ULL * GiB) == "1.5 TB");

    CHECK(lares::clean_cpu_name("Intel(R) Core(TM) i5-8400 CPU @ 2.80GHz") == "Intel Core i5-8400");
    CHECK(lares::clean_cpu_name("   ") == "Processor");
    CHECK(lares::clean_gpu_name("AMD Radeon (TM) R9 380 Series") == "AMD Radeon R9 380");

    CHECK(lares::meter_filled_blocks(-1.0, 14) == 0);
    CHECK(lares::meter_filled_blocks(0.0, 14) == 0);
    CHECK(lares::meter_filled_blocks(0.01, 14) == 1);
    CHECK(lares::meter_filled_blocks(0.5, 14) == 7);
    CHECK(lares::meter_filled_blocks(1.0, 14) == 14);

    CHECK(lares::graph_step_height(0.0f, 18) == 0);
    CHECK(lares::graph_step_height(0.01f, 18) == 1);
    CHECK(lares::graph_step_height(0.5f, 18) == 9);
    CHECK(lares::graph_step_height(1.0f, 18) == 18);

    lares::GraphRing ring;
    CHECK(ring.len == 0);
    ring.push(0.25);
    ring.push(0.75);
    CHECK(ring.len == 2);
    CHECK(std::fabs(ring.latest(0) - 0.75f) < 1e-5f);
    CHECK(std::fabs(ring.latest(1) - 0.25f) < 1e-5f);
}

static void test_remote_helpers()
{
    int w = 0, h = 0;
    double r = 0.0;
    CHECK(lares::parse_screen_spec("1920x1080@75", w, h, r));
    CHECK(w == 1920 && h == 1080 && std::fabs(r - 75.0) < 1e-5);
    CHECK(lares::parse_screen_spec("1680x1050", w, h, r));
    CHECK(w == 1680 && h == 1050 && std::fabs(r - 60.0) < 1e-5);
    CHECK(!lares::parse_screen_spec("invalid", w, h, r));

    CHECK(lares::describe_screen_mode(1920, 1080, 75.0) == "1920 \xC3\x97 1080 \xC2\xB7 75 Hz");
    CHECK(lares::remote_describe(lares::RemoteItem::Screen, "leave") == "Not changed");
    CHECK(lares::remote_describe(lares::RemoteItem::Sunshine, "on") == "On");
    CHECK(lares::remote_describe(lares::RemoteItem::Tailscale, "off") == "Off");

    // Apply order: Home stops Sunshine first and Tailscale last; Remote does the opposite.
    const auto* home_ord = lares::remote_apply_order(lares::RemoteMode::Home);
    CHECK(home_ord[0] == lares::RemoteItem::Sunshine);
    CHECK(home_ord[1] == lares::RemoteItem::Screen);
    CHECK(home_ord[2] == lares::RemoteItem::Tailscale);

    const auto* rem_ord = lares::remote_apply_order(lares::RemoteMode::Remote);
    CHECK(rem_ord[0] == lares::RemoteItem::Tailscale);
    CHECK(rem_ord[1] == lares::RemoteItem::Screen);
    CHECK(rem_ord[2] == lares::RemoteItem::Sunshine);

    // lares.ini round-trip
    lares::RemoteConfig cfg;
    cfg.mode = lares::RemoteMode::Remote;
    cfg.mode_known = true;
    cfg.ready = true;
    cfg.value(lares::RemoteMode::Home, lares::RemoteItem::Screen) = "1920x1080@75";
    cfg.value(lares::RemoteMode::Remote, lares::RemoteItem::Screen) = "1680x1050@60";
    std::string ini = lares::serialize_lares_ini(cfg);
    lares::RemoteConfig back = lares::parse_lares_ini(ini);
    CHECK(back.mode_known && back.mode == lares::RemoteMode::Remote);
    CHECK(back.ready == true);
    CHECK(back.value(lares::RemoteMode::Home, lares::RemoteItem::Screen) == "1920x1080@75");
    CHECK(back.value(lares::RemoteMode::Remote, lares::RemoteItem::Screen) == "1680x1050@60");
    CHECK(back.value(lares::RemoteMode::Home, lares::RemoteItem::Sunshine) == "off");
    CHECK(back.value(lares::RemoteMode::Remote, lares::RemoteItem::Sunshine) == "on");

    // Sunshine state and log detection
    CHECK(!lares::sunshine_state_has_paired_device("{\"named_devices\": []}"));
    CHECK(lares::sunshine_state_has_paired_device("{\"named_devices\": [{\"name\":\"Mac\"}]}"));
    CHECK(lares::sunshine_log_is_streaming("Info: CLIENT CONNECTED\n"));
    CHECK(!lares::sunshine_log_is_streaming("Info: CLIENT CONNECTED\nInfo: CLIENT DISCONNECTED\n"));
}

static void test_hyperx_and_hidpp()
{
    // HyperX Pulsefire Haste 2 Wireless receiver and cable models
    const auto* rx = lares::hyperx_model(0x03f0, 0x0f98);
    const auto* cb = lares::hyperx_model(0x03f0, 0x0d97);
    CHECK(rx != nullptr && rx->receiver);
    CHECK(cb != nullptr && !cb->receiver);
    const lares::HyperxModel* plugged[] = { rx, cb };
    CHECK(lares::hyperx_on_cable(rx, plugged, 2));

    // Battery & event parse
    uint8_t batt_rep[64] = { 0x51, 0x02, 85, 1 };
    int lvl = 0; bool chg = false;
    CHECK(lares::hyperx_battery_parse(batt_rep, 64, &lvl, &chg));
    CHECK(lvl == 85 && chg == true);

    lares::HyperxState hx;
    lares::hyperx_sample(&hx);
    lares::MouseSettings ms;
    lares::hyperx_decode(rx, &hx, &ms);
    CHECK(ms.stages == 5 && ms.active == 2 && ms.dpi[2] == 1600);
    CHECK(ms.rate == 1000 && ms.lift_mm == 1 && ms.brightness == 100);

    lares::MouseSettings want = ms;
    want.active = 3;
    want.dpi[3] = 3600;
    want.rate = 500;
    want.lift_mm = 2;
    want.brightness = 50;
    want.light = lares::LightEffect::Solid;
    want.rgb[0] = 0x10; want.rgb[1] = 0x20; want.rgb[2] = 0x30;

    uint8_t pkts[lares::kHyperxBlocks][lares::kHyperxReport];
    int n_pkts = lares::hyperx_packets(&hx, &want, 0, pkts);
    CHECK(n_pkts == 4);
    for (int i = 0; i < n_pkts; ++i) lares::hyperx_apply(&hx, pkts[i]);
    lares::MouseSettings after;
    lares::hyperx_decode(rx, &hx, &after);
    CHECK(after.active == 3 && after.dpi[3] == 3600 && after.rate == 500);
    CHECK(after.lift_mm == 2 && after.brightness == 50 && after.light == lares::LightEffect::Solid);
    CHECK(lares::hyperx_shows(&ms, &after));

    // Logitech G715 HID++ 2.0 allowlist safety gate, CRC-16, and profile codecs
    const auto* gm = lares::hidpp_model(0xc547, 0x40a2);
    CHECK(gm != nullptr && gm->levels == 5);

    // Allowed: reading root feature and writing whole sector 0x0001
    const uint8_t ok_sec[6] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0xff };
    CHECK(lares::hidpp_allowed(lares::HIDPP_PROFILES, 6, ok_sec, 6));
    // Refused: writing sector 0 (directory) or ROM (0x0100)
    const uint8_t bad_sec0[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xff };
    const uint8_t bad_rom[6]  = { 0x01, 0x01, 0x00, 0x00, 0x00, 0xff };
    CHECK(!lares::hidpp_allowed(lares::HIDPP_PROFILES, 6, bad_sec0, 6));
    CHECK(!lares::hidpp_allowed(lares::HIDPP_PROFILES, 6, bad_rom, 6));
    // Refused: unknown feature (e.g. firmware update 0x00c2)
    CHECK(!lares::hidpp_allowed(0x00c2, 0, nullptr, 0));

    lares::HidppKeyboard hk;
    lares::hidpp_sample(&hk);
    CHECK(hk.has_profile && hk.has_brightness);
    CHECK(lares::hidpp_sector_valid(hk.profile, hk.profile_size));

    lares::KeyboardSettings ks;
    lares::hidpp_decode(&hk, gm, &ks);
    CHECK(ks.brightness == 12 && ks.dim_s == 60 && ks.off_s == 300);
    CHECK(ks.startup == true && ks.shutdown == false);

    // Rewrite onboard profile lighting to Wave and timers to 120s / 600s
    uint8_t updated[lares::kHidppSectorMax];
    lares::HidppLighting wave_l = { lares::KeyLight::Wave, { 0, 0, 0 } };
    CHECK(lares::hidpp_profile_lit(hk.profile, hk.profile_size, &wave_l, updated));
    CHECK(lares::hidpp_profile_timed(updated, hk.profile_size, 120, 600, updated));
    CHECK(lares::hidpp_sector_valid(updated, hk.profile_size));

    lares::HidppKeyboard hk2 = hk;
    lares::hidpp_decode_profile(updated, hk.profile_size, &hk2);
    CHECK(hk2.light.effect == lares::KeyLight::Wave);
    CHECK(hk2.dim_s == 120 && hk2.off_s == 600);
    CHECK(lares::hidpp_dark_after(120, 600) == 480);
}

static void test_pad_map_and_controller()
{
    // 1. Parse sample map and real data/pad/controller.txt
    const char* mini =
        "; comment line\n"
        ".#aAH(\n"
        "+=bBY)\n";
    lares::PadMap map;
    std::string err;
    CHECK(lares::pad_map_parse(mini, std::strlen(mini), map, &err));
    CHECK(map.width == 6 && map.height == 2);
    CHECK(lares::pad_cell_paint(map.at(0, 0), 0, 0) == lares::PadPaint::None);
    CHECK(lares::pad_cell_paint(map.at(1, 0), 0, 0) == lares::PadPaint::Line);
    // Button A area: None -> Seen -> Lit
    CHECK(lares::pad_cell_paint(map.at(2, 0), 0, 0) == lares::PadPaint::None);
    CHECK(lares::pad_cell_paint(map.at(2, 0), 0, lares::pb_bit(lares::PB_A)) == lares::PadPaint::Seen);
    CHECK(lares::pad_cell_paint(map.at(2, 0), lares::pb_bit(lares::PB_A), 0) == lares::PadPaint::Lit);
    // Button A glyph is cut out when held
    CHECK(lares::pad_cell_paint(map.at(3, 0), 0, 0) == lares::PadPaint::Line);
    CHECK(lares::pad_cell_paint(map.at(3, 0), lares::pb_bit(lares::PB_A), 0) == lares::PadPaint::None);
    // Xbox logo glyph lights itself
    CHECK(lares::pad_cell_paint(map.at(4, 0), 0, 0) == lares::PadPaint::Line);
    CHECK(lares::pad_cell_paint(map.at(4, 0), lares::pb_bit(lares::PB_XBOX), 0) == lares::PadPaint::Lit);
    CHECK(lares::pad_cell_paint(map.at(4, 0), 0, lares::pb_bit(lares::PB_XBOX)) == lares::PadPaint::Seen);

    // Reject uneven row length or unknown legend char
    const char* bad_len = ".#\n.#.\n";
    CHECK(!lares::pad_map_parse(bad_len, std::strlen(bad_len), map, &err));
    const char* bad_ch = ".?\n";
    CHECK(!lares::pad_map_parse(bad_ch, std::strlen(bad_ch), map, &err));

    // Load real data/pad/controller.txt from repo root or parent
    std::ifstream f("../data/pad/controller.txt", std::ios::binary);
    if (!f.is_open()) f.open("../../data/pad/controller.txt", std::ios::binary);
    if (!f.is_open()) f.open("data/pad/controller.txt", std::ios::binary);
    CHECK(f.is_open());
    if (f.is_open()) {
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string txt = ss.str();
        lares::PadMap full;
        CHECK(lares::pad_map_parse(txt.data(), txt.size(), full, &err));
        CHECK(full.width == 216 && full.height == 162);
        CHECK(full.gauges[0].x1 > full.gauges[0].x0);
        CHECK(full.gauges[1].x1 > full.gauges[1].x0);
    }

    // 2. Nickname sanitization & keyed hash
    CHECK(lares::pad_clean_nickname("   Blue   pad  \n ") == "Blue pad");
    CHECK(lares::pad_clean_nickname("1234567890123456789012345678901234567890").size() == 32);
    std::string k1 = lares::pad_nickname_key("address aa:bb:cc", "salt-1");
    std::string k2 = lares::pad_nickname_key("address aa:bb:cc", "salt-2");
    CHECK(k1.size() == 64 && k1 != k2);

    // 3. Battery stages & formatting
    lares::PadBattery bat;
    bat.known = true;
    bat.percentage = 85.0;
    bat.state = 1; // charging
    CHECK(lares::pad_battery_charging(bat));
    CHECK(lares::pad_battery_low_stage(bat, 20) == 0);
    CHECK(lares::pad_battery_text(bat) == "85% \xC2\xB7 Charging");
    bat.state = 2;
    bat.percentage = 15.0;
    CHECK(lares::pad_battery_low_stage(bat, 20) == 1);
    bat.percentage = 10.0;
    CHECK(lares::pad_battery_low_stage(bat, 20) == 2);

    // 4. Stick rest & drift tracker
    lares::StickTracker st;
    lares::stick_feed(st, 0.02, -0.01, 1000);
    CHECK(!st.has_rest);
    lares::stick_feed(st, 0.02, -0.01, 1650);
    CHECK(st.has_rest && !lares::stick_drifting(st));

    lares::StickTracker drift_st;
    lares::stick_feed(drift_st, 0.18, 0.0, 2000);
    lares::stick_feed(drift_st, 0.18, 0.0, 2700);
    CHECK(drift_st.has_rest && lares::stick_drifting(drift_st));
}

static void test_helper()
{
    lares::PadPrefs prefs;
    CHECK(lares::padconf_needs_helper(prefs));
    prefs.background = false;
    CHECK(!lares::padconf_needs_helper(prefs));
    prefs.background = true;
    prefs.alerts = false;
    prefs.notices = false;
    CHECK(!lares::padconf_needs_helper(prefs));
    prefs.key_light = "cycle";
    CHECK(lares::padconf_needs_helper(prefs));

    std::vector<lares::PadInfo> pads;
    prefs.topbar = true;
    prefs.topbar_hide = true;
    CHECK(!lares::helper_should_show_tray_icon(pads, prefs));
    prefs.topbar_hide = false;
    CHECK(lares::helper_should_show_tray_icon(pads, prefs));
    CHECK(lares::helper_tooltip_text(pads, prefs) == "Lares \xC2\xB7 No controller connected");

    lares::PadInfo p;
    p.path = "xinput-0";
    p.name = "Xbox Wireless Controller";
    p.connection = lares::PadConnection::Bluetooth;
    p.battery.known = true;
    p.battery.percentage = 15.0;
    p.battery.state = 2;
    pads.push_back(p);

    prefs.topbar_level = true;
    CHECK(lares::helper_tooltip_text(pads, prefs) == "Lares \xC2\xB7 Xbox Wireless Controller (15%)");

    std::string title, body;
    lares::helper_format_connection_notice(p, true, title, body);
    CHECK(title == "Xbox Wireless Controller connected");
    CHECK(body == "Bluetooth \xC2\xB7 15%");

    lares::helper_format_battery_alert(p, 1, title, body);
    CHECK(title == "Xbox Wireless Controller battery low");
    CHECK(body == "15% left. Charge it or change its batteries soon.");

    p.battery.percentage = 10.0;
    lares::helper_format_battery_alert(p, 2, title, body);
    CHECK(title == "Xbox Wireless Controller battery very low");
    CHECK(body == "10% left. Charge it or change its batteries before it switches off.");

    lares::PadState s0{}, s1{};
    s1.lx = 0.05f;
    CHECK(!lares::helper_state_has_activity(s0, s1));
    s1.lx = 0.30f;
    CHECK(lares::helper_state_has_activity(s0, s1));
}

static void test_hidpp_allowed_matrix()
{
    // Verify every HID++ 2.0 function ID & payload shape used by win/src/devices.cpp
    const uint8_t sw_get[1] = { 0x00 };
    const uint8_t sw_take_fw[3] = { 0x01, 0x00, 0x06 };
    const uint8_t sw_take_sw[3] = { 0x01, 0x03, 0x06 };
    const uint8_t sw_rel[3] = { 0x01, 0x00, 0x00 };
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 5, sw_get, 1));
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 5, sw_take_fw, 3));
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 5, sw_take_sw, 3));
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 5, sw_rel, 3));
    CHECK(!lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 8, sw_get, 1));

    const uint8_t nv_get_start[3] = { 0x00, 0x00, 0x01 };
    const uint8_t nv_get_shut[3] = { 0x00, 0x00, 0x40 };
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 3, nv_get_start, 3));
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 3, nv_get_shut, 3));
    CHECK(!lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 12, nv_get_start, 3));

    lares::HidppNv nv;
    nv.known = true;
    uint8_t nv_set[lares::kHidppNvLen] = {};
    lares::hidpp_nv_encode(lares::kHidppNvStartup, true, &nv, nv_set);
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 3, nv_set, sizeof(nv_set)));
    lares::hidpp_nv_encode(lares::kHidppNvShutdown, false, &nv, nv_set);
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 3, nv_set, sizeof(nv_set)));

    uint8_t fx_set[13] = { 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    CHECK(lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 1, fx_set, 13));
    fx_set[12] = 0; // non-volatile must be rejected
    CHECK(!lares::hidpp_allowed(lares::HIDPP_RGB_EFFECTS, 1, fx_set, 13));

    CHECK(lares::key_light_from_name("off") == lares::KeyLight::Off);
    CHECK(lares::key_light_from_name("solid") == lares::KeyLight::Solid);
    CHECK(lares::key_light_from_name("breathing") == lares::KeyLight::Breathing);
    CHECK(lares::key_light_from_name("cycle") == lares::KeyLight::Cycle);
    CHECK(lares::key_light_from_name("own") == lares::KeyLight::Own);
    CHECK(lares::key_light_from_name("") == lares::KeyLight::Own);
}

static void test_config_escaping_and_pad_shutdown()
{
    // Verify backslash + tab escaping in nickname values round-trips cleanly
    lares::PadPrefs p;
    p.nicknames["k1"] = "Player\\One\tXbox Wireless Controller";
    std::string text = lares::serialize_pad_ini(p);
    lares::PadPrefs p2 = lares::parse_pad_ini(text);
    CHECK(p2.nicknames["k1"] == "Player\\One\tXbox Wireless Controller");

    // Verify destroying PadManager while rumble/find is active does not UAF or hang
    {
        lares::PadManager pm(true);
        pm.rumble_async("xinput-0", 80, 40, 2000);
        pm.toggle_find_controller("xinput-0");
    }
}

/* What the helper tells the keyboard next, from what it says (issue #92):
 * the sequence the G715 was seen to send while the helper held its lights. */
static void test_livelight_steps()
{
    using lares::HidppLive;
    lares::HidppEvent idle, active, up, down;
    idle.kind = lares::HidppEventKind::Idle;
    active.kind = lares::HidppEventKind::Active;
    up.kind = lares::HidppEventKind::Link;
    up.linked = true;
    down.kind = lares::HidppEventKind::Link;
    down.linked = false;

    // A setting while nothing is held takes the lights; while dark, it shows dimmed.
    auto c = lares::livelight_on_conf(HidppLive::Active, false);
    CHECK(c.state == HidppLive::Take && c.again);
    c = lares::livelight_on_conf(HidppLive::Dark, true);
    CHECK(c.state == HidppLive::Idle && c.again && c.held);
    c = lares::livelight_on_conf(HidppLive::Active, true);
    CHECK(c.state == HidppLive::Active);

    // Left alone: half strength; again while dark: stays dark, answered again.
    auto s = lares::livelight_on_event(HidppLive::Active, true, idle);
    CHECK(s.state == HidppLive::Idle && s.again && s.held);
    s = lares::livelight_on_event(HidppLive::Dark, true, idle);
    CHECK(s.state == HidppLive::Dark && s.again);

    // A key after dark brings it back at full strength and stops the dark timer.
    s = lares::livelight_on_event(HidppLive::Dark, true, active);
    CHECK(s.state == HidppLive::Active && s.again && s.stop_dark);

    // Events while nothing is held aren't answered.
    s = lares::livelight_on_event(HidppLive::Take, false, idle);
    CHECK(!s.again);
    s = lares::livelight_on_event(HidppLive::Take, false, active);
    CHECK(!s.again);

    // Gone: nothing held; back: taken again.
    s = lares::livelight_on_event(HidppLive::Idle, true, down);
    CHECK(!s.held && !s.again && s.stop_dark);
    s = lares::livelight_on_event(HidppLive::Idle, false, up);
    CHECK(s.state == HidppLive::Take && s.again && !s.held);
}

static void test_key_change_plan()
{
    lares::HidppKeyboard k;
    lares::hidpp_sample(&k);
    lares::KeyboardSettings base;
    lares::hidpp_decode(&k, lares::hidpp_model(0xc547, 0x40a2), &base);
    lares::KeyChange c;

    // A live effect goes nowhere near the keyboard: the helper plays it.
    lares::KeyboardSettings want = base;
    want.light = lares::KeyLight::Solid;
    CHECK(lares::key_change_plan(base, want, k, c));
    CHECK(lares::key_change_empty(c));

    // A kept effect goes to the profile and takes the lights back from software.
    want = base;
    want.light = lares::KeyLight::Ripple;
    want.rgb[0] = 0x00; want.rgb[1] = 0xff; want.rgb[2] = 0x00;
    CHECK(lares::key_change_plan(base, want, k, c));
    CHECK(c.light && c.release && c.lit.effect == lares::KeyLight::Ripple && c.lit.rgb[1] == 0xff);
    CHECK(c.brightness < 0 && c.dim_s < 0 && c.startup < 0);

    // From a live Solid, Built-in is a change even though the profile already says it.
    lares::KeyboardSettings live = base;
    live.light = lares::KeyLight::Solid;
    want = live;
    want.light = lares::KeyLight::Own;
    CHECK(lares::key_change_plan(live, want, k, c));
    CHECK(c.light && c.lit.effect == lares::KeyLight::Own);

    // Brightness is the device's value; timers and effects only when changed.
    want = base;
    want.brightness = 100;
    want.off_s = 600;
    want.shutdown = !base.shutdown;
    CHECK(lares::key_change_plan(base, want, k, c));
    CHECK(c.brightness == k.brightness_max && c.off_s == 600 && c.dim_s < 0 && !c.light);
    CHECK(c.shutdown == (want.shutdown ? 1 : 0) && c.startup < 0);

    // Nothing it can't hold.
    lares::HidppKeyboard bare;
    CHECK(!lares::key_change_plan(base, want, bare, c));
}

static void test_hid_find()
{
    std::vector<lares::HidCollection> all;
    auto add = [&](const wchar_t* path, uint16_t vid, uint16_t pid, uint16_t page, uint16_t usage, const wchar_t* parent) {
        lares::HidCollection c;
        c.path = path;
        c.vid = vid;
        c.pid = pid;
        c.usage_page = page;
        c.usage = usage;
        c.parent = parent;
        all.push_back(c);
    };
    add(L"\\\\?\\hid#vid_03f0&pid_0f98&mi_02#a", 0x03f0, 0x0f98, 0xff00, 1, L"USB\\VID_03F0&PID_0F98&MI_02\\x");
    add(L"\\\\?\\hid#vid_03f0&pid_0f98&mi_03#b", 0x03f0, 0x0f98, 0x0059, 1, L"USB\\VID_03F0&PID_0F98&MI_03\\x");
    add(L"\\\\?\\hid#vid_03f0&pid_0f98&mi_00#c", 0x03f0, 0x0f98, 0x0001, 2, L"USB\\VID_03F0&PID_0F98&MI_00\\x");
    add(L"\\\\?\\hid#vid_046d&pid_c547&mi_02&col01#d", 0x046d, 0xc547, 0xff00, 1, L"USB\\VID_046D&PID_C547&MI_02\\x");
    add(L"\\\\?\\hid#vid_046d&pid_c547&mi_02&col02#e", 0x046d, 0xc547, 0xff00, 2, L"USB\\VID_046D&PID_C547&MI_02\\x");
    add(L"\\\\?\\hid#hid_device_system_vhf#f", 0x046d, 0x0000, 0x0059, 1,
        L"USB\\VID_046D&PID_C547&LAMPARRAY\\1&ABCDEF&0&1_SLOT01");
    add(L"\\\\?\\hid#hid_device_system_vhf#g", 0x046d, 0x0000, 0x0059, 1,
        L"USB\\VID_046D&PID_C547&LAMPARRAY\\1&ABCDEF&0&1_SLOT00");
    auto f = lares::hid_find(all);
    CHECK(f.hyperx_receiver == L"\\\\?\\hid#vid_03f0&pid_0f98&mi_02#a");
    CHECK(f.hyperx_lamps_receiver == L"\\\\?\\hid#vid_03f0&pid_0f98&mi_03#b");
    CHECK(f.hyperx_cable.empty());
    CHECK(f.hidpp_short == L"\\\\?\\hid#vid_046d&pid_c547&mi_02&col01#d");
    CHECK(f.hidpp_long == L"\\\\?\\hid#vid_046d&pid_c547&mi_02&col02#e");
    CHECK(f.keyboard_lamps == L"\\\\?\\hid#hid_device_system_vhf#g");   // the keyboard's slot, not another's

    // The key Windows keeps a device's Dynamic Lighting under.
    CHECK(lares::lighting_key_name(L"\\\\?\\hid#vid_03f0&pid_0f98&mi_03#1&abcdef&0&0000#{4d1e55b2}") ==
          L"hid#vid_03f0&pid_0f98&mi_03#1&abcdef&0&0000#{4d1e55b2}");
    CHECK(!lares::windows_lighting_on(L""));
}

/* AirDrop's Windows end: what lares-share says, the paths between Windows
 * and WSL, and the Bluetooth wake-up's bytes. */
static void test_share()
{
    using namespace lares;

    ShareSession s;
    CHECK(!share_parse_block({ "name pc" }, s));   // no state line: not a block

    std::vector<std::string> idle = {
        "state setup=ready link=off receiving=0 left=0 sending=0 wake=0 quiet=0",
        "name my-pc",
        "card Wi-Fi card for testing",
    };
    CHECK(share_parse_block(idle, s));
    CHECK(s.ready && s.link == ShareLink::Off && !s.receiving && !s.sending && !s.wake && !s.quiet);
    CHECK(s.name == "my-pc" && s.card == "Wi-Fi card for testing");
    CHECK(s.idle() && !s.has_incoming && s.devices.empty());

    std::vector<std::string> busy = {
        "state setup=ready link=up receiving=1 left=540 sending=1 wake=1 quiet=1",
        "name my pc",
        "error the card left",
        "incoming id=3 state=receiving done=1024 total=4096 links=0",
        "incoming-sender Ana's iPhone",
        "incoming-name IMG 1.HEIC",
        "incoming-name IMG 2.HEIC",
        "incoming-message none",
        "device state=sending sent=5 total=10\tkey one\tAna's iPad\tiPad13,1\t",
        "device state=failed sent=0 total=0\tk2\tMac\t\tThey said no. Really.",
        "device state=found sent=0 total=0",   // no key: dropped
        "something new",                        // a line a later lares-share adds
    };
    CHECK(share_parse_block(busy, s));
    CHECK(s.link == ShareLink::Up && s.receiving && s.left == 540 && s.sending && s.wake && s.quiet);
    CHECK(s.name == "my pc" && s.error == "the card left" && !s.idle());
    CHECK(s.has_incoming && s.incoming.id == 3 && s.incoming.state == ShareIncomingState::Receiving);
    CHECK(s.incoming.done == 1024 && s.incoming.total == 4096 && !s.incoming.links);
    CHECK(s.incoming.sender == "Ana's iPhone" && s.incoming.names.size() == 2 && s.incoming.names[1] == "IMG 2.HEIC");
    CHECK(s.devices.size() == 2);
    CHECK(s.devices[0].state == ShareDeviceState::Sending && s.devices[0].sent == 5 && s.devices[0].total == 10);
    CHECK(s.devices[0].key == "key one" && s.devices[0].name == "Ana's iPad" && s.devices[0].model == "iPad13,1");
    CHECK(s.devices[0].message.empty());
    CHECK(s.devices[1].state == ShareDeviceState::Failed && s.devices[1].model.empty());
    CHECK(s.devices[1].message == "They said no. Really.");

    /* A name can't pass itself off as another line's fields. */
    CHECK(share_parse_block({ "state setup=ready link=off receiving=0 left=0 sending=0 wake=0 quiet=0",
                              "name pc link=up receiving=1" }, s));
    CHECK(s.link == ShareLink::Off && !s.receiving && s.name == "pc link=up receiving=1");
    CHECK(share_parse_block({ "state setup=needs-allow link=failed receiving=0 left=99999999999 sending=0 wake=0 quiet=0",
                              "incoming id=1 state=done done=0 total=0 links=1",
                              "incoming-name https://example.com/a",
                              "incoming-saved https://example.com/a" }, s));
    CHECK(!s.ready && s.link == ShareLink::Failed && s.left == 24 * 3600);
    CHECK(s.incoming.state == ShareIncomingState::Done && s.incoming.links && s.incoming.saved.size() == 1);
    CHECK(!s.idle());   // a card that's still shown keeps the session

    CHECK(wsl_path_from_windows(L"C:\\Users\\me\\My file.txt") == "/mnt/c/Users/me/My file.txt");
    CHECK(wsl_path_from_windows(L"d:/x/y.bin") == "/mnt/d/x/y.bin");
    CHECK(wsl_path_from_windows(L"E:\\ni\u00F1o\\\U0001F600.png") == "/mnt/e/ni\xC3\xB1o/\xF0\x9F\x98\x80.png");
    CHECK(wsl_path_from_windows(L"\\\\server\\share\\a.txt").empty());
    CHECK(wsl_path_from_windows(L"relative\\a.txt").empty());
    CHECK(wsl_path_from_windows(L"C:").empty());
    CHECK(windows_path_from_wsl("/mnt/c/Users/me/Downloads/IMG 1.HEIC") == L"C:\\Users\\me\\Downloads\\IMG 1.HEIC");
    CHECK(windows_path_from_wsl("/mnt/e/ni\xC3\xB1o/\xF0\x9F\x98\x80.png") == L"E:\\ni\u00F1o\\\U0001F600.png");
    CHECK(windows_path_from_wsl("/home/me/a.txt").empty());
    CHECK(windows_path_from_wsl("/mnt/wsl/a.txt").empty());
    CHECK(windows_path_from_wsl("/mnt/c").empty());

    ShareIncoming in;
    CHECK(share_offer_text(in) == "files");
    in.names = { "a.jpg" };
    CHECK(share_offer_text(in) == "a.jpg");
    in.names = { "a.jpg", "b.jpg", "c.jpg" };
    CHECK(share_offer_text(in) == "a.jpg and 2 more");
    in.links = true;
    CHECK(share_offer_text(in) == "links");
    in.names = { "https://example.com" };
    CHECK(share_offer_text(in) == "a link");

    CHECK(share_format_size(0) == "0 bytes");
    CHECK(share_format_size(999) == "999 bytes");
    CHECK(share_format_size(1000) == "1.0 kB");
    CHECK(share_format_size(5200000) == "5.2 MB");
    CHECK(share_format_size(3400000000ull) == "3.4 GB");

    CHECK(share_link_opens("https://example.com/a?b=c"));
    CHECK(share_link_opens("HTTP://example.com"));
    CHECK(!share_link_opens("https://"));
    CHECK(!share_link_opens("file:///C:/Windows/system32/calc.exe"));
    CHECK(!share_link_opens("C:\\Windows\\system32\\calc.exe"));
    CHECK(!share_link_opens("javascript:alert(1)"));
    CHECK(!share_link_opens("https://example.com/\" & calc"));
    CHECK(!share_link_opens("https://example.com/a b"));

    /* The same bytes src/airdrop/ble.c hands BlueZ, after the type and length. */
    const uint8_t tag[3] = { 0xAA, 0xBB, 0xCC };
    uint8_t wake[kAirdropWakeBytes];
    airdrop_wake_data(tag, wake);
    const uint8_t want[kAirdropWakeBytes] = { 0x05, 0x12, 0x40, 0xAA, 0xBB, 0xCC, 0, 0, 0, 0, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(std::memcmp(wake, want, sizeof(want)) == 0);
}

int main()
{
    test_share();
    test_livelight_steps();
    test_key_change_plan();
    test_hid_find();
    test_cli();
    test_theme();
    test_config();
    test_shared_art();
    test_hw_helpers();
    test_remote_helpers();
    test_hyperx_and_hidpp();
    test_pad_map_and_controller();
    test_helper();
    test_hidpp_allowed_matrix();
    test_config_escaping_and_pad_shutdown();
    if (failures == 0) {
        std::printf("lares-win-unit: all passed\n");
        return 0;
    }
    std::fprintf(stderr, "lares-win-unit: %d failure(s)\n", failures);
    return 1;
}
