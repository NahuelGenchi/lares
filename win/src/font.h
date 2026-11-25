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
    /* Clean system sans-serif formats for the macOS-style AirDrop stage. */
    IDWriteTextFormat* ui_body() const { return fmt_ui_body_; }                 // 14 px Regular
    IDWriteTextFormat* ui_semibold() const { return fmt_ui_semibold_; }         // 13.5 px SemiBold
    IDWriteTextFormat* ui_sub() const { return fmt_ui_sub_; }                   // 12 px Regular
    IDWriteTextFormat* ui_sub_semibold() const { return fmt_ui_sub_semibold_; } // 12 px SemiBold
    IDWriteTextFormat* ui_initials() const { return fmt_ui_initials_; }         // 24 px SemiBold
    IDWriteTextFormat* ui_title() const { return fmt_ui_title_; }               // 18 px SemiBold
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
    IDWriteTextFormat* fmt_ui_body_ = nullptr;
    IDWriteTextFormat* fmt_ui_semibold_ = nullptr;
    IDWriteTextFormat* fmt_ui_sub_ = nullptr;
    IDWriteTextFormat* fmt_ui_sub_semibold_ = nullptr;
    IDWriteTextFormat* fmt_ui_initials_ = nullptr;
    IDWriteTextFormat* fmt_ui_title_ = nullptr;
    IDWriteTypography* typography_ = nullptr;
    bool loader_registered_ = false;
    bool using_monocraft_ = false;
};

} // namespace lares
