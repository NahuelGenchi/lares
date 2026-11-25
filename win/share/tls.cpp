// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* TLS for AirDrop on Schannel (issue #109). */
#include "tls.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include <windows.h>
#define SECURITY_WIN32
#include <ncrypt.h>
#include <security.h>
#include <sspi.h>
#include <wincrypt.h>
#define SCHANNEL_USE_BLACKLISTS
#include <subauth.h>
#include <schannel.h>

namespace lares::tls {

namespace {

constexpr wchar_t kKeyName[] = L"Lares AirDrop";
constexpr size_t kMaxRecord = 16384 + 2048;     /* one TLS record, with room */

} // namespace

// ------------------------------------------------------------------ identity

struct Identity {
    PCCERT_CONTEXT cert = nullptr;
    NCRYPT_KEY_HANDLE key = 0;
    NCRYPT_PROV_HANDLE prov = 0;
    ~Identity()
    {
        if (cert) CertFreeCertificateContext(cert);
        if (key) NCryptFreeObject(key);
        if (prov) NCryptFreeObject(prov);
    }
};

std::shared_ptr<Identity> identity(std::string& err)
{
    static std::mutex mu;
    static std::shared_ptr<Identity> made;
    std::lock_guard<std::mutex> lock(mu);
    if (made) return made;
    auto id = std::make_shared<Identity>();
    if (NCryptOpenStorageProvider(&id->prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) {
        err = "Windows' key store didn't open.";
        return nullptr;
    }
    /* A P-256 key, as owly's and Linux's, kept by Windows for this user. */
    if (NCryptOpenKey(id->prov, &id->key, kKeyName, 0, 0) != ERROR_SUCCESS) {
        id->key = 0;
        if (NCryptCreatePersistedKey(id->prov, &id->key, NCRYPT_ECDSA_P256_ALGORITHM, kKeyName, 0, 0) != ERROR_SUCCESS ||
            NCryptFinalizeKey(id->key, 0) != ERROR_SUCCESS) {
            err = "Couldn't make AirDrop's key.";
            return nullptr;
        }
    }
    BYTE name[128];
    DWORD name_len = sizeof name;
    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=Lares", CERT_X500_NAME_STR, nullptr, name, &name_len, nullptr)) {
        err = "Couldn't make AirDrop's certificate.";
        return nullptr;
    }
    CERT_NAME_BLOB subject{ name_len, name };
    CRYPT_KEY_PROV_INFO kpi{};
    kpi.pwszContainerName = const_cast<wchar_t*>(kKeyName);
    kpi.pwszProvName = const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER);
    CRYPT_ALGORITHM_IDENTIFIER alg{ const_cast<char*>(szOID_ECDSA_SHA256), {} };
    SYSTEMTIME start, end;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t{ { ft.dwLowDateTime, ft.dwHighDateTime } };
    t.QuadPart -= 86400ull * 10000000ull;   /* a day back, for clocks behind ours */
    ft = { t.LowPart, t.HighPart };
    FileTimeToSystemTime(&ft, &start);
    t.QuadPart += 3650ull * 86400ull * 10000000ull;
    ft = { t.LowPart, t.HighPart };
    FileTimeToSystemTime(&ft, &end);
    id->cert = CertCreateSelfSignCertificate(id->key, &subject, 0, &kpi, &alg, &start, &end, nullptr);
    if (!id->cert) {
        err = "Couldn't make AirDrop's certificate.";
        return nullptr;
    }
    made = id;
    return made;
}

// ------------------------------------------------------------------ the stream

struct Stream::Impl {
    Transport t;
    CredHandle cred{};
    CtxtHandle ctx{};
    bool have_cred = false, have_ctx = false, server = false, closed = false;
    std::shared_ptr<Identity> id;
    SecPkgContext_StreamSizes sizes{};
    std::vector<uint8_t> in;        /* ciphertext not yet decrypted */
    std::vector<uint8_t> plain;     /* decrypted, not yet read */
    size_t plain_at = 0;
    DWORD protocol = 0;

    ~Impl()
    {
        if (have_ctx) DeleteSecurityContext(&ctx);
        if (have_cred) FreeCredentialsHandle(&cred);
    }

    bool send_all(const void* p, size_t n) { return n == 0 || t.send(t.ctx, static_cast<const uint8_t*>(p), n) == 0; }

    /* More ciphertext; false at the end or on an error. */
    bool fill()
    {
        uint8_t buf[8192];
        int n = t.recv(t.ctx, buf, sizeof buf);
        if (n <= 0) return false;
        in.insert(in.end(), buf, buf + n);
        return in.size() <= 4 * kMaxRecord;
    }

    /* Non-blocking ciphertext read: 1 read, 0 EOF, -1 error, -2 would block. */
    int fill_nb()
    {
        if (!t.recv_nonblocking) return -2;
        uint8_t buf[8192];
        int n = t.recv_nonblocking(t.ctx, buf, sizeof buf);
        if (n == -2) return -2;
        if (n == 0) return 0;
        if (n < 0) return -1;
        in.insert(in.end(), buf, buf + n);
        return in.size() <= 4 * kMaxRecord ? 1 : -1;
    }

    SECURITY_STATUS step(SecBufferDesc* indesc, SecBufferDesc* outdesc, bool first)
    {
        ULONG attrs = 0;
        if (server) {
            ULONG req = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                        ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH;
            SECURITY_STATUS s = AcceptSecurityContext(&cred, first ? nullptr : &ctx, indesc, req, 0, &ctx, outdesc,
                                                      &attrs, nullptr);
            if (first && !FAILED(s)) have_ctx = true;
            return s;
        }
        ULONG req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                    ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                    ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_USE_SUPPLIED_CREDS;
        SECURITY_STATUS s = InitializeSecurityContextW(&cred, first ? nullptr : &ctx, nullptr, req, 0, 0, indesc, 0,
                                                       &ctx, outdesc, &attrs, nullptr);
        if (first && !FAILED(s)) have_ctx = true;
        return s;
    }

    /* The handshake, or the rest of one Schannel asks for after the
     * connection is up (TLS 1.3's post-handshake messages). */
    bool handshake(bool first, std::string& err)
    {
        bool started = !first;
        if (first && !server) {
            /* The client speaks first. */
            SecBuffer out{ 0, SECBUFFER_TOKEN, nullptr };
            SecBufferDesc outdesc{ SECBUFFER_VERSION, 1, &out };
            SECURITY_STATUS s = step(nullptr, &outdesc, true);
            started = true;
            if (out.pvBuffer) {
                bool ok = send_all(out.pvBuffer, out.cbBuffer);
                FreeContextBuffer(out.pvBuffer);
                if (!ok) return fail(err, "The connection closed during the TLS handshake.");
            }
            if (s != SEC_I_CONTINUE_NEEDED) return fail(err, "TLS didn't start.");
        }
        for (;;) {
            if (in.empty() && !fill()) return fail(err, "The connection closed during the TLS handshake.");
            SecBuffer inb[2] = { { static_cast<ULONG>(in.size()), SECBUFFER_TOKEN, in.data() },
                                 { 0, SECBUFFER_EMPTY, nullptr } };
            SecBufferDesc indesc{ SECBUFFER_VERSION, 2, inb };
            SecBuffer out[2] = { { 0, SECBUFFER_TOKEN, nullptr }, { 0, SECBUFFER_ALERT, nullptr } };
            SecBufferDesc outdesc{ SECBUFFER_VERSION, 2, out };
            SECURITY_STATUS s = step(&indesc, &outdesc, !started);
            started = true;
            if (s == SEC_E_INCOMPLETE_MESSAGE) {
                if (!fill()) return fail(err, "The connection closed during the TLS handshake.");
                continue;
            }
            bool sent = true;
            if (out[0].pvBuffer) {
                if (out[0].cbBuffer) sent = send_all(out[0].pvBuffer, out[0].cbBuffer);
                FreeContextBuffer(out[0].pvBuffer);
            }
            if (out[1].pvBuffer) FreeContextBuffer(out[1].pvBuffer);
            if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer) {
                std::vector<uint8_t> rest(in.end() - inb[1].cbBuffer, in.end());
                in.swap(rest);
            } else {
                in.clear();
            }
            if (!sent) return fail(err, "The connection closed during the TLS handshake.");
            if (s == SEC_E_OK) return true;
            if (s == SEC_I_INCOMPLETE_CREDENTIALS) {
                /* The receiver asked for our certificate; we go without,
                 * as Linux's sender does. */
                continue;
            }
            if (s != SEC_I_CONTINUE_NEEDED) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "The TLS handshake failed (0x%08lx).", static_cast<unsigned long>(s));
                return fail(err, buf);
            }
        }
    }

    static bool fail(std::string& err, const char* why)
    {
        err = why;
        return false;
    }

    bool start(std::string& err)
    {
        if (!handshake(true, err)) return false;
        if (QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK)
            return fail(err, "TLS didn't say its record sizes.");
        SecPkgContext_ConnectionInfo info{};
        if (QueryContextAttributesW(&ctx, SECPKG_ATTR_CONNECTION_INFO, &info) == SEC_E_OK) protocol = info.dwProtocol;
        return true;
    }

    /* Decrypts what's buffered into `plain`: 1 some, 0 needs more, -1 the
     * peer closed, -2 an error. */
    int decrypt()
    {
        if (in.empty()) return 0;
        SecBuffer b[4] = { { static_cast<ULONG>(in.size()), SECBUFFER_DATA, in.data() },
                           { 0, SECBUFFER_EMPTY, nullptr },
                           { 0, SECBUFFER_EMPTY, nullptr },
                           { 0, SECBUFFER_EMPTY, nullptr } };
        SecBufferDesc desc{ SECBUFFER_VERSION, 4, b };
        SECURITY_STATUS s = DecryptMessage(&ctx, &desc, 0, nullptr);
        if (s == SEC_E_INCOMPLETE_MESSAGE) return 0;
        if (s == SEC_I_CONTEXT_EXPIRED) return -1;
        if (s != SEC_E_OK && s != SEC_I_RENEGOTIATE) return -2;
        std::vector<uint8_t> rest;
        for (SecBuffer& x : b) {
            if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) {
                const uint8_t* p = static_cast<const uint8_t*>(x.pvBuffer);
                plain.insert(plain.end(), p, p + x.cbBuffer);
            } else if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer) {
                const uint8_t* p = static_cast<const uint8_t*>(x.pvBuffer);
                rest.assign(p, p + x.cbBuffer);
            }
        }
        in.swap(rest);
        if (s == SEC_I_RENEGOTIATE) {
            /* TLS 1.3 sends tickets and key updates after the handshake;
             * Schannel takes them through the handshake call. */
            std::string err;
            if (!handshake(false, err)) return -2;
        }
        return 1;
    }
};

Stream::Stream(Impl* impl) : impl_(impl) {}

Stream::~Stream()
{
    delete impl_;
}

namespace {

bool acquire(CredHandle& cred, bool server, PCCERT_CONTEXT cert)
{
    SCH_CREDENTIALS sc{};
    sc.dwVersion = SCH_CREDENTIALS_VERSION;
    if (server) {
        sc.cCreds = 1;
        sc.paCred = &cert;
        sc.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER;
    } else {
        sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_NO_SERVERNAME_CHECK;
    }
    TimeStamp expiry;
    return AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
                                     server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr,
                                     nullptr, &cred, &expiry) == SEC_E_OK;
}

} // namespace

std::unique_ptr<Stream> Stream::server(const Transport& t, const std::shared_ptr<Identity>& id, std::string& err)
{
    auto* impl = new Impl;
    std::unique_ptr<Stream> s(new Stream(impl));
    impl->t = t;
    impl->server = true;
    impl->id = id;
    if (!id || !acquire(impl->cred, true, id->cert)) {
        err = "Schannel refused AirDrop's certificate.";
        return nullptr;
    }
    impl->have_cred = true;
    if (!impl->start(err)) return nullptr;
    return s;
}

std::unique_ptr<Stream> Stream::client(const Transport& t, std::string& err)
{
    auto* impl = new Impl;
    std::unique_ptr<Stream> s(new Stream(impl));
    impl->t = t;
    if (!acquire(impl->cred, false, nullptr)) {
        err = "Schannel didn't start.";
        return nullptr;
    }
    impl->have_cred = true;
    if (!impl->start(err)) return nullptr;
    return s;
}

int Stream::read(uint8_t* buf, size_t len)
{
    Impl* d = impl_;
    while (d->plain_at >= d->plain.size()) {
        d->plain.clear();
        d->plain_at = 0;
        if (d->closed) return 0;
        int r = d->decrypt();
        if (r == 1) continue;
        if (r == -1) {
            d->closed = true;
            return 0;
        }
        if (r == -2) return -1;
        if (!d->fill()) return d->in.empty() ? 0 : -1;
    }
    size_t n = d->plain.size() - d->plain_at;
    if (n > len) n = len;
    std::memcpy(buf, d->plain.data() + d->plain_at, n);
    d->plain_at += n;
    return static_cast<int>(n);
}

int Stream::try_read(uint8_t* buf, size_t len)
{
    Impl* d = impl_;
    while (d->plain_at >= d->plain.size()) {
        d->plain.clear();
        d->plain_at = 0;
        if (d->closed) return 0;
        int r = d->decrypt();
        if (r == 1) continue;
        if (r == -1) {
            d->closed = true;
            return 0;
        }
        if (r == -2) return -1;
        int f = d->fill_nb();
        if (f == -2) return -2;
        if (f == 0) return d->in.empty() ? 0 : -1;
        if (f < 0) return -1;
    }
    size_t n = d->plain.size() - d->plain_at;
    if (n > len) n = len;
    std::memcpy(buf, d->plain.data() + d->plain_at, n);
    d->plain_at += n;
    return static_cast<int>(n);
}

bool Stream::write(const uint8_t* buf, size_t len)
{
    Impl* d = impl_;
    if (d->closed) return false;
    const SecPkgContext_StreamSizes& z = d->sizes;
    std::vector<uint8_t> rec(z.cbHeader + z.cbMaximumMessage + z.cbTrailer);
    while (len) {
        size_t n = len < z.cbMaximumMessage ? len : z.cbMaximumMessage;
        std::memcpy(rec.data() + z.cbHeader, buf, n);
        SecBuffer b[4] = { { z.cbHeader, SECBUFFER_STREAM_HEADER, rec.data() },
                           { static_cast<ULONG>(n), SECBUFFER_DATA, rec.data() + z.cbHeader },
                           { z.cbTrailer, SECBUFFER_STREAM_TRAILER, rec.data() + z.cbHeader + n },
                           { 0, SECBUFFER_EMPTY, nullptr } };
        SecBufferDesc desc{ SECBUFFER_VERSION, 4, b };
        if (EncryptMessage(&d->ctx, 0, &desc, 0) != SEC_E_OK) return false;
        if (!d->send_all(rec.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return false;
        buf += n;
        len -= n;
    }
    return true;
}

void Stream::close()
{
    Impl* d = impl_;
    if (d->closed || !d->have_ctx) return;
    d->closed = true;
    DWORD type = SCHANNEL_SHUTDOWN;
    SecBuffer tok{ sizeof type, SECBUFFER_TOKEN, &type };
    SecBufferDesc desc{ SECBUFFER_VERSION, 1, &tok };
    if (ApplyControlToken(&d->ctx, &desc) != SEC_E_OK) return;
    SecBuffer none{ 0, SECBUFFER_EMPTY, nullptr };
    SecBufferDesc indesc{ SECBUFFER_VERSION, 1, &none };
    SecBuffer out{ 0, SECBUFFER_TOKEN, nullptr };
    SecBufferDesc outdesc{ SECBUFFER_VERSION, 1, &out };
    d->step(d->server ? &indesc : nullptr, &outdesc, false);
    if (out.pvBuffer) {
        d->send_all(out.pvBuffer, out.cbBuffer);
        FreeContextBuffer(out.pvBuffer);
    }
}

const char* Stream::version() const
{
    DWORD p = impl_->protocol;
    if (p & (SP_PROT_TLS1_3_CLIENT | SP_PROT_TLS1_3_SERVER)) return "TLS 1.3";
    if (p & (SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_2_SERVER)) return "TLS 1.2";
    return "TLS";
}

} // namespace lares::tls
