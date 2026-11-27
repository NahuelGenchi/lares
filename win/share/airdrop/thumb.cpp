// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "thumb.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <wincodec.h>

namespace lares::airdrop {

namespace {

enum class MediaKind { None, Image, Video };

MediaKind media_kind_of(const std::wstring& path)
{
    size_t dot = path.rfind(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return MediaKind::None;
    std::wstring ext = path.substr(dot);
    if (ext.size() > 8) return MediaKind::None;
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    static const wchar_t* const kImages[] = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".webp", L".tif", L".tiff", L".heic", L".heif", L".avif",
    };
    for (const wchar_t* e : kImages)
        if (ext == e) return MediaKind::Image;
    static const wchar_t* const kVideos[] = {
        L".mp4", L".mov", L".m4v", L".mkv", L".avi", L".webm", L".wmv",
    };
    for (const wchar_t* e : kVideos)
        if (ext == e) return MediaKind::Video;
    return MediaKind::None;
}

struct ComGuard {
    bool uninit = false;
    ComGuard()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (hr == S_OK || hr == S_FALSE) uninit = true;
    }
    ~ComGuard()
    {
        if (uninit) CoUninitialize();
    }
};

template <class T> struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    void reset()
    {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
    T** put()
    {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool load_wic_image(IWICImagingFactory* wic, const std::wstring& path, ComPtr<IWICBitmapSource>& out)
{
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              dec.put())) ||
        !dec)
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.put())) || !frame) return false;
    out.p = frame.p;
    frame.p = nullptr;
    return true;
}

bool load_shell_thumb(IWICImagingFactory* wic, const std::wstring& path, unsigned max_dim,
                      ComPtr<IWICBitmapSource>& out)
{
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(factory.put()))) || !factory)
        return false;
    SIZE want = { static_cast<LONG>(max_dim), static_cast<LONG>(max_dim) };
    HBITMAP hbmp = nullptr;
    if (FAILED(factory->GetImage(want, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &hbmp)) || !hbmp) return false;
    ComPtr<IWICBitmap> bmp;
    HRESULT hr = wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapIgnoreAlpha, bmp.put());
    DeleteObject(hbmp);
    if (FAILED(hr) || !bmp) return false;
    out.p = bmp.p;
    bmp.p = nullptr;
    return true;
}

bool encode_scaled(IWICImagingFactory* wic, IWICBitmapSource* src, unsigned max_dim, float jpeg_quality, bool as_png,
                   Bytes& out)
{
    UINT sw = 0, sh = 0;
    if (FAILED(src->GetSize(&sw, &sh)) || sw == 0 || sh == 0) return false;
    UINT dw = sw, dh = sh;
    if (sw > max_dim || sh > max_dim) {
        if (sw >= sh) {
            dw = max_dim;
            dh = std::max(1u, static_cast<UINT>((static_cast<uint64_t>(sh) * max_dim + sw / 2) / sw));
        } else {
            dh = max_dim;
            dw = std::max(1u, static_cast<UINT>((static_cast<uint64_t>(sw) * max_dim + sh / 2) / sh));
        }
    }

    IWICBitmapSource* cur = src;
    ComPtr<IWICBitmapScaler> scaler;
    if (dw != sw || dh != sh) {
        if (FAILED(wic->CreateBitmapScaler(scaler.put())) || !scaler) return false;
        if (FAILED(scaler->Initialize(src, dw, dh, WICBitmapInterpolationModeFant))) return false;
        cur = scaler.p;
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.put())) || !conv) return false;
    if (FAILED(conv->Initialize(cur, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut)))
        return false;

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) || !stream) return false;

    ComPtr<IWICBitmapEncoder> enc;
    const GUID& fmt = as_png ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg;
    if (FAILED(wic->CreateEncoder(fmt, nullptr, enc.put())) || !enc) return false;
    if (FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(frame.put(), props.put())) || !frame) return false;
    if (!as_png && props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_R4;
        var.fltVal = jpeg_quality;
        props->Write(1, &opt, &var);
    }
    if (FAILED(frame->Initialize(props.p))) return false;
    if (FAILED(frame->SetSize(dw, dh))) return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&pf))) return false;
    if (FAILED(frame->WriteSource(conv.p, nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(enc->Commit())) return false;

    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    if (st.cbSize.QuadPart == 0 || st.cbSize.QuadPart > 1024 * 1024) return false;
    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.p, &hg)) || !hg) return false;
    void* mem = GlobalLock(hg);
    if (!mem) return false;
    size_t len = static_cast<size_t>(st.cbSize.QuadPart);
    out.assign(static_cast<const uint8_t*>(mem), static_cast<const uint8_t*>(mem) + len);
    GlobalUnlock(hg);
    return !out.empty();
}

} // namespace

bool file_icon_supported(const std::wstring& path)
{
    return media_kind_of(path) != MediaKind::None;
}

bool make_file_icon(const std::wstring& path, Bytes& out, size_t max_bytes, unsigned max_dim)
{
    MediaKind kind = media_kind_of(path);
    if (kind == MediaKind::None || max_bytes < 256 || max_dim < 16) return false;

    ComGuard com;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        !wic)
        return false;

    ComPtr<IWICBitmapSource> src;
    if (kind == MediaKind::Image) {
        if (!load_wic_image(wic.p, path, src)) load_shell_thumb(wic.p, path, max_dim, src);
    } else {
        load_shell_thumb(wic.p, path, max_dim, src);
    }
    if (!src) return false;

    struct Pass {
        unsigned dim;
        float quality;
    };
    const Pass passes[] = {
        { max_dim, 0.78f },
        { std::min(max_dim, 224u), 0.58f },
        { std::min(max_dim, 144u), 0.45f },
    };
    Bytes encoded;
    for (const Pass& p : passes) {
        encoded.clear();
        if (encode_scaled(wic.p, src.p, p.dim, p.quality, false, encoded) && encoded.size() <= max_bytes) {
            out = std::move(encoded);
            return true;
        }
    }
    encoded.clear();
    if (encode_scaled(wic.p, src.p, std::min(max_dim, 160u), 0.0f, true, encoded) && encoded.size() <= max_bytes) {
        out = std::move(encoded);
        return true;
    }
    return false;
}

} // namespace lares::airdrop
