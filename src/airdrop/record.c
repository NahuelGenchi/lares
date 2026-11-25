// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Validation-record parsing for AirDrop's Contacts Only mode (SPEC §7, ID-1
 * to ID-3): unwraps an Apple CMS SignedData blob and extracts
 * ValidatedEmailHashes and ValidatedPhoneHashes from either an XML plist or
 * a binary plist. Hashes are never logged (SEC-5). */
#include "formats.h"
#include <string.h>

#define MAX_RECORD_BYTES (256 * 1024)
#define MAX_HASHES       64

static gboolean is_hex64(const char *s, gsize len)
{
    if (len != 64) return FALSE;
    for (gsize i = 0; i < 64; i++)
        if (!g_ascii_isxdigit(s[i])) return FALSE;
    return TRUE;
}

static void add_hash(GPtrArray *out, const char *s, gsize len)
{
    if (out->len >= MAX_HASHES || !is_hex64(s, len)) return;
    char *lo = g_ascii_strdown(s, (gssize)len);
    for (guint i = 0; i < out->len; i++) {
        if (g_str_equal(out->pdata[i], lo)) {
            g_free(lo);
            return;
        }
    }
    g_ptr_array_add(out, lo);
}

static void add_bytes32(GPtrArray *out, const guint8 *b)
{
    static const char HEX[] = "0123456789abcdef";
    char s[64];
    for (int i = 0; i < 32; i++) {
        s[i * 2] = HEX[b[i] >> 4];
        s[i * 2 + 1] = HEX[b[i] & 0x0f];
    }
    add_hash(out, s, 64);
}

static void collect_bplist_array(GVariant *dict, const char *key, GPtrArray *out)
{
    GVariant *arr = g_variant_lookup_value(dict, key, G_VARIANT_TYPE("av"));
    if (!arr) return;
    gsize n = g_variant_n_children(arr);
    for (gsize i = 0; i < n && out->len < MAX_HASHES; i++) {
        GVariant *wrap = g_variant_get_child_value(arr, i);
        GVariant *item = g_variant_get_variant(wrap);
        if (g_variant_is_of_type(item, G_VARIANT_TYPE_STRING)) {
            gsize slen = 0;
            const char *s = g_variant_get_string(item, &slen);
            add_hash(out, s, slen);
        } else if (g_variant_is_of_type(item, G_VARIANT_TYPE_BYTESTRING)) {
            gsize blen = 0;
            const guint8 *b = g_variant_get_fixed_array(item, &blen, 1);
            if (blen == 32) add_bytes32(out, b);
        }
        g_variant_unref(item);
        g_variant_unref(wrap);
    }
    g_variant_unref(arr);
}

static void collect_xml_array(const char *xml, gsize len, const char *key, GPtrArray *out)
{
    char *needle = g_strdup_printf("<key>%s</key>", key);
    const char *end = xml + len;
    const char *p = g_strstr_len(xml, (gssize)len, needle);
    g_free(needle);
    if (!p) return;
    p += strlen("<key>") + strlen(key) + strlen("</key>");
    const char *arr = g_strstr_len(p, end - p, "<array>");
    const char *next_key = g_strstr_len(p, end - p, "<key>");
    if (!arr || (next_key && arr > next_key)) return;
    const char *arr_end = g_strstr_len(arr, end - arr, "</array>");
    if (!arr_end) return;
    const char *cur = arr + 7;
    while (cur < arr_end && out->len < MAX_HASHES) {
        const char *s0 = g_strstr_len(cur, arr_end - cur, "<string>");
        if (!s0) break;
        s0 += 8;
        const char *s1 = g_strstr_len(s0, arr_end - s0, "</string>");
        if (!s1) break;
        while (s0 < s1 && g_ascii_isspace(*s0)) s0++;
        const char *se = s1;
        while (se > s0 && g_ascii_isspace(se[-1])) se--;
        add_hash(out, s0, (gsize)(se - s0));
        cur = s1 + 9;
    }
}

static gboolean der_read_tlv(const guint8 *buf, gsize len, gsize *pos, guint8 *tag,
                             const guint8 **val, gsize *vlen)
{
    if (*pos >= len) return FALSE;
    *tag = buf[(*pos)++];
    if (*pos >= len) return FALSE;
    guint8 first = buf[(*pos)++];
    gsize n = 0;
    if (first < 0x80) {
        n = first;
    } else {
        gsize nb = first & 0x7f;
        if (nb == 0 || nb > 4 || *pos + nb > len) return FALSE;
        for (gsize i = 0; i < nb; i++) n = (n << 8) | buf[(*pos)++];
    }
    if (*pos + n > len) return FALSE;
    *val = buf + *pos;
    *vlen = n;
    *pos += n;
    return TRUE;
}

/* Walks DER CMS ContentInfo -> SignedData -> encapContentInfo -> [0] OCTET STRING. */
static gboolean extract_der_cms_content(const guint8 *data, gsize len, const guint8 **out, gsize *out_len)
{
    static const guint8 OID_SIGNED_DATA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02 };
    gsize pos = 0, vl = 0;
    guint8 tag = 0;
    const guint8 *v = NULL;
    if (!der_read_tlv(data, len, &pos, &tag, &v, &vl) || tag != 0x30) return FALSE;

    gsize p1 = 0, oid_len = 0, ctx0_len = 0;
    const guint8 *oid = NULL, *ctx0 = NULL;
    if (!der_read_tlv(v, vl, &p1, &tag, &oid, &oid_len) || tag != 0x06) return FALSE;
    if (oid_len != sizeof OID_SIGNED_DATA || memcmp(oid, OID_SIGNED_DATA, oid_len) != 0) return FALSE;
    if (!der_read_tlv(v, vl, &p1, &tag, &ctx0, &ctx0_len) || tag != 0xa0) return FALSE;

    gsize p2 = 0, sd_len = 0;
    const guint8 *sd = NULL;
    if (!der_read_tlv(ctx0, ctx0_len, &p2, &tag, &sd, &sd_len) || tag != 0x30) return FALSE;

    gsize p3 = 0, skip_len = 0, eci_len = 0;
    const guint8 *skip = NULL, *eci = NULL;
    if (!der_read_tlv(sd, sd_len, &p3, &tag, &skip, &skip_len) || tag != 0x02) return FALSE;
    if (!der_read_tlv(sd, sd_len, &p3, &tag, &skip, &skip_len) || tag != 0x31) return FALSE;
    if (!der_read_tlv(sd, sd_len, &p3, &tag, &eci, &eci_len) || tag != 0x30) return FALSE;

    gsize p4 = 0, ec0_len = 0;
    const guint8 *ec0 = NULL;
    if (!der_read_tlv(eci, eci_len, &p4, &tag, &skip, &skip_len) || tag != 0x06) return FALSE;
    if (!der_read_tlv(eci, eci_len, &p4, &tag, &ec0, &ec0_len) || tag != 0xa0) return FALSE;

    gsize p5 = 0;
    if (!der_read_tlv(ec0, ec0_len, &p5, &tag, out, out_len) || tag != 0x04) return FALSE;
    return TRUE;
}

static gboolean parse_payload(const guint8 *payload, gsize len, GPtrArray *out)
{
    if (!payload || !len || len > MAX_RECORD_BYTES) return FALSE;
    if (len >= 8 && memcmp(payload, "bplist00", 8) == 0) {
        GVariant *v = bplist_decode(payload, len, NULL);
        if (!v) return FALSE;
        if (g_variant_is_of_type(v, G_VARIANT_TYPE("a{sv}"))) {
            collect_bplist_array(v, "ValidatedEmailHashes", out);
            collect_bplist_array(v, "ValidatedPhoneHashes", out);
        }
        g_variant_unref(v);
        return out->len > 0;
    }
    const char *text = (const char *)payload;
    if (g_strstr_len(text, (gssize)len, "<plist") || g_strstr_len(text, (gssize)len, "ValidatedEmailHashes")
        || g_strstr_len(text, (gssize)len, "ValidatedPhoneHashes")) {
        collect_xml_array(text, len, "ValidatedEmailHashes", out);
        collect_xml_array(text, len, "ValidatedPhoneHashes", out);
        return out->len > 0;
    }
    return FALSE;
}

GPtrArray *airdrop_extract_record_hashes(const guint8 *data, gsize len)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    if (!data || !len || len > MAX_RECORD_BYTES) return out;
    const guint8 *payload = NULL;
    gsize plen = 0;
    if (extract_der_cms_content(data, len, &payload, &plen)) {
        parse_payload(payload, plen, out);
        return out;
    }
    parse_payload(data, len, out);
    return out;
}
