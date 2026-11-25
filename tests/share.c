// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Tests for sharing over AirDrop above the link: lares-awdl's events, peer
 * addresses, and whole transfers between Lares' own sender and receiver
 * over TLS on the loopback interface, through every format on the way
 * (plists, CPIO, gzip and DVZip, HTTP). They need the openssl command and a
 * TLS backend for GIO, and skip without them. `make check` runs them from
 * the repository's root. */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include "../src/airdrop/session.h"
#include "../src/share.h"

static void test_parse_event(void)
{
    GHashTable *t = share_parse_event("{\"event\":\"ready\",\"interface\":\"awdl0\",\"channel\":6,"
                                      "\"address\":\"fe80::1\"}");
    g_assert_nonnull(t);
    g_assert_cmpstr(g_hash_table_lookup(t, "event"), ==, "ready");
    g_assert_cmpstr(g_hash_table_lookup(t, "interface"), ==, "awdl0");
    g_assert_cmpstr(g_hash_table_lookup(t, "channel"), ==, "6");
    g_hash_table_unref(t);

    t = share_parse_event(" { \"event\" : \"peer\", \"name\":\"Ann\\u2019s \\\"Phone\\\"\" } ");
    g_assert_nonnull(t);
    g_assert_cmpstr(g_hash_table_lookup(t, "name"), ==, "Ann\u2019s \"Phone\"");
    g_hash_table_unref(t);

    t = share_parse_event("{}");
    g_assert_nonnull(t);
    g_assert_cmpuint(g_hash_table_size(t), ==, 0);
    g_hash_table_unref(t);

    const char *bad[] = { "", "event", "{\"a\":}", "{\"a\" \"b\"}", "{\"a\":\"b\"", "{\"a\":\"\\u12\"}", "[1]" };
    for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) g_assert_null(share_parse_event(bad[i]));
}

static void test_link_local(void)
{
    /* RFC 4291's modified EUI-64, on a documentation address. */
    char *a = share_link_local("02:00:5e:00:53:01", "awdl0");
    g_assert_cmpstr(a, ==, "fe80::0:5eff:fe00:5301%awdl0");
    GInetAddress *ia = g_inet_address_new_from_string("fe80::0:5eff:fe00:5301");
    g_assert_nonnull(ia);
    g_object_unref(ia);
    g_free(a);
    /* A universally administered address gets the bit set. */
    a = share_link_local("00:00:5e:00:53:02", "awdl0");
    g_assert_cmpstr(a, ==, "fe80::200:5eff:fe00:5302%awdl0");
    g_free(a);
    g_assert_null(share_link_local("not a mac", "awdl0"));
}

static void test_uti(void)
{
    g_assert_cmpstr(airdrop_uti("IMG_1.HEIC", FALSE), ==, "public.heic");
    g_assert_cmpstr(airdrop_uti("photo.JpG", FALSE), ==, "public.jpeg");
    g_assert_cmpstr(airdrop_uti("notes", FALSE), ==, "public.data");
    g_assert_cmpstr(airdrop_uti("archive.tar.zst", FALSE), ==, "public.data");
    g_assert_cmpstr(airdrop_uti("Trip", TRUE), ==, "public.folder");
}

static void test_device_and_contacts(void)
{
    g_assert_cmpstr(share_model_label("iPhone17,1", "Susuin"), ==, "iPhone");
    g_assert_cmpstr(share_model_label("MacBookAir10,1", "Alex"), ==, "MacBook Air");
    g_assert_cmpstr(share_model_label("MacBookPro18,3", "Work"), ==, "MacBook Pro");
    g_assert_cmpstr(share_model_label("iPad14,5", "Maya"), ==, "iPad");
    g_assert_cmpint(share_device_kind("iPhone17,1", "Susuin"), ==, DEVICE_KIND_PHONE);
    g_assert_cmpint(share_device_kind("MacBookAir10,1", "Alex"), ==, DEVICE_KIND_LAPTOP);
    g_assert_cmpint(share_device_kind("iPad14,5", "Maya"), ==, DEVICE_KIND_TABLET);

    char *init = share_initials("Susuin");
    g_assert_cmpstr(init, ==, "S");
    g_free(init);
    init = share_initials("Alex Rivera");
    g_assert_cmpstr(init, ==, "AR");
    g_free(init);

    char *eh = share_hash_email("  Alice@Example.COM ");
    char *want_eh = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "alice@example.com", -1);
    g_assert_cmpstr(eh, ==, want_eh);
    g_free(want_eh);
    g_free(eh);

    GPtrArray *ph = share_hash_phone_variants("+1 (555) 123-4567");
    g_assert_cmpuint(ph->len, ==, 2);
    g_ptr_array_unref(ph);

    /* iOS curly apostrophe (U+2019) and possessive owner-prefix matching. */
    ShareState st = { 0 };
    st.contacts = g_ptr_array_new();
    ShareContact c1 = { .name = "Alex", .device = "Alex's iPhone", .email = "alex@example.com" };
    ShareContact c2 = { .name = "Maya", .device = NULL, .email = NULL };
    g_ptr_array_add(st.contacts, &c1);
    g_ptr_array_add(st.contacts, &c2);
    const ShareContact *hit = share_find_contact(&st, "Alex\xE2\x80\x99s iPhone", NULL);
    g_assert_nonnull(hit);
    g_assert_cmpstr(hit->name, ==, "Alex");
    hit = share_find_contact(&st, "Maya\xE2\x80\x99s iPad", NULL);
    g_assert_nonnull(hit);
    g_assert_cmpstr(hit->name, ==, "Maya");
    hit = share_find_contact(&st, "Alex\xE2\x80\x99s MacBook Air", NULL);
    g_assert_nonnull(hit);
    g_assert_cmpstr(hit->name, ==, "Alex");
    g_assert_null(share_find_contact(&st, "Nobody\xE2\x80\x99s iPhone", NULL));
    g_ptr_array_unref(st.contacts);
}

/* ------------------------------------------------------------- loopback */

typedef struct {
    GMainLoop *loop;
    AirdropReceiver *r;
    gboolean accept;
    guint offers, received, withdrawn, failed;
    char *sender;
    GPtrArray *names, *saved;
    AirdropPeer *peer;
    gboolean sent, accepted;
    GError *error;
} Fx;

static char *tmp_root;

static void on_offer(const AirdropOffer *o, gpointer user)
{
    Fx *f = user;
    f->offers++;
    g_free(f->sender);
    f->sender = g_strdup(o->sender);
    if (f->names) g_ptr_array_unref(f->names);
    f->names = g_ptr_array_ref(o->names);
    airdrop_receiver_answer(f->r, o->id, f->accept);
}

static void on_withdrawn(guint id, gpointer user) { ((Fx *)user)->withdrawn++; }
static void on_progress(guint id, guint64 done, guint64 total, gpointer user) {}

static void on_received(guint id, gboolean links, GPtrArray *items, gpointer user)
{
    Fx *f = user;
    f->received++;
    if (f->saved) g_ptr_array_unref(f->saved);
    f->saved = g_ptr_array_ref(items);
}

static void on_failed(guint id, const char *message, gpointer user)
{
    ((Fx *)user)->failed++;
    g_test_message("receiver failed: %s", message);
}

/* The trace never names a file: the loopback's file is called Café. */
static guint traces;

static void on_trace(guint id, const char *line, gpointer user)
{
    traces++;
    g_test_message("receiver trace: %s", line);
    g_assert_null(strstr(line, "Caf"));
}

static const AirdropReceiverCallbacks CB = { on_offer, on_withdrawn, on_progress, on_received, on_failed, on_trace };

static void discovered(GObject *src, GAsyncResult *res, gpointer user)
{
    Fx *f = user;
    f->peer = airdrop_discover_finish(res, &f->error);
    g_main_loop_quit(f->loop);
}

static void send_progress(gboolean accepted, guint64 sent, guint64 total, gpointer user)
{
    if (accepted) ((Fx *)user)->accepted = TRUE;
}

static void sent(GObject *src, GAsyncResult *res, gpointer user)
{
    Fx *f = user;
    f->sent = airdrop_send_finish(res, &f->error);
    g_main_loop_quit(f->loop);
}

static gboolean too_long(gpointer user)
{
    g_error("a loopback transfer took more than 30 s");
    return G_SOURCE_REMOVE;
}

static void run(Fx *f)
{
    guint t = g_timeout_add_seconds(30, too_long, f);
    g_main_loop_run(f->loop);
    g_source_remove(t);
}

static gboolean send_and_wait(Fx *f, guint16 port, guint flags, char **paths)
{
    AirdropSendRequest req = { .address = "::1", .port = port, .flags = flags, .name = "Sender",
                               .id = "0123456789ab", .paths = paths };
    g_clear_error(&f->error);
    f->sent = f->accepted = FALSE;
    guint received = f->received, failed = f->failed;
    airdrop_send_async(&req, NULL, send_progress, f, sent, f);
    run(f);
    /* The receiver's note can come just after the sender's answer. */
    gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (f->sent && f->received == received && f->failed == failed && g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
    return f->sent;
}

static char *write_file(const char *dir, const char *name, const char *text)
{
    char *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    return path;
}

static void assert_contents(const char *path, const char *text)
{
    char *got = NULL;
    g_assert_true(g_file_get_contents(path, &got, NULL, NULL));
    g_assert_cmpstr(got, ==, text);
    g_free(got);
}

static void test_loopback(void)
{
    if (!g_tls_backend_supports_tls(g_tls_backend_get_default())) {
        g_test_skip("no TLS backend for GIO (glib-networking)");
        return;
    }
    char *openssl = g_find_program_in_path("openssl");
    if (!openssl) {
        g_test_skip("no openssl command to make a certificate");
        return;
    }
    g_free(openssl);
    GError *e = NULL;
    GTlsCertificate *cert = airdrop_identity(&e);
    g_assert_no_error(e);
    g_assert_nonnull(cert);

    char *inbox = g_build_filename(tmp_root, "inbox", NULL), *outbox = g_build_filename(tmp_root, "outbox", NULL);
    g_mkdir_with_parents(inbox, 0700);
    g_mkdir_with_parents(outbox, 0700);

    Fx f = { .loop = g_main_loop_new(NULL, FALSE), .accept = TRUE };
    f.r = airdrop_receiver_new("::1", "Test PC", inbox, cert, &CB, &f, &e);
    if (!f.r && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
        g_test_skip("no IPv6 loopback");
        g_error_free(e);
        return;
    }
    g_assert_no_error(e);
    guint16 port = airdrop_receiver_port(f.r);
    g_assert_cmpuint(port, >, 0);

    /* /Discover: the receiver lists us (DISC-1). */
    airdrop_discover_async("::1", port, NULL, discovered, &f);
    run(&f);
    g_assert_no_error(f.error);
    g_assert_nonnull(f.peer);
    g_assert_cmpstr(f.peer->name, ==, "Test PC");
    g_assert_cmpstr(f.peer->model, ==, "Linux");

    /* Contacts Only policy rejects unauthenticated /Discover (StatusCode 401). */
    const char *allowed[] = { "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", NULL };
    airdrop_receiver_set_policy(f.r, TRUE, allowed);
    AirdropPeer *prev_peer = f.peer;
    f.peer = NULL;
    airdrop_discover_async("::1", port, NULL, discovered, &f);
    run(&f);
    g_assert_null(f.peer);
    g_clear_error(&f.error);
    airdrop_receiver_set_policy(f.r, FALSE, NULL);
    f.peer = prev_peer;

    /* A file, in DVZip (flags unknown, UP-1b). */
    const char *text = "Hello from the loopback.\n";
    char *a = write_file(outbox, "Café ☕.txt", text);
    char *one[] = { a, NULL };
    g_assert_true(send_and_wait(&f, port, 0, one));
    g_assert_no_error(f.error);
    g_assert_true(f.accepted);
    g_assert_cmpstr(f.sender, ==, "Sender");
    g_assert_cmpuint(f.names->len, ==, 1);
    g_assert_cmpstr(f.names->pdata[0], ==, "Café ☕.txt");
    g_assert_cmpuint(f.received, ==, 1);
    g_assert_cmpuint(traces, >, 0); /* the upload's format and its entries */
    char *want = g_build_filename(inbox, "Café ☕.txt", NULL);
    g_assert_cmpstr(f.saved->pdata[0], ==, want);
    assert_contents(want, text);
    g_free(want);

    /* Again, in gzip: never over the first (SEC-2). */
    g_assert_true(send_and_wait(&f, port, AIRDROP_RECEIVER_FLAGS, one));
    want = g_build_filename(inbox, "Café ☕ (2).txt", NULL);
    g_assert_cmpstr(f.saved->pdata[0], ==, want);
    assert_contents(want, text);
    g_free(want);

    /* A folder with a file inside, and a large incompressible file beside
     * it: several DVZip pieces, some stored. */
    char *dir = g_build_filename(outbox, "Trip", NULL), *sub = g_build_filename(dir, "day 1", NULL);
    g_mkdir_with_parents(sub, 0700);
    char *inner = write_file(sub, "notes.txt", "sunny");
    gsize noise_len = 300000;
    guint8 *noise = g_malloc(noise_len);
    GRand *rand = g_rand_new_with_seed(7);
    for (gsize i = 0; i < noise_len; i++) noise[i] = g_rand_int_range(rand, 0, 256);
    g_rand_free(rand);
    char *big = g_build_filename(outbox, "big.bin", NULL);
    g_assert_true(g_file_set_contents(big, (const char *)noise, noise_len, NULL));
    char *two[] = { dir, big, NULL };
    g_assert_true(send_and_wait(&f, port, 0, two));
    g_assert_cmpuint(f.saved->len, ==, 2);
    char *got_inner = g_build_filename(inbox, "Trip", "day 1", "notes.txt", NULL);
    assert_contents(got_inner, "sunny");
    char *got_big = g_build_filename(inbox, "big.bin", NULL), *got = NULL;
    gsize got_len = 0;
    g_assert_true(g_file_get_contents(got_big, &got, &got_len, NULL));
    g_assert_cmpmem(got, got_len, noise, noise_len);
    g_free(got);
    g_free(noise);

    /* A decline: the sender is told, and nothing is written (ASK-1). */
    f.accept = FALSE;
    guint received = f.received;
    g_assert_false(send_and_wait(&f, port, 0, one));
    g_assert_error(f.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_cmpuint(f.received, ==, received);

    /* No staging folder is left behind. */
    GDir *d = g_dir_open(inbox, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(d))) g_assert_false(g_str_has_prefix(name, ".lares-airdrop-"));
    g_dir_close(d);

    airdrop_receiver_free(f.r);
    airdrop_peer_free(f.peer);
    g_clear_error(&f.error);
    g_ptr_array_unref(f.names);
    g_ptr_array_unref(f.saved);
    g_free(f.sender);
    g_main_loop_unref(f.loop);
    g_object_unref(cert);
    g_free(a);
    g_free(dir);
    g_free(sub);
    g_free(inner);
    g_free(big);
    g_free(got_inner);
    g_free(got_big);
    g_free(inbox);
    g_free(outbox);
}

static void remove_all(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            char *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK)) remove_all(child);
            else g_unlink(child);
            g_free(child);
        }
        g_dir_close(d);
    }
    g_rmdir(path);
}

int main(int argc, char **argv)
{
    /* The certificate goes into a throwaway data folder, not the user's. */
    tmp_root = g_dir_make_tmp("lares-share-test-XXXXXX", NULL);
    char *data = g_build_filename(tmp_root, "data", NULL);
    g_setenv("XDG_DATA_HOME", data, TRUE);
    g_free(data);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/share/parse-event", test_parse_event);
    g_test_add_func("/share/link-local", test_link_local);
    g_test_add_func("/share/uti", test_uti);
    g_test_add_func("/share/device-and-contacts", test_device_and_contacts);
    g_test_add_func("/share/loopback", test_loopback);
    int status = g_test_run();
    remove_all(tmp_root);
    g_free(tmp_root);
    return status;
}
