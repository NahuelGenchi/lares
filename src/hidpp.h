#pragma once
#include <gio/gio.h>
#include "devices.h"

/* Logitech's HID++ 2.0, through a receiver's HID++ interface: what Lares
 * reads and changes on a Logitech keyboard paired to it. Today: the G715's
 * brightness, the lighting its onboard profile keeps, and the lighting the
 * helper plays live, for effects the keyboard can't keep. */

#define HIDPP_SHORT      0x10
#define HIDPP_LONG       0x11
#define HIDPP_SHORT_LEN  7
#define HIDPP_LONG_LEN   20
#define HIDPP_PARAMS     16     /* parameter bytes in a long report */
/* The low nibble of every request's function byte, echoed in its answer.
 * The kernel uses 1 and notifications 0; OpenRGB 7, LGSTrayEx 0xA, Solaar
 * 0xB, G HUB 0xD and firmware 0xF, so Lares' answers can't be theirs. */
#define HIDPP_SW_ID      0x0c
#define HIDPP_SW_HELPER  0x0e   /* the helper's own, so its answers and the window's never cross */
#define HIDPP_SECTOR_MAX 256    /* the largest profile sector Lares keeps */
#define HIDPP_SLOT       11     /* bytes in a lighting slot of a profile */
#define HIDPP_SECTOR     0x0001 /* the one profile sector Lares ever writes */
#define HIDPP_CLUSTERS   2      /* light groups Lares plays live: the G715's keys and its edge */
#define HIDPP_EFFECTS    8      /* effects Lares looks up in each group's list */

enum {
    HIDPP_ROOT        = 0x0000,
    HIDPP_FEATURE_SET = 0x0001,
    HIDPP_STATUS      = 0x1d4b,   /* wireless device status: it says when it reconnects */
    HIDPP_BRIGHTNESS  = 0x8040,
    HIDPP_RGB_EFFECTS = 0x8071,
    HIDPP_PROFILES    = 0x8100,
};

/* A keyboard Lares knows, by its receiver's USB product and its own
 * wireless one, both Logitech's. Others wait until one has been tried. */
typedef struct {
    guint16 receiver, product;
    int interface;          /* the receiver's HID++ interface */
    /* The brightness values it keeps, from off: its key steps through
     * them, and it keeps no other. 0 when it keeps any. */
    int levels;
    int level[KEY_LEVELS];
} HidppModel;

/* The lighting in a profile slot, or played live. */
typedef struct {
    KeyLight effect;
    guint8 rgb[3];          /* Solid, Breathing and Ripple */
} HidppLighting;

/* What a keyboard's RGB effects feature offers, read once: where it sits,
 * and each light group's effects by their index. */
typedef struct {
    guint8 feature, status;  /* 0x8071's index, and wireless status's for its reconnects */
    int clusters;
    int count[HIDPP_CLUSTERS];
    guint16 effect[HIDPP_CLUSTERS][HIDPP_EFFECTS];
} HidppEffects;

/* What the keyboard was last told while the helper plays its lighting:
 * that software takes it, or that the keyboard is idle or in use again. */
typedef enum { HIDPP_LIVE_TAKE, HIDPP_LIVE_IDLE, HIDPP_LIVE_ACTIVE } HidppLive;

/* A keyboard's state, as its own answers. */
typedef struct {
    gboolean has_brightness;
    int brightness, brightness_min, brightness_max;
    int brightness_steps;   /* the levels its brightness key steps through, 0 if not said */
    gboolean has_profile;   /* the lighting below comes from its onboard profile */
    gboolean app_controls;  /* an app holds the lighting: the profile isn't what shows */
    HidppLighting light;    /* the keys, while in use */
    guint16 sector;         /* the profile in use */
    gsize profile_size;
    guint8 profile[HIDPP_SECTOR_MAX];  /* kept whole, so a change rewrites only its slots */
    /* Where its features sit, 0 when it hasn't them, to tell its events apart. */
    guint8 status_index, brightness_index, profiles_index, rgb_index;
} HidppKeyboard;

/* What an input report is to a request. */
typedef enum { HIDPP_OTHER, HIDPP_ANSWER, HIDPP_ERROR, HIDPP_RECEIVER_ERROR } HidppMatch;

/* Something a keyboard or its receiver says unasked: the receiver lost or
 * found it, or it reconnected; its brightness changed, by its own key or
 * by a request; another of its settings changed, so read it again; or,
 * while software holds its lights, it was left alone a while or used again. */
typedef enum {
    HIDPP_EVENT_LINK, HIDPP_EVENT_BRIGHTNESS, HIDPP_EVENT_CHANGED, HIDPP_EVENT_IDLE, HIDPP_EVENT_ACTIVE,
} HidppEventKind;

typedef struct {
    HidppEventKind kind;
    gboolean linked;        /* HIDPP_EVENT_LINK: awake, or asleep and off */
    int brightness;         /* HIDPP_EVENT_BRIGHTNESS: the new value */
} HidppEvent;

/* The model with this receiver and wireless product, or NULL. */
const HidppModel *hidpp_model(guint16 receiver, guint16 product);

/* A device's index on its receiver, from the HID_PHYS of its uevent, such
 * as "usb-0000:00:14.0-8.4/input2:1"; -1 when it has none. */
int hidpp_phys_index(const char *phys);

/* For a keyboard Lares knows, from its HID device in sysfs, such as
 * .../1-8.4:1.2/0003:046D:C547.0007/0003:046D:40A2.0009: the receiver's
 * HID++ node (/dev/hidrawN), with the keyboard's index on it and its model;
 * NULL for any other device. hidpp_find looks for the first one plugged in. */
char *hidpp_node(const char *device, guint8 *index, const HidppModel **model);
char *hidpp_find(guint8 *index, const HidppModel **model);

/* Whether Lares may send function `function` of feature `feature` with
 * these parameters: the reads it needs, setBrightness, a write of the whole
 * profile in HIDPP_SECTOR, and the live lighting: an effect for one light
 * group, and taking the lights into software control or giving them back.
 * Everything else, such as other sectors, firmware updates, resets or the
 * keyboard's test modes, is refused before it's sent. */
gboolean hidpp_allowed(guint16 feature, guint8 function, const guint8 *params, gsize n);

/* Builds a request from software ID `sw` in `out`, short when the
 * parameters fit, and returns its length, or 0 when there are more than
 * HIDPP_PARAMS. */
gsize hidpp_packet(guint8 sw, guint8 index, guint8 feature_index, guint8 function, const guint8 *params, gsize n,
                   guint8 out[HIDPP_LONG_LEN]);

/* What `report` is to the request `packet`: its answer (parameters from
 * byte 4), the device's error (code in byte 5), the receiver's error, or
 * something else, such as the kernel's traffic or an event. */
HidppMatch hidpp_match(const guint8 *report, gsize len, const guint8 *packet);

/* CRC-16/CCITT (start 0xffff, polynomial 0x1021), which closes every
 * profile sector, big-endian, in its last two bytes. */
guint16 hidpp_crc(const guint8 *data, gsize len);
gboolean hidpp_sector_valid(const guint8 *sector, gsize size);

/* Whether the keyboard keeps `effect` in its onboard profile and plays it
 * with nothing running: Wave, Ripple and its own. The profile's lighting
 * slot plays the effects every light group shares, and the G715 shares no
 * other (issue #57). Off, Solid, Breathing and Colour cycle play live. */
gboolean hidpp_kept(KeyLight effect);

/* The effect ID of `l`, and its ten parameters in the RGB effects layout:
 * the colour first, then speed, saturation and intensity as each effect
 * has them. `dim` gives them at half strength, as for an idle keyboard. -1
 * for an effect Lares can't play. */
int hidpp_effect(const HidppLighting *l, gboolean dim, guint8 params[10]);

/* The lighting in an 11-byte profile slot: the effect, then its
 * parameters. */
void hidpp_slot_decode(const guint8 *slot, HidppLighting *out);
/* And back, for the slot in use or the one for saving power (`saving`),
 * which plays it dimmed. FALSE for an effect the keyboard doesn't keep. */
gboolean hidpp_slot_encode(const HidppLighting *l, gboolean saving, guint8 slot[HIDPP_SLOT]);

/* `sector` with the keys' lighting changed to `l`, in use and while saving
 * power, and its CRC made again, in `out`; everything else stays. FALSE when
 * `sector` isn't a whole profile or the keyboard doesn't keep `l`. */
gboolean hidpp_profile_lit(const guint8 *sector, gsize size, const HidppLighting *l, guint8 *out);

/* The index of effect `id` in light group `cluster`'s list, or -1. */
int hidpp_effect_index(const HidppEffects *fx, int cluster, guint16 id);

/* Where the keys' lighting sits in a profile, while in use, or -1 when the
 * profile is too short to hold it. */
int hidpp_keys_slot(gsize profile_size);

/* Fills `out` from what the keyboard answered: brightness getInfo and
 * getBrightness (either may be NULL), and the profile. */
void hidpp_decode_brightness(const guint8 *info, const guint8 *now, HidppKeyboard *out);
void hidpp_decode_profile(const guint8 *sector, gsize size, HidppKeyboard *out);

/* `k` in plain units, with the levels `model` keeps (it may be NULL), and
 * back: the brightness value that is `percent`. */
void hidpp_decode(const HidppKeyboard *k, const HidppModel *model, KeyboardSettings *out);
int hidpp_brightness_value(const HidppKeyboard *k, int percent);

/* Whether `report` is something the device at `index` behind this receiver
 * says unasked (`k` holds where its features sit). Answers to anyone's
 * requests, Lares' or the kernel's, aren't. */
gboolean hidpp_event(const guint8 *report, gsize len, guint8 index, const HidppKeyboard *k, HidppEvent *out);

/* How a G715 comes, for a dry run without one: its factory profile, at the
 * brightness key's second-lowest step. */
void hidpp_sample(HidppKeyboard *out);

/* These ask the device at `index` behind the receiver on `node`
 * (/dev/hidrawN) and wait for it: they block, so run them in a worker
 * thread. They take turns. G_IO_ERROR_NOT_CONNECTED while it sleeps. */
gboolean hidpp_read(const char *node, guint8 index, HidppKeyboard *out, GError **error);
/* Sets the brightness, then reads it back into `now`. */
gboolean hidpp_set_brightness(const char *node, guint8 index, int value, int *now, GError **error);
/* Writes `l` into the onboard profile in use, which must be HIDPP_SECTOR:
 * reads the profile, changes its lighting slots, writes it whole with a new
 * CRC, and reads it back, which fills `out`. Nothing is written when it
 * already holds `l`. */
gboolean hidpp_set_lighting(const char *node, guint8 index, const HidppLighting *l, HidppKeyboard *out,
                            GError **error);

/* For the helper, with its own software ID. They block too. Reads what the
 * RGB effects feature offers; plays `l` on every light group that has it,
 * under software control, telling the keyboard `state` (dimmed when idle);
 * or gives the lights back to the keyboard, which plays its profile's. */
gboolean hidpp_live_effects(const char *node, guint8 index, HidppEffects *out, GError **error);
gboolean hidpp_live_play(const char *node, guint8 index, const HidppEffects *fx, const HidppLighting *l,
                         HidppLive state, GError **error);
gboolean hidpp_live_release(const char *node, guint8 index, const HidppEffects *fx, GError **error);

/* The udev rules that let the person at the screen open each known
 * receiver's HID++ node, and nothing else of it. The keyboard's own node
 * sits under the receiver's interface too, and carries its keystrokes:
 * DEVPATH leaves it out. */
char *hidpp_udev_rules(void);
