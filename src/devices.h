#pragma once
#include <gio/gio.h>
#include "keylight.h"

/* Controllers, headphones, keyboards and mice, from BlueZ (pairing and
 * connection), UPower (batteries) and sysfs (keyboards and mice plugged in
 * by USB, wired or on a receiver). BlueZ and UPower send signals and
 * /dev/input is watched, so nothing here polls. */

typedef enum { DEV_CONTROLLER, DEV_AUDIO, DEV_KEYBOARD, DEV_MOUSE, N_DEV_KINDS } DevKind;

/* How a device reaches the PC, when Lares can tell. A wireless mouse whose
 * receiver looks like any USB device counts as USB. */
typedef enum { LINK_UNKNOWN, LINK_USB, LINK_RECEIVER, LINK_BLUETOOTH } DevLink;

/* The lighting effects a mouse's own firmware plays. LIGHT_OTHER is one it
 * reports but Lares doesn't offer, such as an animation another app saved. */
typedef enum { LIGHT_OFF, LIGHT_SOLID, LIGHT_BREATHING, LIGHT_CYCLE, LIGHT_OTHER } LightEffect;

#define MOUSE_STAGES 5

/* A mouse's settings in plain units, as it last reported them. */
typedef struct {
    int stages, active;          /* DPI stages in use, and the one in use, from 0 */
    int dpi[MOUSE_STAGES];
    int dpi_min, dpi_max;        /* what the sensor allows, in steps of 50 */
    int rate, rate_max;          /* polling rate, in Hz */
    int lift_mm;                 /* lift-off distance: 1 or 2 */
    int brightness;              /* LED brightness, 0 to 100 % */
    LightEffect light;
    guint8 rgb[3];               /* the colour of a solid or breathing light */
} MouseSettings;

#define KEY_LEVELS 8

/* A keyboard's settings in plain units, as it last reported them. */
typedef struct {
    gboolean has_brightness;
    int brightness;              /* 0 to 100 % */
    int levels;                  /* the brightness levels it keeps, from off, or 0 when it keeps any */
    int level[KEY_LEVELS];       /* those levels, in %, rising */
    gboolean has_lighting;       /* its onboard profile holds the lighting below */
    gboolean app_controls;       /* an app holds the lighting, so the profile's isn't what shows */
    KeyLight light;
    guint8 rgb[3];               /* the colour of a solid, breathing or ripple light */
    gboolean has_timers;         /* its onboard profile holds the two below */
    int dim_s, off_s;            /* seconds without a key before its lights dim, and go off */
    gboolean has_startup, has_shutdown;
    gboolean startup, shutdown;  /* it plays its effect as it turns on, and as it turns off */
} KeyboardSettings;

typedef struct {
    char *id;          /* the BlueZ or UPower object path */
    char *name;
    char *address;     /* Bluetooth address, only to match a battery to its device */
    char *model;       /* only when it differs from the name, as after a rename */
    DevLink link;
    DevKind kind;
    gboolean bluetooth, connected;
    int battery;       /* 0 to 100, or -1 when the device doesn't say */
    gboolean charging;
    gboolean battery_locked;  /* its battery can be read once the person allows it */
    gboolean settings_locked; /* and its settings */
    gboolean has_settings;    /* its own protocol reports its settings (src/hyperx.c, src/hidpp.c) */
    MouseSettings *mouse;     /* what they were last time, or NULL until it answers */
    KeyboardSettings *keyboard;
    gboolean asleep;          /* its receiver says it's asleep or off */
} Device;

typedef struct Devices Devices;
typedef void (*DevNotify)(gpointer user);
typedef void (*DevDone)(gboolean ok, const char *message, gpointer user);

Devices *devices_new(void);
GPtrArray *devices_list(Devices *d);        /* Device*, sorted by kind, then name */
gboolean devices_any(Devices *d, DevKind kind);
void devices_watch(Devices *d, DevNotify fn, gpointer user);
/* done may be NULL; watchers hear about the change either way. */
void devices_set_connected(Devices *d, const char *id, gboolean on, DevDone done, gpointer user);
/* Batteries that only the device's own protocol reports (src/hyperx.c)
 * are asked for while something on screen wants them: now, then every few
 * minutes. Calls nest; each `on` needs its `off`. */
void devices_want_batteries(Devices *d, gboolean on);
/* Lets the person at the screen read those batteries and settings from now
 * on: udev rules installed through pkexec, which asks for the password
 * once. */
void devices_allow_batteries(Devices *d, DevDone done, gpointer user);
/* The rules that does install: each device's own interface, and nothing
 * else of it. */
char *devices_udev_rules(void);
/* The settings of mice, or of keyboards, are read while something on screen
 * wants them, and the devices are listened to meanwhile, so pressing a
 * mouse's DPI button or waking a device shows at once. Calls nest; each
 * `on` needs its `off`. */
void devices_want_settings(Devices *d, DevKind kind, gboolean on);
/* Writes what `want` changes from `base`, the settings the person changed,
 * to the mouse, live and to its own memory, then reads it back. Whatever
 * they didn't change keeps the mouse's own. A dry run only pretends. */
void devices_set_mouse(Devices *d, const char *id, const MouseSettings *base, const MouseSettings *want,
                       DevDone done, gpointer user);
/* The same for a keyboard: its brightness; the lighting in its onboard
 * profile, which it keeps and plays by itself, when it's an effect it keeps
 * (hidpp_kept), as the helper plays the others from the settings; the
 * profile's dim and lights-off timers; and its startup and shutdown
 * effects. */
void devices_set_keyboard(Devices *d, const char *id, const KeyboardSettings *base, const KeyboardSettings *want,
                          DevDone done, gpointer user);

/* How an input node in sysfs is read, kept apart for the unit tests. */
/* Whether `bit` is set in a capability bitmap: words of hex, most
 * significant first, as in "1f0000 0 0 0 0". */
gboolean devices_bitmap_has(const char *bitmap, guint bit);
/* What a node is, from its key and relative-axis bitmaps, by udev's rules: a
 * gamepad has gamepad or joystick buttons (DEV_CONTROLLER); a mouse moves on
 * two axes and has a left button; a keyboard has every key from Esc to S.
 * -1 for anything else, such as volume or power buttons. */
int devices_input_kind(const char *keys, const char *rel);
/* The physical device a node's sysfs device belongs to, so a mouse's extra
 * keyboard node isn't a second device: a device paired to a receiver (a HID
 * device inside the receiver's own), or else the USB device. NULL off USB. */
char *devices_input_owner(const char *device);
/* The USB device a sysfs path sits under, such as the receiver of a device
 * paired to one. NULL off USB. */
char *devices_usb_parent(const char *path);
