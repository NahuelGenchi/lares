// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the parts of Lares that work on plain data: nickname keys,
 * battery stages and wording, screen sizes, the controller picture's map,
 * how keyboards and mice are told apart in sysfs, HyperX mice's batteries
 * and settings, and Logitech HID++ requests and profiles. `make check` runs
 * them from the repository's root. */
#include <glib.h>
#include <string.h>
#include "../src/devices.h"
#include "../src/hidpp.h"
#include "../src/hyperx.h"
#include "../src/pad/pads.h"
#include "../src/pad_map.h"
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

/* Services are on or off: an old "leave" loads as the mode's own value;
 * the screen keeps it. */
static void test_saved_values(void)
{
    char *v = remote_saved_value(MODE_HOME, ITEM_SUNSHINE, "leave");
    g_assert_cmpstr(v, ==, "off");
    g_free(v);
    v = remote_saved_value(MODE_REMOTE, ITEM_TAILSCALE, "leave");
    g_assert_cmpstr(v, ==, "on");
    g_free(v);
    v = remote_saved_value(MODE_HOME, ITEM_TAILSCALE, "on");
    g_assert_cmpstr(v, ==, "on");
    g_free(v);
    v = remote_saved_value(MODE_REMOTE, ITEM_SCREEN, "leave");
    g_assert_cmpstr(v, ==, "leave");
    g_free(v);
    v = remote_saved_value(MODE_REMOTE, ITEM_SUNSHINE, NULL);
    g_assert_cmpstr(v, ==, "on");
    g_free(v);
}

/* A mode's screen until a size is picked: the monitor's preferred mode, its
 * current one when none is preferred, nothing before it's read. */
static void test_own_size(void)
{
    Live l = { 0 };
    g_assert_null(remote_own_size(&l));
    ScreenMode big = { .w = 3840, .h = 2160, .rate = 59.94, .current = TRUE };
    ScreenMode native = { .w = 2560, .h = 1440, .rate = 143.97, .preferred = TRUE };
    l.modes = g_ptr_array_new();
    g_ptr_array_add(l.modes, &big);
    char *v = remote_own_size(&l);
    g_assert_cmpstr(v, ==, "3840x2160@60");
    g_free(v);
    g_ptr_array_add(l.modes, &native);
    v = remote_own_size(&l);
    g_assert_cmpstr(v, ==, "2560x1440@144");
    g_free(v);
    g_ptr_array_unref(l.modes);
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

/* Reports as the Pulsefire Haste 2 Wireless's receiver sends them. */
static void test_hid_battery(void)
{
    int level = -1;
    gboolean charging = TRUE;
    /* 54 %, not charging, 26 °C, 3840 mV */
    const guint8 answer[64] = { 0x51, 0x02, 0x36, 0x00, 0x1a, 0x00, 0x00, 0x0f };
    g_assert_true(hyperx_battery_parse(answer, sizeof answer, &level, &charging));
    g_assert_cmpint(level, ==, 54);
    g_assert_false(charging);
    const guint8 on_cable[64] = { 0x51, 0x02, 0x64, 0x01 };
    g_assert_true(hyperx_battery_parse(on_cable, sizeof on_cable, &level, &charging));
    g_assert_cmpint(level, ==, 100);
    g_assert_true(charging);
    const guint8 full[64] = { 0x51, 0x02, 0x64, 0x02 };
    g_assert_true(hyperx_battery_parse(full, sizeof full, &level, &charging));
    g_assert_false(charging);

    /* The receiver's acknowledgement, a link event, a wrong level and a
     * short read are not answers. */
    const guint8 ack[64] = { 0xff, 0x01, 0xbf, 0xb4 };
    const guint8 link[64] = { 0xfb, 0x0a, 0x01 };
    const guint8 wrong[64] = { 0x51, 0x02, 0x65, 0x00 };
    g_assert_false(hyperx_battery_parse(ack, sizeof ack, &level, &charging));
    g_assert_false(hyperx_battery_parse(link, sizeof link, &level, &charging));
    g_assert_false(hyperx_battery_parse(wrong, sizeof wrong, &level, &charging));
    g_assert_false(hyperx_battery_parse(answer, 3, &level, &charging));

    const HyperxModel *m = hyperx_model(0x03f0, 0x0f98);
    g_assert_nonnull(m);
    g_assert_cmpint(m->interface, ==, 2);
    g_assert_true(m->receiver);
    g_assert_null(hyperx_model(0x03f0, 0x0f99));
    g_assert_null(hyperx_model(0x046d, 0x0d97));

    /* The rule opens the battery interface only, to the person at the screen. */
    char *rules = hyperx_udev_rules();
    g_assert_nonnull(strstr(rules, "SUBSYSTEM==\"hidraw\", ENV{ID_USB_VENDOR_ID}==\"03f0\", "
                                   "ENV{ID_USB_MODEL_ID}==\"0f98\", ENV{ID_USB_INTERFACE_NUM}==\"02\", "
                                   "TAG+=\"uaccess\"\n"));
    g_assert_null(strstr(rules, "MODE="));  /* no world-readable node */
    g_free(rules);
}

/* The Pulsefire Haste 2 Wireless plugged in by its cable: a USB device of
 * its own, which answers on one interface, and whose receiver then says
 * it isn't linked. */
static void test_hyperx_cable(void)
{
    const HyperxModel *receiver = hyperx_model(0x03f0, 0x0f98), *wired = hyperx_model(0x03f0, 0x0d97);
    g_assert_nonnull(wired);
    g_assert_false(wired->receiver);
    g_assert_cmpint(wired->interface, ==, 2);
    g_assert_cmpint(wired->dpi_max, ==, receiver->dpi_max);
    g_assert_cmpint(wired->rate_max, ==, receiver->rate_max);

    /* The rule opens that interface, and only that one, as well. */
    char *rules = hyperx_udev_rules();
    g_assert_nonnull(strstr(rules, "SUBSYSTEM==\"hidraw\", ENV{ID_USB_VENDOR_ID}==\"03f0\", "
                                   "ENV{ID_USB_MODEL_ID}==\"0d97\", ENV{ID_USB_INTERFACE_NUM}==\"02\", "
                                   "TAG+=\"uaccess\"\n"));
    g_assert_nonnull(strstr(rules, "ENV{ID_USB_MODEL_ID}==\"0f98\""));
    int lines = 0;
    for (const char *c = strstr(rules, "0d97"); c; c = strstr(c + 1, "0d97")) lines++;
    g_assert_cmpint(lines, ==, 1);
    g_free(rules);

    /* The receiver gives way to the mouse on its cable, whatever else is
     * plugged in, and only to it. */
    const HyperxModel *both[] = { receiver, NULL, wired };
    g_assert_true(hyperx_on_cable(receiver, both, G_N_ELEMENTS(both)));
    g_assert_false(hyperx_on_cable(wired, both, G_N_ELEMENTS(both)));
    g_assert_false(hyperx_on_cable(NULL, both, G_N_ELEMENTS(both)));
    const HyperxModel *alone[] = { receiver, NULL };
    g_assert_false(hyperx_on_cable(receiver, alone, G_N_ELEMENTS(alone)));
    g_assert_false(hyperx_on_cable(receiver, NULL, 0));
    const HyperxModel *cable_only[] = { wired };
    g_assert_true(hyperx_on_cable(receiver, cable_only, 1));

    /* Its answers on the cable, as it gave them: charged full, and its
     * settings in the same layout as over the receiver. */
    int level = -1;
    gboolean charging = TRUE;
    const guint8 battery[64] = { 0x51, 0x02, 0x64, 0x02, 0x18, 0x00, 0x55, 0x10, 0x01 };
    g_assert_true(hyperx_battery_parse(battery, sizeof battery, &level, &charging));
    g_assert_cmpint(level, ==, 100);
    g_assert_false(charging);
    static const guint8 dpi[] = {
        0x33, 0x02, 0x08, 0x0f, 0x01,
        0x07, 0x00, 0xff, 0x00, 0x00,   0x0f, 0x00, 0x00, 0x00, 0xff,   0x1f, 0x00, 0xff, 0xff, 0x00,
        0x3f, 0x00, 0x00, 0xff, 0x00,   0x07, 0x02, 0xff, 0xff, 0xff,
    };
    static const guint8 light[] = { 0x43, 0x03, 0x01, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0xff };
    HyperxState s = { 0 };
    memcpy(s.dpi, dpi, sizeof dpi);
    s.sensor[0] = 0x37;
    s.sensor[1] = 0x02;
    s.brightness[0] = 0x41;
    s.brightness[1] = 0x02;
    s.brightness[2] = 0xff;
    memcpy(s.light, light, sizeof light);
    MouseSettings m;
    hyperx_decode(wired, &s, &m);
    g_assert_cmpint(m.stages, ==, 4);   /* the fifth stage, 26000, is off */
    g_assert_cmpint(m.active, ==, 1);
    const int want[] = { 400, 800, 1600, 3200 };
    for (int i = 0; i < 4; i++) g_assert_cmpint(m.dpi[i], ==, want[i]);
    g_assert_cmpint(m.rate, ==, 1000);
    g_assert_cmpint(m.lift_mm, ==, 1);
    g_assert_cmpint(m.brightness, ==, 100);
    g_assert_cmpint(m.light, ==, LIGHT_SOLID);
    g_assert_cmpint(m.rgb[0], ==, 0x80);
    g_assert_cmpint(m.rgb[1], ==, 0x00);
    g_assert_cmpint(m.rgb[2], ==, 0xff);
    g_assert_cmpint(m.dpi_max, ==, 26000);
}

/* The settings as the protocol notes read them from a Pulsefire Haste 2
 * Wireless (hyperx_sample), decoded and changed. */
static void test_hyperx_settings(void)
{
    HyperxState s;
    hyperx_sample(&s);
    MouseSettings m;
    hyperx_decode(hyperx_model(0x03f0, 0x0f98), &s, &m);
    g_assert_cmpint(m.stages, ==, 5);
    g_assert_cmpint(m.active, ==, 2);
    const int dpi[] = { 400, 800, 1600, 3200, 6400 };
    for (int i = 0; i < 5; i++) g_assert_cmpint(m.dpi[i], ==, dpi[i]);
    g_assert_cmpint(m.rate, ==, 1000);
    g_assert_cmpint(m.lift_mm, ==, 1);
    g_assert_cmpint(m.brightness, ==, 100);
    g_assert_cmpint(m.light, ==, LIGHT_OTHER);
    g_assert_cmpint(m.dpi_min, ==, 200);
    g_assert_cmpint(m.dpi_max, ==, 26000);

    /* Nothing to change, nothing sent. */
    guint8 p[HYPERX_BLOCKS][HYPERX_REPORT];
    g_assert_cmpint(hyperx_packets(&s, &m, 0, p), ==, 0);

    /* A new active stage and polling rate rewrite the DPI table and keep the
     * stages' colours. */
    MouseSettings want = m;
    want.active = 3;
    want.rate = 500;
    want.dpi[3] = 3600;
    g_assert_cmpint(hyperx_packets(&s, &want, 1, p), ==, 1);
    const guint8 table[] = { 0x32, 0x01, 0x01, 0x00, 0x10, 0x1f, 0x03,
                             0x07, 0x00, 0xff, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xff,
                             0x1f, 0x00, 0xff, 0xff, 0x00, 0x47, 0x00, 0x00, 0xff, 0x00,
                             0x7f, 0x00, 0xff, 0xff, 0xff };
    g_assert_cmpmem(p[0], sizeof table, table, sizeof table);

    /* Lift-off, brightness and a solid colour: one packet each, in the
     * protocol's own words (51 % is 0x82, as NGENUITY sends it). */
    want = m;
    want.lift_mm = 2;
    want.brightness = 51;
    want.light = LIGHT_SOLID;
    want.rgb[0] = 0xff;
    want.rgb[1] = want.rgb[2] = 0;
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, 3);
    const guint8 sensor[] = { 0x36, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00 };
    const guint8 bright[] = { 0x40, 0x01, 0x00, 0x00, 0x82, 0x00 };
    const guint8 solid[] = { 0x42, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00 };
    g_assert_cmpmem(p[0], sizeof sensor, sensor, sizeof sensor);
    g_assert_cmpmem(p[1], sizeof bright, bright, sizeof bright);
    g_assert_cmpmem(p[2], sizeof solid, solid, sizeof solid);

    /* The mouse, given those packets, holds what was wanted. */
    for (int i = 0; i < 3; i++) hyperx_apply(&s, p[i]);
    MouseSettings got;
    hyperx_decode(NULL, &s, &got);
    g_assert_cmpint(got.lift_mm, ==, 2);
    g_assert_cmpint(got.brightness, ==, 51);
    g_assert_cmpint(got.light, ==, LIGHT_SOLID);
    g_assert_cmpmem(got.rgb, 3, want.rgb, 3);

    /* Breathing and the colour cycle, as tested on the mouse. */
    want = got;
    want.light = LIGHT_BREATHING;
    want.rgb[0] = 0;
    want.rgb[1] = 0xff;
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, 1);
    const guint8 breath[] = { 0x42, 0x02, 0x00, 0x00, 0x02, 0x32, 0x00, 0x00, 0x01, 0x00, 0xff, 0x00 };
    g_assert_cmpmem(p[0], sizeof breath, breath, sizeof breath);
    want.light = LIGHT_CYCLE;
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, 1);
    const guint8 cycle[] = { 0x42, 0x02, 0x00, 0x00, 0x04, 0x32, 0x00, 0x00, 0xfe, 0x00 };
    g_assert_cmpmem(p[0], sizeof cycle, cycle, sizeof cycle);

    /* Every brightness survives the trip to a byte and back. */
    for (int pct = 0; pct <= 100; pct++) {
        want = got;
        want.brightness = pct;
        int n = hyperx_packets(&s, &want, 0, p);
        HyperxState t = s;
        for (int i = 0; i < n; i++) hyperx_apply(&t, p[i]);
        MouseSettings back;
        hyperx_decode(NULL, &t, &back);
        g_assert_cmpint(back.brightness, ==, pct);
    }

    /* What the mouse can't hold is refused, not sent. */
    hyperx_sample(&s);
    want = m;
    want.dpi[0] = 425;
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, -1);
    want = m;
    want.dpi[0] = 26050;
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, -1);
    want = m;
    want.rate = 2000;
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, -1);
    want = m;
    want.active = 5;
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, -1);
    want = m;
    want.light = LIGHT_OTHER;
    want.lift_mm = 2;
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, 1);  /* only the lift-off */
}

/* The answers of a real Pulsefire Haste 2 Wireless: four stages in use
 * (400, 800 in use, 1600, 3200) and a fifth, at 26000, left out; the
 * firmware's colour cycle, with stale bytes where a colour would be. */
static void test_hyperx_real(void)
{
    HyperxState s = { 0 };
    const guint8 dpi[] = { 0x33, 0x02, 0x08, 0x0f, 0x01, 0x07, 0x00, 0xff, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xff,
                           0x1f, 0x00, 0xff, 0xff, 0x00, 0x3f, 0x00, 0x00, 0xff, 0x00, 0x07, 0x02, 0xff, 0xff, 0xff };
    const guint8 light[] = { 0x43, 0x03, 0x04, 0x2b, 0x00, 0x00, 0xfe, 0xff, 0xff, 0xff };
    memcpy(s.dpi, dpi, sizeof dpi);
    memcpy(s.light, light, sizeof light);
    s.sensor[0] = 0x37;
    s.brightness[0] = 0x41;
    s.brightness[2] = 0xff;
    MouseSettings m;
    hyperx_decode(NULL, &s, &m);
    g_assert_cmpint(m.stages, ==, 4);
    g_assert_cmpint(m.active, ==, 1);
    g_assert_cmpint(m.dpi[1], ==, 800);
    g_assert_cmpint(m.dpi[3], ==, 3200);
    g_assert_cmpint(m.light, ==, LIGHT_CYCLE);
    const guint8 none[3] = { 0 };
    g_assert_cmpmem(m.rgb, 3, none, 3);

    /* A new stage in use leaves the left-out one as it was. */
    MouseSettings want = m;
    want.active = 3;
    guint8 p[HYPERX_BLOCKS][HYPERX_REPORT];
    g_assert_cmpint(hyperx_packets(&s, &want, 0, p), ==, 1);
    g_assert_cmpint(p[0][6], ==, 3);
    g_assert_cmpmem(p[0] + 7, 25, dpi + 5, 25);
    hyperx_apply(&s, p[0]);
    hyperx_decode(NULL, &s, &m);
    g_assert_cmpint(m.active, ==, 3);
    g_assert_cmpint(m.stages, ==, 4);
}

static void test_hyperx_events(void)
{
    HyperxEvent ev;
    const guint8 stage[64] = { 0xfb, 0x08, 0x03, 0x3f, 0x00 };
    g_assert_true(hyperx_event(stage, sizeof stage, &ev));
    g_assert_cmpint(ev.kind, ==, HYPERX_STAGE);
    g_assert_cmpint(ev.stage, ==, 3);
    g_assert_cmpint(ev.code, ==, 0x3f);
    /* The sample's stage 3 is 3200 DPI, so the event is taken; the one a
     * click sends on waking, stage 0 with code 0, matches nothing. */
    HyperxState s;
    hyperx_sample(&s);
    g_assert_true(hyperx_take_stage(&s, &ev));
    g_assert_cmpint(s.dpi[4], ==, 3);
    const guint8 waking[64] = { 0xfb, 0x08, 0x00, 0x00, 0x00 };
    g_assert_true(hyperx_event(waking, sizeof waking, &ev));
    g_assert_false(hyperx_take_stage(&s, &ev));
    g_assert_cmpint(s.dpi[4], ==, 3);
    const guint8 off_mask[64] = { 0xfb, 0x08, 0x06, 0x3f, 0x00 };
    g_assert_true(hyperx_event(off_mask, sizeof off_mask, &ev));
    g_assert_false(hyperx_take_stage(&s, &ev));
    const guint8 asleep[64] = { 0xfb, 0x0a, 0x00 }, awake[64] = { 0xfb, 0x0a, 0x01 };
    g_assert_true(hyperx_event(asleep, sizeof asleep, &ev));
    g_assert_cmpint(ev.kind, ==, HYPERX_LINK);
    g_assert_false(ev.linked);
    g_assert_true(hyperx_event(awake, sizeof awake, &ev));
    g_assert_true(ev.linked);
    const guint8 battery[64] = { 0x51, 0x02, 0x36, 0x01 };
    g_assert_true(hyperx_event(battery, sizeof battery, &ev));
    g_assert_cmpint(ev.kind, ==, HYPERX_BATTERY);
    g_assert_cmpint(ev.level, ==, 54);
    g_assert_true(ev.charging);
    /* Button presses, answers and acknowledgements are not events Lares uses. */
    const guint8 button[64] = { 0xfb, 0x01, 0x20, 0x00, 0x01 };
    const guint8 table[64] = { 0x33, 0x02, 0x08, 0x1f, 0x02 };
    const guint8 ack[64] = { 0xff, 0x01 };
    g_assert_false(hyperx_event(button, sizeof button, &ev));
    g_assert_false(hyperx_event(table, sizeof table, &ev));
    g_assert_false(hyperx_event(ack, sizeof ack, &ev));
}

/* A change carries only what the person changed: what the mouse changed
 * by itself meanwhile, here its DPI button, stays. */
static void test_hyperx_merge(void)
{
    const HyperxModel *model = hyperx_model(0x03f0, 0x0f98);
    HyperxState now;
    hyperx_sample(&now);
    MouseSettings base, want, out;
    hyperx_decode(model, &now, &base);
    now.dpi[4] = 4;                  /* the DPI button, after the view read it */

    want = base;
    want.light = LIGHT_SOLID;
    want.rgb[0] = 0xff;
    hyperx_merge(&now, &base, &want, &out);
    g_assert_cmpint(out.active, ==, 4);
    g_assert_cmpint(out.light, ==, LIGHT_SOLID);
    g_assert_cmpint(out.rgb[0], ==, 0xff);
    g_assert_cmpint(out.dpi_max, ==, model->dpi_max);

    want = base;
    want.active = 0;
    want.dpi[1] = 900;
    hyperx_merge(&now, &base, &want, &out);
    g_assert_cmpint(out.active, ==, 0);
    g_assert_cmpint(out.dpi[1], ==, 900);
    g_assert_cmpint(out.dpi[2], ==, 1600);
    g_assert_cmpint(out.light, ==, LIGHT_OTHER);   /* the sample's animation, untouched */

    /* Nothing changed: nothing to send. */
    guint8 packets[HYPERX_BLOCKS][HYPERX_REPORT];
    hyperx_merge(&now, &base, &base, &out);
    g_assert_cmpint(hyperx_packets(&now, &out, 0, packets), ==, 0);
}

/* Only a Solid light's new brightness is streamed: the mouse's own effect
 * takes it up late, and only a still colour can stand in for it. */
static void test_hyperx_shows(void)
{
    HyperxState s;
    hyperx_sample(&s);
    MouseSettings was, now;
    hyperx_decode(NULL, &s, &was);
    was.light = LIGHT_SOLID;
    now = was;
    g_assert_false(hyperx_shows(&was, &now));
    now.brightness = 40;
    g_assert_true(hyperx_shows(&was, &now));
    now.light = LIGHT_BREATHING;
    g_assert_false(hyperx_shows(&was, &now));
    now.light = LIGHT_CYCLE;
    g_assert_false(hyperx_shows(&was, &now));
    now = was;
    now.rgb[1] = 0x80;               /* a new colour shows at once by itself */
    g_assert_false(hyperx_shows(&was, &now));
}

static void test_usb_parent(void)
{
    char *p = devices_usb_parent("/sys/devices/pci0000:00/0000:00:14.0/usb1/1-8/1-8.4/1-8.4:1.2/"
                                 "0003:046D:C547.0007/0003:046D:40A2.0009");
    g_assert_cmpstr(p, ==, "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-8/1-8.4");
    g_free(p);
    g_assert_null(devices_usb_parent("/sys/devices/platform/i8042/serio0"));
    g_assert_null(devices_usb_parent(NULL));
}

/* Logitech HID++ 2.0 requests, as a G715 on its Lightspeed receiver
 * answered them (issue #23). */
static void test_hidpp_packets(void)
{
    guint8 out[HIDPP_LONG_LEN];
    /* Root getFeature(0x8040) to device 1, with Lares' software ID. */
    const guint8 id[] = { 0x80, 0x40 };
    g_assert_cmpuint(hidpp_packet(1, 0x00, 0, id, sizeof id, out), ==, HIDPP_SHORT_LEN);
    const guint8 get_feature[] = { 0x10, 0x01, 0x00, 0x0c, 0x80, 0x40, 0x00 };
    g_assert_cmpmem(out, HIDPP_SHORT_LEN, get_feature, sizeof get_feature);
    /* memoryRead of sector 1 at 0xf0 takes a long report. */
    const guint8 where[] = { 0x00, 0x01, 0x00, 0xf0 };
    g_assert_cmpuint(hidpp_packet(1, 0x10, 5, where, sizeof where, out), ==, HIDPP_LONG_LEN);
    const guint8 read[] = { 0x11, 0x01, 0x10, 0x5c, 0x00, 0x01, 0x00, 0xf0 };
    g_assert_cmpmem(out, sizeof read, read, sizeof read);
    g_assert_cmpint(out[19], ==, 0);
    guint8 many[HIDPP_PARAMS + 1] = { 0 };
    g_assert_cmpuint(hidpp_packet(1, 0x10, 5, many, sizeof many, out), ==, 0);
    g_assert_cmpuint(hidpp_packet(1, 0x10, 16, NULL, 0, out), ==, 0);

    /* Its answer, the device's refusal and the receiver's; then the
     * kernel's answer to the same question, another device, and an event. */
    hidpp_packet(1, 0x00, 0, id, sizeof id, out);
    const guint8 answer[] = { 0x11, 0x01, 0x00, 0x0c, 0x0f, 0x00, 0x00 };
    const guint8 refused[] = { 0x11, 0x01, 0xff, 0x00, 0x0c, 0x05, 0x00 };
    const guint8 asleep[] = { 0x10, 0x01, 0x8f, 0x00, 0x0c, 0x09, 0x00 };
    const guint8 kernel[] = { 0x11, 0x01, 0x00, 0x01, 0x0f, 0x00, 0x00 };
    const guint8 other[] = { 0x11, 0x02, 0x00, 0x0c, 0x0f, 0x00, 0x00 };
    const guint8 event[] = { 0x11, 0x01, 0x0f, 0x00, 0x00, 0x32, 0x00 };
    g_assert_cmpint(hidpp_match(answer, sizeof answer, out), ==, HIDPP_ANSWER);
    g_assert_cmpint(hidpp_match(refused, sizeof refused, out), ==, HIDPP_ERROR);
    g_assert_cmpint(hidpp_match(asleep, sizeof asleep, out), ==, HIDPP_RECEIVER_ERROR);
    g_assert_cmpint(hidpp_match(kernel, sizeof kernel, out), ==, HIDPP_OTHER);
    g_assert_cmpint(hidpp_match(other, sizeof other, out), ==, HIDPP_OTHER);
    g_assert_cmpint(hidpp_match(event, sizeof event, out), ==, HIDPP_OTHER);
    g_assert_cmpint(hidpp_match(answer, 4, out), ==, HIDPP_OTHER);
}

/* Only the reads Lares needs, setBrightness, and a whole write of profile
 * sector 1 ever leave. */
static void test_hidpp_allowed(void)
{
    const guint8 get[] = { 0x00 }, take[] = { 0x01, 0x03, 0x05 };
    g_assert_true(hidpp_allowed(HIDPP_ROOT, 0, NULL, 0));
    g_assert_true(hidpp_allowed(HIDPP_BRIGHTNESS, 1, NULL, 0));
    g_assert_true(hidpp_allowed(HIDPP_BRIGHTNESS, 2, NULL, 0));
    g_assert_true(hidpp_allowed(HIDPP_PROFILES, 5, NULL, 0));
    g_assert_true(hidpp_allowed(HIDPP_RGB_EFFECTS, 5, get, sizeof get));
    /* Taking the lighting into software control, setting an effect, the
     * onboard mode or profile, profile memory writes, firmware updates,
     * resets, hidden features and unknown features are refused. */
    g_assert_false(hidpp_allowed(HIDPP_RGB_EFFECTS, 5, take, sizeof take));
    g_assert_false(hidpp_allowed(HIDPP_RGB_EFFECTS, 5, NULL, 0));
    g_assert_false(hidpp_allowed(HIDPP_RGB_EFFECTS, 1, NULL, 0));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 1, NULL, 0));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 3, NULL, 0));
    g_assert_false(hidpp_allowed(0x00d0, 0, NULL, 0));
    g_assert_false(hidpp_allowed(0x1802, 0, NULL, 0));
    g_assert_false(hidpp_allowed(0x1e00, 0, NULL, 0));
    g_assert_false(hidpp_allowed(0x8081, 1, NULL, 0));

    /* memoryAddrWrite: sector 1, from its start, as long as a sector. Never
     * the directory, the ROM's copy, another profile, part of one, or more. */
    const guint8 begin[] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0xff };
    const guint8 directory[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xff };
    const guint8 rom[] = { 0x01, 0x01, 0x00, 0x00, 0x00, 0xff };
    const guint8 second[] = { 0x00, 0x02, 0x00, 0x00, 0x00, 0xff };
    const guint8 middle[] = { 0x00, 0x01, 0x00, 0xd0, 0x00, 0x10 };
    const guint8 longer[] = { 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c };
    const guint8 none[] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
    g_assert_true(hidpp_allowed(HIDPP_PROFILES, 6, begin, sizeof begin));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 6, directory, sizeof directory));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 6, rom, sizeof rom));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 6, second, sizeof second));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 6, middle, sizeof middle));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 6, longer, sizeof longer));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 6, none, sizeof none));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 6, begin, 4));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 6, NULL, 0));
    /* memoryWrite takes 16 bytes, memoryWriteEnd nothing. */
    const guint8 chunk[16] = { 0 };
    g_assert_true(hidpp_allowed(HIDPP_PROFILES, 7, chunk, sizeof chunk));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 7, chunk, 8));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 7, NULL, 0));
    g_assert_true(hidpp_allowed(HIDPP_PROFILES, 8, NULL, 0));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 8, chunk, 1));
}

static void test_hidpp_profile(void)
{
    g_assert_cmphex(hidpp_crc((const guint8 *)"123456789", 9), ==, 0x29b1);

    /* The G715's factory profile: its CRC holds, and the keys run its own
     * effect at the brightness key's 12 % step. */
    HidppKeyboard k;
    hidpp_sample(&k);
    g_assert_true(k.has_profile);
    g_assert_cmpuint(k.profile_size, ==, 255);
    g_assert_true(hidpp_sector_valid(k.profile, k.profile_size));
    g_assert_cmphex(hidpp_crc(k.profile, 253), ==, 0xa9f7);
    g_assert_cmpint(k.light.effect, ==, KEY_LIGHT_OWN);
    g_assert_true(k.has_brightness);
    g_assert_cmpint(k.brightness, ==, 12);
    g_assert_cmpint(k.brightness_max, ==, 100);
    g_assert_cmpint(k.brightness_steps, ==, 5);
    g_assert_false(k.app_controls);

    /* One changed byte, or a sector too short for the lighting, is refused. */
    guint8 bad[255];
    memcpy(bad, k.profile, sizeof bad);
    bad[0x10] ^= 1;
    HidppKeyboard b = { 0 };
    hidpp_decode_profile(bad, sizeof bad, &b);
    g_assert_false(b.has_profile);
    hidpp_decode_profile(k.profile, 128, &b);
    g_assert_false(b.has_profile);
    g_assert_cmpint(hidpp_keys_slot(255), ==, 0xd0);
    g_assert_cmpint(hidpp_keys_slot(200), ==, -1);

    /* Slots: the effect, then its colour. */
    HidppLighting l;
    const guint8 solid[HIDPP_SLOT] = { 0x01, 0xff, 0x00, 0x00 };
    hidpp_slot_decode(solid, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_SOLID);
    g_assert_cmpmem(l.rgb, 3, solid + 1, 3);
    const guint8 breathing[HIDPP_SLOT] = { 0x0a, 0x00, 0xff, 0x00, 0x11 };
    hidpp_slot_decode(breathing, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_BREATHING);
    g_assert_cmpint(l.rgb[1], ==, 0xff);
    const guint8 ripple[HIDPP_SLOT] = { 0x17, 0x80, 0x00, 0xff };
    hidpp_slot_decode(ripple, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_RIPPLE);
    g_assert_cmpint(l.rgb[2], ==, 0xff);
    const guint8 cycle[HIDPP_SLOT] = { 0x15, 0xff, 0x13, 0x88 }, wave[HIDPP_SLOT] = { 0x16 }, off[HIDPP_SLOT] = { 0 };
    hidpp_slot_decode(cycle, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_CYCLE);
    g_assert_cmpint(l.rgb[0], ==, 0);
    hidpp_slot_decode(wave, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_WAVE);
    hidpp_slot_decode(off, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_OFF);
    /* The factory effect, in use and while saving power, and one nobody named. */
    const guint8 own[HIDPP_SLOT] = { 0x18 }, saving[HIDPP_SLOT] = { 0x19 }, other[HIDPP_SLOT] = { 0x42, 0xff };
    hidpp_slot_decode(own, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_OWN);
    hidpp_slot_decode(saving, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_OWN);
    hidpp_slot_decode(other, &l);
    g_assert_cmpint(l.effect, ==, KEY_LIGHT_OTHER);
    g_assert_cmpint(l.rgb[0], ==, 0);

    /* Brightness: a device that says no range has none. */
    const guint8 none[] = { 0, 0, 0, 0, 0, 0 }, now[] = { 0x00, 0x32 };
    HidppKeyboard n = { 0 };
    hidpp_decode_brightness(none, now, &n);
    g_assert_false(n.has_brightness);
}

/* Lighting written into a profile slot: the effect, its colour, then its
 * speed, saturation and intensity where it has them. */
static void test_hidpp_slots(void)
{
    guint8 slot[HIDPP_SLOT];
    HidppLighting l = { KEY_LIGHT_SOLID, { 0xff, 0x50, 0x00 } }, back;
    const guint8 solid[HIDPP_SLOT] = { 0x01, 0xff, 0x50, 0x00 };
    g_assert_true(hidpp_slot_encode(&l, FALSE, slot));
    g_assert_cmpmem(slot, HIDPP_SLOT, solid, sizeof solid);
    l.effect = KEY_LIGHT_BREATHING;
    const guint8 breathing[HIDPP_SLOT] = { 0x0a, 0xff, 0x50, 0x00, 0x13, 0x88, 0x00, 0x64 };
    g_assert_true(hidpp_slot_encode(&l, FALSE, slot));
    g_assert_cmpmem(slot, HIDPP_SLOT, breathing, sizeof breathing);
    l.effect = KEY_LIGHT_CYCLE;
    const guint8 cycle[HIDPP_SLOT] = { 0x15, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x13, 0x88, 0x64 };
    g_assert_true(hidpp_slot_encode(&l, FALSE, slot));
    g_assert_cmpmem(slot, HIDPP_SLOT, cycle, sizeof cycle);
    l.effect = KEY_LIGHT_WAVE;
    const guint8 wave[HIDPP_SLOT] = { 0x16, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x13, 0x88, 0x64, 0x01 };
    g_assert_true(hidpp_slot_encode(&l, FALSE, slot));
    g_assert_cmpmem(slot, HIDPP_SLOT, wave, sizeof wave);
    l.effect = KEY_LIGHT_RIPPLE;
    const guint8 ripple[HIDPP_SLOT] = { 0x17, 0xff, 0x50, 0x00, 0xff, 0x00, 0x00, 0x00, 0x14 };
    g_assert_true(hidpp_slot_encode(&l, FALSE, slot));
    g_assert_cmpmem(slot, HIDPP_SLOT, ripple, sizeof ripple);
    /* Built-in is the factory effect, and its own while saving power. */
    l.effect = KEY_LIGHT_OWN;
    g_assert_true(hidpp_slot_encode(&l, FALSE, slot));
    g_assert_cmphex(slot[0], ==, 0x18);
    g_assert_true(hidpp_slot_encode(&l, TRUE, slot));
    g_assert_cmphex(slot[0], ==, 0x19);
    l.effect = KEY_LIGHT_OTHER;
    g_assert_false(hidpp_slot_encode(&l, FALSE, slot));

    /* Every effect Lares writes reads back as itself, with its colour. */
    for (KeyLight e = KEY_LIGHT_OFF; e < KEY_LIGHT_OTHER; e++) {
        for (int saving = 0; saving < 2; saving++) {
            l.effect = e;
            g_assert_true(hidpp_slot_encode(&l, saving, slot));
            hidpp_slot_decode(slot, &back);
            g_assert_cmpint(back.effect, ==, e);
            if (key_light_coloured(e)) g_assert_cmpmem(back.rgb, 3, l.rgb, 3);
            else g_assert_cmpint(back.rgb[0] | back.rgb[1] | back.rgb[2], ==, 0);
        }
    }
}

/* The G715's factory profile, its keys' lighting changed: only the two
 * slots of the keys and the CRC change, and Built-in brings back the
 * factory sector byte for byte. */
static void test_hidpp_profile_write(void)
{
    HidppKeyboard k;
    hidpp_sample(&k);
    const gsize size = k.profile_size;
    guint8 lit[HIDPP_SECTOR_MAX], again[HIDPP_SECTOR_MAX];
    HidppLighting blue = { KEY_LIGHT_SOLID, { 0x00, 0x00, 0xff } };
    g_assert_true(hidpp_profile_lit(k.profile, size, &blue, lit));
    g_assert_true(hidpp_sector_valid(lit, size));
    for (gsize i = 0; i < size - 2; i++) {
        gboolean slot = (i >= 0xd0 && i < 0xd0 + HIDPP_SLOT) || (i >= 0xe6 && i < 0xe6 + HIDPP_SLOT);
        if (!slot) g_assert_cmphex(lit[i], ==, k.profile[i]);
    }
    const guint8 solid[HIDPP_SLOT] = { 0x01, 0x00, 0x00, 0xff };
    g_assert_cmpmem(lit + 0xd0, HIDPP_SLOT, solid, sizeof solid);
    g_assert_cmpmem(lit + 0xe6, HIDPP_SLOT, solid, sizeof solid);
    g_assert_cmpmem(lit + 0xdb, HIDPP_SLOT, k.profile + 0xdb, HIDPP_SLOT);   /* the other cluster stays */
    HidppKeyboard r = { 0 };
    hidpp_decode_profile(lit, size, &r);
    g_assert_true(r.has_profile);
    g_assert_cmpint(r.light.effect, ==, KEY_LIGHT_SOLID);
    g_assert_cmpmem(r.light.rgb, 3, blue.rgb, 3);

    HidppLighting own = { KEY_LIGHT_OWN, { 0 } };
    g_assert_true(hidpp_profile_lit(lit, size, &own, again));
    g_assert_cmpmem(again, size, k.profile, size);
    g_assert_cmphex(again[size - 2] << 8 | again[size - 1], ==, 0xa9f7);

    /* The same effect in a new colour keeps the speed it was saved with. */
    guint8 slow[HIDPP_SECTOR_MAX];
    memcpy(slow, k.profile, size);
    const guint8 breathing[HIDPP_SLOT] = { 0x0a, 0x00, 0xff, 0x00, 0x11, 0x00 };
    memcpy(slow + 0xd0, breathing, HIDPP_SLOT);
    guint16 crc = hidpp_crc(slow, size - 2);
    slow[size - 2] = crc >> 8;
    slow[size - 1] = crc & 0xff;
    HidppLighting red = { KEY_LIGHT_BREATHING, { 0xff, 0x00, 0x00 } };
    g_assert_true(hidpp_profile_lit(slow, size, &red, lit));
    const guint8 kept[HIDPP_SLOT] = { 0x0a, 0xff, 0x00, 0x00, 0x11, 0x00 };
    g_assert_cmpmem(lit + 0xd0, HIDPP_SLOT, kept, sizeof kept);
    g_assert_cmphex(lit[0xe6 + 4], ==, 0x13);   /* the saving slot played another, so it starts fresh */

    /* A sector whose CRC fails, one too short for the slots, and an effect
     * Lares doesn't know are refused. */
    memcpy(slow, k.profile, size);
    slow[3] ^= 1;
    g_assert_false(hidpp_profile_lit(slow, size, &blue, lit));
    g_assert_false(hidpp_profile_lit(k.profile, 200, &blue, lit));
    HidppLighting other = { KEY_LIGHT_OTHER, { 0 } };
    g_assert_false(hidpp_profile_lit(k.profile, size, &other, lit));
}

/* A G715 as the Keyboard section sees it: found behind its receiver,
 * decoded to plain units, and listened to. */
static void test_hidpp_keyboard(void)
{
    g_assert_nonnull(hidpp_model(0xc547, 0x40a2));
    g_assert_null(hidpp_model(0xc547, 0x4099));   /* untried on this receiver */
    g_assert_null(hidpp_model(0xc52b, 0x40a2));   /* or on another */
    g_assert_cmpint(hidpp_phys_index("usb-0000:00:14.0-8.4/input2:1"), ==, 1);
    g_assert_cmpint(hidpp_phys_index("usb-0000:00:14.0-8.4/input2:6"), ==, 6);
    g_assert_cmpint(hidpp_phys_index("usb-0000:00:14.0-8.4/input2"), ==, -1);
    g_assert_cmpint(hidpp_phys_index("usb-0000:00:14.0-8.4/input2:0"), ==, -1);
    g_assert_cmpint(hidpp_phys_index("usb-0000:00:14.0-8.4/input2:1x"), ==, -1);
    g_assert_cmpint(hidpp_phys_index(NULL), ==, -1);

    HidppKeyboard k;
    hidpp_sample(&k);
    KeyboardSettings s;
    hidpp_decode(&k, hidpp_model(0xc547, 0x40a2), &s);
    g_assert_true(s.has_brightness);
    g_assert_cmpint(s.brightness, ==, 12);
    /* The levels its key steps through, as the real one showed them. */
    const int levels[] = { 0, 12, 25, 50, 100 };
    g_assert_cmpint(s.levels, ==, 5);
    g_assert_cmpmem(s.level, sizeof levels, levels, sizeof levels);
    g_assert_true(s.has_lighting);
    g_assert_cmpint(s.light, ==, KEY_LIGHT_OWN);
    g_assert_false(s.app_controls);
    g_assert_cmpint(hidpp_brightness_value(&k, 50), ==, 50);
    g_assert_cmpint(hidpp_brightness_value(&k, 140), ==, 100);
    /* Another range scales both ways. */
    HidppKeyboard r = k;
    r.brightness_min = 10;
    r.brightness_max = 210;
    r.brightness = 110;
    hidpp_decode(&r, NULL, &s);
    g_assert_cmpint(s.brightness, ==, 50);
    g_assert_cmpint(s.levels, ==, 0);
    g_assert_cmpint(hidpp_brightness_value(&r, 25), ==, 60);
    g_assert_cmpint(hidpp_brightness_value(&r, 0), ==, 10);

    /* Events, from device 1 with its features where the G715 has them. */
    HidppEvent ev;
    const guint8 down[] = { 0x10, 0x01, 0x41, 0x0c, 0x42, 0xa2, 0x40 };
    g_assert_true(hidpp_event(down, sizeof down, 1, &k, &ev));
    g_assert_cmpint(ev.kind, ==, HIDPP_EVENT_LINK);
    g_assert_false(ev.linked);
    const guint8 up[] = { 0x10, 0x01, 0x41, 0x0c, 0x02, 0xa2, 0x40 };
    g_assert_true(hidpp_event(up, sizeof up, 1, &k, &ev));
    g_assert_true(ev.linked);
    g_assert_false(hidpp_event(up, sizeof up, 2, &k, &ev));   /* another device's */
    guint8 status[HIDPP_LONG_LEN] = { 0x11, 0x01, 0x04, 0x00, 0x01, 0x01, 0x01 };
    g_assert_true(hidpp_event(status, sizeof status, 1, &k, &ev));
    g_assert_cmpint(ev.kind, ==, HIDPP_EVENT_LINK);
    g_assert_true(ev.linked);
    status[4] = 0x00;
    g_assert_false(hidpp_event(status, sizeof status, 1, &k, &ev));
    /* Its brightness key stepping from 50 to 25, as the G715 said it. */
    const guint8 bright[HIDPP_LONG_LEN] = { 0x11, 0x01, 0x0f, 0x00, 0x00, 0x19 };
    g_assert_true(hidpp_event(bright, sizeof bright, 1, &k, &ev));
    g_assert_cmpint(ev.kind, ==, HIDPP_EVENT_BRIGHTNESS);
    g_assert_cmpint(ev.brightness, ==, 25);
    const guint8 profile[HIDPP_LONG_LEN] = { 0x11, 0x01, 0x10, 0x00, 0x00, 0x01 };
    g_assert_true(hidpp_event(profile, sizeof profile, 1, &k, &ev));
    g_assert_cmpint(ev.kind, ==, HIDPP_EVENT_CHANGED);
    /* Answers to Lares' requests or the kernel's, the battery's news, and
     * errors aren't. */
    const guint8 mine[HIDPP_LONG_LEN] = { 0x11, 0x01, 0x0f, 0x1c, 0x00, 0x32 };
    const guint8 kernel[HIDPP_LONG_LEN] = { 0x11, 0x01, 0x0f, 0x11, 0x00, 0x32 };
    const guint8 battery[HIDPP_LONG_LEN] = { 0x11, 0x01, 0x07, 0x00, 0x32, 0x00 };
    const guint8 error[HIDPP_LONG_LEN] = { 0x11, 0x01, 0xff, 0x0f, 0x1c, 0x05 };
    g_assert_false(hidpp_event(mine, sizeof mine, 1, &k, &ev));
    g_assert_false(hidpp_event(kernel, sizeof kernel, 1, &k, &ev));
    g_assert_false(hidpp_event(battery, sizeof battery, 1, &k, &ev));
    g_assert_false(hidpp_event(error, sizeof error, 1, &k, &ev));
    g_assert_false(hidpp_event(bright, 4, 1, &k, &ev));

    /* The rule opens the receiver's HID++ node to the person at the screen,
     * and not the keyboard's own node under it, which carries keystrokes. */
    char *rules = devices_udev_rules();
    g_assert_nonnull(strstr(rules, "ENV{ID_USB_MODEL_ID}==\"0f98\""));   /* the mouse's stays */
    const char *line = strstr(rules, "ENV{ID_USB_MODEL_ID}==\"c547\"");
    g_assert_nonnull(line);
    g_assert_nonnull(strstr(rules, "ENV{ID_USB_VENDOR_ID}==\"046d\", ENV{ID_USB_MODEL_ID}==\"c547\", "
                                   "ENV{ID_USB_INTERFACE_NUM}==\"02\", "
                                   "DEVPATH==\"*/0003:046D:C547.\?\?\?\?/hidraw/hidraw*\", TAG+=\"uaccess\"\n"));
    g_assert_null(strstr(rules, "MODE="));
    const char *pattern = "*/0003:046D:C547.\?\?\?\?/hidraw/hidraw*";
    const char *receiver = "/devices/pci0000:00/0000:00:14.0/usb1/1-8/1-8.4/1-8.4:1.2/0003:046D:C547.0007/hidraw/hidraw6";
    const char *keyboard = "/devices/pci0000:00/0000:00:14.0/usb1/1-8/1-8.4/1-8.4:1.2/0003:046D:C547.0007/"
                           "0003:046D:40A2.0009/hidraw/hidraw8";
    g_assert_true(g_pattern_match_simple(pattern, receiver));
    g_assert_false(g_pattern_match_simple(pattern, keyboard));
    g_free(rules);
}

static void test_map_parse(void)
{
    const char *good = "; a comment\r\n.#+\r\naH<\n";
    GError *error = NULL;
    PadMap *map = pad_map_parse(good, strlen(good), &error);
    g_assert_no_error(error);
    g_assert_cmpint(map->width, ==, 3);
    g_assert_cmpint(map->height, ==, 2);
    g_assert_cmpint(pad_cell_kind(pad_map_at(map, 1, 0)), ==, PAD_CELL_LINE);
    g_assert_cmpint(pad_cell_kind(pad_map_at(map, 0, 1)), ==, PAD_CELL_AREA);
    g_assert_cmpint(pad_cell_button(pad_map_at(map, 0, 1)), ==, PB_A);
    g_assert_cmpint(pad_cell_button(pad_map_at(map, 1, 1)), ==, PB_XBOX);
    g_assert_cmpint(pad_cell_kind(pad_map_at(map, 2, 1)), ==, PAD_CELL_CAP_LINE);
    g_assert_cmpint(pad_cell_kind(pad_map_at(map, 9, 9)), ==, PAD_CELL_EMPTY);
    pad_map_free(map);

    const char *ragged = "...\n..\n", *unknown = "..?\n", *empty = "; nothing\n";
    g_assert_null(pad_map_parse(ragged, strlen(ragged), &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_null(pad_map_parse(unknown, strlen(unknown), &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_null(pad_map_parse(empty, strlen(empty), &error));
    g_clear_error(&error);
}

static void test_map_paint(void)
{
    guint8 area = PAD_CELL(PAD_CELL_AREA, PB_A), letter = PAD_CELL(PAD_CELL_GLYPH, PB_A);
    guint8 logo = PAD_CELL(PAD_CELL_GLYPH, PB_XBOX);
    g_assert_cmpint(pad_cell_paint(area, 0, 0), ==, PAD_PAINT_NONE);
    g_assert_cmpint(pad_cell_paint(area, PB_BIT(PB_A), 0), ==, PAD_PAINT_LIT);
    g_assert_cmpint(pad_cell_paint(area, PB_BIT(PB_B), PB_BIT(PB_A)), ==, PAD_PAINT_SEEN);
    /* A held button's letter is cut out of it; a seen one keeps its letter. */
    g_assert_cmpint(pad_cell_paint(letter, 0, 0), ==, PAD_PAINT_LINE);
    g_assert_cmpint(pad_cell_paint(letter, PB_BIT(PB_A), 0), ==, PAD_PAINT_NONE);
    g_assert_cmpint(pad_cell_paint(letter, 0, PB_BIT(PB_A)), ==, PAD_PAINT_LINE);
    /* The Xbox logo lights itself. */
    g_assert_cmpint(pad_cell_paint(logo, 0, 0), ==, PAD_PAINT_LINE);
    g_assert_cmpint(pad_cell_paint(logo, PB_BIT(PB_XBOX), 0), ==, PAD_PAINT_LIT);
    g_assert_cmpint(pad_cell_paint(logo, 0, PB_BIT(PB_XBOX)), ==, PAD_PAINT_SEEN);
    g_assert_cmpint(pad_cell_paint(PAD_CELL(PAD_CELL_FAINT, 0), ~0u, ~0u), ==, PAD_PAINT_FAINT);
    g_assert_cmpint(pad_cell_paint(PAD_CELL(PAD_CELL_CAP_AREA, PB_LS), PB_BIT(PB_LS), 0), ==, PAD_PAINT_LIT);
}

/* The map Lares ships: every button is on it, and each stick's cap can move
 * its full travel without running into anything that stays put. */
static void test_map_shipped(void)
{
    char *text;
    gsize length;
    GError *error = NULL;
    g_assert_true(g_file_get_contents("data/pad/controller.txt", &text, &length, &error));
    PadMap *map = pad_map_parse(text, length, &error);
    g_assert_no_error(error);
    g_free(text);
    g_assert_cmpint(map->width, >, map->height);

    guint32 present = 0;
    for (int i = 0; i < map->width * map->height; i++) {
        PadCellKind kind = pad_cell_kind(map->cells[i]);
        if (kind == PAD_CELL_AREA || kind == PAD_CELL_GLYPH || kind == PAD_CELL_CAP_AREA)
            present |= PB_BIT(pad_cell_button(map->cells[i]));
    }
    g_assert_cmphex(present, ==, PB_BIT(PB_COUNT) - 1);

    const int t = PAD_MAP_STICK_TRAVEL;
    for (int y = 0; y < map->height; y++)
        for (int x = 0; x < map->width; x++) {
            guint8 cell = pad_map_at(map, x, y);
            PadCellKind kind = pad_cell_kind(cell);
            if (kind != PAD_CELL_CAP_LINE && kind != PAD_CELL_CAP_AREA) continue;
            for (int dy = -t; dy <= t; dy++)
                for (int dx = -t; dx <= t; dx++) {
                    if (dx * dx + dy * dy > t * t) continue;
                    guint8 under = pad_map_at(map, x + dx, y + dy);
                    PadCellKind k = pad_cell_kind(under);
                    gboolean same_cap = (k == PAD_CELL_CAP_LINE || k == PAD_CELL_CAP_AREA)
                                        && pad_cell_button(under) == pad_cell_button(cell);
                    g_assert_true(k == PAD_CELL_EMPTY || same_cap);
                }
        }
    pad_map_free(map);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nicknames/key", test_nickname_key);
    g_test_add_func("/nicknames/clean", test_clean_nickname);
    g_test_add_func("/battery/stages-and-words", test_battery);
    g_test_add_func("/remote/screen-sizes", test_screen);
    g_test_add_func("/remote/saved-values", test_saved_values);
    g_test_add_func("/remote/own-size", test_own_size);
    g_test_add_func("/devices/input-kinds", test_input_kinds);
    g_test_add_func("/devices/input-owner", test_input_owner);
    g_test_add_func("/devices/hid-battery", test_hid_battery);
    g_test_add_func("/devices/hyperx-cable", test_hyperx_cable);
    g_test_add_func("/devices/hyperx-settings", test_hyperx_settings);
    g_test_add_func("/devices/hyperx-real", test_hyperx_real);
    g_test_add_func("/devices/hyperx-events", test_hyperx_events);
    g_test_add_func("/devices/hyperx-merge", test_hyperx_merge);
    g_test_add_func("/devices/hyperx-shows", test_hyperx_shows);
    g_test_add_func("/devices/usb-parent", test_usb_parent);
    g_test_add_func("/devices/hidpp-packets", test_hidpp_packets);
    g_test_add_func("/devices/hidpp-allowed", test_hidpp_allowed);
    g_test_add_func("/devices/hidpp-profile", test_hidpp_profile);
    g_test_add_func("/devices/hidpp-slots", test_hidpp_slots);
    g_test_add_func("/devices/hidpp-profile-write", test_hidpp_profile_write);
    g_test_add_func("/devices/hidpp-keyboard", test_hidpp_keyboard);
    g_test_add_func("/picture/map-parse", test_map_parse);
    g_test_add_func("/picture/map-paint", test_map_paint);
    g_test_add_func("/picture/map-shipped", test_map_shipped);
    return g_test_run();
}
