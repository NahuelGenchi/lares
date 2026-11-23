// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <dwrite_3.h>
#include <windows.h>

namespace lares {

class FontManager {
public:
    FontManager() = default;
    ~FontManager();

    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    bool init(HINSTANCE hinst);
    void shutdown();

    IDWriteFactory* factory() const { return factory_; }
    IDWriteTextFormat* body() const { return fmt_body_; }     // 18 px
    IDWriteTextFormat* big() const { return fmt_big_; }       // 27 px
    IDWriteTextFormat* title() const { return fmt_title_; }   // 36 px
    IDWriteTextFormat* caption() const { return fmt_small_; } // 18 px too: muted text differs by colour only
    IDWriteTypography* no_ligatures() const { return typography_; }
    bool using_monocraft() const { return using_monocraft_; }

private:
    IDWriteFactory5* factory_ = nullptr;
    IDWriteInMemoryFontFileLoader* loader_ = nullptr;
    IDWriteFontCollection1* collection_ = nullptr;
    IDWriteTextFormat* fmt_body_ = nullptr;
    IDWriteTextFormat* fmt_big_ = nullptr;
    IDWriteTextFormat* fmt_title_ = nullptr;
    IDWriteTextFormat* fmt_small_ = nullptr;
    IDWriteTypography* typography_ = nullptr;
    bool loader_registered_ = false;
    bool using_monocraft_ = false;
};

} // namespace lares
