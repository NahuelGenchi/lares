// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Logitech's HID++ 2.0 through a receiver: requests of 7 (short) or 20
 * (long) bytes, the report ID, the device's index on the receiver, a
 * feature's index on the device, then the function in the high nibble and
 * the asker's software ID in the low one, then the parameters. The answer
 * comes back the same way. A device that refuses says ff, the feature
 * index, the function byte and an error code; a receiver that can't reach
 * its device says 8f the same way. A feature's index comes from asking the
 * root feature (index 0) for its ID.
 *
 * Written from the behaviour Logitech documents for HID++ 2.0 and what
 * issue #23 found on a real G715; libratbag (MIT) and Solaar (GPL-2.0 or
 * later) were read for the onboard profiles' layout and the effects'
 * parameters, and no code is taken from either. Lares never takes the
 * lighting into software control: the keyboard would then stop dimming
 * itself, and Lares would have to keep running to do it. It changes the
 * lighting the onboard profile holds instead, which the keyboard keeps and
 * plays by itself (issue #57). */
#include "hidpp.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ANSWER_MS  1000
#define SAVED_MS   3000  /* the end of a profile write, which the keyboard stores before it answers */
#define ROOT_GET_FEATURE 0
#define ROOT_PING        1
#define BRIGHTNESS_INFO  0
#define BRIGHTNESS_GET   1
#define BRIGHTNESS_SET   2
#define BRIGHTNESS_LIGHT 3   /* getIllumination */
#define RGB_INFO         0   /* <group or ff> <effect or ff> 00 -> what it has */
#define RGB_SET_EFFECT   1   /* SetEffectByIndex: <group> <index> <10 parameters> <01> */
#define RGB_SW_CONTROL   5   /* 00 gets <sub-op> <mode> <flags>; 01 sets mode 3 (software) or 0 (the firmware's own) */
#define PROFILES_INFO    0   /* [7..8] the sector size */
#define PROFILES_MODE    2   /* 1 onboard, 2 host: an app sends the settings */
#define PROFILES_CURRENT 4   /* the sector in use */
#define PROFILES_READ    5   /* <sector:2> <offset:2> -> 16 bytes */
#define PROFILES_BEGIN   6   /* memoryAddrWrite <sector:2> <offset:2> <length:2>: a write begins */
#define PROFILES_WRITE   7   /* memoryWrite: the next 16 bytes of it */
#define PROFILES_END     8   /* memoryWriteEnd: the keyboard checks the CRC and stores it */
#define KEYS_SLOT        0xd0 /* the keys' lighting while in use */
#define SAVING_SLOT      0xe6 /* and once they dim after a minute alone */

/* Effects by their ID. Colour cycle, wave and ripple each have an older ID
 * and the one the G715 lists, which adds a saturation; Lares writes the
 * listed ones. Its factory effect is 0x18, and 0x19 while saving power,
 * which only the list of effects every light group shares names (issue #57). */
enum {
    FX_OFF = 0x00, FX_SOLID = 0x01, FX_BREATHING = 0x0a,
    FX_CYCLE_OLD = 0x03, FX_WAVE_OLD = 0x04, FX_RIPPLE_OLD = 0x0b,
    FX_CYCLE = 0x15, FX_WAVE = 0x16, FX_RIPPLE = 0x17,
    FX_OWN = 0x18, FX_OWN_SAVING = 0x19,
};

/* What Lares sends with an effect, after its colour, as Logitech G HUB
 * does: periods in ms, big-endian, full intensity and saturation, a solid
 * colour without its ramp, and the wave moving right. Changing only the
 * colour of a slot keeps what the slot already says. */
#define BREATH_MS     5000
#define CYCLE_MS      5000
#define WAVE_MS       5000
#define RIPPLE_PERIOD 20
#define FULL          100   /* intensity, in % */
#define SATURATED     0xff
#define NO_RAMP       2
#define WAVE_RIGHT    1
#define VOLATILE      1     /* the last byte of a live effect, as G HUB sends it */

/* Taking the lights, and answering the keyboard's idle and active events,
 * as G HUB does: its flags in the second byte. A take is two steps, the
 * firmware's mode with the take's flags first: taken in one, while the
 * keyboard's own lights-off is on, the keyboard says software has them and
 * goes on playing its own effect (issue #68). */
#define SW_FIRMWARE   0
#define SW_SOFTWARE   3
static const guint8 SW_FLAGS[] = { [HIDPP_LIVE_TAKE] = 6, [HIDPP_LIVE_IDLE] = 3, [HIDPP_LIVE_ACTIVE] = 5 };

#define CONNECTION       0x41 /* a receiver's notice that a device's link came up or went down */
#define LINK_DOWN        0x40 /* in its flags */
#define STATUS_RECONNECT 1    /* wireless device status: it reconnected, and may have lost its settings */

static GMutex turns;   /* one exchange with a receiver at a time */

static const HidppModel MODELS[] = {
    /* G715, on its Lightspeed receiver, whose HID++ interface is the third.
     * Its brightness key steps 100, 50, 25, 12, off, and 60 reads back as 50
     * (issue #25). */
    { 0xc547, 0x40a2, 2, 5, { 0, 12, 25, 50, 100 } },
};

const HidppModel *hidpp_model(guint16 receiver, guint16 product)
{
    for (guint i = 0; i < G_N_ELEMENTS(MODELS); i++)
        if (MODELS[i].receiver == receiver && MODELS[i].product == product) return &MODELS[i];
    return NULL;
}

int hidpp_phys_index(const char *phys)
{
    const char *colon = phys ? strrchr(phys, ':') : NULL;
    if (!colon || !g_ascii_isdigit(colon[1])) return -1;
    char *end;
    guint64 n = g_ascii_strtoull(colon + 1, &end, 10);
    return *end == '\0' && n >= 1 && n <= 6 ? (int)n : -1;   /* a receiver pairs up to six */
}

/* ------------------------------------------------------------- finding */

static gboolean is_hid_id(const char *s)      /* such as 0003:046D:C547.0007 */
{
    if (strlen(s) != 19 || s[4] != ':' || s[9] != ':' || s[14] != '.') return FALSE;
    for (int i = 0; i < 19; i++)
        if (i != 4 && i != 9 && i != 14 && !g_ascii_isxdigit(s[i])) return FALSE;
    return TRUE;
}

static char *sys_text(const char *dir, const char *file)
{
    char *path = g_build_filename(dir, file, NULL), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) g_strstrip(text);
    g_free(path);
    return text;
}

char *hidpp_node(const char *device, guint8 *index, const HidppModel **model)
{
    char *name = g_path_get_basename(device), *receiver = g_path_get_dirname(device);
    char *rname = g_path_get_basename(receiver), *node = NULL;
    const HidppModel *m = NULL;
    if (is_hid_id(name) && is_hid_id(rname) && g_str_has_prefix(name, "0003:046D:")
        && g_str_has_prefix(rname, "0003:046D:"))
        m = hidpp_model(g_ascii_strtoull(rname + 10, NULL, 16), g_ascii_strtoull(name + 10, NULL, 16));
    char *iface = m ? g_path_get_dirname(receiver) : NULL;
    char *number = iface ? sys_text(iface, "bInterfaceNumber") : NULL;
    char *uevent = number && g_ascii_strtoll(number, NULL, 16) == m->interface ? sys_text(device, "uevent") : NULL;
    char *phys = uevent ? strstr(uevent, "HID_PHYS=") : NULL;
    int at = -1;
    if (phys) {
        phys += strlen("HID_PHYS=");
        char *nl = strchr(phys, '\n');
        if (nl) *nl = '\0';
        at = hidpp_phys_index(phys);
    }
    char *dir = at > 0 ? g_build_filename(receiver, "hidraw", NULL) : NULL;
    GDir *raw = dir ? g_dir_open(dir, 0, NULL) : NULL;
    const char *entry = raw ? g_dir_read_name(raw) : NULL;
    if (entry && g_str_has_prefix(entry, "hidraw")) {
        node = g_build_filename("/dev", entry, NULL);
        *index = (guint8)at;
        *model = m;
    }
    if (raw) g_dir_close(raw);
    g_free(dir);
    g_free(uevent);
    g_free(number);
    g_free(iface);
    g_free(rname);
    g_free(receiver);
    g_free(name);
    return node;
}

char *hidpp_find(guint8 *index, const HidppModel **model)
{
    const char *bus = "/sys/bus/hid/devices";
    GDir *d = g_dir_open(bus, 0, NULL);
    char *node = NULL;
    const char *entry;
    while (d && !node && (entry = g_dir_read_name(d))) {
        if (!g_str_has_prefix(entry, "0003:046D:")) continue;
        char *link = g_build_filename(bus, entry, NULL), *device = realpath(link, NULL);
        if (device) node = hidpp_node(device, index, model);
        free(device);
        g_free(link);
    }
    if (d) g_dir_close(d);
    return node;
}

/* ------------------------------------------------------------ requests */

/* A write that starts at the beginning of HIDPP_SECTOR and covers it
 * whole: never the profile directory (sector 0), the ROM (0x01xx), or
 * another profile. */
static gboolean whole_sector(const guint8 *p, gsize n)
{
    if (n != 6) return FALSE;
    guint sector = p[0] << 8 | p[1], offset = p[2] << 8 | p[3], length = p[4] << 8 | p[5];
    return sector == HIDPP_SECTOR && offset == 0 && length >= 16 && length <= HIDPP_SECTOR_MAX;
}

static gboolean one_chunk(const guint8 *params, gsize n)
{
    return n == 16;
}

static gboolean nothing(const guint8 *params, gsize n)
{
    return n == 0;
}

/* An effect for one light group from its own list: never every group at
 * once (0xff), whose list holds the effects uploaded to the keyboard. */
static gboolean one_group(const guint8 *p, gsize n)
{
    return n == 13 && p[0] < HIDPP_CLUSTERS && p[1] < HIDPP_EFFECTS && p[12] == VOLATILE;
}

/* Gets; takes the lights into software control with G HUB's flags; or
 * gives them back to the firmware. */
static gboolean sw_control(const guint8 *p, gsize n)
{
    if (n > 0 && p[0] == 0x00) return TRUE;
    if (n < 3 || p[0] != 0x01) return FALSE;
    if (p[1] == SW_FIRMWARE) return p[2] == 0 || p[2] == SW_FLAGS[HIDPP_LIVE_TAKE];
    for (guint i = 0; i < G_N_ELEMENTS(SW_FLAGS); i++)
        if (p[1] == SW_SOFTWARE && p[2] == SW_FLAGS[i]) return TRUE;
    return FALSE;
}

static const struct {
    guint16 feature;
    guint8 function;
    gboolean (*check)(const guint8 *params, gsize n);   /* NULL: any parameters */
} ALLOWED[] = {
    { HIDPP_ROOT, ROOT_GET_FEATURE, NULL },
    { HIDPP_ROOT, ROOT_PING, NULL },
    { HIDPP_BRIGHTNESS, BRIGHTNESS_INFO, NULL },
    { HIDPP_BRIGHTNESS, BRIGHTNESS_GET, NULL },
    { HIDPP_BRIGHTNESS, BRIGHTNESS_SET, NULL },
    { HIDPP_BRIGHTNESS, BRIGHTNESS_LIGHT, NULL },
    { HIDPP_RGB_EFFECTS, RGB_INFO, NULL },
    { HIDPP_RGB_EFFECTS, RGB_SET_EFFECT, one_group },
    { HIDPP_RGB_EFFECTS, RGB_SW_CONTROL, sw_control },
    { HIDPP_PROFILES, PROFILES_INFO, NULL },
    { HIDPP_PROFILES, PROFILES_MODE, NULL },
    { HIDPP_PROFILES, PROFILES_CURRENT, NULL },
    { HIDPP_PROFILES, PROFILES_READ, NULL },
    { HIDPP_PROFILES, PROFILES_BEGIN, whole_sector },
    { HIDPP_PROFILES, PROFILES_WRITE, one_chunk },
    { HIDPP_PROFILES, PROFILES_END, nothing },
};

gboolean hidpp_allowed(guint16 feature, guint8 function, const guint8 *params, gsize n)
{
    for (guint i = 0; i < G_N_ELEMENTS(ALLOWED); i++)
        if (ALLOWED[i].feature == feature && ALLOWED[i].function == function)
            return !ALLOWED[i].check || ALLOWED[i].check(params, n);
    return FALSE;
}

gsize hidpp_packet(guint8 sw, guint8 index, guint8 feature_index, guint8 function, const guint8 *params, gsize n,
                   guint8 out[HIDPP_LONG_LEN])
{
    if (n > HIDPP_PARAMS || function > 0x0f) return 0;
    gsize len = n <= HIDPP_SHORT_LEN - 4 ? HIDPP_SHORT_LEN : HIDPP_LONG_LEN;
    memset(out, 0, HIDPP_LONG_LEN);
    out[0] = len == HIDPP_SHORT_LEN ? HIDPP_SHORT : HIDPP_LONG;
    out[1] = index;
    out[2] = feature_index;
    out[3] = (guint8)(function << 4 | (sw & 0x0f));
    if (n) memcpy(out + 4, params, n);
    return len;
}

HidppMatch hidpp_match(const guint8 *r, gsize len, const guint8 *packet)
{
    if (len < 5 || (r[0] != HIDPP_SHORT && r[0] != HIDPP_LONG) || r[1] != packet[1]) return HIDPP_OTHER;
    if (r[2] == packet[2] && r[3] == packet[3]) return HIDPP_ANSWER;
    if (len >= 6 && r[3] == packet[2] && r[4] == packet[3]) {
        if (r[2] == 0xff) return HIDPP_ERROR;
        if (r[2] == 0x8f) return HIDPP_RECEIVER_ERROR;
    }
    return HIDPP_OTHER;
}

/* --------------------------------------------------------- the profile */

guint16 hidpp_crc(const guint8 *data, gsize len)
{
    guint16 crc = 0xffff;
    for (gsize i = 0; i < len; i++) {
        crc ^= (guint16)(data[i] << 8);
        for (int bit = 0; bit < 8; bit++)
            crc = crc & 0x8000 ? (guint16)(crc << 1 ^ 0x1021) : (guint16)(crc << 1);
    }
    return crc;
}

gboolean hidpp_sector_valid(const guint8 *sector, gsize size)
{
    return size > 2 && hidpp_crc(sector, size - 2) == (sector[size - 2] << 8 | sector[size - 1]);
}

int hidpp_keys_slot(gsize profile_size)
{
    return profile_size >= KEYS_SLOT + HIDPP_SLOT + 2 ? KEYS_SLOT : -1;
}

gboolean hidpp_kept(KeyLight effect)
{
    return effect == KEY_LIGHT_WAVE || effect == KEY_LIGHT_RIPPLE || effect == KEY_LIGHT_OWN;
}

void hidpp_slot_decode(const guint8 *slot, HidppLighting *out)
{
    memset(out, 0, sizeof *out);
    switch (slot[0]) {
    case FX_OFF: out->effect = KEY_LIGHT_OFF; break;
    case FX_SOLID: out->effect = KEY_LIGHT_SOLID; break;
    case FX_BREATHING: out->effect = KEY_LIGHT_BREATHING; break;
    case FX_CYCLE_OLD: case FX_CYCLE: out->effect = KEY_LIGHT_CYCLE; break;
    case FX_WAVE_OLD: case FX_WAVE: out->effect = KEY_LIGHT_WAVE; break;
    case FX_RIPPLE_OLD: case FX_RIPPLE: out->effect = KEY_LIGHT_RIPPLE; break;
    case FX_OWN: case FX_OWN_SAVING: out->effect = KEY_LIGHT_OWN; break;
    default: out->effect = KEY_LIGHT_OTHER; break;
    }
    if (key_light_coloured(out->effect)) memcpy(out->rgb, slot + 1, 3);
}

static void put16(guint8 *at, guint value)
{
    at[0] = value >> 8 & 0xff;
    at[1] = value & 0xff;
}

/* Dimmed, a colour is half as bright, and an intensity half as strong. */
int hidpp_effect(const HidppLighting *l, gboolean dim, guint8 p[10])
{
    int intensity = dim ? FULL / 2 : FULL;
    memset(p, 0, 10);
    if (key_light_coloured(l->effect))
        for (int i = 0; i < 3; i++) p[i] = dim ? l->rgb[i] / 2 : l->rgb[i];
    switch (l->effect) {
    case KEY_LIGHT_OFF:
        return FX_OFF;
    case KEY_LIGHT_SOLID:
        p[3] = NO_RAMP;
        return FX_SOLID;
    case KEY_LIGHT_BREATHING:
        put16(p + 3, BREATH_MS);
        p[6] = intensity;      /* after the waveform, 0 for the keyboard's own */
        return FX_BREATHING;
    case KEY_LIGHT_CYCLE:
        p[1] = SATURATED;
        put16(p + 6, CYCLE_MS);
        p[8] = intensity;
        return FX_CYCLE;
    case KEY_LIGHT_WAVE:
        p[1] = SATURATED;
        put16(p + 6, WAVE_MS);
        p[8] = intensity;
        p[9] = WAVE_RIGHT;
        return FX_WAVE;
    case KEY_LIGHT_RIPPLE:
        p[3] = SATURATED;
        put16(p + 6, RIPPLE_PERIOD);
        return FX_RIPPLE;
    case KEY_LIGHT_OWN:
        return dim ? FX_OWN_SAVING : FX_OWN;
    default:
        return -1;
    }
}

gboolean hidpp_slot_encode(const HidppLighting *l, gboolean saving, guint8 slot[HIDPP_SLOT])
{
    if (!hidpp_kept(l->effect)) return FALSE;
    slot[0] = (guint8)hidpp_effect(l, saving, slot + 1);
    return TRUE;
}

/* A slot that already plays the effect keeps its speed and the rest, and
 * takes only the new colour. */
static void put_slot(guint8 *at, const HidppLighting *l, const guint8 fresh[HIDPP_SLOT])
{
    if (at[0] != fresh[0]) memcpy(at, fresh, HIDPP_SLOT);
    else if (key_light_coloured(l->effect)) memcpy(at + 1, fresh + 1, 3);
}

gboolean hidpp_profile_lit(const guint8 *sector, gsize size, const HidppLighting *l, guint8 *out)
{
    guint8 on[HIDPP_SLOT], saving[HIDPP_SLOT];
    if (size > HIDPP_SECTOR_MAX || size < SAVING_SLOT + HIDPP_SLOT + 2 || !hidpp_sector_valid(sector, size)
        || !hidpp_slot_encode(l, FALSE, on) || !hidpp_slot_encode(l, TRUE, saving))
        return FALSE;
    memmove(out, sector, size);
    put_slot(out + KEYS_SLOT, l, on);
    put_slot(out + SAVING_SLOT, l, saving);
    put16(out + size - 2, hidpp_crc(out, size - 2));
    return TRUE;
}

int hidpp_effect_index(const HidppEffects *fx, int cluster, guint16 id)
{
    if (cluster < 0 || cluster >= fx->clusters) return -1;
    for (int i = 0; i < fx->count[cluster]; i++)
        if (fx->effect[cluster][i] == id) return i;
    return -1;
}

void hidpp_decode_brightness(const guint8 *info, const guint8 *now, HidppKeyboard *out)
{
    out->has_brightness = FALSE;
    if (!info) return;
    out->brightness_max = info[0] << 8 | info[1];
    out->brightness_steps = info[2] & 0x0f;
    out->brightness_min = info[4] << 8 | info[5];
    if (out->brightness_max <= out->brightness_min) return;
    out->has_brightness = now != NULL;
    if (now) out->brightness = CLAMP(now[0] << 8 | now[1], 0, out->brightness_max);
}

void hidpp_decode_profile(const guint8 *sector, gsize size, HidppKeyboard *out)
{
    int slot = hidpp_keys_slot(size);
    out->has_profile = size <= HIDPP_SECTOR_MAX && slot >= 0 && hidpp_sector_valid(sector, size);
    if (!out->has_profile) return;
    memcpy(out->profile, sector, size);
    out->profile_size = size;
    hidpp_slot_decode(sector + slot, &out->light);
}

static int percent_of(const HidppKeyboard *k, int value)
{
    int range = k->brightness_max - k->brightness_min;
    return CLAMP((int)((value - k->brightness_min) * 100.0 / range + 0.5), 0, 100);
}

void hidpp_decode(const HidppKeyboard *k, const HidppModel *model, KeyboardSettings *out)
{
    memset(out, 0, sizeof *out);
    out->has_brightness = k->has_brightness;
    if (k->has_brightness) {
        out->brightness = percent_of(k, k->brightness);
        if (model && model->levels > 1) {
            out->levels = model->levels;
            for (int i = 0; i < model->levels; i++) out->level[i] = percent_of(k, model->level[i]);
        }
    }
    out->has_lighting = k->has_profile;
    out->app_controls = k->app_controls;
    out->light = k->has_profile ? k->light.effect : KEY_LIGHT_OWN;
    memcpy(out->rgb, k->light.rgb, 3);
}

int hidpp_brightness_value(const HidppKeyboard *k, int percent)
{
    int range = k->brightness_max - k->brightness_min;
    return k->brightness_min + (int)(CLAMP(percent, 0, 100) * range / 100.0 + 0.5);
}

/* The G715's factory profile, as its ROM holds it (issue #23): 1000 Hz,
 * F1 to F12 as themselves, lights dimmed after 60 s and off after 300 s,
 * and its own effect on the keys. */
static const guint8 G715_PROFILE[255] = {
    0x01, 0x01, 0x00, 0x90, 0x01, 0x20, 0x03, 0x40, 0x06, 0x80, 0x0c, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3c, 0x00, 0x2c, 0x01,
    0x80, 0x02, 0x00, 0x3a, 0x80, 0x02, 0x00, 0x3b, 0x80, 0x02, 0x00, 0x3c, 0x80, 0x02, 0x00, 0x3d,
    0x80, 0x02, 0x00, 0x3e, 0x80, 0x02, 0x00, 0x3f, 0x80, 0x02, 0x00, 0x40, 0x80, 0x02, 0x00, 0x41,
    0x80, 0x02, 0x00, 0x42, 0x80, 0x02, 0x00, 0x43, 0x80, 0x02, 0x00, 0x44, 0x80, 0x02, 0x00, 0x45,
    [0x50 ... 0xcf] = 0xff,
    0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0xff, 0x00, 0x11,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0a, 0x00, 0x00, 0xff, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xa9, 0xf7,
};

void hidpp_sample(HidppKeyboard *out)
{
    static const guint8 info[] = { 0x00, 0x64, 0x05, 0x03, 0x00, 0x00 }, now[] = { 0x00, 0x0c };
    memset(out, 0, sizeof *out);
    hidpp_decode_brightness(info, now, out);
    out->sector = 0x0001;
    hidpp_decode_profile(G715_PROFILE, sizeof G715_PROFILE, out);
    out->status_index = 0x04;
    out->rgb_index = 0x08;
    out->brightness_index = 0x0f;
    out->profiles_index = 0x10;
}

/* ------------------------------------------------------------ events */

gboolean hidpp_event(const guint8 *r, gsize len, guint8 index, const HidppKeyboard *k, HidppEvent *out)
{
    if (len < HIDPP_SHORT_LEN || (r[0] != HIDPP_SHORT && r[0] != HIDPP_LONG) || r[1] != index) return FALSE;
    /* The receiver's own notice, in HID++ 1.0. */
    if (r[0] == HIDPP_SHORT && r[2] == CONNECTION) {
        out->kind = HIDPP_EVENT_LINK;
        out->linked = !(r[4] & LINK_DOWN);
        return TRUE;
    }
    /* The device's, in HID++ 2.0: software ID 0, which no request uses. */
    if (r[2] == 0 || r[2] == 0xff || r[2] == 0x8f || (r[3] & 0x0f) != 0) return FALSE;
    if (k->status_index && r[2] == k->status_index) {
        if (r[4] != STATUS_RECONNECT) return FALSE;
        out->kind = HIDPP_EVENT_LINK;
        out->linked = TRUE;
        return TRUE;
    }
    /* Its brightness key says the new value, as the G715 does (issue #25). */
    if (k->brightness_index && r[2] == k->brightness_index && r[3] == 0x00) {
        out->kind = HIDPP_EVENT_BRIGHTNESS;
        out->brightness = r[4] << 8 | r[5];
        return TRUE;
    }
    if ((k->brightness_index && r[2] == k->brightness_index) || (k->profiles_index && r[2] == k->profiles_index)) {
        out->kind = HIDPP_EVENT_CHANGED;
        return TRUE;
    }
    /* While software holds the lights, the keyboard says when it's been left
     * alone about a minute (00) and when it's used again (01), and says it
     * again until answered (issue #57). Its effects' timing (function 0)
     * isn't news. */
    if (k->rgb_index && r[2] == k->rgb_index && r[3] == 0x10 && r[4] <= 1) {
        out->kind = r[4] ? HIDPP_EVENT_ACTIVE : HIDPP_EVENT_IDLE;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------ the link */

typedef struct {
    int fd;
    guint8 index, sw;
} Link;

static gboolean fail(GError **error, int code, const char *message)
{
    g_set_error_literal(error, G_IO_ERROR, code, message);
    return FALSE;
}

static gboolean link_open(Link *l, const char *node, guint8 index, guint8 sw, GError **error)
{
    l->index = index;
    l->sw = sw;
    l->fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (l->fd >= 0) return TRUE;
    int e = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s", g_strerror(e));
    return FALSE;
}

/* Sends function `function` of `feature`, at `feature_index`, and waits up
 * to `ms` for its answer's parameters, skipping everything else on the way:
 * the kernel's own requests and answers, and events. */
static gboolean call_for(Link *l, int ms, guint16 feature, guint8 feature_index, guint8 function,
                         const guint8 *params, gsize n, guint8 out[HIDPP_PARAMS], GError **error)
{
    if (!hidpp_allowed(feature, function, params, n))
        return fail(error, G_IO_ERROR_PERMISSION_DENIED, "Lares doesn't send that request.");
    guint8 packet[HIDPP_LONG_LEN];
    gsize len = hidpp_packet(l->sw, l->index, feature_index, function, params, n, packet);
    guint8 junk[64];
    while (read(l->fd, junk, sizeof junk) > 0) {}   /* what came before isn't for us */
    if (!len || write(l->fd, packet, len) != (ssize_t)len)
        return fail(error, G_IO_ERROR_FAILED, "Its receiver couldn't be asked.");
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    for (;;) {
        int left = (int)((end - g_get_monotonic_time()) / 1000);
        if (left <= 0) break;
        struct pollfd pfd = { .fd = l->fd, .events = POLLIN };
        int ready = poll(&pfd, 1, left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        guint8 r[64] = { 0 };
        ssize_t got = read(l->fd, r, sizeof r);
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (got <= 0) break;
        switch (hidpp_match(r, (gsize)got, packet)) {
        case HIDPP_ANSWER:
            memset(out, 0, HIDPP_PARAMS);
            memcpy(out, r + 4, MIN((gsize)got - 4, (gsize)HIDPP_PARAMS));
            return TRUE;
        case HIDPP_ERROR:
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "It refused the request (error %d).", r[5]);
            return FALSE;
        case HIDPP_RECEIVER_ERROR:
            return fail(error, G_IO_ERROR_NOT_CONNECTED, "It's asleep. Press a key to wake it.");
        case HIDPP_OTHER:
            break;
        }
    }
    return fail(error, G_IO_ERROR_TIMED_OUT, "It didn't answer; it may be asleep.");
}

static gboolean call(Link *l, guint16 feature, guint8 feature_index, guint8 function, const guint8 *params,
                     gsize n, guint8 out[HIDPP_PARAMS], GError **error)
{
    return call_for(l, ANSWER_MS, feature, feature_index, function, params, n, out, error);
}

/* The feature's index on the device, 0 when it hasn't the feature. */
static gboolean feature_index(Link *l, guint16 feature, guint8 *index, GError **error)
{
    const guint8 id[] = { feature >> 8, feature & 0xff };
    guint8 a[HIDPP_PARAMS];
    if (!call(l, HIDPP_ROOT, 0, ROOT_GET_FEATURE, id, sizeof id, a, error)) return FALSE;
    *index = a[0];
    return TRUE;
}

static gboolean read_profile(Link *l, guint8 fi, HidppKeyboard *out, GError **error)
{
    guint8 info[HIDPP_PARAMS], mode[HIDPP_PARAMS], current[HIDPP_PARAMS];
    if (!call(l, HIDPP_PROFILES, fi, PROFILES_INFO, NULL, 0, info, error)
        || !call(l, HIDPP_PROFILES, fi, PROFILES_MODE, NULL, 0, mode, error)
        || !call(l, HIDPP_PROFILES, fi, PROFILES_CURRENT, NULL, 0, current, error))
        return FALSE;
    gsize size = info[7] << 8 | info[8];
    out->sector = (guint16)(current[0] << 8 | current[1]);
    if (mode[0] == 2) out->app_controls = TRUE;
    if (size < 16 || size > HIDPP_SECTOR_MAX) return TRUE;
    guint8 sector[HIDPP_SECTOR_MAX];
    /* Sixteen bytes a time; the last read ends at the sector's end. */
    for (gsize got = 0; got < size;) {
        gsize at = MIN(got, size - 16);
        const guint8 where[] = { out->sector >> 8, out->sector & 0xff, at >> 8, at & 0xff };
        guint8 chunk[HIDPP_PARAMS];
        if (!call(l, HIDPP_PROFILES, fi, PROFILES_READ, where, sizeof where, chunk, error)) return FALSE;
        memcpy(sector + at, chunk, 16);
        got = at + 16;
    }
    hidpp_decode_profile(sector, size, out);
    return TRUE;
}

static gboolean read_all(Link *l, HidppKeyboard *out, GError **error)
{
    guint8 bi, pi, ri, info[HIDPP_PARAMS], now[HIDPP_PARAMS];
    memset(out, 0, sizeof *out);
    if (!feature_index(l, HIDPP_BRIGHTNESS, &bi, error) || !feature_index(l, HIDPP_PROFILES, &pi, error)
        || !feature_index(l, HIDPP_RGB_EFFECTS, &ri, error) || !feature_index(l, HIDPP_STATUS, &out->status_index, error))
        return FALSE;
    out->brightness_index = bi;
    out->profiles_index = pi;
    out->rgb_index = ri;
    if (bi) {
        if (!call(l, HIDPP_BRIGHTNESS, bi, BRIGHTNESS_INFO, NULL, 0, info, error)
            || !call(l, HIDPP_BRIGHTNESS, bi, BRIGHTNESS_GET, NULL, 0, now, error))
            return FALSE;
        hidpp_decode_brightness(info, now, out);
    }
    if (pi && !read_profile(l, pi, out, error)) return FALSE;
    if (ri) {
        /* Software holds the lighting, another app's or the helper's: what
         * shows isn't the profile. */
        const guint8 get[] = { 0x00 };
        guint8 a[HIDPP_PARAMS];
        if (!call(l, HIDPP_RGB_EFFECTS, ri, RGB_SW_CONTROL, get, sizeof get, a, error)) return FALSE;
        if (a[1] != 0) out->app_controls = TRUE;
    }
    return TRUE;
}

gboolean hidpp_read(const char *node, guint8 index, HidppKeyboard *out, GError **error)
{
    Link l;
    HidppKeyboard k;
    g_mutex_lock(&turns);
    gboolean ok = link_open(&l, node, index, HIDPP_SW_ID, error);
    if (ok) {
        ok = read_all(&l, &k, error);
        close(l.fd);
    }
    g_mutex_unlock(&turns);
    if (ok) *out = k;
    return ok;
}

static gboolean set_brightness(Link *l, int value, int *now, GError **error)
{
    guint8 fi, info[HIDPP_PARAMS], a[HIDPP_PARAMS];
    if (!feature_index(l, HIDPP_BRIGHTNESS, &fi, error)) return FALSE;
    if (!fi) return fail(error, G_IO_ERROR_NOT_SUPPORTED, "It has no brightness to set.");
    if (!call(l, HIDPP_BRIGHTNESS, fi, BRIGHTNESS_INFO, NULL, 0, info, error)) return FALSE;
    HidppKeyboard k = { 0 };
    hidpp_decode_brightness(info, NULL, &k);
    if (k.brightness_max <= k.brightness_min) return fail(error, G_IO_ERROR_NOT_SUPPORTED, "It has no brightness to set.");
    value = CLAMP(value, 0, k.brightness_max);
    const guint8 v[] = { value >> 8, value & 0xff };
    if (!call(l, HIDPP_BRIGHTNESS, fi, BRIGHTNESS_SET, v, sizeof v, a, error)
        || !call(l, HIDPP_BRIGHTNESS, fi, BRIGHTNESS_GET, NULL, 0, a, error))
        return FALSE;
    *now = a[0] << 8 | a[1];
    return TRUE;
}

gboolean hidpp_set_brightness(const char *node, guint8 index, int value, int *now, GError **error)
{
    Link l;
    g_mutex_lock(&turns);
    gboolean ok = link_open(&l, node, index, HIDPP_SW_ID, error);
    if (ok) {
        ok = set_brightness(&l, value, now, error);
        close(l.fd);
    }
    g_mutex_unlock(&turns);
    return ok;
}

/* The whole sector, 16 bytes a time, the last padded; the keyboard keeps
 * the length the write began with, checks the CRC at its end, and only
 * then stores it. */
static gboolean write_profile(Link *l, guint8 fi, guint16 sector, const guint8 *data, gsize size, GError **error)
{
    const guint8 begin[] = { sector >> 8, sector & 0xff, 0, 0, size >> 8, size & 0xff };
    guint8 a[HIDPP_PARAMS];
    if (!call(l, HIDPP_PROFILES, fi, PROFILES_BEGIN, begin, sizeof begin, a, error)) return FALSE;
    for (gsize at = 0; at < size; at += 16) {
        guint8 chunk[16] = { 0 };
        memcpy(chunk, data + at, MIN((gsize)16, size - at));
        if (!call(l, HIDPP_PROFILES, fi, PROFILES_WRITE, chunk, sizeof chunk, a, error)) return FALSE;
    }
    return call_for(l, SAVED_MS, HIDPP_PROFILES, fi, PROFILES_END, NULL, 0, a, error);
}

static gboolean set_lighting(Link *l, const HidppLighting *want, HidppKeyboard *out, GError **error)
{
    HidppKeyboard k;
    guint8 sector[HIDPP_SECTOR_MAX];
    if (!read_all(l, &k, error)) return FALSE;
    if (!k.profiles_index || !k.has_profile || !hidpp_profile_lit(k.profile, k.profile_size, want, sector))
        return fail(error, G_IO_ERROR_NOT_SUPPORTED, "Its lighting can't be changed.");
    if (k.sector != HIDPP_SECTOR)
        return fail(error, G_IO_ERROR_NOT_SUPPORTED, "It's using a profile Lares doesn't change.");
    if (memcmp(sector, k.profile, k.profile_size) != 0) {
        if (!write_profile(l, k.profiles_index, k.sector, sector, k.profile_size, error) || !read_all(l, &k, error))
            return FALSE;
        if (!k.has_profile || memcmp(k.profile, sector, k.profile_size) != 0)
            return fail(error, G_IO_ERROR_FAILED, "It didn't keep the new lighting.");
    }
    *out = k;
    return TRUE;
}

gboolean hidpp_set_lighting(const char *node, guint8 index, const HidppLighting *l, HidppKeyboard *out,
                            GError **error)
{
    Link link;
    HidppKeyboard k;
    g_mutex_lock(&turns);
    gboolean ok = link_open(&link, node, index, HIDPP_SW_ID, error);
    if (ok) {
        ok = set_lighting(&link, l, &k, error);
        close(link.fd);
    }
    g_mutex_unlock(&turns);
    if (ok) *out = k;
    return ok;
}

/* ------------------------------------------------------------- live */

static gboolean read_effects(Link *l, HidppEffects *out, GError **error)
{
    memset(out, 0, sizeof *out);
    if (!feature_index(l, HIDPP_RGB_EFFECTS, &out->feature, error) || !feature_index(l, HIDPP_STATUS, &out->status, error))
        return FALSE;
    if (!out->feature) return fail(error, G_IO_ERROR_NOT_SUPPORTED, "It has no lighting to play.");
    const guint8 device[] = { 0xff, 0xff, 0x00 };
    guint8 a[HIDPP_PARAMS];
    if (!call(l, HIDPP_RGB_EFFECTS, out->feature, RGB_INFO, device, sizeof device, a, error)) return FALSE;
    out->clusters = MIN(a[2], HIDPP_CLUSTERS);
    for (int c = 0; c < out->clusters; c++) {
        const guint8 group[] = { c, 0xff, 0x00 };
        if (!call(l, HIDPP_RGB_EFFECTS, out->feature, RGB_INFO, group, sizeof group, a, error)) return FALSE;
        out->count[c] = MIN(a[4], HIDPP_EFFECTS);
        for (int i = 0; i < out->count[c]; i++) {
            const guint8 which[] = { c, i, 0x00 };
            if (!call(l, HIDPP_RGB_EFFECTS, out->feature, RGB_INFO, which, sizeof which, a, error)) return FALSE;
            out->effect[c][i] = (guint16)(a[2] << 8 | a[3]);
        }
    }
    return TRUE;
}

static gboolean sw_set(Link *l, const HidppEffects *fx, guint8 mode, guint8 flags, GError **error)
{
    const guint8 set[] = { 0x01, mode, flags };
    guint8 a[HIDPP_PARAMS];
    return call(l, HIDPP_RGB_EFFECTS, fx->feature, RGB_SW_CONTROL, set, sizeof set, a, error);
}

static gboolean live_play(Link *l, const HidppEffects *fx, const HidppLighting *light, HidppLive state,
                          GError **error)
{
    guint8 params[10];
    int id = hidpp_effect(light, state == HIDPP_LIVE_IDLE, params);
    if (id < 0 || hidpp_kept(light->effect)) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Lares doesn't play that live.");
    if ((state == HIDPP_LIVE_TAKE && !sw_set(l, fx, SW_FIRMWARE, SW_FLAGS[state], error))
        || !sw_set(l, fx, SW_SOFTWARE, SW_FLAGS[state], error))
        return FALSE;
    gboolean any = FALSE;
    for (int c = 0; c < fx->clusters; c++) {
        int at = hidpp_effect_index(fx, c, (guint16)id);
        if (at < 0) continue;
        guint8 set[13] = { c, at }, a[HIDPP_PARAMS];
        memcpy(set + 2, params, 10);
        set[12] = VOLATILE;
        if (!call(l, HIDPP_RGB_EFFECTS, fx->feature, RGB_SET_EFFECT, set, sizeof set, a, error)) return FALSE;
        any = TRUE;
    }
    return any || fail(error, G_IO_ERROR_NOT_SUPPORTED, "It has no such effect.");
}

/* The helper's requests, under the same turns as the window's in its own
 * process; answers to each carry their own software ID. */
static gboolean with_link(const char *node, guint8 index, GError **error,
                          gboolean (*fn)(Link *l, gpointer data, GError **error), gpointer data)
{
    Link l;
    g_mutex_lock(&turns);
    gboolean ok = link_open(&l, node, index, HIDPP_SW_HELPER, error);
    if (ok) {
        ok = fn(&l, data, error);
        close(l.fd);
    }
    g_mutex_unlock(&turns);
    return ok;
}

static gboolean do_effects(Link *l, gpointer data, GError **error) { return read_effects(l, data, error); }

gboolean hidpp_live_effects(const char *node, guint8 index, HidppEffects *out, GError **error)
{
    HidppEffects fx;
    gboolean ok = with_link(node, index, error, do_effects, &fx);
    if (ok) *out = fx;
    return ok;
}

typedef struct { const HidppEffects *fx; const HidppLighting *light; HidppLive state; } Play;

static gboolean do_play(Link *l, gpointer data, GError **error)
{
    Play *p = data;
    return live_play(l, p->fx, p->light, p->state, error);
}

gboolean hidpp_live_play(const char *node, guint8 index, const HidppEffects *fx, const HidppLighting *light,
                         HidppLive state, GError **error)
{
    Play p = { fx, light, state };
    return with_link(node, index, error, do_play, &p);
}

static gboolean do_release(Link *l, gpointer data, GError **error) { return sw_set(l, data, SW_FIRMWARE, 0, error); }

gboolean hidpp_live_release(const char *node, guint8 index, const HidppEffects *fx, GError **error)
{
    return with_link(node, index, error, do_release, (gpointer)fx);
}

/* ------------------------------------------------------------ the rules */

char *hidpp_udev_rules(void)
{
    GString *s = g_string_new("");
    for (guint i = 0; i < G_N_ELEMENTS(MODELS); i++) {
        gboolean seen = FALSE;   /* one rule a receiver */
        for (guint j = 0; j < i; j++) seen |= MODELS[j].receiver == MODELS[i].receiver;
        if (seen) continue;
        g_string_append_printf(s, "SUBSYSTEM==\"hidraw\", ENV{ID_USB_VENDOR_ID}==\"046d\", "
                               "ENV{ID_USB_MODEL_ID}==\"%04x\", ENV{ID_USB_INTERFACE_NUM}==\"%02d\", "
                               "DEVPATH==\"*/0003:046D:%04X.\?\?\?\?/hidraw/hidraw*\", TAG+=\"uaccess\"\n",
                               MODELS[i].receiver, MODELS[i].interface, MODELS[i].receiver);
    }
    return g_string_free(s, FALSE);
}
