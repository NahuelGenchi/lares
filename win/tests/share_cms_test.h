// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Ephemeral in-memory CNG/CryptoAPI certificate and CMS SignedData generator
 * for lares-share-unit tests (issue #113). Creates non-persisted keys so tests
 * never touch Windows' key store or certificate stores. */
#include "formats.h"

#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#ifdef CMSG_DATA
#undef CMSG_DATA
#endif
#include <wincrypt.h>

namespace lares::airdrop::test_cms {

struct EphemeralCert {
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    Bytes der;
    PCCERT_CONTEXT ctx = nullptr;
    bool rsa = false;

    ~EphemeralCert()
    {
        if (ctx) CertFreeCertificateContext(ctx);
        if (key) NCryptFreeObject(key);
        if (prov) NCryptFreeObject(prov);
    }
    EphemeralCert() = default;
    EphemeralCert(const EphemeralCert&) = delete;
    EphemeralCert& operator=(const EphemeralCert&) = delete;
};

struct CertSpec {
    const wchar_t* subject = L"CN=Lares Test";
    bool is_ca = false;
    bool rsa = false;               /* false = ECDSA P-256, true = RSA-2048 */
    int not_before_days = -1;       /* relative to now */
    int not_after_days = 7;         /* relative to now */
    const char* extra_critical_oid = nullptr;
};

inline FILETIME offset_filetime(int delta_days)
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{ { ft.dwLowDateTime, ft.dwHighDateTime } };
    int64_t ticks = static_cast<int64_t>(delta_days) * 86400ll * 10000000ll;
    u.QuadPart = static_cast<ULONGLONG>(static_cast<int64_t>(u.QuadPart) + ticks);
    return FILETIME{ u.LowPart, u.HighPart };
}

inline bool encode_obj(LPCSTR lpszStructType, const void* pvStructInfo, Bytes& out)
{
    DWORD n = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, lpszStructType, pvStructInfo, 0, nullptr, nullptr, &n) || n == 0)
        return false;
    out.resize(n);
    return CryptEncodeObjectEx(X509_ASN_ENCODING, lpszStructType, pvStructInfo, 0, nullptr, out.data(), &n) != FALSE;
}

inline bool make_cert(const CertSpec& spec, const EphemeralCert* issuer, EphemeralCert& out)
{
    if (NCryptOpenStorageProvider(&out.prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return false;
    LPCWSTR alg = spec.rsa ? NCRYPT_RSA_ALGORITHM : NCRYPT_ECDSA_P256_ALGORITHM;
    if (NCryptCreatePersistedKey(out.prov, &out.key, alg, nullptr, 0, 0) != ERROR_SUCCESS) return false;
    if (spec.rsa) {
        DWORD bits = 2048;
        if (NCryptSetProperty(out.key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof(bits), 0) !=
            ERROR_SUCCESS) {
            return false;
        }
    }
    if (NCryptFinalizeKey(out.key, 0) != ERROR_SUCCESS) return false;
    out.rsa = spec.rsa;

    DWORD pub_len = 0;
    if (!CryptExportPublicKeyInfo(out.key, 0, X509_ASN_ENCODING, nullptr, &pub_len) || pub_len == 0) return false;
    Bytes pub_buf(pub_len);
    if (!CryptExportPublicKeyInfo(out.key, 0, X509_ASN_ENCODING,
                                  reinterpret_cast<PCERT_PUBLIC_KEY_INFO>(pub_buf.data()), &pub_len)) {
        return false;
    }

    DWORD subj_len = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, spec.subject, CERT_X500_NAME_STR, nullptr, nullptr, &subj_len, nullptr) ||
        subj_len == 0) {
        return false;
    }
    Bytes subj_buf(subj_len);
    if (!CertStrToNameW(X509_ASN_ENCODING, spec.subject, CERT_X500_NAME_STR, nullptr, subj_buf.data(), &subj_len,
                        nullptr)) {
        return false;
    }

    uint8_t serial[8] = {};
    BCryptGenRandom(nullptr, serial, sizeof(serial), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    serial[7] |= 0x01; /* ensure positive non-zero little-endian integer for CryptoAPI */

    Bytes bc_der;
    CERT_BASIC_CONSTRAINTS2_INFO bc{};
    bc.fCA = spec.is_ca ? TRUE : FALSE;
    if (!encode_obj(X509_BASIC_CONSTRAINTS2, &bc, bc_der)) return false;

    Bytes ku_der;
    BYTE ku_byte = spec.is_ca ? static_cast<BYTE>(CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE)
                              : static_cast<BYTE>(CERT_DIGITAL_SIGNATURE_KEY_USAGE);
    CRYPT_BIT_BLOB ku_blob{ 1, &ku_byte, 0 };
    if (!encode_obj(X509_KEY_USAGE, &ku_blob, ku_der)) return false;

    static const uint8_t kDerNull[] = { 0x05, 0x00 };
    std::vector<CERT_EXTENSION> exts;
    exts.push_back({ const_cast<char*>(szOID_BASIC_CONSTRAINTS2), TRUE,
                     { static_cast<DWORD>(bc_der.size()), bc_der.data() } });
    exts.push_back({ const_cast<char*>(szOID_KEY_USAGE), TRUE,
                     { static_cast<DWORD>(ku_der.size()), ku_der.data() } });
    if (spec.extra_critical_oid) {
        exts.push_back({ const_cast<char*>(spec.extra_critical_oid), TRUE,
                         { static_cast<DWORD>(sizeof(kDerNull)), const_cast<BYTE*>(kDerNull) } });
    }

    const bool signer_rsa = issuer ? issuer->rsa : out.rsa;
    CRYPT_ALGORITHM_IDENTIFIER sig_alg{};
    sig_alg.pszObjId = const_cast<char*>(signer_rsa ? szOID_RSA_SHA256RSA : szOID_ECDSA_SHA256);

    CERT_INFO info{};
    info.dwVersion = CERT_V3;
    info.SerialNumber = { static_cast<DWORD>(sizeof(serial)), serial };
    info.SignatureAlgorithm = sig_alg;
    info.Issuer = issuer ? issuer->ctx->pCertInfo->Subject : CERT_NAME_BLOB{ subj_len, subj_buf.data() };
    info.Subject = { subj_len, subj_buf.data() };
    info.NotBefore = offset_filetime(spec.not_before_days);
    info.NotAfter = offset_filetime(spec.not_after_days);
    info.SubjectPublicKeyInfo = *reinterpret_cast<PCERT_PUBLIC_KEY_INFO>(pub_buf.data());
    info.cExtension = static_cast<DWORD>(exts.size());
    info.rgExtension = exts.data();

    NCRYPT_KEY_HANDLE signer_key = issuer ? issuer->key : out.key;
    DWORD der_len = 0;
    if (!CryptSignAndEncodeCertificate(signer_key, 0, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &sig_alg,
                                       nullptr, nullptr, &der_len) ||
        der_len == 0) {
        return false;
    }
    out.der.resize(der_len);
    if (!CryptSignAndEncodeCertificate(signer_key, 0, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &sig_alg,
                                       nullptr, out.der.data(), &der_len)) {
        return false;
    }
    out.der.resize(der_len);
    out.ctx = CertCreateCertificateContext(X509_ASN_ENCODING, out.der.data(), static_cast<DWORD>(out.der.size()));
    return out.ctx != nullptr;
}

inline bool sign_cms(const EphemeralCert& signer, const std::vector<const EphemeralCert*>& embedded_certs,
                     const uint8_t* payload, size_t payload_len, Bytes& out_cms)
{
    out_cms.clear();
    if (!signer.ctx || !signer.key || !payload || payload_len == 0) return false;

    CMSG_SIGNER_ENCODE_INFO sei{};
    sei.cbSize = sizeof(sei);
    sei.pCertInfo = signer.ctx->pCertInfo;
    sei.hNCryptKey = signer.key;
    sei.dwKeySpec = CERT_NCRYPT_KEY_SPEC;
    sei.HashAlgorithm.pszObjId = const_cast<char*>(szOID_NIST_sha256);

    std::vector<CERT_BLOB> cert_blobs;
    cert_blobs.reserve(embedded_certs.size());
    for (const EphemeralCert* c : embedded_certs) {
        if (c && !c->der.empty()) {
            cert_blobs.push_back({ static_cast<DWORD>(c->der.size()), const_cast<BYTE*>(c->der.data()) });
        }
    }

    CMSG_SIGNED_ENCODE_INFO signed_info{};
    signed_info.cbSize = sizeof(signed_info);
    signed_info.cSigners = 1;
    signed_info.rgSigners = &sei;
    signed_info.cCertEncoded = static_cast<DWORD>(cert_blobs.size());
    signed_info.rgCertEncoded = cert_blobs.empty() ? nullptr : cert_blobs.data();

    HCRYPTMSG msg = CryptMsgOpenToEncode(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CMSG_SIGNED, &signed_info,
                                         nullptr, nullptr);
    if (!msg) return false;

    bool ok = false;
    if (CryptMsgUpdate(msg, payload, static_cast<DWORD>(payload_len), TRUE)) {
        DWORD cms_len = 0;
        if (CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, nullptr, &cms_len) && cms_len > 0) {
            out_cms.resize(cms_len);
            if (CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, out_cms.data(), &cms_len)) {
                out_cms.resize(cms_len);
                ok = true;
            }
        }
    }
    CryptMsgClose(msg);
    if (!ok) out_cms.clear();
    return ok;
}

} // namespace lares::airdrop::test_cms
