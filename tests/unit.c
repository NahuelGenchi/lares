// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the parts of Lares that work on plain data: nickname keys,
 * battery stages and wording, screen sizes, and how keyboards and mice are
 * told apart in sysfs. `make check` runs them. */
#include <glib.h>
#include <string.h>
#include "../src/devices.h"
#include "../src/pad/pads.h"
#include "../src/remote_model.h"

/* The documentation address of RFC 7042, never a real device's. */
#define ADDRESS "00:00:5E:00:53:01"

static void test_nickname_key(void)
{
    Pad pad = { 0 };
    pad.address = (char *)ADDRESS;
    char *key = pad_nickname_key(&pad, "salt-123");
    /* Padlight's key for the same address and salt, worked out apart with
     * Python's hmac module: nicknames carry over from Padlight. */
    g_assert_cmpstr(key, ==, "993745de2606c47246364ec0c9f33eab56376221dcb5e31799b9a3ac6e72c49a");
    g_assert_null(strstr(key, "00005e005301"));
    g_free(key);

    Pad wired = { 0 };
    wired.serial = (char *)"3039363234333530";
    key = pad_nickname_key(&wired, "salt-123");
    g_assert_cmpint(strlen(key), ==, 64);
    g_free(key);

    Pad unknown = { 0 };
    g_assert_false(pad_can_nickname(&unknown));
    g_assert_null(pad_nickname_key(&unknown, "salt-123"));
    g_assert_null(pad_nickname_key(&pad, NULL));
}

static void test_clean_nickname(void)
{
    char *c = pad_clean_nickname("  Blue \t  pad  ");
    g_assert_cmpstr(c, ==, "Blue pad");
    g_free(c);
    c = pad_clean_nickname("abcdefghijklmnopqrstuvwxyz0123456789");
    g_assert_cmpint(g_utf8_strlen(c, -1), ==, PAD_NICKNAME_LENGTH);
    g_free(c);
    c = pad_clean_nickname("   ");
    g_assert_cmpstr(c, ==, "");
    g_free(c);
}

static void test_battery(void)
{
    PadBattery b = { .known = TRUE, .percentage = 50 };
    g_assert_cmpint(pad_battery_low_stage(&b, 20), ==, 0);
    b.percentage = 15;
    g_assert_cmpint(pad_battery_low_stage(&b, 20), ==, 1);
    b.percentage = 8;
    g_assert_cmpint(pad_battery_low_stage(&b, 20), ==, 2);
    /* A low level of 5 % never makes 8 % "very low" at 10 %. */
    g_assert_cmpint(pad_battery_low_stage(&b, 5), ==, 0);

    /* Pads that report words instead of percents */
    PadBattery w = { .known = TRUE, .level = 3 };
    g_assert_cmpint(pad_battery_low_stage(&w, 20), ==, 1);
    w.level = 4;
    g_assert_cmpint(pad_battery_low_stage(&w, 20), ==, 2);
    w.level = 6;
    g_assert_cmpint(pad_battery_low_stage(&w, 20), ==, 0);
    char *charge = pad_battery_charge(&w);
    g_assert_cmpstr(charge, ==, "Normal");
    g_free(charge);

    PadBattery c = { .known = TRUE, .percentage = 85, .state = 1 };
    g_assert_true(pad_battery_charging(&c));
    char *text = pad_battery_text(&c);
    g_assert_cmpstr(text, ==, "85% · Charging");
    g_free(text);
}

static void test_screen(void)
{
    int w, h;
    double r;
    g_assert_true(parse_screen("1680x1050@60", &w, &h, &r));
    g_assert_cmpint(w, ==, 1680);
    g_assert_cmpint(h, ==, 1050);
    g_assert_cmpfloat(r, ==, 60);
    g_assert_false(parse_screen("leave", &w, &h, &r));
    char *d = remote_describe(ITEM_SCREEN, "1920x1080@59.94");
    g_assert_cmpstr(d, ==, "1920 × 1080 · 60 Hz");
    g_free(d);
    d = remote_describe(ITEM_SUNSHINE, "leave");
    g_assert_cmpstr(d, ==, "Not changed");
    g_free(d);
    d = remote_describe(ITEM_TAILSCALE, "on");
    g_assert_cmpstr(d, ==, "On");
    g_free(d);
}

/* Capability bitmaps as a 64-bit kernel shows them, from a wireless mouse
 * whose receiver also has a keyboard node, a keyboard on a receiver, a
 * wired gamepad and a power button. */
#define MOUSE_KEYS   "1f0000 0 0 0 0"
#define MOUSE_REL    "903"
#define KEYBOARD_KEYS "733eff 0 0 483ffe317aff32d bfc4444600000000 1 130c730b17c007 ffa07bfad9415fff " \
                      "febeffdff3cfffff fffffffffffffffe"
#define GAMEPAD_KEYS "7cdb000000000000 0 8000000000 0 0"
#define POWER_KEYS   "8000 10000000000000 0"

static void test_input_kinds(void)
{
    g_assert_true(devices_bitmap_has(MOUSE_KEYS, 0x110));   /* left button */
    g_assert_true(devices_bitmap_has(MOUSE_KEYS "\n", 0x114));
    g_assert_false(devices_bitmap_has(MOUSE_KEYS, 0x115));
    g_assert_false(devices_bitmap_has(MOUSE_KEYS, 0x400));  /* past the last word */
    g_assert_true(devices_bitmap_has(MOUSE_REL, 1));
    g_assert_false(devices_bitmap_has("0", 0));
    g_assert_false(devices_bitmap_has(NULL, 0));

    g_assert_cmpint(devices_input_kind(MOUSE_KEYS, MOUSE_REL), ==, DEV_MOUSE);
    g_assert_cmpint(devices_input_kind(KEYBOARD_KEYS, "0"), ==, DEV_KEYBOARD);
    /* A keyboard's sideways scroll wheel doesn't make it a mouse. */
    g_assert_cmpint(devices_input_kind(KEYBOARD_KEYS, "1040"), ==, DEV_KEYBOARD);
    g_assert_cmpint(devices_input_kind(GAMEPAD_KEYS, "0"), ==, DEV_CONTROLLER);
    g_assert_cmpint(devices_input_kind(POWER_KEYS, "0"), ==, -1);
    g_assert_cmpint(devices_input_kind(NULL, NULL), ==, -1);
}

#define HUB "/sys/devices/pci0000:00/0000:00:14.0/usb1"

static void test_input_owner(void)
{
    /* Both nodes of a mouse on its own receiver belong to the receiver. */
    char *a = devices_input_owner(HUB "/1-1/1-1:1.0/0003:03F0:0F98.0001");
    char *b = devices_input_owner(HUB "/1-1/1-1:1.1/0003:03F0:0F98.0002");
    g_assert_cmpstr(a, ==, HUB "/1-1");
    g_assert_cmpstr(b, ==, HUB "/1-1");
    g_free(a);
    g_free(b);
    /* A keyboard paired to a receiver behind a hub is its own device. */
    a = devices_input_owner(HUB "/1-8/1-8.4/1-8.4:1.2/0003:046D:C547.0007/0003:046D:40A2.0009");
    g_assert_cmpstr(a, ==, HUB "/1-8/1-8.4/1-8.4:1.2/0003:046D:C547.0007/0003:046D:40A2.0009");
    g_free(a);
    /* A device on a hub port belongs to that port, not the hub. */
    a = devices_input_owner(HUB "/1-8/1-8.2/1-8.2:1.0");
    g_assert_cmpstr(a, ==, HUB "/1-8/1-8.2");
    g_free(a);
    g_assert_null(devices_input_owner("/sys/devices/platform/i8042/serio0"));
    g_assert_null(devices_input_owner("/sys/devices/virtual/misc/uinput"));
    g_assert_null(devices_input_owner(NULL));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nicknames/key", test_nickname_key);
    g_test_add_func("/nicknames/clean", test_clean_nickname);
    g_test_add_func("/battery/stages-and-words", test_battery);
    g_test_add_func("/remote/screen-sizes", test_screen);
    g_test_add_func("/devices/input-kinds", test_input_kinds);
    g_test_add_func("/devices/input-owner", test_input_owner);
    return g_test_run();
}
