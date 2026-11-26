// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Validation-record verification and parsing for AirDrop's Contacts Only mode
 * (SPEC §7, ID-1 to ID-3, issue #113): verifies a DER-encoded CMS SignedData
 * blob with Windows CryptoAPI, checks that its signer certificate chains
 * exclusively to Apple's embedded Root CAs (never trusting Windows' system
 * roots or accepting unsigned plists), and extracts ValidatedEmailHashes and
 * ValidatedPhoneHashes from the signed XML or binary plist payload. Hashes are
 * never logged (SEC-5). */
#include "formats.h"
#include "apple_ca.h"

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
constexpr DWORD kMaxSignersToCheck = 8;

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
        size_t a = 0, b = val.size();
        while (a < b && std::isspace(static_cast<unsigned char>(val[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(val[b - 1]))) --b;
        add_hash(out, val.substr(a, b - a));
        cur = s1 + 9;
    }
}

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
    std::string text(reinterpret_cast<const char*>(payload), len);
    if (text.find("<plist") != std::string::npos || text.find("ValidatedEmailHashes") != std::string::npos ||
        text.find("ValidatedPhoneHashes") != std::string::npos) {
        collect_xml_array(text, "ValidatedEmailHashes", out);
        collect_xml_array(text, "ValidatedPhoneHashes", out);
        return !out.empty();
    }
    return false;
}

#ifdef _WIN32

bool is_known_standard_critical_oid(const char* oid)
{
    static const char* const kKnown[] = {
        szOID_KEY_USAGE,
        szOID_BASIC_CONSTRAINTS,
        szOID_BASIC_CONSTRAINTS2,
        szOID_CERT_POLICIES,
        szOID_ENHANCED_KEY_USAGE,
        szOID_SUBJECT_ALT_NAME,
        szOID_SUBJECT_ALT_NAME2,
        szOID_NAME_CONSTRAINTS,
        szOID_POLICY_CONSTRAINTS,
        szOID_INHIBIT_ANY_POLICY,
        szOID_SUBJECT_KEY_IDENTIFIER,
        szOID_AUTHORITY_KEY_IDENTIFIER,
        szOID_AUTHORITY_KEY_IDENTIFIER2,
        szOID_CRL_DIST_POINTS,
    };
    if (!oid) return false;
    for (const char* k : kKnown) {
        if (std::strcmp(oid, k) == 0) return true;
    }
    return false;
}

bool is_apple_private_oid(const char* oid)
{
    static constexpr char kAppleArc[] = "1.2.840.113635.";
    return oid && std::strncmp(oid, kAppleArc, sizeof(kAppleArc) - 1) == 0;
}

/* Apple PKI leaf/intermediate certificates can mark proprietary marker OIDs
 * under 1.2.840.113635.* as critical. Allow ignoring unsupported critical
 * extensions only when every unrecognized critical extension in the chain is
 * under Apple's OID arc (never for arbitrary unknown critical extensions). */
bool chain_has_only_apple_unknown_critical_exts(PCCERT_SIMPLE_CHAIN simple)
{
    if (!simple) return false;
    bool saw_apple_critical = false;
    for (DWORD i = 0; i < simple->cElement; ++i) {
        PCCERT_CONTEXT c = simple->rgpElement[i]->pCertContext;
        if (!c || !c->pCertInfo) return false;
        for (DWORD e = 0; e < c->pCertInfo->cExtension; ++e) {
            const CERT_EXTENSION& ext = c->pCertInfo->rgExtension[e];
            if (!ext.fCritical) continue;
            if (is_known_standard_critical_oid(ext.pszObjId)) continue;
            if (is_apple_private_oid(ext.pszObjId)) {
                saw_apple_critical = true;
                continue;
            }
            return false;
        }
    }
    return saw_apple_critical;
}

/* Verifies `cert` against an exclusive trust-anchor store (`roots`) using an
 * isolated CryptoAPI chain engine: Windows' system Root and TrustedPeople
 * stores are excluded, AIA network fetches are disabled, and the chain must
 * have length >= 2 (a CA-issued certificate, never a bare self-signed cert)
 * terminating at a certificate present in `roots`. */
bool verify_cert_chain(PCCERT_CONTEXT cert, HCERTSTORE msg_store,
                       const DerCertSpan* roots, size_t root_count,
                       const DerCertSpan* intermediates, size_t inter_count)
{
    if (!cert || !roots || root_count == 0) return false;

    HCERTSTORE root_store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!root_store) return false;
    bool any_root = false;
    for (size_t i = 0; i < root_count; ++i) {
        if (roots[i].data && roots[i].len > 0 &&
            CertAddEncodedCertificateToStore(root_store, X509_ASN_ENCODING, roots[i].data,
                                             static_cast<DWORD>(roots[i].len), CERT_STORE_ADD_USE_EXISTING, nullptr)) {
            any_root = true;
        }
    }
    if (!any_root) {
        CertCloseStore(root_store, 0);
        return false;
    }

    HCERTSTORE inter_store = nullptr;
    if (intermediates && inter_count > 0) {
        inter_store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
        if (inter_store) {
            for (size_t i = 0; i < inter_count; ++i) {
                if (intermediates[i].data && intermediates[i].len > 0) {
                    CertAddEncodedCertificateToStore(inter_store, X509_ASN_ENCODING, intermediates[i].data,
                                                     static_cast<DWORD>(intermediates[i].len),
                                                     CERT_STORE_ADD_USE_EXISTING, nullptr);
                }
            }
        }
    }

    CERT_CHAIN_ENGINE_CONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hExclusiveRoot = root_store;
    if (inter_store) {
        cfg.cAdditionalStore = 1;
        cfg.rghAdditionalStore = &inter_store;
    }
    cfg.dwFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AIA;

    HCERTCHAINENGINE engine = nullptr;
    bool verified = false;
    if (CertCreateCertificateChainEngine(&cfg, &engine) && engine) {
        CERT_CHAIN_PARA para{};
        para.cbSize = sizeof(para);
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        DWORD flags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL |
                      CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE |
                      CERT_CHAIN_DISABLE_MY_PEER_TRUST |
                      CERT_CHAIN_DISABLE_AIA;
        if (CertGetCertificateChain(engine, cert, nullptr, msg_store, &para, flags, nullptr, &chain) && chain) {
            if (chain->cChain >= 1 && chain->rgpChain[0] && chain->rgpChain[0]->cElement >= 2) {
                PCCERT_SIMPLE_CHAIN simple = chain->rgpChain[0];
                PCCERT_CONTEXT top = simple->rgpElement[simple->cElement - 1]->pCertContext;
                PCCERT_CONTEXT anchor = top ? CertFindCertificateInStore(root_store, X509_ASN_ENCODING, 0,
                                                                         CERT_FIND_EXISTING, top, nullptr)
                                            : nullptr;
                if (anchor) {
                    CertFreeCertificateContext(anchor);
                    DWORD err = chain->TrustStatus.dwErrorStatus;
                    DWORD policy_flags = 0;
                    bool status_ok = (err == CERT_TRUST_NO_ERROR);
                    if (!status_ok && (err & ~static_cast<DWORD>(CERT_TRUST_HAS_NOT_SUPPORTED_CRITICAL_EXT |
                                                                 CERT_TRUST_INVALID_EXTENSION)) == 0 &&
                        chain_has_only_apple_unknown_critical_exts(simple)) {
                        policy_flags = CERT_CHAIN_POLICY_IGNORE_NOT_SUPPORTED_CRITICAL_EXT_FLAG;
                        status_ok = true;
                    }
                    if (status_ok) {
                        CERT_CHAIN_POLICY_PARA ppara{};
                        ppara.cbSize = sizeof(ppara);
                        ppara.dwFlags = policy_flags;
                        CERT_CHAIN_POLICY_STATUS pstatus{};
                        pstatus.cbSize = sizeof(pstatus);
                        BOOL pol_ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASE, chain, &ppara, &pstatus);
                        if (pol_ok && pstatus.dwError == static_cast<DWORD>(CERT_E_CRITICAL) && policy_flags == 0 &&
                            chain_has_only_apple_unknown_critical_exts(simple)) {
                            ppara.dwFlags = CERT_CHAIN_POLICY_IGNORE_NOT_SUPPORTED_CRITICAL_EXT_FLAG;
                            pstatus = {};
                            pstatus.cbSize = sizeof(pstatus);
                            pol_ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASE, chain, &ppara, &pstatus);
                        }
                        if (pol_ok && pstatus.dwError == 0) {
                            verified = true;
                        }
                    }
                }
            }
            CertFreeCertificateChain(chain);
        }
        CertFreeCertificateChainEngine(engine);
    }

    if (inter_store) CertCloseStore(inter_store, 0);
    CertCloseStore(root_store, 0);
    return verified;
}

/* Decodes a CMS SignedData message, requires at least one signer with a valid
 * cryptographic signature over the encapsulated content (CMSG_CTRL_VERIFY_SIGNATURE_EX),
 * and requires the signer certificate to chain cleanly to `roots`. */
bool extract_verified_cms_content(const uint8_t* data, size_t len,
                                  const DerCertSpan* roots, size_t root_count,
                                  const DerCertSpan* intermediates, size_t inter_count,
                                  Bytes& out)
{
    if (!data || len == 0 || len > kMaxRecordBytes || !roots || root_count == 0) return false;
    HCRYPTMSG msg = CryptMsgOpenToDecode(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, 0, 0, nullptr, nullptr);
    if (!msg) return false;

    bool ok = false;
    do {
        if (!CryptMsgUpdate(msg, data, static_cast<DWORD>(len), TRUE)) break;

        DWORD msg_type = 0;
        DWORD mt_size = sizeof(msg_type);
        if (!CryptMsgGetParam(msg, CMSG_TYPE_PARAM, 0, &msg_type, &mt_size) || msg_type != CMSG_SIGNED) break;

        DWORD content_len = 0;
        if (!CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, nullptr, &content_len) ||
            content_len == 0 || content_len > kMaxRecordBytes) {
            break;
        }
        Bytes payload(content_len);
        if (!CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, payload.data(), &content_len) || content_len == 0) break;
        payload.resize(content_len);

        DWORD signer_count = 0;
        DWORD sc_size = sizeof(signer_count);
        if (!CryptMsgGetParam(msg, CMSG_SIGNER_COUNT_PARAM, 0, &signer_count, &sc_size) || signer_count == 0) break;

        HCERTSTORE msg_store = CertOpenStore(CERT_STORE_PROV_MSG, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                             0, 0, msg);
        if (!msg_store) break;

        DWORD check_limit = std::min<DWORD>(signer_count, kMaxSignersToCheck);
        for (DWORD idx = 0; idx < check_limit && !ok; ++idx) {
            PCCERT_CONTEXT cert = nullptr;

            /* Prefer CMSG_SIGNER_CERT_ID_PARAM (handles both IssuerAndSerialNumber
             * and SubjectKeyIdentifier SignerIdentifiers per RFC 5652). */
            DWORD id_size = 0;
            if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_ID_PARAM, idx, nullptr, &id_size) && id_size > 0) {
                Bytes id_buf(id_size);
                if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_ID_PARAM, idx, id_buf.data(), &id_size)) {
                    cert = CertFindCertificateInStore(msg_store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                      0, CERT_FIND_CERT_ID, id_buf.data(), nullptr);
                }
            }
            if (!cert) {
                DWORD si_size = 0;
                if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, idx, nullptr, &si_size) && si_size > 0) {
                    Bytes si_buf(si_size);
                    if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, idx, si_buf.data(), &si_size)) {
                        cert = CertGetSubjectCertificateFromStore(
                            msg_store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                            reinterpret_cast<PCERT_INFO>(si_buf.data()));
                    }
                }
            }
            if (!cert) continue;

            CMSG_CTRL_VERIFY_SIGNATURE_EX_PARA vpara{};
            vpara.cbSize = sizeof(vpara);
            vpara.dwSignerIndex = idx;
            vpara.dwSignerType = CMSG_VERIFY_SIGNER_CERT;
            vpara.pvSigner = const_cast<PCERT_CONTEXT>(cert);

            bool sig_ok = CryptMsgControl(msg, 0, CMSG_CTRL_VERIFY_SIGNATURE_EX, &vpara) != FALSE;
            if (sig_ok && verify_cert_chain(cert, msg_store, roots, root_count, intermediates, inter_count)) {
                out = std::move(payload);
                ok = true;
            }
            CertFreeCertificateContext(cert);
        }

        CertCloseStore(msg_store, 0);
    } while (false);

    CryptMsgClose(msg);
    return ok;
}

#endif // _WIN32

} // namespace

bool airdrop_extract_record_hashes(const uint8_t* data, size_t len, std::vector<std::string>& out_hashes)
{
    out_hashes.clear();
#ifdef _WIN32
    Bytes payload;
    if (!extract_verified_cms_content(data, len, kAppleRootCerts, std::size(kAppleRootCerts),
                                      kAppleIntermediateCerts, std::size(kAppleIntermediateCerts), payload)) {
        return false;
    }
    return parse_payload_hashes(payload.data(), payload.size(), out_hashes);
#else
    (void)data;
    (void)len;
    return false;
#endif
}

bool airdrop_extract_record_hashes_with_roots(const uint8_t* data, size_t len,
                                              const std::vector<Bytes>& trusted_roots_der,
                                              std::vector<std::string>& out_hashes)
{
    out_hashes.clear();
#ifdef _WIN32
    if (trusted_roots_der.empty()) return false;
    std::vector<DerCertSpan> spans;
    spans.reserve(trusted_roots_der.size());
    for (const Bytes& r : trusted_roots_der) {
        if (!r.empty()) spans.push_back({ r.data(), r.size() });
    }
    if (spans.empty()) return false;
    Bytes payload;
    if (!extract_verified_cms_content(data, len, spans.data(), spans.size(), nullptr, 0, payload)) {
        return false;
    }
    return parse_payload_hashes(payload.data(), payload.size(), out_hashes);
#else
    (void)data;
    (void)len;
    (void)trusted_roots_der;
    return false;
#endif
}

bool airdrop_verify_apple_cert(const uint8_t* cert_der, size_t len)
{
#ifdef _WIN32
    if (!cert_der || len == 0 || len > kMaxRecordBytes) return false;
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, cert_der, static_cast<DWORD>(len));
    if (!cert) return false;
    bool ok = verify_cert_chain(cert, nullptr, kAppleRootCerts, std::size(kAppleRootCerts),
                                kAppleIntermediateCerts, std::size(kAppleIntermediateCerts));
    CertFreeCertificateContext(cert);
    return ok;
#else
    (void)cert_der;
    (void)len;
    return false;
#endif
}

} // namespace lares::airdrop
