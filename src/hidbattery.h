#pragma once
#include <gio/gio.h>

/* Batteries that only a device's own HID protocol reports, read through its
 * hidraw node. Today: the HyperX Pulsefire Haste 2 Wireless on its USB
 * receiver, over the protocol HyperX's NGENUITY app uses. */

#define HIDBATTERY_RULES "/etc/udev/rules.d/70-lares-batteries.rules"

typedef struct {
    guint16 vendor, product;
    int interface;        /* the USB interface that answers battery questions */
} HidBatteryModel;

/* The model with this USB vendor and product, or NULL. */
const HidBatteryModel *hidbattery_model(guint16 vendor, guint16 product);

/* Whether `report`, one input report as hidraw returns it, is a battery
 * answer; if so, its level (0 to 100) and whether it's charging. */
gboolean hidbattery_parse(const guint8 *report, gsize len, int *level, gboolean *charging);

/* Asks the device on `node` (/dev/hidrawN) and waits up to about a second.
 * Blocks: run it in a worker thread. A sleeping mouse doesn't answer. */
gboolean hidbattery_query(const char *node, int *level, gboolean *charging, GError **error);

/* The udev rules that let the person at the screen open those battery
 * interfaces, and nothing else of the devices. */
char *hidbattery_udev_rules(void);
