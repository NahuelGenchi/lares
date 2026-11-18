// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* HyperX's wireless mice speak what NGENUITY calls its universal protocol,
 * on their receiver's vendor interface, in 64-byte reports whose first byte
 * is the report ID, which is also the command. The ngenuity-linux-cli
 * project (MIT) worked it out from NGENUITY and tested it on the Pulsefire
 * Haste 2 Wireless; Lares follows its notes and takes no code from it.
 *
 * A request is a command, a sub-command, then its payload. The receiver
 * acknowledges every report with report ff (ff 01, a 12-byte id, the
 * command, the sub-command, a status), and the mouse answers a question
 * with report command + 1 and the same sub-command. Settings live in
 * profiles: 0 is what the mouse does now, 1 what it reloads when switched
 * on. Packets go at least 10 ms apart and each waits for its answer, or
 * the mouse drops some over the air. Nothing here sends a factory reset. */
#include "hyperx.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#define ACK           0xff
#define EVENT         0xfb   /* fb 08 <stage> <dpi>: DPI button; fb 0a <0|1>: asleep or awake */
#define LINK          0x52   /* 52 01 -> 53 01 <linked> */
#define BATTERY       0x50   /* 50 02 -> 51 02 <level> <charge state> */
#define DPI           0x32   /* 32 01 <profile> <table> writes, 32 02 <profile> reads */
#define SENSOR        0x36   /* 36 01 / 36 02, the same way */
#define BRIGHTNESS    0x40   /* 40 01 / 40 02 */
#define LIGHTING      0x42   /* 42 02 <profile> <effect> writes, 42 03 <profile> reads */
#define SET           0x01
#define GET           0x02
#define LIGHT_SET     0x02
#define LIGHT_GET     0x03
#define PACE_US       10000
#define ANSWER_MS     500
#define DPI_STEP      50
#define DEFAULT_SPEED 50     /* of breathing and the colour cycle; lower is faster */

static const HyperxModel MODELS[] = {
    /* Pulsefire Haste 2 Wireless, its 2.4 GHz receiver */
    { 0x03f0, 0x0f98, "HyperX", 2, TRUE, 200, 26000, 1000 },
};

/* Polling rates, and their codes in the DPI table. */
static const struct { guint8 code; int hz; } RATES[] = {
    { 0x40, 125 }, { 0x20, 250 }, { 0x10, 500 }, { 0x08, 1000 },
    { 0x04, 2000 }, { 0x02, 4000 }, { 0x01, 8000 },
};

/* The firmware's own effects, by LightEffect. */
static const guint8 EFFECTS[] = { [LIGHT_OFF] = 0, [LIGHT_SOLID] = 1, [LIGHT_BREATHING] = 2, [LIGHT_CYCLE] = 4 };

static GMutex turns;   /* one exchange with a receiver at a time */

const HyperxModel *hyperx_model(guint16 vendor, guint16 product)
{
    for (guint i = 0; i < G_N_ELEMENTS(MODELS); i++)
        if (MODELS[i].vendor == vendor && MODELS[i].product == product) return &MODELS[i];
    return NULL;
}

/* 51 02: byte 2 the level, byte 3 the charging state (0 not charging,
 * 1 charging, 2 full). */
gboolean hyperx_battery_parse(const guint8 *report, gsize len, int *level, gboolean *charging)
{
    if (len < 4 || report[0] != BATTERY + 1 || report[1] != 0x02 || report[2] > 100) return FALSE;
    *level = report[2];
    *charging = report[3] == 1;
    return TRUE;
}

gboolean hyperx_event(const guint8 *report, gsize len, HyperxEvent *event)
{
    int level;
    gboolean charging;
    if (hyperx_battery_parse(report, len, &level, &charging)) {
        *event = (HyperxEvent){ .kind = HYPERX_BATTERY, .level = level, .charging = charging };
        return TRUE;
    }
    if (len < 3 || report[0] != EVENT) return FALSE;
    if (report[1] == 0x08 && len >= 5 && report[2] < 8) {
        *event = (HyperxEvent){ .kind = HYPERX_STAGE, .stage = report[2] };
        return TRUE;
    }
    if (report[1] == 0x0a) {
        *event = (HyperxEvent){ .kind = HYPERX_LINK, .linked = report[2] == 1 };
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------- settings */

static int rate_hz(guint8 code)
{
    for (guint i = 0; i < G_N_ELEMENTS(RATES); i++)
        if (RATES[i].code == code) return RATES[i].hz;
    return 0;
}

static guint8 rate_code(int hz)
{
    for (guint i = 0; i < G_N_ELEMENTS(RATES); i++)
        if (RATES[i].hz == hz) return RATES[i].code;
    return 0;
}

/* A stage is its DPI as a little-endian code, DPI / 50 - 1, then the RGB
 * of the light that shows it. */
static int stage_dpi(const guint8 *stage)
{
    return ((stage[0] | stage[1] << 8) + 1) * DPI_STEP;
}

static gboolean has_colour(LightEffect light)
{
    return light == LIGHT_SOLID || light == LIGHT_BREATHING;
}

void hyperx_decode(const HyperxModel *model, const HyperxState *s, MouseSettings *out)
{
    memset(out, 0, sizeof *out);
    for (int bit = 0; bit < 8 && out->stages < MOUSE_STAGES; bit++) {
        if (!(s->dpi[3] >> bit & 1)) continue;
        if (bit == s->dpi[4]) out->active = out->stages;
        out->dpi[out->stages++] = stage_dpi(s->dpi + 5 + 5 * bit);
    }
    out->rate = rate_hz(s->dpi[2]);
    out->lift_mm = s->sensor[5] == 2 ? 2 : 1;
    out->brightness = (s->brightness[2] * 100 + 127) / 255;
    switch (s->light[2]) {
    case 0:  out->light = LIGHT_OFF; break;
    case 1:  out->light = LIGHT_SOLID; break;
    case 2:  out->light = LIGHT_BREATHING; break;
    case 4:  out->light = LIGHT_CYCLE; break;
    default: out->light = LIGHT_OTHER; break;
    }
    /* Other effects leave old bytes there. */
    if (has_colour(out->light)) memcpy(out->rgb, s->light + 7, 3);
    if (model) {
        out->dpi_min = model->dpi_min;
        out->dpi_max = model->dpi_max;
        out->rate_max = model->rate_max;
    }
}

static gboolean same_light(const MouseSettings *a, const MouseSettings *b)
{
    return a->light == b->light && (!has_colour(a->light) || memcmp(a->rgb, b->rgb, 3) == 0);
}

static gboolean same_dpi(const MouseSettings *a, const MouseSettings *b)
{
    if (a->rate != b->rate || a->stages != b->stages || a->active != b->active) return FALSE;
    for (int i = 0; i < a->stages; i++)
        if (a->dpi[i] != b->dpi[i]) return FALSE;
    return TRUE;
}

/* Whether `got`, read back from the mouse, holds `want`. */
static gboolean holds(const MouseSettings *got, const MouseSettings *want)
{
    return same_dpi(got, want) && got->lift_mm == want->lift_mm && got->brightness == want->brightness
        && same_light(got, want);
}

static guint8 *packet(guint8 out[][HYPERX_REPORT], int *n, guint8 command, guint8 sub, guint16 slot)
{
    guint8 *p = out[(*n)++];
    memset(p, 0, HYPERX_REPORT);
    p[0] = command;
    p[1] = sub;
    p[2] = slot & 0xff;
    p[3] = slot >> 8;
    return p;
}

/* Each packet carries what the answer to its question does, after the two
 * bytes of the profile: the answer's bytes from 2 on are the packet's from
 * 4 on. */
int hyperx_packets(const HyperxState *now, const MouseSettings *want, guint16 slot,
                   guint8 out[HYPERX_BLOCKS][HYPERX_REPORT])
{
    MouseSettings was;
    hyperx_decode(NULL, now, &was);
    int n = 0;

    if (!same_dpi(want, &was)) {
        guint8 code = rate_code(want->rate);
        if (!code || (want->rate_max && want->rate > want->rate_max) || want->stages != was.stages
            || want->active < 0 || want->active >= was.stages)
            return -1;
        guint8 *p = packet(out, &n, DPI, SET, slot);
        memcpy(p + 4, now->dpi + 2, HYPERX_REPORT - 4);
        p[4] = code;
        for (int bit = 0, i = 0; bit < 8 && i < was.stages; bit++) {
            if (!(now->dpi[3] >> bit & 1)) continue;
            int dpi = want->dpi[i];
            if (dpi % DPI_STEP || dpi < MAX(want->dpi_min, DPI_STEP) || (want->dpi_max && dpi > want->dpi_max))
                return -1;
            int stage_code = dpi / DPI_STEP - 1;
            p[7 + 5 * bit] = stage_code & 0xff;
            p[8 + 5 * bit] = stage_code >> 8;
            if (i == want->active) p[6] = bit;
            i++;
        }
    }
    if (want->lift_mm != was.lift_mm) {
        if (want->lift_mm != 1 && want->lift_mm != 2) return -1;
        guint8 *p = packet(out, &n, SENSOR, SET, slot);
        memcpy(p + 4, now->sensor + 2, 5);
        p[7] = want->lift_mm == 2 ? 2 : 0;
    }
    if (want->brightness != was.brightness) {
        if (want->brightness < 0 || want->brightness > 100) return -1;
        packet(out, &n, BRIGHTNESS, SET, slot)[4] = want->brightness * 255 / 100;
    }
    if (!same_light(want, &was)) {
        if ((guint)want->light >= LIGHT_OTHER) return -1;
        guint8 *p = packet(out, &n, LIGHTING, LIGHT_SET, slot);
        p[4] = EFFECTS[want->light];
        if (want->light == LIGHT_BREATHING || want->light == LIGHT_CYCLE)
            p[5] = want->light == was.light && now->light[3] ? now->light[3] : DEFAULT_SPEED;
        if (has_colour(want->light)) {
            p[8] = 1;            /* one colour */
            memcpy(p + 9, want->rgb, 3);
        } else if (want->light == LIGHT_CYCLE) {
            p[8] = 0xfe;         /* the whole spectrum */
        }
    }
    return n;
}

static void answer_from(guint8 *answer, guint8 command, guint8 sub, const guint8 *p)
{
    memset(answer, 0, HYPERX_REPORT);
    answer[0] = command + 1;
    answer[1] = sub;
    memcpy(answer + 2, p + 4, HYPERX_REPORT - 4);
}

void hyperx_apply(HyperxState *s, const guint8 *p)
{
    if (p[0] == DPI && p[1] == SET) answer_from(s->dpi, DPI, GET, p);
    else if (p[0] == SENSOR && p[1] == SET) answer_from(s->sensor, SENSOR, GET, p);
    else if (p[0] == BRIGHTNESS && p[1] == SET) answer_from(s->brightness, BRIGHTNESS, GET, p);
    else if (p[0] == LIGHTING && p[1] == LIGHT_SET) answer_from(s->light, LIGHTING, LIGHT_GET, p);
}

/* What the protocol notes read from one: 1000 Hz; 400, 800, 1600 (active),
 * 3200 and 6400 DPI in red, blue, yellow, green and white; 1 mm; full
 * brightness; and the colour cycle NGENUITY saves as an animation. */
void hyperx_sample(HyperxState *s)
{
    static const guint8 dpi[] = {
        DPI + 1, GET, 0x08, 0x1f, 0x02,
        0x07, 0x00, 0xff, 0x00, 0x00,   0x0f, 0x00, 0x00, 0x00, 0xff,   0x1f, 0x00, 0xff, 0xff, 0x00,
        0x3f, 0x00, 0x00, 0xff, 0x00,   0x7f, 0x00, 0xff, 0xff, 0xff,
    };
    memset(s, 0, sizeof *s);
    memcpy(s->dpi, dpi, sizeof dpi);
    s->sensor[0] = SENSOR + 1;
    s->sensor[1] = GET;
    s->brightness[0] = BRIGHTNESS + 1;
    s->brightness[1] = GET;
    s->brightness[2] = 0xff;
    s->light[0] = LIGHTING + 1;
    s->light[1] = LIGHT_GET;
    s->light[2] = 6;               /* an uploaded animation */
    s->light[3] = DEFAULT_SPEED;
}

/* ------------------------------------------------------------ the link */

typedef struct {
    int fd;
    gint64 sent;   /* monotonic time of the last packet */
} Port;

typedef gboolean (*Match)(const guint8 *report, gsize len, const guint8 *packet);

static gboolean fail(GError **error, int code, const char *message)
{
    g_set_error_literal(error, G_IO_ERROR, code, message);
    return FALSE;
}

static gboolean port_open(Port *p, const char *node, GError **error)
{
    p->sent = 0;
    p->fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (p->fd >= 0) return TRUE;
    int e = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s", g_strerror(e));
    return FALSE;
}

static gboolean port_send(Port *p, const guint8 *packet)
{
    gint64 wait = p->sent + PACE_US - g_get_monotonic_time();
    if (p->sent && wait > 0) g_usleep(wait);
    gboolean ok = write(p->fd, packet, HYPERX_REPORT) == HYPERX_REPORT;
    p->sent = g_get_monotonic_time();
    return ok;
}

/* Reads reports until one that `match` takes for `packet`, or `ms` pass,
 * skipping everything else: acknowledgements, events, other answers. */
static gboolean port_wait(Port *p, int ms, Match match, const guint8 *packet, guint8 *out)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    for (;;) {
        int left = (int)((end - g_get_monotonic_time()) / 1000);
        if (left <= 0) return FALSE;
        struct pollfd pfd = { .fd = p->fd, .events = POLLIN };
        int ready = poll(&pfd, 1, left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return FALSE;
        guint8 report[HYPERX_REPORT] = { 0 };
        ssize_t got = read(p->fd, report, sizeof report);
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (got <= 0) return FALSE;
        if (match(report, (gsize)got, packet)) {
            if (out) memcpy(out, report, sizeof report);
            return TRUE;
        }
    }
}

static gboolean is_answer(const guint8 *r, gsize len, const guint8 *packet)
{
    return len >= 2 && r[0] == packet[0] + 1 && r[1] == packet[1];
}

static gboolean is_battery(const guint8 *r, gsize len, const guint8 *packet)
{
    int level;
    gboolean charging;
    return hyperx_battery_parse(r, len, &level, &charging);
}

static gboolean is_ack(const guint8 *r, gsize len, const guint8 *packet)
{
    return len > 16 && r[0] == ACK && r[1] == 0x01 && r[14] == packet[0] && r[15] == packet[1] && r[16] == 0;
}

static gboolean ask(Port *p, const guint8 *packet, guint8 *answer)
{
    return port_send(p, packet) && port_wait(p, ANSWER_MS, is_answer, packet, answer);
}

static gboolean ask_live(Port *p, guint8 command, guint8 sub, guint8 *answer)
{
    const guint8 q[HYPERX_REPORT] = { command, sub, 0 };
    return ask(p, q, answer);
}

/* The receiver answers this itself, asleep or not. */
static gboolean linked(Port *p, GError **error)
{
    const guint8 q[HYPERX_REPORT] = { LINK, 0x01 };
    guint8 a[HYPERX_REPORT];
    if (!ask(p, q, a)) return fail(error, G_IO_ERROR_TIMED_OUT, "Its receiver didn't answer.");
    if (a[2] != 1) return fail(error, G_IO_ERROR_NOT_CONNECTED, "It's asleep. Move it to wake it.");
    return TRUE;
}

static gboolean read_state(Port *p, HyperxState *s, GError **error)
{
    gboolean ok = ask_live(p, DPI, GET, s->dpi) && ask_live(p, SENSOR, GET, s->sensor)
               && ask_live(p, BRIGHTNESS, GET, s->brightness) && ask_live(p, LIGHTING, LIGHT_GET, s->light);
    return ok || fail(error, G_IO_ERROR_TIMED_OUT, "It didn't answer; it may have fallen asleep.");
}

/* Sends a change and waits until it's taken: the mouse answers a new
 * effect itself, and the receiver acknowledges the rest. */
static gboolean send_change(Port *p, const guint8 *packet)
{
    return port_send(p, packet) && port_wait(p, ANSWER_MS, packet[0] == LIGHTING ? is_answer : is_ack, packet, NULL);
}

gboolean hyperx_battery(const char *node, int *level, gboolean *charging, GError **error)
{
    Port p;
    g_mutex_lock(&turns);
    gboolean ok = FALSE, sent = TRUE, opened = port_open(&p, node, error);
    if (opened) {
        const guint8 q[HYPERX_REPORT] = { BATTERY, 0x02 };
        guint8 a[HYPERX_REPORT];
        /* The receiver may only wake the mouse on the first ask. */
        for (int round = 0; round < 2 && !ok && sent; round++) {
            sent = port_send(&p, q);
            ok = sent && port_wait(&p, round == 0 ? 400 : 800, is_battery, q, a)
                 && hyperx_battery_parse(a, sizeof a, level, charging);
        }
        close(p.fd);
    }
    g_mutex_unlock(&turns);
    if (opened && !ok)
        fail(error, sent ? G_IO_ERROR_TIMED_OUT : G_IO_ERROR_FAILED,
             sent ? "It didn't answer; it may be asleep." : "It couldn't be asked.");
    return ok;
}

gboolean hyperx_read(const char *node, HyperxState *state, GError **error)
{
    Port p;
    HyperxState s;
    g_mutex_lock(&turns);
    gboolean ok = port_open(&p, node, error);
    if (ok) {
        ok = linked(&p, error) && read_state(&p, &s, error);
        close(p.fd);
    }
    g_mutex_unlock(&turns);
    if (ok) *state = s;
    return ok;
}

static gboolean write_both(Port *p, const MouseSettings *want, HyperxState *state, GError **error)
{
    HyperxState now;
    if (!linked(p, error) || !read_state(p, &now, error)) return FALSE;
    /* Live first, then the profile it reloads when switched on, as NGENUITY
     * saves. */
    guint8 packets[HYPERX_BLOCKS][HYPERX_REPORT];
    for (guint16 slot = 0; slot < 2; slot++) {
        int n = hyperx_packets(&now, want, slot, packets);
        if (n < 0) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "The mouse can't hold that setting.");
        for (int i = 0; i < n; i++)
            if (!send_change(p, packets[i]))
                return fail(error, G_IO_ERROR_TIMED_OUT, "It didn't take the change; it may have fallen asleep.");
    }
    if (!read_state(p, state, error)) return FALSE;
    MouseSettings got;
    hyperx_decode(NULL, state, &got);
    return holds(&got, want) || fail(error, G_IO_ERROR_FAILED, "It kept its earlier setting.");
}

gboolean hyperx_write(const char *node, const MouseSettings *want, HyperxState *state, GError **error)
{
    Port p;
    HyperxState s;
    g_mutex_lock(&turns);
    gboolean ok = port_open(&p, node, error);
    if (ok) {
        ok = write_both(&p, want, &s, error);
        close(p.fd);
    }
    g_mutex_unlock(&turns);
    if (ok) *state = s;
    return ok;
}

char *hyperx_udev_rules(void)
{
    GString *s = g_string_new("# Installed by Lares: lets the person at the screen read these devices'\n"
                              "# batteries and settings, through that one interface only.\n");
    for (guint i = 0; i < G_N_ELEMENTS(MODELS); i++)
        g_string_append_printf(s, "SUBSYSTEM==\"hidraw\", ENV{ID_USB_VENDOR_ID}==\"%04x\", "
                               "ENV{ID_USB_MODEL_ID}==\"%04x\", ENV{ID_USB_INTERFACE_NUM}==\"%02d\", "
                               "TAG+=\"uaccess\"\n", MODELS[i].vendor, MODELS[i].product, MODELS[i].interface);
    return g_string_free(s, FALSE);
}
