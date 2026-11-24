// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-FileCopyrightText: 2026 T4T5
// SPDX-License-Identifier: GPL-2.0-or-later
/* The Bluetooth LE advertisement that wakes AirDrop receivers (SPEC §2),
 * ported from owly's proto/ble.rs and owly-cli's bluez.rs (MIT): Lares
 * exports an org.bluez.LEAdvertisement1 object on the system bus and
 * registers it with the first adapter that offers LE advertising. Every
 * call is asynchronous, and a failure only stops the advertisement: it's
 * logged once, for debugging. BlueZ advertises a registration as soon as
 * the adapter is powered, so a switched-off adapter is left alone. While
 * BlueZ or an adapter is missing it looks again every second for a while:
 * an adapter can arrive after the advertisement is asked for, as one lent
 * to WSL does (issue #107). */
#include "discovery.h"
#include <string.h>

#define BLUEZ            "org.bluez"
#define MANAGER          "org.bluez.LEAdvertisingManager1"
#define ADVERTISEMENT    "org.bluez.LEAdvertisement1"
#define APPLE_COMPANY_ID 0x004c
#define AIRDROP_TYPE     0x05
/* BLE-5: the interval current Apple devices answer to. */
#define INTERVAL_MS      100
#define RETRY_MS         1000
#define RETRIES          30

/* BLE-2, version 3: reserved 0x40, the tag, four reserved zeros, the
 * version, then four 2-byte contact hashes, all zero here (BLE-3), and a
 * reserved zero. */
void ble_airdrop_payload(const guint8 tag[3], guint8 out[18])
{
    memset(out, 0, 18);
    out[0] = 0x40;
    memcpy(out + 1, tag, 3);
    out[8] = 0x03;
}

/* The object BlueZ reads the advertisement from. It's connectable, as
 * Apple's own are, though nothing connects to it (BLE-5). Older BlueZ only
 * took the interval with its experimental features on, so the object
 * comes without it when a registration with it fails. */
static const char ADVERT_XML[] =
    "<node><interface name='" ADVERTISEMENT "'>"
    "<method name='Release'/>"
    "<property name='Type' type='s' access='read'/>"
    "<property name='ManufacturerData' type='a{qv}' access='read'/>"
    "<property name='MinInterval' type='u' access='read'/>"
    "<property name='MaxInterval' type='u' access='read'/>"
    "</interface></node>";
static const char PLAIN_ADVERT_XML[] =
    "<node><interface name='" ADVERTISEMENT "'>"
    "<method name='Release'/>"
    "<property name='Type' type='s' access='read'/>"
    "<property name='ManufacturerData' type='a{qv}' access='read'/>"
    "</interface></node>";

struct AirdropBle {
    gboolean active;          /* as set_active last asked */
    GDBusConnection *bus;
    GCancellable *cancel;     /* this activation's calls */
    char *path;               /* our object, while exported */
    guint object;
    char *adapter;            /* where RegisterAdvertisement went */
    gboolean registering;     /* it went, and BlueZ hasn't dropped it */
    gboolean interval;        /* offer MinInterval and MaxInterval, till BlueZ refuses them */
    guint serial;
    guint8 data[20];          /* the AirDrop TLV: type, length, payload */
    gboolean logged;
    guint retry;              /* the next look for BlueZ and an adapter */
    guint tries;
};

static void log_once(AirdropBle *b, const char *what, GError *e)
{
    if (b->logged) return;
    b->logged = TRUE;
    if (e) g_dbus_error_strip_remote_error(e);
    g_debug("AirDrop Bluetooth advertisement: %s%s%s", what, e ? ": " : "", e ? e->message : "");
}

static gboolean cancelled(GError *e)
{
    return g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static GDBusInterfaceInfo *advert_info(gboolean interval)
{
    static GDBusNodeInfo *nodes[2];
    if (!nodes[interval]) nodes[interval] = g_dbus_node_info_new_for_xml(interval ? ADVERT_XML : PLAIN_ADVERT_XML, NULL);
    return nodes[interval]->interfaces[0];
}

static GVariant *get_property(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                              const char *name, GError **error, gpointer data)
{
    AirdropBle *b = data;
    if (g_str_equal(name, "Type")) return g_variant_new_string("peripheral");
    if (g_str_equal(name, "MinInterval") || g_str_equal(name, "MaxInterval")) return g_variant_new_uint32(INTERVAL_MS);
    if (g_str_equal(name, "ManufacturerData")) {
        /* BLE-1: Apple's company ID holding the AirDrop TLV; BlueZ adds the
         * AD structure's header and the ID itself. */
        GVariantBuilder d;
        g_variant_builder_init(&d, G_VARIANT_TYPE("a{qv}"));
        g_variant_builder_add(&d, "{qv}", (guint16)APPLE_COMPANY_ID,
                              g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, b->data, sizeof b->data, 1));
        return g_variant_builder_end(&d);
    }
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "No property %s", name);
    return NULL;
}

/* BlueZ dropped the advertisement on its own, the adapter gone for one,
 * and needs no UnregisterAdvertisement for it. */
static void method_call(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                        const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    AirdropBle *b = data;
    if (g_str_equal(method, "Release")) {
        b->registering = FALSE;
        log_once(b, "released by BlueZ", NULL);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable VTABLE = { method_call, get_property, NULL, { 0 } };

static void unexport(AirdropBle *b)
{
    if (b->object) g_dbus_connection_unregister_object(b->bus, b->object);
    b->object = 0;
    g_clear_pointer(&b->path, g_free);
}

static gboolean export(AirdropBle *b)
{
    GError *e = NULL;
    b->path = g_strdup_printf("/com/nahuelgenchi/Lares/AirDrop/advert%u", ++b->serial);
    b->object = g_dbus_connection_register_object(b->bus, b->path, advert_info(b->interval), &VTABLE, b, NULL, &e);
    if (b->object) return TRUE;
    log_once(b, "can't export it", e);
    g_error_free(e);
    g_clear_pointer(&b->path, g_free);
    return FALSE;
}

static void do_register(AirdropBle *b);
static void start(AirdropBle *b);

static gboolean look_again(gpointer data)
{
    AirdropBle *b = data;
    b->retry = 0;
    if (b->active) start(b);
    return G_SOURCE_REMOVE;
}

/* BlueZ or an adapter isn't there yet: look again in a second, for a while. */
static void again(AirdropBle *b, const char *what, GError *e)
{
    if (b->active && b->tries++ < RETRIES) b->retry = g_timeout_add(RETRY_MS, look_again, b);
    else log_once(b, what, e);
}

static void registered(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (v) {
        g_variant_unref(v);
        return;
    }
    if (cancelled(e)) {
        g_error_free(e);
        return;
    }
    AirdropBle *b = data;
    b->registering = FALSE;
    unexport(b);
    if (b->interval) {
        g_debug("AirDrop Bluetooth advertisement: refused with an interval (%s), trying without", e->message);
        b->interval = FALSE;
        do_register(b);
    } else {
        log_once(b, "BlueZ refused it", e);
    }
    g_error_free(e);
}

static void do_register(AirdropBle *b)
{
    if (!export(b)) return;
    GVariant *params = g_variant_new("(o@a{sv})", b->path, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
    g_dbus_connection_call(b->bus, BLUEZ, b->adapter, MANAGER, "RegisterAdvertisement", params, NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, b->cancel, registered, b);
    b->registering = TRUE;
}

/* The first adapter, by path, that offers LE advertising. */
static void objects_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (!v && cancelled(e)) {
        g_error_free(e);
        return;
    }
    AirdropBle *b = data;
    if (!v) {
        again(b, "can't list BlueZ's adapters", e);
        g_error_free(e);
        return;
    }
    GVariantIter *objects;
    const char *path;
    GVariant *interfaces;
    char *best = NULL;
    g_variant_get(v, "(a{oa{sa{sv}}})", &objects);
    while (g_variant_iter_loop(objects, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        GVariant *manager = g_variant_lookup_value(interfaces, MANAGER, NULL);
        if (manager && (!best || strcmp(path, best) < 0)) {
            g_free(best);
            best = g_strdup(path);
        }
        if (manager) g_variant_unref(manager);
    }
    g_variant_iter_free(objects);
    g_variant_unref(v);
    if (!best) {
        again(b, "no Bluetooth adapter offers LE advertising", NULL);
        return;
    }
    g_free(b->adapter);
    b->adapter = best;
    do_register(b);
}

static void start(AirdropBle *b)
{
    g_dbus_connection_call(b->bus, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                           G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, -1, b->cancel, objects_ready, b);
}

static void bus_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *e = NULL;
    GDBusConnection *bus = g_bus_get_finish(res, &e);
    if (!bus && cancelled(e)) {
        g_error_free(e);
        return;
    }
    AirdropBle *b = data;
    if (!bus) {
        log_once(b, "no system bus", e);
        g_error_free(e);
        return;
    }
    if (b->bus) g_object_unref(bus);
    else b->bus = bus;
    start(b);
}

/* Waiting for the reply keeps the connection open until the call is out,
 * even if ours was its last reference. */
static void unregistered(GObject *src, GAsyncResult *res, gpointer data)
{
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (v) g_variant_unref(v);
}

/* Takes the advertisement down: BlueZ gets UnregisterAdvertisement after
 * any RegisterAdvertisement still on its way, as one sender's messages
 * arrive in order. */
static void stop(AirdropBle *b)
{
    g_clear_handle_id(&b->retry, g_source_remove);
    b->tries = 0;
    if (b->cancel) {
        g_cancellable_cancel(b->cancel);
        g_clear_object(&b->cancel);
    }
    if (b->registering && b->path)
        g_dbus_connection_call(b->bus, BLUEZ, b->adapter, MANAGER, "UnregisterAdvertisement",
                               g_variant_new("(o)", b->path), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, unregistered,
                               NULL);
    b->registering = FALSE;
    unexport(b);
}

AirdropBle *airdrop_ble_new(void)
{
    AirdropBle *b = g_new0(AirdropBle, 1);
    b->interval = TRUE;
    return b;
}

void airdrop_ble_set_active(AirdropBle *b, gboolean active)
{
    g_return_if_fail(b != NULL);
    active = !!active;
    if (active == b->active) return;
    b->active = active;
    if (!active) {
        stop(b);
        return;
    }
    /* BLE-5: a fresh random tag each time it starts. */
    guint32 r = g_random_int();
    guint8 tag[3] = { (guint8)r, (guint8)(r >> 8), (guint8)(r >> 16) };
    b->data[0] = AIRDROP_TYPE;
    b->data[1] = 18;
    ble_airdrop_payload(tag, b->data + 2);
    b->cancel = g_cancellable_new();
    if (b->bus) start(b);
    else g_bus_get(G_BUS_TYPE_SYSTEM, b->cancel, bus_ready, b);
}

/* Whether the advertisement is asked for; BlueZ may still be refusing it. */
gboolean airdrop_ble_is_active(AirdropBle *b)
{
    g_return_val_if_fail(b != NULL, FALSE);
    return b->active;
}

void airdrop_ble_free(AirdropBle *b)
{
    if (!b) return;
    stop(b);
    g_clear_object(&b->bus);
    g_free(b->adapter);
    g_free(b);
}
