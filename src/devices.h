#pragma once
#include <gio/gio.h>

/* Controllers, headphones, keyboards and mice, from BlueZ (pairing and
 * connection) and UPower (batteries). Both send signals when something
 * changes, so nothing here polls. */

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
