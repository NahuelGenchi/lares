// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the parts of Lares that work on plain data: nickname keys,
 * battery stages and wording, screen sizes and the controller picture's map.
 * `make check` runs them from the repository's root. */
#include <glib.h>
#include <string.h>
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
    g_test_add_func("/picture/map-parse", test_map_parse);
    g_test_add_func("/picture/map-paint", test_map_paint);
    g_test_add_func("/picture/map-shipped", test_map_shipped);
    return g_test_run();
}
