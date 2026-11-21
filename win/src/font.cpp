// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "font.h"
#include "resource.h"

namespace lares {

template <typename T>
static void safe_release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

FontManager::~FontManager()
{
    shutdown();
}

void FontManager::shutdown()
{
    safe_release(typography_);
    safe_release(fmt_small_);
    safe_release(fmt_title_);
    safe_release(fmt_big_);
    safe_release(fmt_body_);
    safe_release(collection_);
    if (factory_ && loader_ && loader_registered_) {
        factory_->UnregisterFontFileLoader(loader_);
        loader_registered_ = false;
    }
    safe_release(loader_);
    safe_release(factory_);
}

bool FontManager::init(HINSTANCE hinst)
{
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory5),
        reinterpret_cast<IUnknown**>(&factory_));
    if (FAILED(hr) || !factory_) return false;

    const wchar_t* family = L"Consolas";
    IDWriteFontCollection* coll_for_format = nullptr;

    HRSRC res = FindResourceW(hinst, MAKEINTRESOURCEW(IDR_FONT_MONOCRAFT), RT_RCDATA);
    if (res) {
        DWORD sz = SizeofResource(hinst, res);
        HGLOBAL hg = LoadResource(hinst, res);
        void* data = hg ? LockResource(hg) : nullptr;
        if (data && sz > 0) {
            if (SUCCEEDED(factory_->CreateInMemoryFontFileLoader(&loader_)) && loader_) {
                if (SUCCEEDED(factory_->RegisterFontFileLoader(loader_))) {
                    loader_registered_ = true;
                    IDWriteFontFile* font_file = nullptr;
                    if (SUCCEEDED(loader_->CreateInMemoryFontFileReference(factory_, data, sz, nullptr, &font_file)) && font_file) {
                        IDWriteFontSetBuilder1* builder = nullptr;
                        if (SUCCEEDED(factory_->CreateFontSetBuilder(&builder)) && builder) {
                            if (SUCCEEDED(builder->AddFontFile(font_file))) {
                                IDWriteFontSet* set = nullptr;
                                if (SUCCEEDED(builder->CreateFontSet(&set)) && set) {
                                    if (SUCCEEDED(factory_->CreateFontCollectionFromFontSet(set, &collection_)) && collection_) {
                                        UINT32 idx = 0;
                                        BOOL exists = FALSE;
                                        if (SUCCEEDED(collection_->FindFamilyName(L"Monocraft", &idx, &exists)) && exists) {
                                            family = L"Monocraft";
                                            coll_for_format = collection_;
                                            using_monocraft_ = true;
                                        }
                                    }
                                    set->Release();
                                }
                            }
                            builder->Release();
                        }
                        font_file->Release();
                    }
                }
            }
        }
    }

    auto make_fmt = [&](float px, IDWriteTextFormat** out) -> bool {
        HRESULT r = factory_->CreateTextFormat(
            family,
            coll_for_format,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            px,
            L"en-US",
            out);
        if (SUCCEEDED(r) && *out) {
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            return true;
        }
        return false;
    };

    if (!make_fmt(18.0f, &fmt_body_)) return false;
    if (!make_fmt(27.0f, &fmt_big_)) return false;
    if (!make_fmt(36.0f, &fmt_title_)) return false;
    if (!make_fmt(14.0f, &fmt_small_)) return false;

    if (SUCCEEDED(factory_->CreateTypography(&typography_)) && typography_) {
        DWRITE_FONT_FEATURE f_liga = { DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES, 0 };
        DWRITE_FONT_FEATURE f_calt = { DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_ALTERNATES, 0 };
        typography_->AddFontFeature(f_liga);
        typography_->AddFontFeature(f_calt);
    }
    return true;
}

} // namespace lares
