#pragma once
#include <gio/gio.h>

/* Controllers, headphones, keyboards and mice, from BlueZ (pairing and
 * connection), UPower (batteries) and sysfs (keyboards and mice plugged in
 * by USB, wired or on a receiver). BlueZ and UPower send signals and
 * /dev/input is watched, so nothing here polls. */

typedef enum { DEV_CONTROLLER, DEV_AUDIO, DEV_KEYBOARD, DEV_MOUSE, N_DEV_KINDS } DevKind;

typedef struct {
    char *id;          /* the BlueZ or UPower object path */
    char *name;
    char *address;     /* Bluetooth address, only to match a battery to its device */
    DevKind kind;
    gboolean bluetooth, connected;
    int battery;       /* 0 to 100, or -1 when the device doesn't say */
} Device;

typedef struct Devices Devices;
typedef void (*DevNotify)(gpointer user);
typedef void (*DevDone)(gboolean ok, const char *message, gpointer user);

Devices *devices_new(void);
GPtrArray *devices_list(Devices *d);        /* Device*, sorted by kind, then name */
gboolean devices_any(Devices *d, DevKind kind);
void devices_watch(Devices *d, DevNotify fn, gpointer user);
void devices_set_connected(Devices *d, const char *id, gboolean on, DevDone done, gpointer user);

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
