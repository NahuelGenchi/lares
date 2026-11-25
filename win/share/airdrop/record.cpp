// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Validation-record parsing for AirDrop's Contacts Only mode (SPEC §7, ID-1
 * to ID-3): unwraps an Apple CMS SignedData blob and reads its
 * ValidatedEmailHashes and ValidatedPhoneHashes from either an XML plist or
 * a binary plist. Hashes are never logged (SEC-5). */
#include "formats.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace lares::airdrop {

namespace {

constexpr size_t kMaxRecordBytes = 256 * 1024;
constexpr size_t kMaxHashes = 64;

bool is_hex64(const std::string& s)
{
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string lower_hex(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string bytes_to_hex(const Bytes& b)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (uint8_t byte : b) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

void add_hash(std::vector<std::string>& out, const std::string& h)
{
    if (out.size() >= kMaxHashes || !is_hex64(h)) return;
    std::string lo = lower_hex(h);
    if (std::find(out.begin(), out.end(), lo) == out.end()) out.push_back(std::move(lo));
}

/* Extracts hash strings (64 hex chars) or raw 32-byte digests from a decoded
 * binary plist dictionary. */
void collect_bplist_array(const Value& dict, const char* key, std::vector<std::string>& out)
{
    const Value* arr = dict.find(key, Value::Type::Array);
    if (!arr) return;
    for (size_t i = 0; i < arr->size() && out.size() < kMaxHashes; ++i) {
        const Value& item = arr->at(i);
        if (item.is(Value::Type::String)) {
            add_hash(out, item.get_string());
        } else if (item.is(Value::Type::Data) && item.get_data().size() == 32) {
            add_hash(out, bytes_to_hex(item.get_data()));
        }
    }
}

/* Lightweight XML plist extractor for <key>ValidatedEmailHashes</key> and
 * <key>ValidatedPhoneHashes</key> followed by <array><string>...</string></array>. */
void collect_xml_array(const std::string& xml, const char* key, std::vector<std::string>& out)
{
    std::string needle = std::string("<key>") + key + "</key>";
    size_t pos = xml.find(needle);
    if (pos == std::string::npos) return;
    pos += needle.size();
    size_t arr_start = xml.find("<array>", pos);
    size_t next_key = xml.find("<key>", pos);
    if (arr_start == std::string::npos || (next_key != std::string::npos && arr_start > next_key)) return;
    size_t arr_end = xml.find("</array>", arr_start);
    if (arr_end == std::string::npos) return;
    size_t cur = arr_start + 7;
    while (cur < arr_end && out.size() < kMaxHashes) {
        size_t s0 = xml.find("<string>", cur);
        if (s0 == std::string::npos || s0 >= arr_end) break;
        s0 += 8;
        size_t s1 = xml.find("</string>", s0);
        if (s1 == std::string::npos || s1 > arr_end) break;
        std::string val = xml.substr(s0, s1 - s0);
        // Trim whitespace around the hex digest
        size_t a = 0, b = val.size();
        while (a < b && std::isspace(static_cast<unsigned char>(val[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(val[b - 1]))) --b;
        add_hash(out, val.substr(a, b - a));
        cur = s1 + 9;
    }
}

/* Minimal DER TLV reader for extracting encapsulated OCTET STRING content
 * inside a CMS SignedData envelope when CryptoAPI isn't used or has no signer. */
bool der_read_tlv(const uint8_t* buf, size_t len, size_t& pos, uint8_t& tag, const uint8_t*& val, size_t& vlen)
{
    if (pos >= len) return false;
    tag = buf[pos++];
    if (pos >= len) return false;
    uint8_t first = buf[pos++];
    if (first < 0x80) {
        vlen = first;
    } else {
        size_t nbytes = first & 0x7f;
        if (nbytes == 0 || nbytes > 4 || pos + nbytes > len) return false;
        vlen = 0;
        for (size_t i = 0; i < nbytes; ++i) vlen = (vlen << 8) | buf[pos++];
    }
    if (pos + vlen > len) return false;
    val = buf + pos;
    pos += vlen;
    return true;
}

/* Walks CMS ContentInfo -> SignedData -> encapContentInfo -> [0] OCTET STRING. */
bool extract_der_cms_content(const uint8_t* data, size_t len, Bytes& out)
{
    static const uint8_t kOidSignedData[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02 };
    size_t pos = 0;
    uint8_t tag = 0;
    const uint8_t* v = nullptr;
    size_t vl = 0;
    if (!der_read_tlv(data, len, pos, tag, v, vl) || tag != 0x30) return false;

    size_t p1 = 0;
    const uint8_t* oid = nullptr;
    size_t oid_len = 0;
    if (!der_read_tlv(v, vl, p1, tag, oid, oid_len) || tag != 0x06) return false;
    if (oid_len != sizeof(kOidSignedData) || std::memcmp(oid, kOidSignedData, oid_len) != 0) return false;

    // [0] EXPLICIT SignedData
    const uint8_t* ctx0 = nullptr;
    size_t ctx0_len = 0;
    if (!der_read_tlv(v, vl, p1, tag, ctx0, ctx0_len) || tag != 0xa0) return false;

    size_t p2 = 0;
    const uint8_t* sd = nullptr;
    size_t sd_len = 0;
    if (!der_read_tlv(ctx0, ctx0_len, p2, tag, sd, sd_len) || tag != 0x30) return false;

    // SignedData ::= SEQUENCE { version, digestAlgorithms, encapContentInfo, ... }
    size_t p3 = 0;
    const uint8_t* skip = nullptr;
    size_t skip_len = 0;
    if (!der_read_tlv(sd, sd_len, p3, tag, skip, skip_len) || tag != 0x02) return false; // version
    if (!der_read_tlv(sd, sd_len, p3, tag, skip, skip_len) || tag != 0x31) return false; // digestAlgorithms
    const uint8_t* eci = nullptr;
    size_t eci_len = 0;
    if (!der_read_tlv(sd, sd_len, p3, tag, eci, eci_len) || tag != 0x30) return false;   // encapContentInfo

    // encapContentInfo ::= SEQUENCE { eContentType OID, [0] EXPLICIT OCTET STRING }
    size_t p4 = 0;
    if (!der_read_tlv(eci, eci_len, p4, tag, skip, skip_len) || tag != 0x06) return false;
    const uint8_t* ec0 = nullptr;
    size_t ec0_len = 0;
    if (!der_read_tlv(eci, eci_len, p4, tag, ec0, ec0_len) || tag != 0xa0) return false;

    size_t p5 = 0;
    const uint8_t* oct = nullptr;
    size_t oct_len = 0;
    if (!der_read_tlv(ec0, ec0_len, p5, tag, oct, oct_len) || tag != 0x04) return false;
    out.assign(oct, oct + oct_len);
    return true;
}

#ifdef _WIN32
/* Unwraps a CMS SignedData message with Windows CryptoAPI and verifies any
 * embedded signer's cryptographic signature over the content. Sets `sig_failed`
 * if a signer was present and its signature failed verification. */
bool extract_win_cms_content(const uint8_t* data, size_t len, Bytes& out, bool& sig_failed)
{
    sig_failed = false;
    HCRYPTMSG msg = CryptMsgOpenToDecode(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, 0, 0, nullptr, nullptr);
    if (!msg) return false;
    bool ok = false;
    if (CryptMsgUpdate(msg, data, static_cast<DWORD>(len), TRUE)) {
        DWORD content_len = 0;
        if (CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, nullptr, &content_len) && content_len > 0 &&
            content_len <= kMaxRecordBytes) {
            Bytes payload(content_len);
            if (CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, payload.data(), &content_len)) {
                payload.resize(content_len);
                DWORD signer_count = 0;
                DWORD sc_size = sizeof(signer_count);
                bool sig_ok = true;
                if (CryptMsgGetParam(msg, CMSG_SIGNER_COUNT_PARAM, 0, &signer_count, &sc_size) && signer_count > 0) {
                    DWORD cert_count = 0;
                    DWORD cc_size = sizeof(cert_count);
                    if (CryptMsgGetParam(msg, CMSG_CERT_COUNT_PARAM, 0, &cert_count, &cc_size) && cert_count > 0) {
                        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MSG, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                         0, 0, msg);
                        if (store) {
                            DWORD si_size = 0;
                            if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &si_size) && si_size > 0) {
                                Bytes si_buf(si_size);
                                if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, si_buf.data(), &si_size)) {
                                    PCCERT_CONTEXT cert = CertGetSubjectCertificateFromStore(
                                        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                        reinterpret_cast<PCERT_INFO>(si_buf.data()));
                                    if (cert) {
                                        sig_ok = CryptMsgControl(msg, 0, CMSG_CTRL_VERIFY_SIGNATURE, cert->pCertInfo) != FALSE;
                                        CertFreeCertificateContext(cert);
                                    }
                                }
                            }
                            CertCloseStore(store, 0);
                        }
                    }
                }
                if (sig_ok) {
                    out = std::move(payload);
                    ok = true;
                } else {
                    sig_failed = true;
                }
            }
        }
    }
    CryptMsgClose(msg);
    return ok;
}
#endif

bool parse_payload_hashes(const uint8_t* payload, size_t len, std::vector<std::string>& out)
{
    if (!payload || len == 0 || len > kMaxRecordBytes) return false;
    if (len >= 8 && std::memcmp(payload, "bplist00", 8) == 0) {
        Value v;
        if (!bplist_decode(payload, len, v) || !v.is(Value::Type::Dict)) return false;
        collect_bplist_array(v, "ValidatedEmailHashes", out);
        collect_bplist_array(v, "ValidatedPhoneHashes", out);
        return !out.empty();
    }
    // Check for XML plist
    std::string text(reinterpret_cast<const char*>(payload), len);
    if (text.find("<plist") != std::string::npos || text.find("ValidatedEmailHashes") != std::string::npos ||
        text.find("ValidatedPhoneHashes") != std::string::npos) {
        collect_xml_array(text, "ValidatedEmailHashes", out);
        collect_xml_array(text, "ValidatedPhoneHashes", out);
        return !out.empty();
    }
    return false;
}

} // namespace

bool airdrop_extract_record_hashes(const uint8_t* data, size_t len, std::vector<std::string>& out_hashes)
{
    out_hashes.clear();
    if (!data || len == 0 || len > kMaxRecordBytes) return false;

    // 1. Try CMS SignedData unwrap first (what Apple sends in SenderRecordData).
    Bytes payload;
#ifdef _WIN32
    bool sig_failed = false;
    if (extract_win_cms_content(data, len, payload, sig_failed)) {
        return parse_payload_hashes(payload.data(), payload.size(), out_hashes);
    }
    if (sig_failed) return false;
#endif
    if (extract_der_cms_content(data, len, payload)) {
        return parse_payload_hashes(payload.data(), payload.size(), out_hashes);
    }
    // 2. Fallback: raw inner plist (for direct/synthetic validation records).
    return parse_payload_hashes(data, len, out_hashes);
}

} // namespace lares::airdrop
