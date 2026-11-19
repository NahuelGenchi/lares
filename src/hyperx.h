#pragma once
#include <gio/gio.h>
#include "devices.h"

/* HyperX's wireless mice, over the HID protocol HyperX's NGENUITY app uses,
 * on their receiver's vendor interface, or on their own while plugged in by
 * their cable: the battery, and the settings the mouse keeps in its own
 * memory. Today: the Pulsefire Haste 2 Wireless. */

/* The rule file's name predates the settings; it opens the same interface. */
#define HYPERX_RULES "/etc/udev/rules.d/70-lares-batteries.rules"
#define HYPERX_REPORT 64      /* every report, the report ID first */
#define HYPERX_BLOCKS 4       /* DPI table, sensor, brightness, lighting */

typedef struct {
    guint16 vendor, product;
    int interface;            /* the USB interface that answers */
    gboolean receiver;        /* the device is a wireless receiver */
    guint16 cable;            /* a receiver's: its mouse's own product while on its cable, or 0 */
    int dpi_min, dpi_max, rate_max;
} HyperxModel;

/* A mouse's settings as its own answers to the four questions, kept whole
 * so a change rewrites only what it means to and keeps the rest, such as
 * the colours of the DPI stages. Each holds the answer's report from byte
 * 0, so its offsets are the protocol's. */
typedef struct {
    guint8 dpi[HYPERX_REPORT];        /* 33 02: [2] rate, [3] stage mask, [4] active stage, [5 + 5i] stage i */
    guint8 sensor[HYPERX_REPORT];     /* 37 02: [5] lift-off, 0 for 1 mm and 2 for 2 mm */
    guint8 brightness[HYPERX_REPORT]; /* 41 02: [2] 0 to 255 */
    guint8 light[HYPERX_REPORT];      /* 43 03: [2] effect, [3..4] speed, [5] direction, [6] colours, [7..9] RGB */
} HyperxState;

typedef enum { HYPERX_STAGE, HYPERX_LINK, HYPERX_BATTERY } HyperxEventKind;

/* Something the mouse says by itself. */
typedef struct {
    HyperxEventKind kind;
    int stage;               /* HYPERX_STAGE: the stage's bit in the mask */
    int code;                /* and its DPI as the table codes it */
    gboolean linked;         /* HYPERX_LINK: awake, or asleep and off */
    int level;               /* HYPERX_BATTERY */
    gboolean charging;
} HyperxEvent;

/* The model with this USB vendor and product, or NULL. */
const HyperxModel *hyperx_model(guint16 vendor, guint16 product);

/* Whether the mouse `receiver` pairs with is on its cable: among `plugged`,
 * the models of the `n` USB devices there now, any of which may be NULL.
 * The mouse then leaves its receiver unlinked. */
gboolean hyperx_on_cable(const HyperxModel *receiver, const HyperxModel *const *plugged, guint n);

/* Whether `report`, one input report as hidraw returns it, is a battery
 * answer; if so, its level (0 to 100) and whether it's charging. */
gboolean hyperx_battery_parse(const guint8 *report, gsize len, int *level, gboolean *charging);

/* Whether `report` is something the mouse says unasked: its DPI button
 * picked a stage, it woke or fell asleep, or its battery after waking. */
gboolean hyperx_event(const guint8 *report, gsize len, HyperxEvent *event);

/* Takes a HYPERX_STAGE event into `state` when it matches its DPI table,
 * and says whether it did; when it doesn't, read the table again. */
gboolean hyperx_take_stage(HyperxState *state, const HyperxEvent *event);

/* `state` in plain units. `model` gives the limits, and may be NULL. */
void hyperx_decode(const HyperxModel *model, const HyperxState *state, MouseSettings *out);

/* The packets that make profile `slot` (0 live, 1 saved) hold `want`, built
 * on `now`, which is how the live profile is: one for each of the DPI table,
 * sensor, brightness and lighting that differs. Returns how many, or -1 when
 * `want` is something the mouse can't hold. */
int hyperx_packets(const HyperxState *now, const MouseSettings *want, guint16 slot,
                   guint8 out[HYPERX_BLOCKS][HYPERX_REPORT]);

/* The mouse as it is `now`, with what `want` changes from `base`, which is
 * what the person saw when they changed it. Settings they didn't touch keep
 * the mouse's own, even when it changed them meanwhile, such as its DPI
 * button. The limits come from `want`. */
void hyperx_merge(const HyperxState *now, const MouseSettings *base, const MouseSettings *want, MouseSettings *out);

/* A Solid light takes up a stored brightness 2 to 3 s late, and a live one
 * not at all, but a colour streamed to it, as NGENUITY streams its effects,
 * shows the live brightness at once, for a second or so each. So when this
 * says a change from `was` to `now` is a Solid light's new brightness,
 * hyperx_write streams its colour once, and the caller goes on with
 * hyperx_show every HYPERX_SHOW_EVERY_MS for HYPERX_SHOW_MS, until the
 * mouse's own effect has caught up. Breathing and the colour cycle aren't
 * still colours, so they keep the pause. */
#define HYPERX_SHOW_MS       4000
#define HYPERX_SHOW_EVERY_MS 300
gboolean hyperx_shows(const MouseSettings *was, const MouseSettings *now);

/* Updates `state` as one of those packets would update the mouse: what a
 * dry run pretends, and what the tests check them against. */
void hyperx_apply(HyperxState *state, const guint8 *packet);

/* How a new Pulsefire Haste 2 Wireless is set, for a dry run without one. */
void hyperx_sample(HyperxState *state);

/* These ask the receiver on `node` (/dev/hidrawN), or the mouse itself on
 * its cable, and wait for the mouse: they block, so run them in a worker
 * thread. They take turns, as the mouse drops packets that come too fast.
 * A sleeping mouse doesn't answer; its receiver says so, and then nothing
 * is sent to it, since the receiver would keep it for the mouse until it
 * wakes. `model` is the device's on `node`. */
gboolean hyperx_battery(const char *node, int *level, gboolean *charging, GError **error);
/* The live profile. G_IO_ERROR_NOT_CONNECTED while the mouse sleeps. */
gboolean hyperx_read(const char *node, const HyperxModel *model, HyperxState *state, GError **error);
/* Writes what `want` changes from `base` (as hyperx_merge) live and to the
 * mouse's memory, then reads the live profile back into `state` and checks
 * that it holds the change. */
gboolean hyperx_write(const char *node, const HyperxModel *model, const MouseSettings *base,
                      const MouseSettings *want, HyperxState *state, GError **error);

/* Streams `rgb` to the light once, at its live brightness. */
gboolean hyperx_show(const char *node, const guint8 rgb[3], GError **error);

/* The udev rules that let the person at the screen open that interface,
 * and nothing else of these devices. */
char *hyperx_udev_rules(void);
