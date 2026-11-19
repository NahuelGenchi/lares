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
#define RGB_SW_CONTROL   5   /* 00 gets <sub-op> <mode> <flags>; 0 is the firmware's own */
#define PROFILES_INFO    0   /* [7..8] the sector size */
#define PROFILES_MODE    2   /* 1 onboard, 2 host: an app sends the settings */
#define PROFILES_CURRENT 4   /* the sector in use */
#define PROFILES_READ    5   /* <sector:2> <offset:2> -> 16 bytes */
#define PROFILES_BEGIN   6   /* memoryAddrWrite <sector:2> <offset:2> <length:2>: a write begins */
#define PROFILES_WRITE   7   /* memoryWrite: the next 16 bytes of it */
#define PROFILES_END     8   /* memoryWriteEnd: the keyboard checks the CRC and stores it */
#define KEYS_SLOT        0xd0 /* the keys' lighting while in use */
#define SAVING_SLOT      0xe6 /* and once they dim after a minute alone */

/* Effects by their ID in a slot. Colour cycle, wave and ripple each have an
 * older ID and the one the G715 lists, which adds a saturation; Lares writes
 * the listed ones. Its factory effect is 0x18, and 0x19 while saving power,
 * which no list names (issue #23). */
enum {
    FX_OFF = 0x00, FX_SOLID = 0x01, FX_BREATHING = 0x0a,
    FX_CYCLE_OLD = 0x03, FX_WAVE_OLD = 0x04, FX_RIPPLE_OLD = 0x0b,
    FX_CYCLE = 0x15, FX_WAVE = 0x16, FX_RIPPLE = 0x17,
    FX_OWN = 0x18, FX_OWN_SAVING = 0x19,
};

/* What Lares writes with an effect, after its colour: periods in ms,
 * big-endian, full intensity and saturation, and the wave moving right.
 * Changing only the colour keeps what the slot already says. */
#define BREATH_MS     5000
#define CYCLE_MS      5000
#define WAVE_MS       5000
#define RIPPLE_PERIOD 20
#define FULL          100   /* intensity, in % */
#define SATURATED     0xff
#define WAVE_RIGHT    1

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

/* ------------------------------------------------------------ requests */

static gboolean get_sub(const guint8 *params, gsize n)
{
    return n > 0 && params[0] == 0x00;
}

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
    { HIDPP_RGB_EFFECTS, RGB_SW_CONTROL, get_sub },
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

gsize hidpp_packet(guint8 index, guint8 feature_index, guint8 function, const guint8 *params, gsize n,
                   guint8 out[HIDPP_LONG_LEN])
{
    if (n > HIDPP_PARAMS || function > 0x0f) return 0;
    gsize len = n <= HIDPP_SHORT_LEN - 4 ? HIDPP_SHORT_LEN : HIDPP_LONG_LEN;
    memset(out, 0, HIDPP_LONG_LEN);
    out[0] = len == HIDPP_SHORT_LEN ? HIDPP_SHORT : HIDPP_LONG;
    out[1] = index;
    out[2] = feature_index;
    out[3] = (guint8)(function << 4 | HIDPP_SW_ID);
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

/* The parameters follow the effect: the colour first, for those with one. */
gboolean hidpp_slot_encode(const HidppLighting *l, gboolean saving, guint8 slot[HIDPP_SLOT])
{
    guint8 *p = slot + 1;
    memset(slot, 0, HIDPP_SLOT);
    if (key_light_coloured(l->effect)) memcpy(p, l->rgb, 3);
    switch (l->effect) {
    case KEY_LIGHT_OFF:
        slot[0] = FX_OFF;
        break;
    case KEY_LIGHT_SOLID:
        slot[0] = FX_SOLID;   /* then its ramp, 0 for the keyboard's own */
        break;
    case KEY_LIGHT_BREATHING:
        slot[0] = FX_BREATHING;
        put16(p + 3, BREATH_MS);
        p[6] = FULL;          /* after the waveform, 0 for the keyboard's own */
        break;
    case KEY_LIGHT_CYCLE:
        slot[0] = FX_CYCLE;
        p[1] = SATURATED;
        put16(p + 6, CYCLE_MS);
        p[8] = FULL;
        break;
    case KEY_LIGHT_WAVE:
        slot[0] = FX_WAVE;
        p[1] = SATURATED;
        put16(p + 6, WAVE_MS);
        p[8] = FULL;
        p[9] = WAVE_RIGHT;
        break;
    case KEY_LIGHT_RIPPLE:
        slot[0] = FX_RIPPLE;
        p[3] = SATURATED;
        put16(p + 6, RIPPLE_PERIOD);
        break;
    case KEY_LIGHT_OWN:
        slot[0] = saving ? FX_OWN_SAVING : FX_OWN;
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

/* A slot that already plays the effect keeps its speed and the rest, and
 * takes only the new colour. */
static void put_slot(guint8 *at, const HidppLighting *l, const guint8 fresh[HIDPP_SLOT])
{
    if (at[0] != fresh[0]) memcpy(at, fresh, HIDPP_SLOT);
    else if (key_light_coloured(l->effect)) memcpy(at + 1, l->rgb, 3);
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
    return FALSE;
}

/* ------------------------------------------------------------ the link */

typedef struct {
    int fd;
    guint8 index;
} Link;

static gboolean fail(GError **error, int code, const char *message)
{
    g_set_error_literal(error, G_IO_ERROR, code, message);
    return FALSE;
}

static gboolean link_open(Link *l, const char *node, guint8 index, GError **error)
{
    l->index = index;
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
    gsize len = hidpp_packet(l->index, feature_index, function, params, n, packet);
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
    if (bi) {
        if (!call(l, HIDPP_BRIGHTNESS, bi, BRIGHTNESS_INFO, NULL, 0, info, error)
            || !call(l, HIDPP_BRIGHTNESS, bi, BRIGHTNESS_GET, NULL, 0, now, error))
            return FALSE;
        hidpp_decode_brightness(info, now, out);
    }
    if (pi && !read_profile(l, pi, out, error)) return FALSE;
    if (ri) {
        /* Another app took the lighting over: what shows isn't the profile. */
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
    gboolean ok = link_open(&l, node, index, error);
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
    gboolean ok = link_open(&l, node, index, error);
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
    gboolean ok = link_open(&link, node, index, error);
    if (ok) {
        ok = set_lighting(&link, l, &k, error);
        close(link.fd);
    }
    g_mutex_unlock(&turns);
    if (ok) *out = k;
    return ok;
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
