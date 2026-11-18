// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the parts of Lares that work on plain data: nickname keys,
 * battery stages and wording, screen sizes, the controller picture's map,
 * how keyboards and mice are told apart in sysfs, HyperX battery answers,
 * and Logitech HID++ requests and profiles. `make check` runs them from
 * the repository's root. */
#include <glib.h>
#include <string.h>
#include "../src/devices.h"
#include "../src/hidbattery.h"
#include "../src/hidpp.h"
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
    g_assert_true(hidbattery_parse(answer, sizeof answer, &level, &charging));
    g_assert_cmpint(level, ==, 54);
    g_assert_false(charging);
    const guint8 on_cable[64] = { 0x51, 0x02, 0x64, 0x01 };
    g_assert_true(hidbattery_parse(on_cable, sizeof on_cable, &level, &charging));
    g_assert_cmpint(level, ==, 100);
    g_assert_true(charging);
    const guint8 full[64] = { 0x51, 0x02, 0x64, 0x02 };
    g_assert_true(hidbattery_parse(full, sizeof full, &level, &charging));
    g_assert_false(charging);

    /* The receiver's acknowledgement, a link event, a wrong level and a
     * short read are not answers. */
    const guint8 ack[64] = { 0xff, 0x01, 0xbf, 0xb4 };
    const guint8 link[64] = { 0xfb, 0x0a, 0x01 };
    const guint8 wrong[64] = { 0x51, 0x02, 0x65, 0x00 };
    g_assert_false(hidbattery_parse(ack, sizeof ack, &level, &charging));
    g_assert_false(hidbattery_parse(link, sizeof link, &level, &charging));
    g_assert_false(hidbattery_parse(wrong, sizeof wrong, &level, &charging));
    g_assert_false(hidbattery_parse(answer, 3, &level, &charging));

    const HidBatteryModel *m = hidbattery_model(0x03f0, 0x0f98);
    g_assert_nonnull(m);
    g_assert_cmpint(m->interface, ==, 2);
    g_assert_null(hidbattery_model(0x03f0, 0x0d97));

    /* The rule opens the battery interface only, to the person at the screen. */
    char *rules = hidbattery_udev_rules();
    g_assert_nonnull(strstr(rules, "SUBSYSTEM==\"hidraw\", ENV{ID_USB_VENDOR_ID}==\"03f0\", "
                                   "ENV{ID_USB_MODEL_ID}==\"0f98\", ENV{ID_USB_INTERFACE_NUM}==\"02\", "
                                   "TAG+=\"uaccess\"\n"));
    g_assert_null(strstr(rules, "MODE="));  /* no world-readable node */
    g_free(rules);
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

/* Only the reads Lares needs, and setBrightness, ever leave. */
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
    for (guint8 f = 6; f <= 8; f++) g_assert_false(hidpp_allowed(HIDPP_PROFILES, f, NULL, 0));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 1, NULL, 0));
    g_assert_false(hidpp_allowed(HIDPP_PROFILES, 3, NULL, 0));
    g_assert_false(hidpp_allowed(0x00d0, 0, NULL, 0));
    g_assert_false(hidpp_allowed(0x1802, 0, NULL, 0));
    g_assert_false(hidpp_allowed(0x1e00, 0, NULL, 0));
    g_assert_false(hidpp_allowed(0x8081, 1, NULL, 0));
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
    g_assert_cmpint(k.light.effect, ==, HIDPP_LIGHT_OWN);
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
    g_assert_cmpint(l.effect, ==, HIDPP_LIGHT_SOLID);
    g_assert_cmpmem(l.rgb, 3, solid + 1, 3);
    const guint8 breathing[HIDPP_SLOT] = { 0x0a, 0x00, 0xff, 0x00, 0x11 };
    hidpp_slot_decode(breathing, &l);
    g_assert_cmpint(l.effect, ==, HIDPP_LIGHT_BREATHING);
    g_assert_cmpint(l.rgb[1], ==, 0xff);
    const guint8 ripple[HIDPP_SLOT] = { 0x17, 0x80, 0x00, 0xff };
    hidpp_slot_decode(ripple, &l);
    g_assert_cmpint(l.effect, ==, HIDPP_LIGHT_RIPPLE);
    g_assert_cmpint(l.rgb[2], ==, 0xff);
    const guint8 cycle[HIDPP_SLOT] = { 0x15, 0xff, 0x13, 0x88 }, wave[HIDPP_SLOT] = { 0x16 }, off[HIDPP_SLOT] = { 0 };
    hidpp_slot_decode(cycle, &l);
    g_assert_cmpint(l.effect, ==, HIDPP_LIGHT_CYCLE);
    g_assert_cmpint(l.rgb[0], ==, 0);
    hidpp_slot_decode(wave, &l);
    g_assert_cmpint(l.effect, ==, HIDPP_LIGHT_WAVE);
    hidpp_slot_decode(off, &l);
    g_assert_cmpint(l.effect, ==, HIDPP_LIGHT_OFF);

    /* Brightness: a device that says no range has none. */
    const guint8 none[] = { 0, 0, 0, 0, 0, 0 }, now[] = { 0x00, 0x32 };
    HidppKeyboard n = { 0 };
    hidpp_decode_brightness(none, now, &n);
    g_assert_false(n.has_brightness);
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
    g_test_add_func("/devices/input-kinds", test_input_kinds);
    g_test_add_func("/devices/input-owner", test_input_owner);
    g_test_add_func("/devices/hid-battery", test_hid_battery);
    g_test_add_func("/devices/hidpp-packets", test_hidpp_packets);
    g_test_add_func("/devices/hidpp-allowed", test_hidpp_allowed);
    g_test_add_func("/devices/hidpp-profile", test_hidpp_profile);
    g_test_add_func("/picture/map-parse", test_map_parse);
    g_test_add_func("/picture/map-paint", test_map_paint);
    g_test_add_func("/picture/map-shipped", test_map_shipped);
    return g_test_run();
}
