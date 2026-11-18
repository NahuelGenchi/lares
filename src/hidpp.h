#pragma once
#include <gio/gio.h>

/* Logitech's HID++ 2.0, through a receiver's HID++ interface: what Lares
 * reads and changes on a Logitech keyboard paired to it. Today: the G715's
 * brightness, and the lighting its onboard profile holds. */

#define HIDPP_SHORT      0x10
#define HIDPP_LONG       0x11
#define HIDPP_SHORT_LEN  7
#define HIDPP_LONG_LEN   20
#define HIDPP_PARAMS     16     /* parameter bytes in a long report */
/* The low nibble of every request's function byte, echoed in its answer.
 * The kernel uses 1 and notifications 0; OpenRGB 7, LGSTrayEx 0xA, Solaar
 * 0xB, G HUB 0xD and firmware 0xF, so Lares' answers can't be theirs. */
#define HIDPP_SW_ID      0x0c
#define HIDPP_SECTOR_MAX 256    /* the largest profile sector Lares keeps */
#define HIDPP_SLOT       11     /* bytes in a lighting slot of a profile */

enum {
    HIDPP_ROOT        = 0x0000,
    HIDPP_FEATURE_SET = 0x0001,
    HIDPP_BRIGHTNESS  = 0x8040,
    HIDPP_RGB_EFFECTS = 0x8071,
    HIDPP_PROFILES    = 0x8100,
};

/* The lighting a profile slot holds, as Lares names it. */
typedef enum {
    HIDPP_LIGHT_OFF,
    HIDPP_LIGHT_SOLID,
    HIDPP_LIGHT_BREATHING,
    HIDPP_LIGHT_CYCLE,
    HIDPP_LIGHT_WAVE,
    HIDPP_LIGHT_RIPPLE,
    HIDPP_LIGHT_OWN,        /* one of the keyboard's own, such as the G715's factory effect */
} HidppLight;

typedef struct {
    HidppLight effect;
    guint8 rgb[3];          /* Solid, Breathing and Ripple */
} HidppLighting;

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
} HidppKeyboard;

/* What an input report is to a request. */
typedef enum { HIDPP_OTHER, HIDPP_ANSWER, HIDPP_ERROR, HIDPP_RECEIVER_ERROR } HidppMatch;

/* Whether Lares may send function `function` of feature `feature` with
 * these parameters: the reads it needs, and setBrightness. Everything
 * else, such as firmware updates, resets or the keyboard's test modes, is
 * refused before it's sent. */
gboolean hidpp_allowed(guint16 feature, guint8 function, const guint8 *params, gsize n);

/* Builds a request in `out`, short when the parameters fit, and returns its
 * length, or 0 when there are more than HIDPP_PARAMS. */
gsize hidpp_packet(guint8 index, guint8 feature_index, guint8 function, const guint8 *params, gsize n,
                   guint8 out[HIDPP_LONG_LEN]);

/* What `report` is to the request `packet`: its answer (parameters from
 * byte 4), the device's error (code in byte 5), the receiver's error, or
 * something else, such as the kernel's traffic or an event. */
HidppMatch hidpp_match(const guint8 *report, gsize len, const guint8 *packet);

/* CRC-16/CCITT (start 0xffff, polynomial 0x1021), which closes every
 * profile sector, big-endian, in its last two bytes. */
guint16 hidpp_crc(const guint8 *data, gsize len);
gboolean hidpp_sector_valid(const guint8 *sector, gsize size);

/* The lighting in an 11-byte profile slot: the effect, then its
 * parameters, the colour first. */
void hidpp_slot_decode(const guint8 *slot, HidppLighting *out);

/* Where the keys' lighting sits in a profile, while in use, or -1 when the
 * profile is too short to hold it. */
int hidpp_keys_slot(gsize profile_size);

/* Fills `out` from what the keyboard answered: brightness getInfo and
 * getBrightness (either may be NULL), and the profile. */
void hidpp_decode_brightness(const guint8 *info, const guint8 *now, HidppKeyboard *out);
void hidpp_decode_profile(const guint8 *sector, gsize size, HidppKeyboard *out);

/* How a G715 comes, for a dry run without one: its factory profile, at the
 * brightness key's second-lowest step. */
void hidpp_sample(HidppKeyboard *out);

/* These ask the device at `index` behind the receiver on `node`
 * (/dev/hidrawN) and wait for it: they block, so run them in a worker
 * thread. They take turns. G_IO_ERROR_NOT_CONNECTED while it sleeps. */
gboolean hidpp_read(const char *node, guint8 index, HidppKeyboard *out, GError **error);
/* Sets the brightness, then reads it back into `now`. */
gboolean hidpp_set_brightness(const char *node, guint8 index, int value, int *now, GError **error);
