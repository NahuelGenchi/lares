// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"
#include "dwrite_c.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <wctype.h>

/*
 * The text and Markdown editor in lares-view.exe (issue #151).
 *
 * - A light and a dark palette, following Windows' mode.
 * - A centred 780 px document column, a 38 px title bar of its own, an
 *   outline of the headings at the left (Ctrl+J) that follows the reading
 *   position, and a 26 px status bar with words, characters and lines.
 * - Text through the shell's drawing layer (DirectWrite, ClearType blended
 *   in draw.c) and GDI+ shapes: no browser engine, no Direct3D, no .NET.
 * - Markdown shown formatted: headings H1 to H6 (1.875em down to 1em, H1
 *   and H2 underlined), bold, italic, strikethrough, inline code, fenced code
 *   blocks with their language and a Copy button, quotes and GitHub-style
 *   alerts, nested lists, task boxes that can be ticked, rules, tables and
 *   links.
 * - Source mode (Ctrl+E), editing and saving (Ctrl+S) with the file's own
 *   encoding (UTF-8 with or without BOM, UTF-16 LE or BE, ANSI) and line
 *   endings, find (Ctrl+F) in both modes, zoom (Ctrl+- / Ctrl++ / Ctrl+0),
 *   word wrap (Alt+Z), plain monospace editing for code, settings and logs,
 *   and files over 2 MB opened read-only with their first 512 KB.
 * - Every buffer of the document, its layout and find is freed when the
 *   window closes.
 */

#define TEXTVIEW_CLASS      L"LaresTextViewWnd"
#define ID_TV_EDIT          7001
#define ID_TV_FIND          7002

#define TV_MAX_EDIT_BYTES   (2U * 1024U * 1024U)   /* 2 MB safe editable limit */
#define TV_PREVIEW_BYTES    (512U * 1024U)         /* 512 KB read-only preview for giant files */

#define TV_TITLE_H          38
#define TV_STATUS_H         26
#define TV_SIDEBAR_W        232
#define TV_DOC_MAX_W        780
#define TV_FIND_BAR_H       38
#define TV_BANNER_H         32
#define TV_SCROLLBAR_W      10

/* Shared IID_IDWriteFactory = {b859ee5a-d838-4b5b-a2e8-1adc7d93db48} */
static const GUID TV_IID_IDWriteFactory =
    { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

typedef enum {
    TV_ENC_UTF8 = 0,
    TV_ENC_UTF8_BOM,
    TV_ENC_UTF16LE,
    TV_ENC_UTF16BE,
    TV_ENC_ANSI
} TvEncoding;

/* Light/Dark palette */
typedef struct {
    bool     is_dark;
    COLORREF bg_ref;
    COLORREF text_ref;
    COLORREF input_ref;
    ARGB     bg;
    ARGB     sidebar_bg;
    ARGB     sidebar_border;
    ARGB     sidebar_title;
    ARGB     sidebar_text;
    ARGB     sidebar_muted;
    ARGB     sidebar_hover;
    ARGB     sidebar_active_bg;
    ARGB     text;
    ARGB     heading;
    ARGB     heading6;
    ARGB     muted;
    ARGB     subtle;
    ARGB     divider;
    ARGB     surface_alt;
    ARGB     accent;
    ARGB     accent_hover;
    ARGB     accent_soft;
    ARGB     fg_on_accent;
    ARGB     link;
    ARGB     strong;
    ARGB     em;
    ARGB     strike;
    ARGB     list_marker;
    ARGB     quote_bar;
    ARGB     quote_text;
    ARGB     inline_code_bg;
    ARGB     inline_code_fg;
    ARGB     code_block_bg;
    ARGB     code_block_border;
    ARGB     code_block_fg;
    ARGB     table_border;
    ARGB     table_header_bg;
    ARGB     table_alt_bg;
    ARGB     hr;
    ARGB     input_bg;
    ARGB     hover_bg;
    ARGB     find_hl;
    ARGB     find_cur;
    ARGB     scroll_thumb;
    ARGB     scroll_thumb_hov;
    ARGB     syn_kw;
    ARGB     syn_str;
    ARGB     syn_com;
    ARGB     syn_num;
} MtPalette;

/* =========================================================================
 * Markdown AST & Flat Arena Layout Structures (Zero per-node malloc spam)
 * ========================================================================= */
typedef enum {
    MDB_PARAGRAPH = 0,
    MDB_HEADING,
    MDB_CODE_BLOCK,
    MDB_BLOCKQUOTE,
    MDB_LIST_ITEM,
    MDB_HR,
    MDB_TABLE
} MdBlockType;

typedef enum {
    MD_ALERT_NONE = 0,
    MD_ALERT_NOTE,
    MD_ALERT_TIP,
    MD_ALERT_IMPORTANT,
    MD_ALERT_WARNING,
    MD_ALERT_CAUTION
} MdAlertKind;

#define MD_SPAN_BOLD    0x01u
#define MD_SPAN_ITALIC  0x02u
#define MD_SPAN_STRIKE  0x04u
#define MD_SPAN_CODE    0x08u
#define MD_SPAN_LINK    0x10u
#define MD_SPAN_BR      0x20u
#define MD_SPAN_IMAGE   0x40u

typedef struct {
    uint32_t text_off;   /* offset into g_doc_text */
    uint32_t text_len;
    uint32_t url_off;    /* offset into g_doc_text when LINK/IMAGE */
    uint32_t url_len;
    uint8_t  flags;
} MdSpan;

typedef struct {
    int32_t  row;
    int32_t  col;
    int8_t   align;      /* 0=left, 1=center, 2=right */
    uint32_t first_span;
    uint32_t span_count;
    uint32_t first_line; /* populated during layout */
    uint32_t line_count;
    float    box_x;
    float    box_w;
} MdTableCell;

typedef struct {
    MdBlockType type;
    int32_t     src_line;       /* 1-based line in source */
    uint32_t    src_off;        /* char offset in g_doc_text */
    uint32_t    first_span;
    uint32_t    span_count;
    int8_t      level;          /* heading 1..6, quote depth 1..3, list depth 0..4 */
    bool        is_ordered;     /* ordered vs bullet list */
    int32_t     ordinal;        /* ordered list number */
    bool        is_task;        /* task checkbox item */
    bool        task_checked;
    uint32_t    task_char_off;  /* offset of ' ' / 'x' in g_doc_text */
    MdAlertKind alert_kind;     /* GitHub alert blockquote */
    wchar_t     lang[24];       /* fenced code block language */
    uint32_t    code_off;       /* raw code text slice in g_doc_text */
    uint32_t    code_len;
    uint32_t    first_cell;     /* table cells */
    int32_t     table_rows;
    int32_t     table_cols;

    /* Layout results (document coordinates relative to doc_y=0) */
    float       y;
    float       h;
    float       divider_y;      /* >0 for H1/H2 bottom divider */
    float       cb_x, cb_y;     /* task checkbox pos in doc coords */
    float       copy_x, copy_y; /* code block copy btn pos in doc coords */
    uint32_t    first_line;
    uint32_t    line_count;
} MdBlock;

typedef struct {
    int32_t  block_idx;
    float    y;
    float    line_h;
    float    font_px;
    int32_t  cell_idx;   /* >=0 for table cell line, else -1 */
    uint32_t first_frag;
    uint32_t frag_count;
    bool     is_code_line;
} MdLine;

typedef struct {
    uint32_t text_off;
    uint32_t text_len;
    uint32_t url_off;
    uint32_t url_len;
    float    x;
    float    w;
    uint8_t  flags;
} MdFrag;

typedef struct {
    int32_t  block_idx;
    int32_t  src_line;
    int8_t   level;
    float    doc_y;
    wchar_t  title[128];
} MdTocItem;

typedef struct {
    int32_t  line_idx;
    float    x;
    float    y;
    float    w;
    float    h;
} MdFindMatch;

/* Growable VirtualAlloc-backed arena helper */
typedef struct {
    void  *ptr;
    size_t count;
    size_t cap;
    size_t elem_sz;
} TvArena;

static void arena_free(TvArena *a) {
    if (a && a->ptr) {
        VirtualFree(a->ptr, 0, MEM_RELEASE);
        a->ptr = NULL;
    }
    if (a) { a->count = 0; a->cap = 0; }
}

static void *arena_push_n(TvArena *a, size_t n) {
    if (!a || n == 0) return NULL;
    if (a->count + n > a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 128;
        while (ncap < a->count + n) ncap *= 2;
        size_t bytes = ncap * a->elem_sz;
        void *np = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!np) return NULL;
        if (a->ptr && a->count > 0) {
            memcpy(np, a->ptr, a->count * a->elem_sz);
        }
        if (a->ptr) VirtualFree(a->ptr, 0, MEM_RELEASE);
        a->ptr = np;
        a->cap = ncap;
    }
    BYTE *res = (BYTE *)a->ptr + a->count * a->elem_sz;
    ZeroMemory(res, n * a->elem_sz);
    a->count += n;
    return res;
}

static void *arena_push(TvArena *a) {
    return arena_push_n(a, 1);
}

/* =========================================================================
 * Module Global State
 * ========================================================================= */
static HWND     g_hTvWnd = NULL;
static HWND     g_hEdit = NULL;
static HWND     g_hFindEdit = NULL;
static WNDPROC  g_orig_edit_proc = NULL;
static WNDPROC  g_orig_find_proc = NULL;
static HFONT    g_hMonoFont = NULL;
static HFONT    g_hUiFont = NULL;
static HBRUSH   g_hEditBrush = NULL;
static HBRUSH   g_hFindBrush = NULL;
static bool     g_destroying = false;
static bool     g_loading = false;

/* Shared CPU-only DirectWrite factory & tiny measurement cache */
#define TV_DW_CACHE_MAX 24
typedef struct {
    bool               is_mono;
    int                size_tenths;
    int                style;
    IDWriteTextFormat *fmt;
    float              ascii_adv[128];
    bool               has_ascii;
} TvDwSlot;

static IDWriteFactory *g_tv_dw = NULL;
static TvDwSlot        g_dw_slots[TV_DW_CACHE_MAX];
static int             g_dw_slot_count = 0;
static wchar_t         g_body_face[64] = L"Segoe UI";
static wchar_t         g_mono_face[64] = L"Consolas";

/* Document state */
static wchar_t    g_cur_path[MAX_PATH] = {0};
static wchar_t    g_cur_dir[MAX_PATH]  = {0};
static wchar_t    g_cur_name[MAX_PATH] = {0};
static uint64_t   g_cur_file_size = 0;
static TvEncoding g_enc = TV_ENC_UTF8;
static bool       g_use_crlf = true;
static bool       g_modified = false;
static bool       g_read_only = false;
static bool       g_is_truncated = false;
static bool       g_is_binary_guard = false;
static bool       g_is_markdown = false;
static bool       g_source_mode = false;
static bool       g_sidebar_open = true;
static bool       g_word_wrap = true;
static int        g_body_px = 16;          /* default: 16px body */
static int        g_code_edit_px = 14;     /* default: 14px code */
static int        g_theme_override = 0;    /* 0=auto, 1=light, 2=dark */

/* Master UTF-16 document buffer (CRLF-normalized, VirtualAlloc) */
static wchar_t   *g_doc_text = NULL;
static size_t     g_doc_len = 0;
static size_t     g_doc_cap = 0;
static int        g_stat_words = 0;
static int        g_stat_chars = 0;
static int        g_stat_lines = 1;

/* Parsed & laid-out Markdown arenas */
static TvArena    g_blocks = { NULL, 0, 0, sizeof(MdBlock) };
static TvArena    g_spans  = { NULL, 0, 0, sizeof(MdSpan) };
static TvArena    g_cells  = { NULL, 0, 0, sizeof(MdTableCell) };
static TvArena    g_lines  = { NULL, 0, 0, sizeof(MdLine) };
static TvArena    g_frags  = { NULL, 0, 0, sizeof(MdFrag) };
static TvArena    g_toc    = { NULL, 0, 0, sizeof(MdTocItem) };
static TvArena    g_md_matches = { NULL, 0, 0, sizeof(MdFindMatch) };

static int        g_layout_col_w = 0;
static int        g_layout_col_x = 0;
static int        g_layout_font_px = 0;
static float      g_doc_h = 0.0f;
static float      g_scroll_y = 0.0f;
static float      g_toc_scroll_y = 0.0f;
static float      g_toc_content_h = 0.0f;
static int        g_toc_min_level = 1;
static int        g_active_toc_idx = -1;

/* Scrollbar drag state */
static bool       g_sb_dragging = false;
static int        g_sb_drag_start_y = 0;
static float      g_sb_drag_start_scroll = 0.0f;

/* Quick Find state */
static bool       g_find_open = false;
static bool       g_find_case = false;
static wchar_t    g_find_query[128] = {0};
static int        g_find_match_idx = 0;
static int        g_find_match_total = 0;

/* Caret & UI hover state */
static int        g_caret_line = 1;
static int        g_caret_col = 1;
static int        g_line_count = 1;
static int        g_hover_cap = 0;         /* 1=min, 2=max, 3=close */
static int        g_hover_tb = -1;         /* 0=outline, 1=mode/wrap, 2=save, 3=find, 4=theme, 5=external */
static int        g_hover_find = -1;       /* 0=prev, 1=next, 2=case, 3=close */
static int        g_hover_toc = -1;
static int        g_hover_cb_blk = -1;
static int        g_hover_copy_blk = -1;
static bool       g_hover_link = false;
static bool       g_hover_sb = false;
static bool       g_hover_banner_ext = false;
static bool       g_tracking = false;
static wchar_t    g_flash_msg[96] = {0};
static DWORD      g_flash_until = 0;
static wchar_t    g_hover_url[256] = {0};

/* Cached layout rects */
static RECT g_rc_cap[3];
static RECT g_rc_tb[6];      /* 0:Outline, 1:Mode/Wrap, 2:Save, 3:Find, 4:Theme, 5:External */
static RECT g_rc_banner = {0};
static RECT g_rc_banner_btn = {0};
static RECT g_rc_find_bar = {0};
static RECT g_rc_find_box = {0};
static RECT g_rc_find_btns[4];
static RECT g_rc_sidebar = {0};
static RECT g_rc_doc = {0};
static RECT g_rc_sb_thumb = {0};
static RECT g_rc_edit_box = {0};

/* Forward declarations */
static void tv_md_rebuild_layout(bool force);
static void tv_md_rebuild_find_matches(bool jump_first);
static void tv_update_layout(int w, int h);
static bool tv_save_file(void);
static void tv_open_find_bar(void);
static void tv_close_find_bar(void);
static void tv_toggle_source_mode(int jump_to_line);
static void tv_toggle_sidebar(void);
static void tv_adjust_zoom(int delta);
static void tv_reset_zoom(void);
static void tv_toggle_word_wrap(void);
static void tv_sync_edit_to_doc(void);
static void tv_load_file(const wchar_t *path);
static LRESULT CALLBACK tv_find_sub_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void tv_invalidate_edit_scrollbar(void);

/* =========================================================================
 * Palette & Utility Helpers
 * ========================================================================= */
static bool tv_is_dark(void) {
    if (g_theme_override == 1) return false;
    if (g_theme_override == 2) return true;
    return g_theme.is_dark;
}

static void tv_get_palette(MtPalette *p) {
    ZeroMemory(p, sizeof(*p));
    p->is_dark = tv_is_dark();
    if (p->is_dark) {
        /* The dark palette */
        p->bg_ref            = RGB(0x28, 0x28, 0x28);
        p->text_ref          = RGB(0xC8, 0xC8, 0xC8);
        p->input_ref         = RGB(0x2F, 0x33, 0x36);
        p->bg                = MAKE_ARGB(255, 0x28, 0x28, 0x28);
        p->sidebar_bg        = MAKE_ARGB(255, 0x23, 0x23, 0x23);
        p->sidebar_border    = MAKE_ARGB(255, 0x1D, 0x1D, 0x1D);
        p->sidebar_title     = MAKE_ARGB(255, 0xD4, 0xD4, 0xD4);
        p->sidebar_text      = MAKE_ARGB(255, 0xA8, 0xA8, 0xA8);
        p->sidebar_muted     = MAKE_ARGB(255, 0x76, 0x76, 0x76);
        p->sidebar_hover     = MAKE_ARGB(255, 0x2D, 0x2D, 0x2F);
        p->sidebar_active_bg = MAKE_ARGB(255, 0x26, 0x33, 0x42);
        p->text              = MAKE_ARGB(255, 0xC2, 0xC2, 0xC2); /* rgba(255,255,255,0.72) on #282828 */
        p->heading           = MAKE_ARGB(255, 0xDC, 0xDC, 0xDC); /* rgba(255,255,255,0.82) on #282828 */
        p->heading6          = MAKE_ARGB(255, 0xA6, 0xA6, 0xA6);
        p->muted             = MAKE_ARGB(255, 0x94, 0x94, 0x94); /* --editorColor50 */
        p->subtle            = MAKE_ARGB(255, 0x6A, 0x6A, 0x6A); /* --editorColor30 */
        p->divider           = MAKE_ARGB(255, 0x3D, 0x3D, 0x3D); /* --editorColor10 */
        p->surface_alt       = MAKE_ARGB(255, 0x31, 0x31, 0x33); /* --editorColor04 */
        p->accent            = MAKE_ARGB(255, 0x40, 0x9E, 0xFF); /* --themeColor */
        p->accent_hover      = MAKE_ARGB(255, 0x5A, 0xAB, 0xFF);
        p->accent_soft       = MAKE_ARGB(42,  0x40, 0x9E, 0xFF);
        p->fg_on_accent      = MAKE_ARGB(255, 0xFF, 0xFF, 0xFF);
        p->link              = MAKE_ARGB(255, 0x40, 0x9E, 0xFF); /* --linkColor */
        p->strong            = MAKE_ARGB(255, 0xE2, 0xE2, 0xE2); /* --strongColor */
        p->em                = MAKE_ARGB(255, 0x66, 0xB1, 0xFF); /* emphasis */
        p->strike            = MAKE_ARGB(255, 0x82, 0x82, 0x82);
        p->list_marker       = MAKE_ARGB(255, 0x40, 0x9E, 0xFF); /* --listMarkerColor */
        p->quote_bar         = MAKE_ARGB(255, 0x40, 0x9E, 0xFF); /* --blockquoteBorderColor */
        p->quote_text        = MAKE_ARGB(255, 0x94, 0x94, 0x94); /* --blockquoteTextColor */
        p->inline_code_bg    = MAKE_ARGB(255, 0x3B, 0x3C, 0x3E); /* --codeBgColor */
        p->inline_code_fg    = MAKE_ARGB(255, 0xEA, 0xEA, 0xE6);
        p->code_block_bg     = MAKE_ARGB(255, 0x34, 0x35, 0x37); /* --codeBlockBgColor */
        p->code_block_border = MAKE_ARGB(255, 0x44, 0x45, 0x48);
        p->code_block_fg     = MAKE_ARGB(255, 0xF8, 0xF8, 0xF2); /* --codeBlockColor */
        p->table_border      = MAKE_ARGB(255, 0x3E, 0x40, 0x42); /* --tableBorderColor */
        p->table_header_bg   = MAKE_ARGB(255, 0x30, 0x31, 0x33);
        p->table_alt_bg      = MAKE_ARGB(255, 0x2C, 0x2C, 0x2E);
        p->hr                = MAKE_ARGB(255, 0x45, 0x45, 0x45); /* --hrColor */
        p->input_bg          = MAKE_ARGB(255, 0x2F, 0x33, 0x36); /* --inputBgColor */
        p->hover_bg          = MAKE_ARGB(255, 0x36, 0x36, 0x38);
        p->find_hl           = MAKE_ARGB(78,  0x66, 0xB1, 0xFF); /* --highlightColor */
        p->find_cur          = MAKE_ARGB(155, 0x40, 0x9E, 0xFF);
        p->scroll_thumb      = MAKE_ARGB(68,  0xFF, 0xFF, 0xFF);
        p->scroll_thumb_hov  = MAKE_ARGB(118, 0xFF, 0xFF, 0xFF);
        p->syn_kw            = MAKE_ARGB(255, 0x66, 0xB1, 0xFF);
        p->syn_str           = MAKE_ARGB(255, 0x98, 0xC3, 0x79);
        p->syn_com           = MAKE_ARGB(255, 0x86, 0x8E, 0x96);
        p->syn_num           = MAKE_ARGB(255, 0xE5, 0xC0, 0x7B);
    } else {
        /* The light palette */
        p->bg_ref            = RGB(0xFF, 0xFF, 0xFF);
        p->text_ref          = RGB(0x4D, 0x4D, 0x4D);
        p->input_ref         = RGB(0xF0, 0xF0, 0xF0);
        p->bg                = MAKE_ARGB(255, 0xFF, 0xFF, 0xFF); /* --editorBgColor */
        p->sidebar_bg        = MAKE_ARGB(255, 0xF2, 0xF2, 0xF2); /* --sideBarBgColor */
        p->sidebar_border    = MAKE_ARGB(255, 0xE5, 0xE5, 0xE5); /* --itemBgColor */
        p->sidebar_title     = MAKE_ARGB(255, 0x26, 0x26, 0x26); /* --sideBarTitleColor */
        p->sidebar_text      = MAKE_ARGB(255, 0x60, 0x60, 0x60); /* --sideBarColor */
        p->sidebar_muted     = MAKE_ARGB(255, 0x99, 0x99, 0x99); /* --sideBarTextColor */
        p->sidebar_hover     = MAKE_ARGB(255, 0xE8, 0xE8, 0xE8); /* --sideBarItemHoverBgColor */
        p->sidebar_active_bg = MAKE_ARGB(255, 0xE3, 0xF5, 0xEC);
        p->text              = MAKE_ARGB(255, 0x4D, 0x4D, 0x4D); /* --editorColor */
        p->heading           = MAKE_ARGB(255, 0x33, 0x33, 0x33); /* --editorColor80 */
        p->heading6          = MAKE_ARGB(255, 0x66, 0x66, 0x66); /* --editorColor60 */
        p->muted             = MAKE_ARGB(255, 0x80, 0x80, 0x80); /* --editorColor50 */
        p->subtle            = MAKE_ARGB(255, 0xB3, 0xB3, 0xB3); /* --editorColor30 */
        p->divider           = MAKE_ARGB(255, 0xE6, 0xE6, 0xE6); /* --editorColor10 */
        p->surface_alt       = MAKE_ARGB(255, 0xF7, 0xF7, 0xF7); /* --editorColor04 */
        p->accent            = MAKE_ARGB(255, 0x21, 0xB5, 0x6F); /* --themeColor */
        p->accent_hover      = MAKE_ARGB(255, 0x1A, 0x9E, 0x5F);
        p->accent_soft       = MAKE_ARGB(36,  0x21, 0xB5, 0x6F);
        p->fg_on_accent      = MAKE_ARGB(255, 0xFF, 0xFF, 0xFF);
        p->link              = MAKE_ARGB(255, 0x21, 0xB5, 0x6F); /* --linkColor */
        p->strong            = MAKE_ARGB(255, 0x33, 0x33, 0x33);
        p->em                = MAKE_ARGB(255, 0x4D, 0x4D, 0x4D); /* --emColor */
        p->strike            = MAKE_ARGB(255, 0x88, 0x88, 0x88);
        p->list_marker       = MAKE_ARGB(255, 0x80, 0x80, 0x80); /* --listMarkerColor */
        p->quote_bar         = MAKE_ARGB(255, 0x21, 0xB5, 0x6F); /* --blockquoteBorderColor */
        p->quote_text        = MAKE_ARGB(255, 0x76, 0x76, 0x76); /* --blockquoteTextColor */
        p->inline_code_bg    = MAKE_ARGB(255, 0xEF, 0xEF, 0xF0); /* --codeBgColor */
        p->inline_code_fg    = MAKE_ARGB(255, 0x38, 0x38, 0x38);
        p->code_block_bg     = MAKE_ARGB(255, 0xF7, 0xF7, 0xF7); /* --codeBlockBgColor */
        p->code_block_border = MAKE_ARGB(255, 0xE6, 0xE6, 0xE6); /* --editorColor10 */
        p->code_block_fg     = MAKE_ARGB(255, 0x44, 0x44, 0x44); /* --codeBlockColor */
        p->table_border      = MAKE_ARGB(255, 0xE5, 0xE5, 0xE5); /* --tableBorderColor */
        p->table_header_bg   = MAKE_ARGB(255, 0xF7, 0xF7, 0xF7);
        p->table_alt_bg      = MAKE_ARGB(255, 0xFB, 0xFB, 0xFC);
        p->hr                = MAKE_ARGB(255, 0xDF, 0xDF, 0xDF); /* --hrColor */
        p->input_bg          = MAKE_ARGB(255, 0xF0, 0xF0, 0xF0); /* --inputBgColor */
        p->hover_bg          = MAKE_ARGB(255, 0xEE, 0xEE, 0xF0);
        p->find_hl           = MAKE_ARGB(72,  0x21, 0xB5, 0x6F); /* --highlightColor */
        p->find_cur          = MAKE_ARGB(145, 0x21, 0xB5, 0x6F);
        p->scroll_thumb      = MAKE_ARGB(62,  0x00, 0x00, 0x00);
        p->scroll_thumb_hov  = MAKE_ARGB(112, 0x00, 0x00, 0x00);
        p->syn_kw            = MAKE_ARGB(255, 0x0B, 0x74, 0xA5);
        p->syn_str           = MAKE_ARGB(255, 0x1B, 0x87, 0x52);
        p->syn_com           = MAKE_ARGB(255, 0x8C, 0x95, 0x9F);
        p->syn_num           = MAKE_ARGB(255, 0x9E, 0x5B, 0x12);
    }
}

static bool tv_is_md_ext(const wchar_t *ext) {
    if (!ext || !ext[0]) return false;
    return (_wcsicmp(ext, L".md") == 0 ||
            _wcsicmp(ext, L".markdown") == 0 ||
            _wcsicmp(ext, L".mkd") == 0);
}

static bool tv_is_code_ext(const wchar_t *ext) {
    if (!ext || !ext[0]) return false;
    return (_wcsicmp(ext, L".c") == 0 || _wcsicmp(ext, L".h") == 0 ||
            _wcsicmp(ext, L".js") == 0 || _wcsicmp(ext, L".ts") == 0 ||
            _wcsicmp(ext, L".py") == 0 || _wcsicmp(ext, L".json") == 0 ||
            _wcsicmp(ext, L".css") == 0 || _wcsicmp(ext, L".html") == 0 ||
            _wcsicmp(ext, L".xml") == 0 || _wcsicmp(ext, L".ps1") == 0 ||
            _wcsicmp(ext, L".bat") == 0);
}

static void tv_format_bytes(uint64_t bytes, wchar_t *out, size_t len) {
    if (bytes < 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%.1f KB", (double)bytes / 1024.0);
    } else {
        _snwprintf_s(out, len, _TRUNCATE, L"%.2f MB", (double)bytes / (1024.0 * 1024.0));
    }
}

static const wchar_t *tv_enc_label(TvEncoding e) {
    switch (e) {
    case TV_ENC_UTF8_BOM: return L"UTF-8 BOM";
    case TV_ENC_UTF16LE:  return L"UTF-16 LE";
    case TV_ENC_UTF16BE:  return L"UTF-16 BE";
    case TV_ENC_ANSI:     return L"ANSI";
    case TV_ENC_UTF8:
    default:              return L"UTF-8";
    }
}

static void tv_flash(const wchar_t *msg) {
    wcsncpy_s(g_flash_msg, 96, msg ? msg : L"", _TRUNCATE);
    g_flash_until = GetTickCount() + 2400;
    if (g_hTvWnd) InvalidateRect(g_hTvWnd, NULL, FALSE);
}

/* =========================================================================
 * DirectWrite measurement cache (shared factory)
 * ========================================================================= */
static bool tv_dw_family_exists(IDWriteFontCollection *col, const wchar_t *name) {
    if (!col || !name) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(col->lpVtbl->FindFamilyName(col, name, &idx, &exists)) && exists;
}

static void tv_dw_clear_cache(void) {
    for (int i = 0; i < g_dw_slot_count; i++) {
        if (g_dw_slots[i].fmt) {
            g_dw_slots[i].fmt->lpVtbl->Release(g_dw_slots[i].fmt);
            g_dw_slots[i].fmt = NULL;
        }
        g_dw_slots[i].has_ascii = false;
    }
    g_dw_slot_count = 0;
}

static void tv_dw_ensure_factory(void) {
    if (g_tv_dw) return;
    DWriteCreateFactory(DW_FACTORY_TYPE_SHARED, &TV_IID_IDWriteFactory, (IUnknown **)&g_tv_dw);
    if (!g_tv_dw) return;

    IDWriteFontCollection *col = NULL;
    if (SUCCEEDED(g_tv_dw->lpVtbl->GetSystemFontCollection(g_tv_dw, &col, FALSE)) && col) {
        if (tv_dw_family_exists(col, L"Inter")) {
            wcsncpy_s(g_body_face, 64, L"Inter", _TRUNCATE);
        } else if (tv_dw_family_exists(col, L"Segoe UI Variable Text")) {
            wcsncpy_s(g_body_face, 64, L"Segoe UI Variable Text", _TRUNCATE);
        } else {
            wcsncpy_s(g_body_face, 64, L"Segoe UI", _TRUNCATE);
        }

        if (tv_dw_family_exists(col, L"Cascadia Mono")) {
            wcsncpy_s(g_mono_face, 64, L"Cascadia Mono", _TRUNCATE);
        } else if (tv_dw_family_exists(col, L"Consolas")) {
            wcsncpy_s(g_mono_face, 64, L"Consolas", _TRUNCATE);
        } else {
            wcsncpy_s(g_mono_face, 64, L"Courier New", _TRUNCATE);
        }
        col->lpVtbl->Release(col);
    }
}

static float tv_dw_measure_raw(IDWriteTextFormat *fmt, const wchar_t *text, size_t len) {
    if (!g_tv_dw || !fmt || !text || len == 0) return 0.0f;
    IDWriteTextLayout *lay = NULL;
    float out_w = 0.0f;
    if (SUCCEEDED(g_tv_dw->lpVtbl->CreateTextLayout(
            g_tv_dw, text, (UINT32)len, fmt, 16000.0f, 400.0f, &lay)) && lay) {
        DW_TEXT_METRICS tm;
        ZeroMemory(&tm, sizeof(tm));
        if (SUCCEEDED(lay->lpVtbl->GetMetrics(lay, &tm))) {
            out_w = tm.widthIncludingTrailingWhitespace;
        }
        lay->lpVtbl->base.Release((IDWriteTextFormat *)lay);
    }
    return out_w;
}

static TvDwSlot *tv_dw_get_slot(bool is_mono, float size, int style) {
    tv_dw_ensure_factory();
    if (!g_tv_dw) return NULL;
    if (size < 8.0f) size = 8.0f;
    if (size > 96.0f) size = 96.0f;
    int size_tenths = (int)(size * 10.0f + 0.5f);

    for (int i = 0; i < g_dw_slot_count; i++) {
        if (g_dw_slots[i].is_mono == is_mono &&
            g_dw_slots[i].size_tenths == size_tenths &&
            g_dw_slots[i].style == style) {
            return &g_dw_slots[i];
        }
    }

    int slot = g_dw_slot_count;
    if (slot >= TV_DW_CACHE_MAX) {
        tv_dw_clear_cache();
        slot = 0;
    }

    const wchar_t *face = is_mono ? g_mono_face : g_body_face;
    DW_FONT_WEIGHT w = (style & GP_FONT_BOLD) ? DW_WEIGHT_SEMIBOLD : DW_WEIGHT_REGULAR;
    DW_FONT_STYLE  s = (style & GP_FONT_ITALIC) ? DW_STYLE_ITALIC : DW_STYLE_NORMAL;
    IDWriteTextFormat *fmt = NULL;
    if (FAILED(g_tv_dw->lpVtbl->CreateTextFormat(
            g_tv_dw, face, NULL, w, s, DW_STRETCH_NORMAL,
            (float)size_tenths * 0.1f, L"en-US", &fmt)) || !fmt) {
        return NULL;
    }
    fmt->lpVtbl->SetWordWrapping(fmt, DW_WRAP_NONE);

    TvDwSlot *sl = &g_dw_slots[slot];
    ZeroMemory(sl, sizeof(*sl));
    sl->is_mono = is_mono;
    sl->size_tenths = size_tenths;
    sl->style = style;
    sl->fmt = fmt;

    /* Precompute fast ASCII character widths once per format slot */
    float base_aa = tv_dw_measure_raw(fmt, L"HH", 2);
    wchar_t tri[3] = { L'H', L' ', L'H' };
    for (int c = 32; c < 127; c++) {
        tri[1] = (wchar_t)c;
        float aw = tv_dw_measure_raw(fmt, tri, 3) - base_aa;
        if (aw < 1.0f) aw = size * 0.45f;
        sl->ascii_adv[c] = aw;
    }
    sl->ascii_adv['\t'] = sl->ascii_adv[' '] * 4.0f;
    sl->has_ascii = true;

    g_dw_slot_count = slot + 1;
    return sl;
}

static float tv_measure_run_exact(const wchar_t *text, size_t len, bool is_mono, float size, int style) {
    if (!text || len == 0) return 0.0f;
    TvDwSlot *sl = tv_dw_get_slot(is_mono, size, style);
    if (!sl || !sl->fmt) return (float)len * size * 0.55f;
    return tv_dw_measure_raw(sl->fmt, text, len);
}

static float tv_estimate_run_fast(const wchar_t *text, size_t len, bool is_mono, float size, int style) {
    if (!text || len == 0) return 0.0f;
    TvDwSlot *sl = tv_dw_get_slot(is_mono, size, style);
    if (!sl || !sl->has_ascii) return (float)len * size * 0.55f;
    float sum = 0.0f;
    bool non_ascii = false;
    for (size_t i = 0; i < len; i++) {
        wchar_t ch = text[i];
        if (ch >= 32 && ch < 127) {
            sum += sl->ascii_adv[ch];
        } else if (ch == L'\t') {
            sum += sl->ascii_adv['\t'];
        } else {
            non_ascii = true;
            break;
        }
    }
    if (non_ascii) {
        return tv_dw_measure_raw(sl->fmt, text, len);
    }
    return sum;
}

/* =========================================================================
 * Fonts, Brushes, & Window Title Helpers
 * ========================================================================= */
static void tv_rebuild_mono_font(void) {
    if (g_hMonoFont) { DeleteObject(g_hMonoFont); g_hMonoFont = NULL; }
    int edit_px = g_is_markdown ? max(11, g_body_px - 1) : g_code_edit_px;
    g_hMonoFont = CreateFontW(-edit_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, g_mono_face);
    if (g_hEdit && g_hMonoFont) {
        SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hMonoFont, TRUE);
    }
}

static void tv_refresh_brushes(void) {
    MtPalette pal;
    tv_get_palette(&pal);
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
    if (g_hFindBrush) { DeleteObject(g_hFindBrush); g_hFindBrush = NULL; }
    g_hEditBrush = CreateSolidBrush(pal.bg_ref);
    g_hFindBrush = CreateSolidBrush(pal.input_ref);
}

static void tv_update_window_title(void) {
    if (!g_hTvWnd) return;
    wchar_t title[MAX_PATH + 64];
    const wchar_t *nm = g_cur_name[0] ? g_cur_name : L"Untitled";
    const wchar_t *app = g_is_markdown ? L"Markdown" : L"Text Editor";
    _snwprintf_s(title, MAX_PATH + 64, _TRUNCATE, L"%ls%ls - %ls",
                 g_modified ? L"*" : L"", nm, app);
    SetWindowTextW(g_hTvWnd, title);
}

static void tv_compute_doc_stats(void) {
    g_stat_words = 0;
    g_stat_chars = 0;
    g_stat_lines = 1;
    if (!g_doc_text || g_doc_len == 0) return;

    bool in_word = false;
    for (size_t i = 0; i < g_doc_len; i++) {
        wchar_t c = g_doc_text[i];
        if (c == L'\r') continue;
        if (c == L'\n') {
            g_stat_lines++;
            in_word = false;
            continue;
        }
        g_stat_chars++;
        if (iswspace(c)) {
            in_word = false;
        } else {
            if (!in_word) {
                g_stat_words++;
                in_word = true;
            }
        }
    }
}

static bool tv_doc_set_text(const wchar_t *src, size_t len) {
    if (g_doc_text) {
        VirtualFree(g_doc_text, 0, MEM_RELEASE);
        g_doc_text = NULL;
    }
    g_doc_len = 0;
    g_doc_cap = 0;

    size_t cap = len + 16;
    wchar_t *buf = (wchar_t *)VirtualAlloc(NULL, cap * sizeof(wchar_t),
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return false;
    if (src && len > 0) memcpy(buf, src, len * sizeof(wchar_t));
    buf[len] = L'\0';
    g_doc_text = buf;
    g_doc_len = len;
    g_doc_cap = cap;
    tv_compute_doc_stats();
    return true;
}

static void tv_free_md_arenas(void) {
    arena_free(&g_blocks);
    arena_free(&g_spans);
    arena_free(&g_cells);
    arena_free(&g_lines);
    arena_free(&g_frags);
    arena_free(&g_toc);
    arena_free(&g_md_matches);
    g_layout_col_w = 0;
    g_layout_font_px = 0;
    g_doc_h = 0.0f;
    g_toc_content_h = 0.0f;
    g_active_toc_idx = -1;
}

static void tv_free_doc_all(void) {
    tv_free_md_arenas();
    if (g_doc_text) {
        VirtualFree(g_doc_text, 0, MEM_RELEASE);
        g_doc_text = NULL;
    }
    g_doc_len = 0;
    g_doc_cap = 0;
    g_stat_words = 0;
    g_stat_chars = 0;
    g_stat_lines = 1;
    g_scroll_y = 0.0f;
    g_toc_scroll_y = 0.0f;
}

/* =========================================================================
 * Markdown Inline Parser (GFM: Bold, Italic, Strike, Code, Links, Autolinks)
 * ========================================================================= */
static bool is_url_char(wchar_t c) {
    if (c <= 32 || c >= 127) return false;
    if (c == L'<' || c == L'>' || c == L'"' || c == L'\'' || c == L'`') return false;
    return true;
}

static void md_emit_span(uint32_t off, uint32_t len, uint8_t flags, uint32_t uoff, uint32_t ulen) {
    if (len == 0 && !(flags & MD_SPAN_BR)) return;
    /* Coalesce adjacent spans with identical non-link, non-code, non-br flags */
    if (g_spans.count > 0 && !(flags & (MD_SPAN_CODE | MD_SPAN_LINK | MD_SPAN_BR | MD_SPAN_IMAGE))) {
        MdSpan *prev = &((MdSpan *)g_spans.ptr)[g_spans.count - 1];
        if (prev->flags == flags && prev->text_off + prev->text_len == off) {
            prev->text_len += len;
            return;
        }
    }
    MdSpan *sp = (MdSpan *)arena_push(&g_spans);
    if (!sp) return;
    sp->text_off = off;
    sp->text_len = len;
    sp->url_off  = uoff;
    sp->url_len  = ulen;
    sp->flags    = flags;
}

static bool md_find_delim(const wchar_t *s, size_t start, size_t end, wchar_t ch, size_t count, size_t *out_pos) {
    for (size_t i = start; i + count <= end; i++) {
        if (s[i] == L'\\' && i + 1 < end) { i++; continue; }
        if (s[i] == L'`') {
            /* Skip inline code span while searching closing emphasis delimiter */
            size_t ticks = 0;
            while (i + ticks < end && s[i + ticks] == L'`') ticks++;
            i += ticks;
            while (i + ticks <= end) {
                size_t k = 0;
                while (k < ticks && s[i + k] == L'`') k++;
                if (k == ticks) { i += ticks - 1; break; }
                i++;
            }
            continue;
        }
        bool ok = true;
        for (size_t k = 0; k < count; k++) {
            if (s[i + k] != ch) { ok = false; break; }
        }
        if (ok) {
            if (i > start && iswspace(s[i - 1])) continue;
            if (ch == L'_' && i + count < end && iswalnum(s[i + count])) continue;
            *out_pos = i;
            return true;
        }
    }
    return false;
}

static void md_parse_inline_range(size_t start, size_t end, uint8_t flags, uint32_t link_uoff, uint32_t link_ulen, int depth) {
    if (!g_doc_text || start >= end || depth > 8) return;
    const wchar_t *s = g_doc_text;
    size_t i = start;
    size_t run_start = i;

    while (i < end) {
        wchar_t c = s[i];

        /* Treat embedded CR/LF inside multi-line paragraph as single space (unless hard break) */
        if (c == L'\r' || c == L'\n') {
            bool hard_br = (i >= start + 2 && s[i - 1] == L' ' && s[i - 2] == L' ') ||
                           (i >= start + 1 && s[i - 1] == L'\\');
            if (i > run_start) {
                size_t rlen = i - run_start;
                if (hard_br && rlen > 0 && s[i - 1] == L'\\') rlen--;
                md_emit_span((uint32_t)run_start, (uint32_t)rlen, flags, link_uoff, link_ulen);
            }
            if (c == L'\r' && i + 1 < end && s[i + 1] == L'\n') i += 2;
            else i++;
            if (hard_br) {
                md_emit_span(0, 0, MD_SPAN_BR, 0, 0);
            } else {
                /* Emit single space if next char isn't whitespace */
                if (i < end && s[i] != L'\r' && s[i] != L'\n') {
                    md_emit_span((uint32_t)(i - 1), 1, flags, link_uoff, link_ulen);
                }
            }
            while (i < end && (s[i] == L' ' || s[i] == L'\t')) i++;
            run_start = i;
            continue;
        }

        /* Backslash escape */
        if (c == L'\\' && i + 1 < end && iswpunct(s[i + 1])) {
            if (i > run_start) md_emit_span((uint32_t)run_start, (uint32_t)(i - run_start), flags, link_uoff, link_ulen);
            md_emit_span((uint32_t)(i + 1), 1, flags, link_uoff, link_ulen);
            i += 2;
            run_start = i;
            continue;
        }

        /* Inline code: `...` or ``...`` */
        if (c == L'`') {
            size_t ticks = 0;
            while (i + ticks < end && s[i + ticks] == L'`') ticks++;
            size_t j = i + ticks;
            size_t close_pos = 0;
            bool found = false;
            while (j + ticks <= end) {
                if (s[j] == L'`') {
                    size_t k = 0;
                    while (j + k < end && s[j + k] == L'`') k++;
                    if (k == ticks) { close_pos = j; found = true; break; }
                    j += k;
                } else {
                    j++;
                }
            }
            if (found) {
                if (i > run_start) md_emit_span((uint32_t)run_start, (uint32_t)(i - run_start), flags, link_uoff, link_ulen);
                size_t cs = i + ticks;
                size_t ce = close_pos;
                if (ce > cs + 1 && s[cs] == L' ' && s[ce - 1] == L' ') { cs++; ce--; }
                if (ce > cs) {
                    md_emit_span((uint32_t)cs, (uint32_t)(ce - cs), (uint8_t)(flags | MD_SPAN_CODE), link_uoff, link_ulen);
                }
                i = close_pos + ticks;
                run_start = i;
                continue;
            }
        }

        /* Image ![alt](url) or Link [text](url) */
        if ((c == L'[' || (c == L'!' && i + 1 < end && s[i + 1] == L'[')) && !(flags & MD_SPAN_LINK)) {
            bool is_img = (c == L'!');
            size_t lb = is_img ? (i + 1) : i;
            size_t rb = 0;
            int br_depth = 0;
            for (size_t k = lb; k < end; k++) {
                if (s[k] == L'\\' && k + 1 < end) { k++; continue; }
                if (s[k] == L'[') br_depth++;
                else if (s[k] == L']') {
                    br_depth--;
                    if (br_depth == 0) { rb = k; break; }
                }
            }
            if (rb > lb && rb + 1 < end && s[rb + 1] == L'(') {
                size_t lp = rb + 1;
                size_t rp = 0;
                int p_depth = 0;
                for (size_t k = lp; k < end; k++) {
                    if (s[k] == L'\\' && k + 1 < end) { k++; continue; }
                    if (s[k] == L'(') p_depth++;
                    else if (s[k] == L')') {
                        p_depth--;
                        if (p_depth == 0) { rp = k; break; }
                    }
                }
                if (rp > lp) {
                    if (i > run_start) md_emit_span((uint32_t)run_start, (uint32_t)(i - run_start), flags, link_uoff, link_ulen);
                    size_t us = lp + 1;
                    size_t ue = rp;
                    while (us < ue && iswspace(s[us])) us++;
                    while (ue > us && iswspace(s[ue - 1])) ue--;
                    /* Strip optional title "title" after URL */
                    for (size_t k = us; k < ue; k++) {
                        if (iswspace(s[k])) { ue = k; break; }
                    }
                    if (us < ue && s[us] == L'<' && s[ue - 1] == L'>') { us++; ue--; }
                    if (is_img) {
                        size_t ts = lb + 1, te = rb;
                        if (te > ts) {
                            md_emit_span((uint32_t)ts, (uint32_t)(te - ts),
                                         (uint8_t)(flags | MD_SPAN_LINK | MD_SPAN_IMAGE),
                                         (uint32_t)us, (uint32_t)(ue - us));
                        } else if (ue > us) {
                            md_emit_span((uint32_t)us, (uint32_t)(ue - us),
                                         (uint8_t)(flags | MD_SPAN_LINK | MD_SPAN_IMAGE),
                                         (uint32_t)us, (uint32_t)(ue - us));
                        }
                    } else {
                        if (rb > lb + 1) {
                            md_parse_inline_range(lb + 1, rb, (uint8_t)(flags | MD_SPAN_LINK),
                                                  (uint32_t)us, (uint32_t)(ue - us), depth + 1);
                        } else if (ue > us) {
                            md_emit_span((uint32_t)us, (uint32_t)(ue - us),
                                         (uint8_t)(flags | MD_SPAN_LINK),
                                         (uint32_t)us, (uint32_t)(ue - us));
                        }
                    }
                    i = rp + 1;
                    run_start = i;
                    continue;
                }
            }
        }

        /* Angle-bracket autolink <https://...> or <mailto:...> */
        if (c == L'<' && !(flags & MD_SPAN_LINK)) {
            size_t gt = i + 1;
            while (gt < end && s[gt] != L'>' && !iswspace(s[gt])) gt++;
            if (gt < end && s[gt] == L'>' && gt > i + 6 &&
                (_wcsnicmp(s + i + 1, L"http://", 7) == 0 ||
                 _wcsnicmp(s + i + 1, L"https://", 8) == 0 ||
                 _wcsnicmp(s + i + 1, L"mailto:", 7) == 0)) {
                if (i > run_start) md_emit_span((uint32_t)run_start, (uint32_t)(i - run_start), flags, link_uoff, link_ulen);
                md_emit_span((uint32_t)(i + 1), (uint32_t)(gt - (i + 1)),
                             (uint8_t)(flags | MD_SPAN_LINK),
                             (uint32_t)(i + 1), (uint32_t)(gt - (i + 1)));
                i = gt + 1;
                run_start = i;
                continue;
            }
            /* Simple <br> / <br/> HTML break */
            if (gt < end && s[gt] == L'>' && gt - i <= 6 &&
                (_wcsnicmp(s + i, L"<br>", 4) == 0 || _wcsnicmp(s + i, L"<br/>", 5) == 0)) {
                if (i > run_start) md_emit_span((uint32_t)run_start, (uint32_t)(i - run_start), flags, link_uoff, link_ulen);
                md_emit_span(0, 0, MD_SPAN_BR, 0, 0);
                i = gt + 1;
                run_start = i;
                continue;
            }
        }

        /* Bare URL autolink https://... or http://... */
        if ((c == L'h' || c == L'H') && !(flags & MD_SPAN_LINK) &&
            (i == start || iswspace(s[i - 1]) || s[i - 1] == L'(') &&
            ((i + 8 < end && _wcsnicmp(s + i, L"https://", 8) == 0) ||
             (i + 7 < end && _wcsnicmp(s + i, L"http://", 7) == 0))) {
            size_t ue = i;
            while (ue < end && is_url_char(s[ue])) ue++;
            while (ue > i && (s[ue - 1] == L'.' || s[ue - 1] == L',' || s[ue - 1] == L')' || s[ue - 1] == L';' || s[ue - 1] == L':')) ue--;
            if (ue > i + 8) {
                if (i > run_start) md_emit_span((uint32_t)run_start, (uint32_t)(i - run_start), flags, link_uoff, link_ulen);
                md_emit_span((uint32_t)i, (uint32_t)(ue - i),
                             (uint8_t)(flags | MD_SPAN_LINK),
                             (uint32_t)i, (uint32_t)(ue - i));
                i = ue;
                run_start = i;
                continue;
            }
        }

        /* Strikethrough ~~...~~ */
        if (c == L'~' && i + 1 < end && s[i + 1] == L'~' && i + 2 < end && !iswspace(s[i + 2])) {
            size_t close_pos = 0;
            if (md_find_delim(s, i + 2, end, L'~', 2, &close_pos) && close_pos > i + 2) {
                if (i > run_start) md_emit_span((uint32_t)run_start, (uint32_t)(i - run_start), flags, link_uoff, link_ulen);
                md_parse_inline_range(i + 2, close_pos, (uint8_t)(flags | MD_SPAN_STRIKE), link_uoff, link_ulen, depth + 1);
                i = close_pos + 2;
                run_start = i;
                continue;
            }
        }

        /* Emphasis: ***, **, *, ___, __, _ */
        if (c == L'*' || c == L'_') {
            if (c == L'_' && i > start && iswalnum(s[i - 1])) {
                i++;
                continue;
            }
            size_t cnt = 1;
            if (i + 1 < end && s[i + 1] == c) {
                cnt = 2;
                if (i + 2 < end && s[i + 2] == c) cnt = 3;
            }
            if (i + cnt < end && !iswspace(s[i + cnt])) {
                size_t close_pos = 0;
                if (md_find_delim(s, i + cnt, end, c, cnt, &close_pos) && close_pos > i + cnt) {
                    if (i > run_start) md_emit_span((uint32_t)run_start, (uint32_t)(i - run_start), flags, link_uoff, link_ulen);
                    uint8_t add = (cnt == 3) ? (MD_SPAN_BOLD | MD_SPAN_ITALIC)
                                : (cnt == 2) ? MD_SPAN_BOLD : MD_SPAN_ITALIC;
                    md_parse_inline_range(i + cnt, close_pos, (uint8_t)(flags | add), link_uoff, link_ulen, depth + 1);
                    i = close_pos + cnt;
                    run_start = i;
                    continue;
                }
            }
        }

        i++;
    }

    if (end > run_start) {
        md_emit_span((uint32_t)run_start, (uint32_t)(end - run_start), flags, link_uoff, link_ulen);
    }
}

/* =========================================================================
 * Markdown Block Parser (GFM Headings, Lists, Tasks, Code, Quotes, Tables, HR)
 * ========================================================================= */
typedef struct {
    size_t off;
    size_t len;
    int    line_no; /* 1-based */
} SrcLine;

static bool line_is_blank(const wchar_t *s, size_t off, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[off + i] != L' ' && s[off + i] != L'\t') return false;
    }
    return true;
}

static size_t count_indent(const wchar_t *s, size_t off, size_t len, size_t *out_chars) {
    size_t cols = 0, i = 0;
    while (i < len) {
        if (s[off + i] == L' ') { cols++; i++; }
        else if (s[off + i] == L'\t') { cols += 4; i++; }
        else break;
    }
    if (out_chars) *out_chars = i;
    return cols;
}

static bool line_is_hr(const wchar_t *s, size_t off, size_t len) {
    size_t ind = 0;
    if (count_indent(s, off, len, &ind) > 3) return false;
    if (ind >= len) return false;
    wchar_t ch = s[off + ind];
    if (ch != L'-' && ch != L'*' && ch != L'_') return false;
    int cnt = 0;
    for (size_t i = ind; i < len; i++) {
        wchar_t c = s[off + i];
        if (c == ch) cnt++;
        else if (c != L' ' && c != L'\t') return false;
    }
    return cnt >= 3;
}

static int line_is_setext(const wchar_t *s, size_t off, size_t len) {
    size_t ind = 0;
    if (count_indent(s, off, len, &ind) > 3) return 0;
    if (ind >= len) return 0;
    wchar_t ch = s[off + ind];
    if (ch != L'=' && ch != L'-') return 0;
    int cnt = 0;
    size_t i = ind;
    while (i < len && s[off + i] == ch) { cnt++; i++; }
    while (i < len && (s[off + i] == L' ' || s[off + i] == L'\t')) i++;
    if (i != len || cnt < 2) return 0;
    return (ch == L'=') ? 1 : 2;
}

static bool line_is_fence(const wchar_t *s, size_t off, size_t len, wchar_t *out_ch, size_t *out_cnt, wchar_t *out_lang, size_t lang_cap) {
    size_t ind = 0;
    if (count_indent(s, off, len, &ind) > 3) return false;
    if (ind + 3 > len) return false;
    wchar_t ch = s[off + ind];
    if (ch != L'`' && ch != L'~') return false;
    size_t cnt = 0;
    while (ind + cnt < len && s[off + ind + cnt] == ch) cnt++;
    if (cnt < 3) return false;
    /* Backtick fence info string cannot contain backticks */
    if (ch == L'`') {
        for (size_t k = ind + cnt; k < len; k++) {
            if (s[off + k] == L'`') return false;
        }
    }
    if (out_ch) *out_ch = ch;
    if (out_cnt) *out_cnt = cnt;
    if (out_lang && lang_cap > 0) {
        size_t ls = ind + cnt;
        while (ls < len && (s[off + ls] == L' ' || s[off + ls] == L'\t')) ls++;
        size_t le = ls;
        while (le < len && !iswspace(s[off + le]) && s[off + le] != L'{') le++;
        size_t n = min(le - ls, lang_cap - 1);
        if (n > 0) memcpy(out_lang, s + off + ls, n * sizeof(wchar_t));
        out_lang[n] = L'\0';
    }
    return true;
}

static int line_is_atx_heading(const wchar_t *s, size_t off, size_t len, size_t *out_ts, size_t *out_te) {
    size_t ind = 0;
    if (count_indent(s, off, len, &ind) > 3) return 0;
    size_t i = ind;
    int lvl = 0;
    while (i < len && s[off + i] == L'#' && lvl < 7) { lvl++; i++; }
    if (lvl < 1 || lvl > 6) return 0;
    if (i < len && s[off + i] != L' ' && s[off + i] != L'\t') return 0;
    while (i < len && (s[off + i] == L' ' || s[off + i] == L'\t')) i++;
    size_t te = len;
    while (te > i && (s[off + te - 1] == L' ' || s[off + te - 1] == L'\t')) te--;
    /* Strip optional closing ### sequence preceded by whitespace */
    size_t k = te;
    while (k > i && s[off + k - 1] == L'#') k--;
    if (k < te && (k == i || s[off + k - 1] == L' ' || s[off + k - 1] == L'\t')) {
        te = k;
        while (te > i && (s[off + te - 1] == L' ' || s[off + te - 1] == L'\t')) te--;
    }
    if (out_ts) *out_ts = off + i;
    if (out_te) *out_te = off + te;
    return lvl;
}

static bool line_is_list(const wchar_t *s, size_t off, size_t len, int *out_depth, bool *out_ord, int *out_num,
                         bool *out_task, bool *out_checked, uint32_t *out_cb_off, size_t *out_ts) {
    size_t ind_chars = 0;
    size_t cols = count_indent(s, off, len, &ind_chars);
    if (ind_chars >= len) return false;

    size_t p = ind_chars;
    bool ordered = false;
    int num = 1;
    wchar_t c = s[off + p];

    if ((c == L'-' || c == L'*' || c == L'+') && p + 1 < len && (s[off + p + 1] == L' ' || s[off + p + 1] == L'\t')) {
        /* Make sure it's not an HR like "- - -" when indent <= 3 */
        if (cols <= 3 && line_is_hr(s, off, len)) return false;
        p += 2;
    } else if (c >= L'0' && c <= L'9') {
        size_t d = p;
        int val = 0;
        while (d < len && s[off + d] >= L'0' && s[off + d] <= L'9' && d - p < 6) {
            val = val * 10 + (int)(s[off + d] - L'0');
            d++;
        }
        if (d < len && (s[off + d] == L'.' || s[off + d] == L')') &&
            d + 1 < len && (s[off + d + 1] == L' ' || s[off + d + 1] == L'\t')) {
            ordered = true;
            num = val;
            p = d + 2;
        } else {
            return false;
        }
    } else {
        return false;
    }

    while (p < len && (s[off + p] == L' ' || s[off + p] == L'\t')) p++;

    bool is_task = false, checked = false;
    uint32_t cb_off = 0;
    if (p + 3 <= len && s[off + p] == L'[' &&
        (s[off + p + 1] == L' ' || s[off + p + 1] == L'x' || s[off + p + 1] == L'X') &&
        s[off + p + 2] == L']' &&
        (p + 3 == len || s[off + p + 3] == L' ' || s[off + p + 3] == L'\t')) {
        is_task = true;
        checked = (s[off + p + 1] == L'x' || s[off + p + 1] == L'X');
        cb_off = (uint32_t)(off + p + 1);
        p += 3;
        while (p < len && (s[off + p] == L' ' || s[off + p] == L'\t')) p++;
    }

    int depth = (int)(cols / 2);
    if (depth > 4) depth = 4;
    if (out_depth) *out_depth = depth;
    if (out_ord) *out_ord = ordered;
    if (out_num) *out_num = num;
    if (out_task) *out_task = is_task;
    if (out_checked) *out_checked = checked;
    if (out_cb_off) *out_cb_off = cb_off;
    if (out_ts) *out_ts = off + p;
    return true;
}

static bool line_is_quote(const wchar_t *s, size_t off, size_t len, int *out_depth, size_t *out_ts) {
    size_t ind = 0;
    if (count_indent(s, off, len, &ind) > 3) return false;
    size_t p = ind;
    int depth = 0;
    while (p < len && s[off + p] == L'>') {
        depth++;
        p++;
        if (p < len && (s[off + p] == L' ' || s[off + p] == L'\t')) p++;
    }
    if (depth == 0) return false;
    if (depth > 3) depth = 3;
    if (out_depth) *out_depth = depth;
    if (out_ts) *out_ts = off + p;
    return true;
}

static int parse_table_sep(const wchar_t *s, size_t off, size_t len, int8_t *aligns, int max_cols) {
    if (len < 3) return 0;
    bool has_pipe = false, has_dash = false;
    for (size_t i = 0; i < len; i++) {
        wchar_t c = s[off + i];
        if (c == L'|') has_pipe = true;
        else if (c == L'-') has_dash = true;
        else if (c != L':' && c != L' ' && c != L'\t') return 0;
    }
    if (!has_pipe || !has_dash) return 0;

    size_t i = 0, e = len;
    while (i < e && (s[off + i] == L' ' || s[off + i] == L'\t')) i++;
    while (e > i && (s[off + e - 1] == L' ' || s[off + e - 1] == L'\t')) e--;
    if (i < e && s[off + i] == L'|') i++;
    if (e > i && s[off + e - 1] == L'|') e--;

    int cols = 0;
    while (i <= e && cols < max_cols) {
        size_t cs = i;
        while (i < e && s[off + i] != L'|') i++;
        size_t ce = i;
        while (cs < ce && (s[off + cs] == L' ' || s[off + cs] == L'\t')) cs++;
        while (ce > cs && (s[off + ce - 1] == L' ' || s[off + ce - 1] == L'\t')) ce--;
        if (ce <= cs) return 0;
        bool lc = (s[off + cs] == L':');
        bool rc = (s[off + ce - 1] == L':');
        size_t d0 = lc ? cs + 1 : cs;
        size_t d1 = rc ? ce - 1 : ce;
        if (d1 <= d0) return 0;
        for (size_t k = d0; k < d1; k++) {
            if (s[off + k] != L'-') return 0;
        }
        aligns[cols++] = (lc && rc) ? 1 : (rc ? 2 : 0);
        if (i < e && s[off + i] == L'|') i++;
        else break;
    }
    return cols;
}

static bool line_has_pipe(const wchar_t *s, size_t off, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[off + i] == L'\\' && i + 1 < len) { i++; continue; }
        if (s[off + i] == L'|') return true;
    }
    return false;
}

static void parse_table_row_cells(const wchar_t *s, size_t off, size_t len, int row_idx,
                                  int ncols, const int8_t *aligns) {
    size_t i = 0, e = len;
    while (i < e && (s[off + i] == L' ' || s[off + i] == L'\t')) i++;
    while (e > i && (s[off + e - 1] == L' ' || s[off + e - 1] == L'\t')) e--;
    if (i < e && s[off + i] == L'|') i++;
    if (e > i && s[off + e - 1] == L'|' && !(e >= i + 2 && s[off + e - 2] == L'\\')) e--;

    for (int col = 0; col < ncols; col++) {
        size_t cs = i;
        while (i < e) {
            if (s[off + i] == L'\\' && i + 1 < e) { i += 2; continue; }
            if (s[off + i] == L'|') break;
            i++;
        }
        size_t ce = i;
        if (i < e && s[off + i] == L'|') i++;
        while (cs < ce && (s[off + cs] == L' ' || s[off + cs] == L'\t')) cs++;
        while (ce > cs && (s[off + ce - 1] == L' ' || s[off + ce - 1] == L'\t')) ce--;

        MdTableCell *cell = (MdTableCell *)arena_push(&g_cells);
        if (!cell) return;
        cell->row = row_idx;
        cell->col = col;
        cell->align = aligns[col];
        cell->first_span = (uint32_t)g_spans.count;
        if (ce > cs) {
            md_parse_inline_range(off + cs, off + ce, 0, 0, 0, 0);
        }
        cell->span_count = (uint32_t)g_spans.count - cell->first_span;
    }
}

static void build_plain_title_from_spans(uint32_t first_span, uint32_t count, wchar_t *out, size_t cap) {
    size_t di = 0;
    const MdSpan *sp_arr = (const MdSpan *)g_spans.ptr;
    for (uint32_t i = 0; i < count && di + 1 < cap; i++) {
        const MdSpan *sp = &sp_arr[first_span + i];
        if (sp->flags & MD_SPAN_BR) {
            if (di > 0 && out[di - 1] != L' ') out[di++] = L' ';
            continue;
        }
        for (uint32_t k = 0; k < sp->text_len && di + 1 < cap; k++) {
            wchar_t ch = g_doc_text[sp->text_off + k];
            if (ch == L'\r' || ch == L'\n') ch = L' ';
            out[di++] = ch;
        }
    }
    while (di > 0 && out[di - 1] == L' ') di--;
    out[di] = L'\0';
}

static void tv_md_parse(void) {
    tv_free_md_arenas();
    if (!g_doc_text || g_doc_len == 0) return;

    /* Step 1: split source into line slices */
    TvArena slines = { NULL, 0, 0, sizeof(SrcLine) };
    size_t pos = 0;
    int lno = 1;
    while (pos <= g_doc_len) {
        size_t start = pos;
        while (pos < g_doc_len && g_doc_text[pos] != L'\r' && g_doc_text[pos] != L'\n') pos++;
        SrcLine *sl = (SrcLine *)arena_push(&slines);
        if (!sl) break;
        sl->off = start;
        sl->len = pos - start;
        sl->line_no = lno++;
        if (pos >= g_doc_len) break;
        if (g_doc_text[pos] == L'\r' && pos + 1 < g_doc_len && g_doc_text[pos + 1] == L'\n') pos += 2;
        else pos++;
    }

    const SrcLine *L = (const SrcLine *)slines.ptr;
    size_t nL = slines.count;
    size_t i = 0;
    g_toc_min_level = 6;

    while (i < nL) {
        if (line_is_blank(g_doc_text, L[i].off, L[i].len)) {
            i++;
            continue;
        }

        /* 1. Fenced code block */
        wchar_t fch = 0;
        size_t fcnt = 0;
        wchar_t flang[24] = {0};
        if (line_is_fence(g_doc_text, L[i].off, L[i].len, &fch, &fcnt, flang, 24)) {
            int start_ln = L[i].line_no;
            size_t start_off = L[i].off;
            i++;
            size_t code_start = (i < nL) ? L[i].off : g_doc_len;
            size_t code_end = code_start;
            while (i < nL) {
                size_t ind = 0;
                if (count_indent(g_doc_text, L[i].off, L[i].len, &ind) <= 3 && ind + fcnt <= L[i].len) {
                    size_t c = 0;
                    while (ind + c < L[i].len && g_doc_text[L[i].off + ind + c] == fch) c++;
                    size_t rest = ind + c;
                    while (rest < L[i].len && (g_doc_text[L[i].off + rest] == L' ' || g_doc_text[L[i].off + rest] == L'\t')) rest++;
                    if (c >= fcnt && rest == L[i].len) {
                        i++;
                        break;
                    }
                }
                code_end = L[i].off + L[i].len;
                i++;
            }
            MdBlock *b = (MdBlock *)arena_push(&g_blocks);
            if (b) {
                b->type = MDB_CODE_BLOCK;
                b->src_line = start_ln;
                b->src_off = (uint32_t)start_off;
                wcsncpy_s(b->lang, 24, flang, _TRUNCATE);
                b->code_off = (uint32_t)code_start;
                b->code_len = (code_end >= code_start) ? (uint32_t)(code_end - code_start) : 0;
            }
            continue;
        }

        /* 2. ATX Heading (#..######) */
        size_t hts = 0, hte = 0;
        int hlvl = line_is_atx_heading(g_doc_text, L[i].off, L[i].len, &hts, &hte);
        if (hlvl > 0) {
            int bidx = (int)g_blocks.count;
            MdBlock *b = (MdBlock *)arena_push(&g_blocks);
            if (b) {
                b->type = MDB_HEADING;
                b->level = (int8_t)hlvl;
                b->src_line = L[i].line_no;
                b->src_off = (uint32_t)L[i].off;
                b->first_span = (uint32_t)g_spans.count;
                if (hte > hts) md_parse_inline_range(hts, hte, 0, 0, 0, 0);
                b->span_count = (uint32_t)g_spans.count - b->first_span;

                MdTocItem *ti = (MdTocItem *)arena_push(&g_toc);
                if (ti) {
                    ti->block_idx = bidx;
                    ti->src_line = L[i].line_no;
                    ti->level = (int8_t)hlvl;
                    build_plain_title_from_spans(b->first_span, b->span_count, ti->title, 128);
                    if (!ti->title[0]) wcsncpy_s(ti->title, 128, L"(Untitled Heading)", _TRUNCATE);
                    if (hlvl < g_toc_min_level) g_toc_min_level = hlvl;
                }
            }
            i++;
            continue;
        }

        /* 3. Horizontal Rule */
        if (line_is_hr(g_doc_text, L[i].off, L[i].len)) {
            MdBlock *b = (MdBlock *)arena_push(&g_blocks);
            if (b) {
                b->type = MDB_HR;
                b->src_line = L[i].line_no;
                b->src_off = (uint32_t)L[i].off;
            }
            i++;
            continue;
        }

        /* 4. Blockquote (including GitHub alerts [!NOTE], [!TIP], [!IMPORTANT], [!WARNING], [!CAUTION]) */
        int qdepth = 0;
        size_t qts = 0;
        if (line_is_quote(g_doc_text, L[i].off, L[i].len, &qdepth, &qts)) {
            int start_ln = L[i].line_no;
            size_t start_off = L[i].off;
            MdBlock *b = (MdBlock *)arena_push(&g_blocks);
            if (b) {
                b->type = MDB_BLOCKQUOTE;
                b->level = (int8_t)qdepth;
                b->src_line = start_ln;
                b->src_off = (uint32_t)start_off;
                b->first_span = (uint32_t)g_spans.count;

                /* Check first line for GitHub alert tag */
                size_t qlen = (L[i].off + L[i].len) - qts;
                bool skip_first = false;
                if (qlen >= 6 && g_doc_text[qts] == L'[' && g_doc_text[qts + 1] == L'!') {
                    if (qlen >= 7 && _wcsnicmp(g_doc_text + qts, L"[!NOTE]", 7) == 0)            { b->alert_kind = MD_ALERT_NOTE; skip_first = true; }
                    else if (_wcsnicmp(g_doc_text + qts, L"[!TIP]", 6) == 0)                     { b->alert_kind = MD_ALERT_TIP; skip_first = true; }
                    else if (qlen >= 12 && _wcsnicmp(g_doc_text + qts, L"[!IMPORTANT]", 12) == 0){ b->alert_kind = MD_ALERT_IMPORTANT; skip_first = true; }
                    else if (qlen >= 10 && _wcsnicmp(g_doc_text + qts, L"[!WARNING]", 10) == 0)  { b->alert_kind = MD_ALERT_WARNING; skip_first = true; }
                    else if (qlen >= 10 && _wcsnicmp(g_doc_text + qts, L"[!CAUTION]", 10) == 0)  { b->alert_kind = MD_ALERT_CAUTION; skip_first = true; }
                }

                bool first_emitted = false;
                while (i < nL) {
                    int d2 = 0;
                    size_t ts2 = 0;
                    if (!line_is_quote(g_doc_text, L[i].off, L[i].len, &d2, &ts2)) break;
                    if (skip_first) { skip_first = false; i++; continue; }
                    size_t te2 = L[i].off + L[i].len;
                    while (te2 > ts2 && (g_doc_text[te2 - 1] == L' ' || g_doc_text[te2 - 1] == L'\t')) te2--;
                    if (te2 > ts2) {
                        if (first_emitted) md_emit_span(0, 0, MD_SPAN_BR, 0, 0);
                        md_parse_inline_range(ts2, te2, 0, 0, 0, 0);
                        first_emitted = true;
                    }
                    i++;
                }
                b->span_count = (uint32_t)g_spans.count - b->first_span;
            } else {
                i++;
            }
            continue;
        }

        /* 5. GFM Pipe Table */
        if (i + 1 < nL && line_has_pipe(g_doc_text, L[i].off, L[i].len)) {
            int8_t aligns[12];
            int ncols = parse_table_sep(g_doc_text, L[i + 1].off, L[i + 1].len, aligns, 12);
            if (ncols >= 1) {
                MdBlock *b = (MdBlock *)arena_push(&g_blocks);
                if (b) {
                    b->type = MDB_TABLE;
                    b->src_line = L[i].line_no;
                    b->src_off = (uint32_t)L[i].off;
                    b->first_cell = (uint32_t)g_cells.count;
                    b->table_cols = ncols;
                    int r = 0;
                    parse_table_row_cells(g_doc_text, L[i].off, L[i].len, r++, ncols, aligns);
                    i += 2; /* skip header + separator */
                    while (i < nL && !line_is_blank(g_doc_text, L[i].off, L[i].len) &&
                           line_has_pipe(g_doc_text, L[i].off, L[i].len)) {
                        if (line_is_atx_heading(g_doc_text, L[i].off, L[i].len, NULL, NULL) > 0) break;
                        parse_table_row_cells(g_doc_text, L[i].off, L[i].len, r++, ncols, aligns);
                        i++;
                    }
                    b->table_rows = r;
                } else {
                    i += 2;
                }
                continue;
            }
        }

        /* 6. List item (unordered, ordered, or interactive task checkbox) */
        int ldepth = 0, lnum = 1;
        bool lord = false, ltask = false, lchk = false;
        uint32_t lcb_off = 0;
        size_t lts = 0;
        if (line_is_list(g_doc_text, L[i].off, L[i].len, &ldepth, &lord, &lnum, &ltask, &lchk, &lcb_off, &lts)) {
            MdBlock *b = (MdBlock *)arena_push(&g_blocks);
            if (b) {
                b->type = MDB_LIST_ITEM;
                b->level = (int8_t)ldepth;
                b->is_ordered = lord;
                b->ordinal = lnum;
                b->is_task = ltask;
                b->task_checked = lchk;
                b->task_char_off = lcb_off;
                b->src_line = L[i].line_no;
                b->src_off = (uint32_t)L[i].off;
                b->first_span = (uint32_t)g_spans.count;

                size_t te = L[i].off + L[i].len;
                if (te > lts) md_parse_inline_range(lts, te, 0, 0, 0, 0);
                i++;
                /* Gather indented continuation lines belonging to this list item */
                while (i < nL && !line_is_blank(g_doc_text, L[i].off, L[i].len)) {
                    if (line_is_atx_heading(g_doc_text, L[i].off, L[i].len, NULL, NULL) > 0 ||
                        line_is_fence(g_doc_text, L[i].off, L[i].len, NULL, NULL, NULL, 0) ||
                        line_is_hr(g_doc_text, L[i].off, L[i].len) ||
                        line_is_quote(g_doc_text, L[i].off, L[i].len, NULL, NULL) ||
                        line_is_list(g_doc_text, L[i].off, L[i].len, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
                        break;
                    }
                    size_t ic = 0;
                    size_t col_ind = count_indent(g_doc_text, L[i].off, L[i].len, &ic);
                    if (col_ind < 2) break;
                    size_t cs = L[i].off + ic;
                    size_t ce = L[i].off + L[i].len;
                    if (ce > cs) {
                        if (g_spans.count > b->first_span && cs > 0) {
                            /* emit separating space */
                            md_emit_span((uint32_t)(cs - 1), 1, 0, 0, 0);
                        }
                        md_parse_inline_range(cs, ce, 0, 0, 0, 0);
                    }
                    i++;
                }
                b->span_count = (uint32_t)g_spans.count - b->first_span;
            } else {
                i++;
            }
            continue;
        }

        /* 7. Setext Heading (line followed by === or ---) */
        if (i + 1 < nL) {
            int slvl = line_is_setext(g_doc_text, L[i + 1].off, L[i + 1].len);
            if (slvl > 0) {
                int bidx = (int)g_blocks.count;
                MdBlock *b = (MdBlock *)arena_push(&g_blocks);
                if (b) {
                    b->type = MDB_HEADING;
                    b->level = (int8_t)slvl;
                    b->src_line = L[i].line_no;
                    b->src_off = (uint32_t)L[i].off;
                    b->first_span = (uint32_t)g_spans.count;
                    size_t ps = L[i].off, pe = L[i].off + L[i].len;
                    while (ps < pe && (g_doc_text[ps] == L' ' || g_doc_text[ps] == L'\t')) ps++;
                    while (pe > ps && (g_doc_text[pe - 1] == L' ' || g_doc_text[pe - 1] == L'\t')) pe--;
                    if (pe > ps) md_parse_inline_range(ps, pe, 0, 0, 0, 0);
                    b->span_count = (uint32_t)g_spans.count - b->first_span;

                    MdTocItem *ti = (MdTocItem *)arena_push(&g_toc);
                    if (ti) {
                        ti->block_idx = bidx;
                        ti->src_line = L[i].line_no;
                        ti->level = (int8_t)slvl;
                        build_plain_title_from_spans(b->first_span, b->span_count, ti->title, 128);
                        if (!ti->title[0]) wcsncpy_s(ti->title, 128, L"(Untitled Heading)", _TRUNCATE);
                        if (slvl < g_toc_min_level) g_toc_min_level = slvl;
                    }
                }
                i += 2;
                continue;
            }
        }

        /* 8. Paragraph (join contiguous plain lines until blank or next block element) */
        size_t p_start = L[i].off;
        size_t p_end = L[i].off + L[i].len;
        int start_ln = L[i].line_no;
        i++;
        while (i < nL && !line_is_blank(g_doc_text, L[i].off, L[i].len)) {
            if (line_is_atx_heading(g_doc_text, L[i].off, L[i].len, NULL, NULL) > 0 ||
                line_is_fence(g_doc_text, L[i].off, L[i].len, NULL, NULL, NULL, 0) ||
                line_is_hr(g_doc_text, L[i].off, L[i].len) ||
                line_is_quote(g_doc_text, L[i].off, L[i].len, NULL, NULL) ||
                line_is_list(g_doc_text, L[i].off, L[i].len, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
                break;
            }
            if (i + 1 < nL && line_has_pipe(g_doc_text, L[i].off, L[i].len)) {
                int8_t tmp_al[12];
                if (parse_table_sep(g_doc_text, L[i + 1].off, L[i + 1].len, tmp_al, 12) >= 1) break;
            }
            p_end = L[i].off + L[i].len;
            i++;
        }
        MdBlock *b = (MdBlock *)arena_push(&g_blocks);
        if (b) {
            b->type = MDB_PARAGRAPH;
            b->src_line = start_ln;
            b->src_off = (uint32_t)p_start;
            b->first_span = (uint32_t)g_spans.count;
            md_parse_inline_range(p_start, p_end, 0, 0, 0, 0);
            b->span_count = (uint32_t)g_spans.count - b->first_span;
        }
    }

    arena_free(&slines);
    if (g_toc.count == 0) g_toc_min_level = 1;
}

/* =========================================================================
 * Markdown Word-Wrap & Layout Engine (Centered 780px Column)
 * ========================================================================= */
static float md_heading_size(int level) {
    float base = (float)g_body_px;
    switch (level) {
    case 1: return base * 1.875f; /* h1 */
    case 2: return base * 1.500f; /* h2 */
    case 3: return base * 1.375f; /* h3 */
    case 4: return base * 1.250f; /* h4 */
    case 5: return base * 1.125f; /* h5 */
    case 6:
    default: return base * 1.000f; /* h6 */
    }
}

static int md_span_style(uint8_t flags, bool force_bold) {
    int st = GP_FONT_REGULAR;
    if (force_bold || (flags & MD_SPAN_BOLD)) st |= GP_FONT_BOLD;
    if (flags & MD_SPAN_ITALIC) st |= GP_FONT_ITALIC;
    return st;
}

static float md_layout_spans(int block_idx, int cell_idx, uint32_t first_span, uint32_t span_count,
                             float start_x, float start_y, float max_w,
                             float font_px, float line_h, bool force_bold,
                             uint32_t *out_first_line, uint32_t *out_line_count) {
    uint32_t fl = (uint32_t)g_lines.count;
    float cur_y = start_y;
    float cur_x = 0.0f;
    if (max_w < 60.0f) max_w = 60.0f;

    MdLine *cur_line = (MdLine *)arena_push(&g_lines);
    if (!cur_line) {
        if (out_first_line) *out_first_line = fl;
        if (out_line_count) *out_line_count = 0;
        return line_h;
    }
    cur_line->block_idx = block_idx;
    cur_line->cell_idx = cell_idx;
    cur_line->y = cur_y;
    cur_line->line_h = line_h;
    cur_line->font_px = font_px;
    cur_line->first_frag = (uint32_t)g_frags.count;
    cur_line->frag_count = 0;
    size_t cur_line_idx = g_lines.count - 1;

    const MdSpan *sp_arr = (const MdSpan *)g_spans.ptr;

    for (uint32_t si = 0; si < span_count; si++) {
        MdSpan sp = sp_arr[first_span + si];

        if (sp.flags & MD_SPAN_BR) {
            cur_y += line_h;
            cur_x = 0.0f;
            MdLine *nl = (MdLine *)arena_push(&g_lines);
            if (!nl) break;
            cur_line_idx = g_lines.count - 1;
            nl->block_idx = block_idx;
            nl->cell_idx = cell_idx;
            nl->y = cur_y;
            nl->line_h = line_h;
            nl->font_px = font_px;
            nl->first_frag = (uint32_t)g_frags.count;
            nl->frag_count = 0;
            continue;
        }

        bool is_code = (sp.flags & MD_SPAN_CODE) != 0;
        float rpx = is_code ? max(10.5f, font_px * 0.85f) : font_px;
        int st = is_code ? GP_FONT_REGULAR : md_span_style(sp.flags, force_bold);
        float pill_pad = is_code ? 10.0f : 0.0f; /* 5px left + 5px right for inline code pill */

        size_t pos = 0;
        size_t slen = sp.text_len;
        const wchar_t *base_txt = g_doc_text + sp.text_off;

        while (pos < slen) {
            /* Trim leading spaces at the start of a wrapped line (except inside code pills) */
            if (cur_x <= 0.1f && !is_code) {
                while (pos < slen && base_txt[pos] == L' ') pos++;
                if (pos >= slen) break;
            }

            float avail = max_w - cur_x;
            /* Inline code pill: try to keep short pills atomic on one line */
            if (is_code) {
                float full_w = tv_measure_run_exact(base_txt + pos, slen - pos, true, rpx, st) + pill_pad;
                if (full_w <= avail || (cur_x > 0.1f && full_w <= max_w)) {
                    if (full_w > avail && cur_x > 0.1f) {
                        cur_y += line_h;
                        cur_x = 0.0f;
                        MdLine *nl = (MdLine *)arena_push(&g_lines);
                        if (!nl) break;
                        cur_line_idx = g_lines.count - 1;
                        nl->block_idx = block_idx;
                        nl->cell_idx = cell_idx;
                        nl->y = cur_y;
                        nl->line_h = line_h;
                        nl->font_px = font_px;
                        nl->first_frag = (uint32_t)g_frags.count;
                        nl->frag_count = 0;
                    }
                    MdFrag *fr = (MdFrag *)arena_push(&g_frags);
                    if (fr) {
                        fr->text_off = sp.text_off + (uint32_t)pos;
                        fr->text_len = (uint32_t)(slen - pos);
                        fr->url_off  = sp.url_off;
                        fr->url_len  = sp.url_len;
                        fr->x        = start_x + cur_x;
                        fr->w        = full_w;
                        fr->flags    = sp.flags;
                        ((MdLine *)g_lines.ptr)[cur_line_idx].frag_count++;
                    }
                    cur_x += full_w;
                    pos = slen;
                    break;
                }
            }

            /* Find longest word-boundary prefix that fits in avail */
            size_t best_end = pos;
            size_t scan = pos;
            while (scan < slen) {
                size_t next = scan;
                while (next < slen && base_txt[next] != L' ') next++;
                while (next < slen && base_txt[next] == L' ') next++;
                size_t trim = next;
                while (trim > pos && base_txt[trim - 1] == L' ') trim--;
                float est = tv_estimate_run_fast(base_txt + pos, trim - pos, is_code, rpx, st) + pill_pad;
                if (est <= avail) {
                    best_end = next;
                    scan = next;
                } else {
                    break;
                }
            }

            if (best_end == pos) {
                /* First word doesn't fit on current line */
                if (cur_x > 0.1f) {
                    cur_y += line_h;
                    cur_x = 0.0f;
                    MdLine *nl = (MdLine *)arena_push(&g_lines);
                    if (!nl) break;
                    cur_line_idx = g_lines.count - 1;
                    nl->block_idx = block_idx;
                    nl->cell_idx = cell_idx;
                    nl->y = cur_y;
                    nl->line_h = line_h;
                    nl->font_px = font_px;
                    nl->first_frag = (uint32_t)g_frags.count;
                    nl->frag_count = 0;
                    continue;
                }
                /* Single long word exceeds max_w: break by character */
                size_t cend = pos + 1;
                while (cend < slen) {
                    float est = tv_estimate_run_fast(base_txt + pos, (cend + 1) - pos, is_code, rpx, st) + pill_pad;
                    if (est > max_w) break;
                    cend++;
                }
                best_end = cend;
            }

            size_t emit_len = best_end - pos;
            /* If breaking to next line, strip trailing spaces from this line's last fragment */
            bool will_wrap = (best_end < slen);
            size_t draw_len = emit_len;
            if (will_wrap && !is_code) {
                while (draw_len > 0 && base_txt[pos + draw_len - 1] == L' ') draw_len--;
            }

            if (draw_len > 0) {
                float exact_w = tv_measure_run_exact(base_txt + pos, draw_len, is_code, rpx, st) + pill_pad;
                /* If measurement slightly exceeds avail on non-empty line, wrap once cleanly */
                if (cur_x > 0.1f && exact_w > avail + 1.5f && draw_len == slen - pos) {
                    cur_y += line_h;
                    cur_x = 0.0f;
                    MdLine *nl = (MdLine *)arena_push(&g_lines);
                    if (!nl) break;
                    cur_line_idx = g_lines.count - 1;
                    nl->block_idx = block_idx;
                    nl->cell_idx = cell_idx;
                    nl->y = cur_y;
                    nl->line_h = line_h;
                    nl->font_px = font_px;
                    nl->first_frag = (uint32_t)g_frags.count;
                    nl->frag_count = 0;
                    continue;
                }
                MdFrag *fr = (MdFrag *)arena_push(&g_frags);
                if (fr) {
                    fr->text_off = sp.text_off + (uint32_t)pos;
                    fr->text_len = (uint32_t)draw_len;
                    fr->url_off  = sp.url_off;
                    fr->url_len  = sp.url_len;
                    fr->x        = start_x + cur_x;
                    fr->w        = exact_w;
                    fr->flags    = sp.flags;
                    ((MdLine *)g_lines.ptr)[cur_line_idx].frag_count++;
                }
                cur_x += exact_w;
            }

            pos = best_end;
            if (pos < slen) {
                cur_y += line_h;
                cur_x = 0.0f;
                MdLine *nl = (MdLine *)arena_push(&g_lines);
                if (!nl) break;
                cur_line_idx = g_lines.count - 1;
                nl->block_idx = block_idx;
                nl->cell_idx = cell_idx;
                nl->y = cur_y;
                nl->line_h = line_h;
                nl->font_px = font_px;
                nl->first_frag = (uint32_t)g_frags.count;
                nl->frag_count = 0;
            }
        }
    }

    uint32_t lcnt = (uint32_t)g_lines.count - fl;
    if (out_first_line) *out_first_line = fl;
    if (out_line_count) *out_line_count = lcnt;
    return max(line_h, (cur_y - start_y) + line_h);
}

static void tv_md_rebuild_layout(bool force) {
    if (!g_is_markdown || g_blocks.count == 0) {
        arena_free(&g_lines);
        arena_free(&g_frags);
        g_doc_h = 0.0f;
        return;
    }

    int view_w = max(240, (int)(g_rc_doc.right - g_rc_doc.left) - TV_SCROLLBAR_W);
    int pad_x = (view_w >= 860) ? 48 : max(24, view_w / 14);
    int col_w = min(TV_DOC_MAX_W, max(220, view_w - pad_x * 2));
    int col_x = g_rc_doc.left + (view_w - col_w) / 2;

    if (!force && col_w == g_layout_col_w && col_x == g_layout_col_x && g_body_px == g_layout_font_px && g_lines.count > 0) {
        return;
    }

    arena_free(&g_lines);
    arena_free(&g_frags);
    g_layout_col_w = col_w;
    g_layout_col_x = col_x;
    g_layout_font_px = g_body_px;

    float base_px = (float)g_body_px;
    float base_lh = base_px * 1.60f; /* line height 1.6 */
    float code_px = max(11.0f, base_px * 0.875f); /* 14px at 16px body */
    float code_lh = code_px * 1.55f;
    float y = 32.0f; /* generous top breathing room */

    MdBlock *blks = (MdBlock *)g_blocks.ptr;
    size_t nblks = g_blocks.count;

    for (size_t bi = 0; bi < nblks; bi++) {
        MdBlock *b = &blks[bi];
        b->divider_y = 0.0f;
        b->first_line = (uint32_t)g_lines.count;
        b->line_count = 0;

        if (b->type == MDB_HEADING) {
            float top_gap = (bi == 0) ? 0.0f : (b->level == 1 ? 26.0f : (b->level == 2 ? 24.0f : 18.0f));
            y += top_gap;
            b->y = y;
            float hpx = md_heading_size(b->level);
            float hlh = hpx * 1.36f;
            float hh = md_layout_spans((int)bi, -1, b->first_span, b->span_count,
                                       (float)col_x, y, (float)col_w, hpx, hlh, true,
                                       &b->first_line, &b->line_count);
            y += hh;
            if (b->level == 1 || b->level == 2) {
                y += (b->level == 1) ? 8.0f : 6.0f;
                b->divider_y = y;
                y += 12.0f;
            } else {
                y += 8.0f;
            }
            b->h = y - b->y;
        } else if (b->type == MDB_PARAGRAPH) {
            if (bi > 0) y += 4.0f;
            b->y = y;
            float ph = md_layout_spans((int)bi, -1, b->first_span, b->span_count,
                                       (float)col_x, y, (float)col_w, base_px, base_lh, false,
                                       &b->first_line, &b->line_count);
            b->h = ph;
            y += ph + 10.0f;
        } else if (b->type == MDB_BLOCKQUOTE) {
            y += 6.0f;
            b->y = y;
            float q_indent = 20.0f + (float)(b->level - 1) * 14.0f;
            float inner_y = y + (b->alert_kind != MD_ALERT_NONE ? 30.0f : 4.0f);
            float qh = md_layout_spans((int)bi, -1, b->first_span, b->span_count,
                                       (float)col_x + q_indent, inner_y, (float)col_w - q_indent - 8.0f,
                                       base_px, base_lh, false, &b->first_line, &b->line_count);
            b->h = (inner_y - y) + qh + (b->alert_kind != MD_ALERT_NONE ? 10.0f : 4.0f);
            y += b->h + 12.0f;
        } else if (b->type == MDB_LIST_ITEM) {
            bool prev_list = (bi > 0 && blks[bi - 1].type == MDB_LIST_ITEM);
            if (!prev_list && bi > 0) y += 4.0f;
            b->y = y;
            float l_indent = 28.0f + (float)b->level * 24.0f;
            b->cb_x = (float)col_x + l_indent - 24.0f;
            b->cb_y = y + (base_lh - 16.0f) * 0.5f;
            float lh_tot = md_layout_spans((int)bi, -1, b->first_span, b->span_count,
                                           (float)col_x + l_indent, y, (float)col_w - l_indent,
                                           base_px, base_lh, false, &b->first_line, &b->line_count);
            b->h = lh_tot;
            bool next_list = (bi + 1 < nblks && blks[bi + 1].type == MDB_LIST_ITEM);
            y += lh_tot + (next_list ? 5.0f : 12.0f);
        } else if (b->type == MDB_HR) {
            y += 10.0f;
            b->y = y;
            b->h = 20.0f;
            y += b->h + 10.0f;
        } else if (b->type == MDB_CODE_BLOCK) {
            y += 8.0f;
            b->y = y;
            float pad_top = b->lang[0] ? 30.0f : 14.0f;
            float pad_bot = 14.0f;
            float pad_lr  = 16.0f;
            float inner_w = max(80.0f, (float)col_w - pad_lr * 2.0f);
            float cy = y + pad_top;
            b->copy_x = (float)(col_x + col_w) - 56.0f;
            b->copy_y = y + 6.0f;

            /* Split code block into wrapped monospace lines */
            uint32_t fl = (uint32_t)g_lines.count;
            size_t pos = b->code_off;
            size_t end = b->code_off + b->code_len;
            if (pos >= end) {
                MdLine *nl = (MdLine *)arena_push(&g_lines);
                if (nl) {
                    nl->block_idx = (int)bi;
                    nl->cell_idx = -1;
                    nl->y = cy;
                    nl->line_h = code_lh;
                    nl->font_px = code_px;
                    nl->first_frag = (uint32_t)g_frags.count;
                    nl->frag_count = 0;
                    nl->is_code_line = true;
                }
                cy += code_lh;
            } else {
                while (pos <= end) {
                    size_t ls = pos;
                    while (pos < end && g_doc_text[pos] != L'\r' && g_doc_text[pos] != L'\n') pos++;
                    size_t llen = pos - ls;

                    if (llen == 0) {
                        MdLine *nl = (MdLine *)arena_push(&g_lines);
                        if (nl) {
                            nl->block_idx = (int)bi;
                            nl->cell_idx = -1;
                            nl->y = cy;
                            nl->line_h = code_lh;
                            nl->font_px = code_px;
                            nl->first_frag = (uint32_t)g_frags.count;
                            nl->frag_count = 0;
                            nl->is_code_line = true;
                        }
                        cy += code_lh;
                    } else {
                        /* Soft-wrap extremely long code lines so nothing clips */
                        size_t sub = 0;
                        while (sub < llen) {
                            size_t chunk = llen - sub;
                            float est = tv_estimate_run_fast(g_doc_text + ls + sub, chunk, true, code_px, GP_FONT_REGULAR);
                            if (est > inner_w) {
                                size_t fit = 1;
                                while (fit < chunk) {
                                    if (tv_estimate_run_fast(g_doc_text + ls + sub, fit + 1, true, code_px, GP_FONT_REGULAR) > inner_w) break;
                                    fit++;
                                }
                                chunk = fit;
                            }
                            MdLine *nl = (MdLine *)arena_push(&g_lines);
                            if (nl) {
                                nl->block_idx = (int)bi;
                                nl->cell_idx = -1;
                                nl->y = cy;
                                nl->line_h = code_lh;
                                nl->font_px = code_px;
                                nl->first_frag = (uint32_t)g_frags.count;
                                nl->frag_count = 1;
                                nl->is_code_line = true;
                                MdFrag *fr = (MdFrag *)arena_push(&g_frags);
                                if (fr) {
                                    fr->text_off = (uint32_t)(ls + sub);
                                    fr->text_len = (uint32_t)chunk;
                                    fr->x = (float)col_x + pad_lr;
                                    fr->w = tv_estimate_run_fast(g_doc_text + ls + sub, chunk, true, code_px, GP_FONT_REGULAR);
                                    fr->flags = 0;
                                }
                            }
                            cy += code_lh;
                            sub += chunk;
                        }
                    }

                    if (pos >= end) break;
                    if (g_doc_text[pos] == L'\r' && pos + 1 < end && g_doc_text[pos + 1] == L'\n') pos += 2;
                    else pos++;
                }
            }

            b->first_line = fl;
            b->line_count = (uint32_t)g_lines.count - fl;
            b->h = (cy - y) + pad_bot;
            y += b->h + 14.0f;
        } else if (b->type == MDB_TABLE && b->table_cols > 0 && b->table_rows > 0) {
            y += 8.0f;
            b->y = y;
            int ncols = b->table_cols;
            int nrows = b->table_rows;
            float cell_px = max(12.0f, base_px * 0.92f);
            float cell_lh = cell_px * 1.48f;
            float cell_pad_x = 12.0f;
            float cell_pad_y = 7.0f;

            /* Estimate natural width per column */
            float natural[12];
            for (int c = 0; c < ncols; c++) natural[c] = 64.0f;
            MdTableCell *cells = (MdTableCell *)g_cells.ptr;
            const MdSpan *sp_arr = (const MdSpan *)g_spans.ptr;

            for (int r = 0; r < nrows; r++) {
                for (int c = 0; c < ncols; c++) {
                    MdTableCell *cell = &cells[b->first_cell + (uint32_t)(r * ncols + c)];
                    float wsum = 0.0f;
                    for (uint32_t s = 0; s < cell->span_count; s++) {
                        const MdSpan *sp = &sp_arr[cell->first_span + s];
                        bool ic = (sp->flags & MD_SPAN_CODE) != 0;
                        wsum += tv_estimate_run_fast(g_doc_text + sp->text_off, sp->text_len,
                                                     ic, ic ? cell_px * 0.88f : cell_px,
                                                     r == 0 ? GP_FONT_BOLD : GP_FONT_REGULAR) + (ic ? 10.0f : 0.0f);
                    }
                    float want = min(360.0f, wsum + cell_pad_x * 2.0f + 6.0f);
                    if (want > natural[c]) natural[c] = want;
                }
            }

            float sum_nat = 0.0f;
            for (int c = 0; c < ncols; c++) sum_nat += natural[c];
            float col_widths[12];
            float target_w = (float)col_w;
            if (sum_nat <= 1.0f) sum_nat = target_w;
            for (int c = 0; c < ncols; c++) {
                col_widths[c] = max(56.0f, target_w * (natural[c] / sum_nat));
            }
            /* Normalize sum to col_w */
            float norm_sum = 0.0f;
            for (int c = 0; c < ncols; c++) norm_sum += col_widths[c];
            float scale = target_w / norm_sum;
            for (int c = 0; c < ncols; c++) col_widths[c] *= scale;

            float ty = y;
            for (int r = 0; r < nrows; r++) {
                float cx = (float)col_x;
                float row_h = cell_lh + cell_pad_y * 2.0f;
                for (int c = 0; c < ncols; c++) {
                    int ci = (int)b->first_cell + r * ncols + c;
                    MdTableCell *cell = &cells[ci];
                    cell->box_x = cx;
                    cell->box_w = col_widths[c];
                    float inner_w = max(24.0f, col_widths[c] - cell_pad_x * 2.0f);
                    float ch = md_layout_spans((int)bi, ci, cell->first_span, cell->span_count,
                                               cx + cell_pad_x, ty + cell_pad_y, inner_w,
                                               cell_px, cell_lh, (r == 0),
                                               &cell->first_line, &cell->line_count);
                    if (ch + cell_pad_y * 2.0f > row_h) row_h = ch + cell_pad_y * 2.0f;
                    /* Apply center / right alignment per wrapped line in cell */
                    if (cell->align != 0 && cell->line_count > 0) {
                        MdLine *larr = (MdLine *)g_lines.ptr;
                        MdFrag *farr = (MdFrag *)g_frags.ptr;
                        for (uint32_t li = 0; li < cell->line_count; li++) {
                            MdLine *ln = &larr[cell->first_line + li];
                            if (ln->frag_count == 0) continue;
                            MdFrag *f0 = &farr[ln->first_frag];
                            MdFrag *flast = &farr[ln->first_frag + ln->frag_count - 1];
                            float used = (flast->x + flast->w) - f0->x;
                            float dx = inner_w - used;
                            if (dx > 0.5f) {
                                float shift = (cell->align == 1) ? (dx * 0.5f) : dx;
                                for (uint32_t fi = 0; fi < ln->frag_count; fi++) {
                                    farr[ln->first_frag + fi].x += shift;
                                }
                            }
                        }
                    }
                    cx += col_widths[c];
                }
                /* Store row height in cell[row*ncols].box_x? No, keep cell y/h clean via line y + block */
                ty += row_h;
            }
            b->h = ty - y;
            y = ty + 14.0f;
        }
    }

    g_doc_h = y + 120.0f; /* generous bottom breathing room */

    /* Update TOC heading doc_y targets */
    MdTocItem *tocs = (MdTocItem *)g_toc.ptr;
    for (size_t t = 0; t < g_toc.count; t++) {
        int bi = tocs[t].block_idx;
        if (bi >= 0 && (size_t)bi < g_blocks.count) {
            tocs[t].doc_y = max(0.0f, blks[bi].y - 18.0f);
        }
    }
    g_toc_content_h = (float)g_toc.count * 30.0f + 16.0f;

    /* Clamp scroll_y */
    float view_h = max(60.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
    float max_sc = max(0.0f, g_doc_h - view_h);
    if (g_scroll_y > max_sc) g_scroll_y = max_sc;
    if (g_scroll_y < 0.0f) g_scroll_y = 0.0f;

    if (g_find_open && g_find_query[0]) {
        tv_md_rebuild_find_matches(false);
    }
}

static void tv_update_active_toc(void) {
    if (g_toc.count == 0) { g_active_toc_idx = -1; return; }
    const MdTocItem *tocs = (const MdTocItem *)g_toc.ptr;
    int best = 0;
    if (g_is_markdown && !g_source_mode) {
        float probe = g_scroll_y + 52.0f;
        for (size_t i = 0; i < g_toc.count; i++) {
            if (tocs[i].doc_y <= probe) best = (int)i;
            else break;
        }
    } else {
        for (size_t i = 0; i < g_toc.count; i++) {
            if (tocs[i].src_line <= g_caret_line) best = (int)i;
            else break;
        }
    }
    g_active_toc_idx = best;
}

/* =========================================================================
 * Quick Find Engine (Both Rendered Markdown Mode & Win32 Source Edit Mode)
 * ========================================================================= */
static bool match_at(const wchar_t *hay, const wchar_t *needle, size_t nlen, bool match_case) {
    if (match_case) return wmemcmp(hay, needle, nlen) == 0;
    return _wcsnicmp(hay, needle, nlen) == 0;
}

static void tv_md_scroll_to_match(int idx_1based) {
    if (idx_1based < 1 || (size_t)idx_1based > g_md_matches.count) return;
    const MdFindMatch *m = &((const MdFindMatch *)g_md_matches.ptr)[idx_1based - 1];
    float view_h = max(80.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
    float target = m->y - view_h * 0.35f;
    float max_sc = max(0.0f, g_doc_h - view_h);
    if (target < 0.0f) target = 0.0f;
    if (target > max_sc) target = max_sc;
    g_scroll_y = target;
    tv_update_active_toc();
}

static void tv_md_rebuild_find_matches(bool jump_first) {
    arena_free(&g_md_matches);
    size_t qlen = wcslen(g_find_query);
    if (qlen == 0 || !g_doc_text || g_lines.count == 0) {
        g_find_match_idx = 0;
        g_find_match_total = 0;
        return;
    }

    const MdLine *larr = (const MdLine *)g_lines.ptr;
    const MdFrag *farr = (const MdFrag *)g_frags.ptr;
    const MdBlock *blks = (const MdBlock *)g_blocks.ptr;

    for (size_t li = 0; li < g_lines.count; li++) {
        const MdLine *ln = &larr[li];
        bool force_b = (ln->block_idx >= 0 && (size_t)ln->block_idx < g_blocks.count &&
                        blks[ln->block_idx].type == MDB_HEADING);
        if (ln->cell_idx >= 0 && (size_t)ln->cell_idx < g_cells.count &&
            ((const MdTableCell *)g_cells.ptr)[ln->cell_idx].row == 0) {
            force_b = true;
        }
        for (uint32_t fi = 0; fi < ln->frag_count; fi++) {
            const MdFrag *fr = &farr[ln->first_frag + fi];
            if (fr->text_len < qlen) continue;
            const wchar_t *txt = g_doc_text + fr->text_off;
            bool is_code = ln->is_code_line || ((fr->flags & MD_SPAN_CODE) != 0);
            float fpx = (fr->flags & MD_SPAN_CODE) ? max(10.5f, ln->font_px * 0.85f) : ln->font_px;
            int st = is_code ? GP_FONT_REGULAR : md_span_style(fr->flags, force_b);
            float pad = (fr->flags & MD_SPAN_CODE) ? 5.0f : 0.0f;

            for (size_t k = 0; k + qlen <= fr->text_len; k++) {
                if (match_at(txt + k, g_find_query, qlen, g_find_case)) {
                    MdFindMatch *m = (MdFindMatch *)arena_push(&g_md_matches);
                    if (m) {
                        float ox = (k > 0) ? tv_measure_run_exact(txt, k, is_code, fpx, st) : 0.0f;
                        float mw = tv_measure_run_exact(txt + k, qlen, is_code, fpx, st);
                        m->line_idx = (int32_t)li;
                        m->x = fr->x + pad + ox;
                        m->y = ln->y + 1.0f;
                        m->w = max(8.0f, mw);
                        m->h = max(14.0f, ln->line_h - 2.0f);
                    }
                    k += qlen - 1;
                }
            }
        }
    }

    g_find_match_total = (int)g_md_matches.count;
    if (g_find_match_total == 0) {
        g_find_match_idx = 0;
    } else if (jump_first || g_find_match_idx < 1 || g_find_match_idx > g_find_match_total) {
        g_find_match_idx = 1;
        if (jump_first) tv_md_scroll_to_match(1);
    }
}

static void tv_update_caret_info(void) {
    if (!g_hEdit) return;
    DWORD s0 = 0, s1 = 0;
    SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    int line = (int)SendMessageW(g_hEdit, EM_LINEFROMCHAR, (WPARAM)s1, 0);
    int lidx = (int)SendMessageW(g_hEdit, EM_LINEINDEX, (WPARAM)line, 0);
    int total = (int)SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0);
    int nl = line + 1;
    int nc = max(1, (int)s1 - lidx + 1);
    int nt = max(1, total);
    if (nl != g_caret_line || nc != g_caret_col || nt != g_line_count) {
        g_caret_line = nl;
        g_caret_col = nc;
        g_line_count = nt;
        tv_update_active_toc();
        if (g_hTvWnd) {
            RECT cr;
            GetClientRect(g_hTvWnd, &cr);
            RECT st = { 0, max(0, cr.bottom - TV_STATUS_H - 2), cr.right, cr.bottom };
            InvalidateRect(g_hTvWnd, &st, FALSE);
        }
    }
}

static void tv_find_step(bool forward, bool allow_current) {
    if (!g_hFindEdit) return;
    GetWindowTextW(g_hFindEdit, g_find_query, 128);
    size_t qlen = wcslen(g_find_query);
    if (qlen == 0) {
        arena_free(&g_md_matches);
        g_find_match_idx = 0;
        g_find_match_total = 0;
        if (g_hTvWnd) InvalidateRect(g_hTvWnd, NULL, FALSE);
        return;
    }

    if (g_is_markdown && !g_source_mode) {
        if (allow_current) {
            tv_md_rebuild_find_matches(true);
        } else {
            if (g_md_matches.count == 0) tv_md_rebuild_find_matches(true);
            else {
                if (forward) {
                    g_find_match_idx++;
                    if (g_find_match_idx > g_find_match_total) g_find_match_idx = 1;
                } else {
                    g_find_match_idx--;
                    if (g_find_match_idx < 1) g_find_match_idx = g_find_match_total;
                }
                tv_md_scroll_to_match(g_find_match_idx);
            }
        }
        if (g_hTvWnd) InvalidateRect(g_hTvWnd, NULL, FALSE);
        return;
    }

    if (!g_hEdit) return;
    int tlen = GetWindowTextLengthW(g_hEdit);
    if (tlen <= 0) {
        g_find_match_idx = 0;
        g_find_match_total = 0;
        if (g_hTvWnd) InvalidateRect(g_hTvWnd, &g_rc_find_bar, FALSE);
        return;
    }

    size_t buf_bytes = ((size_t)tlen + 2) * sizeof(wchar_t);
    wchar_t *text = (wchar_t *)VirtualAlloc(NULL, buf_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!text) return;
    GetWindowTextW(g_hEdit, text, tlen + 1);

    DWORD sel0 = 0, sel1 = 0;
    SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&sel0, (LPARAM)&sel1);

    int total = 0, best_pos = -1, best_ord = 0, first_pos = -1, last_pos = -1;
    size_t utlen = (size_t)tlen;
    if (qlen <= utlen) {
        size_t limit = utlen - qlen;
        for (size_t i = 0; i <= limit; i++) {
            if (match_at(text + i, g_find_query, qlen, g_find_case)) {
                total++;
                if (first_pos < 0) first_pos = (int)i;
                last_pos = (int)i;
                if (forward) {
                    size_t start_from = allow_current ? (size_t)sel0 : (size_t)sel1;
                    if (best_pos < 0 && i >= start_from) {
                        best_pos = (int)i;
                        best_ord = total;
                    }
                } else {
                    if (i < (size_t)sel0) {
                        best_pos = (int)i;
                        best_ord = total;
                    }
                }
            }
        }
    }

    if (total > 0) {
        if (best_pos < 0) {
            best_pos = forward ? first_pos : last_pos;
            best_ord = forward ? 1 : total;
        }
        g_find_match_total = total;
        g_find_match_idx = best_ord;
        SendMessageW(g_hEdit, EM_SETSEL, (WPARAM)best_pos, (LPARAM)(best_pos + (int)qlen));
        SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
        tv_update_caret_info();
    } else {
        g_find_match_total = 0;
        g_find_match_idx = 0;
    }

    VirtualFree(text, 0, MEM_RELEASE);
    if (g_hTvWnd) InvalidateRect(g_hTvWnd, &g_rc_find_bar, FALSE);
}

static void tv_destroy_find_edit(void) {
    if (!g_hFindEdit) return;
    if (g_orig_find_proc) {
        SetWindowLongPtrW(g_hFindEdit, GWLP_WNDPROC, (LONG_PTR)g_orig_find_proc);
        g_orig_find_proc = NULL;
    }
    SetWindowTextW(g_hFindEdit, L"");
    DestroyWindow(g_hFindEdit);
    g_hFindEdit = NULL;
}

static void tv_ensure_find_edit(void) {
    if (g_hFindEdit || !g_hTvWnd) return;
    g_hFindEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL,
        0, 0, 200, 18,
        g_hTvWnd, (HMENU)(INT_PTR)ID_TV_FIND, g_hInst, NULL);
    if (g_hFindEdit) {
        SendMessageW(g_hFindEdit, WM_SETFONT, (WPARAM)g_hUiFont, TRUE);
        SendMessageW(g_hFindEdit, 0x1501 /* EM_SETCUEBANNER */, (WPARAM)TRUE, (LPARAM)L"Find in document...");
        if (g_find_query[0]) SetWindowTextW(g_hFindEdit, g_find_query);
        g_orig_find_proc = (WNDPROC)SetWindowLongPtrW(g_hFindEdit, GWLP_WNDPROC, (LONG_PTR)tv_find_sub_proc);
    }
}

static void tv_open_find_bar(void) {
    if (!g_hTvWnd) return;
    tv_ensure_find_edit();
    if (!g_hFindEdit) return;
    g_find_open = true;

    if (g_hEdit && IsWindowVisible(g_hEdit)) {
        DWORD s0 = 0, s1 = 0;
        SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
        if (s1 > s0 && (s1 - s0) < 100) {
            int tlen = GetWindowTextLengthW(g_hEdit);
            if ((int)s1 <= tlen) {
                wchar_t *tmp = (wchar_t *)VirtualAlloc(NULL, ((size_t)tlen + 2) * sizeof(wchar_t),
                                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (tmp) {
                    GetWindowTextW(g_hEdit, tmp, tlen + 1);
                    wchar_t sel_str[128];
                    size_t n = min((size_t)(s1 - s0), (size_t)127);
                    bool single_line = true;
                    for (size_t i = 0; i < n; i++) {
                        wchar_t c = tmp[s0 + i];
                        if (c == L'\r' || c == L'\n') { single_line = false; break; }
                        sel_str[i] = c;
                    }
                    sel_str[n] = L'\0';
                    if (single_line && n > 0) SetWindowTextW(g_hFindEdit, sel_str);
                    VirtualFree(tmp, 0, MEM_RELEASE);
                }
            }
        }
    }

    RECT cr;
    GetClientRect(g_hTvWnd, &cr);
    tv_update_layout(cr.right, cr.bottom);
    SetFocus(g_hFindEdit);
    SendMessageW(g_hFindEdit, EM_SETSEL, 0, -1);
    tv_find_step(true, true);
    InvalidateRect(g_hTvWnd, NULL, FALSE);
}

static void tv_close_find_bar(void) {
    if (!g_find_open) return;
    g_find_open = false;
    g_find_match_idx = 0;
    g_find_match_total = 0;
    arena_free(&g_md_matches);
    tv_destroy_find_edit();
    if (g_hTvWnd) {
        RECT cr;
        GetClientRect(g_hTvWnd, &cr);
        tv_update_layout(cr.right, cr.bottom);
        if (g_hEdit && IsWindowVisible(g_hEdit)) SetFocus(g_hEdit);
        else SetFocus(g_hTvWnd);
        InvalidateRect(g_hTvWnd, NULL, FALSE);
    }
}

/* =========================================================================
 * Sync Between Source EDIT Control and Master UTF-16 Document Buffer
 * ========================================================================= */
static void tv_sync_edit_to_doc(void) {
    if (!g_hEdit) return;
    int tlen = GetWindowTextLengthW(g_hEdit);
    if (tlen < 0) tlen = 0;
    wchar_t *tmp = (wchar_t *)VirtualAlloc(NULL, ((size_t)tlen + 4) * sizeof(wchar_t),
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tmp) return;
    int got = GetWindowTextW(g_hEdit, tmp, tlen + 1);
    if (got < 0) got = 0;
    tmp[got] = L'\0';
    tv_doc_set_text(tmp, (size_t)got);
    VirtualFree(tmp, 0, MEM_RELEASE);
}

static void tv_sync_doc_to_edit(DWORD sel0, DWORD sel1) {
    if (!g_hEdit) return;
    g_loading = true;
    SetWindowTextW(g_hEdit, g_doc_text ? g_doc_text : L"");
    SendMessageW(g_hEdit, EM_SETREADONLY, g_read_only ? TRUE : FALSE, 0);
    SendMessageW(g_hEdit, EM_SETSEL, (WPARAM)sel0, (LPARAM)sel1);
    SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
    g_loading = false;
    tv_update_caret_info();
}

static void tv_scroll_edit_to_line(int line_1based) {
    if (!g_hEdit || line_1based < 1) return;
    int idx = (int)SendMessageW(g_hEdit, EM_LINEINDEX, (WPARAM)(line_1based - 1), 0);
    if (idx < 0) idx = 0;
    SendMessageW(g_hEdit, EM_SETSEL, (WPARAM)idx, (LPARAM)idx);
    int first_vis = (int)SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    int target = max(0, line_1based - 4);
    SendMessageW(g_hEdit, EM_LINESCROLL, 0, (LPARAM)(target - first_vis));
    tv_update_caret_info();
}

static LRESULT CALLBACK tv_edit_sub_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void tv_destroy_edit_control(void) {
    if (!g_hEdit) return;
    if (g_orig_edit_proc) {
        SetWindowLongPtrW(g_hEdit, GWLP_WNDPROC, (LONG_PTR)g_orig_edit_proc);
        g_orig_edit_proc = NULL;
    }
    g_loading = true;
    SetWindowTextW(g_hEdit, L"");
    SendMessageW(g_hEdit, EM_EMPTYUNDOBUFFER, 0, 0);
    DestroyWindow(g_hEdit);
    g_hEdit = NULL;
    g_loading = false;
}

static bool tv_create_edit_control(bool wrap, const wchar_t *initial_text, DWORD sel0, DWORD sel1) {
    if (!g_hTvWnd) return false;
    tv_destroy_edit_control();

    bool show_now = (!g_is_markdown || g_source_mode);
    /* No native WS_VSCROLL so dark/light themes share our sleek custom pill scrollbar */
    DWORD style = WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL | ES_WANTRETURN;
    if (show_now) style |= WS_VISIBLE;
    if (!wrap) style |= WS_HSCROLL | ES_AUTOHSCROLL;

    g_hEdit = CreateWindowExW(
        0, L"EDIT", L"", style,
        g_rc_edit_box.left, g_rc_edit_box.top,
        max(40, (int)(g_rc_edit_box.right - g_rc_edit_box.left)),
        max(40, (int)(g_rc_edit_box.bottom - g_rc_edit_box.top)),
        g_hTvWnd, (HMENU)(INT_PTR)ID_TV_EDIT, g_hInst, NULL);
    if (!g_hEdit) return false;

    SendMessageW(g_hEdit, EM_SETLIMITTEXT, (WPARAM)(4 * 1024 * 1024), 0);
    if (g_hMonoFont) SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hMonoFont, FALSE);
    SendMessageW(g_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(16, 16));

    g_loading = true;
    SetWindowTextW(g_hEdit, initial_text ? initial_text : L"");
    SendMessageW(g_hEdit, EM_SETREADONLY, g_read_only ? TRUE : FALSE, 0);
    SendMessageW(g_hEdit, EM_SETSEL, (WPARAM)sel0, (LPARAM)sel1);
    SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
    g_loading = false;

    g_orig_edit_proc = (WNDPROC)SetWindowLongPtrW(g_hEdit, GWLP_WNDPROC, (LONG_PTR)tv_edit_sub_proc);
    return true;
}

/* =========================================================================
 * File Save & Load (UTF-8, UTF-8 BOM, UTF-16 LE/BE, ANSI, CRLF/LF)
 * ========================================================================= */
static bool tv_save_file(void) {
    if (!g_cur_path[0]) {
        tv_flash(L"No file path set to save");
        return false;
    }
    if (g_read_only) {
        tv_flash(L"File is read-only");
        return false;
    }

    if (!g_is_markdown || g_source_mode) {
        tv_sync_edit_to_doc();
    }

    size_t src_chars = g_doc_len;
    wchar_t *wbuf = (wchar_t *)VirtualAlloc(NULL, (src_chars + 4) * sizeof(wchar_t),
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!wbuf) return false;
    if (g_doc_text && src_chars > 0) memcpy(wbuf, g_doc_text, src_chars * sizeof(wchar_t));
    wbuf[src_chars] = L'\0';

    size_t out_chars = src_chars;
    if (!g_use_crlf) {
        size_t di = 0;
        for (size_t i = 0; i < src_chars; i++) {
            if (wbuf[i] == L'\r' && i + 1 < src_chars && wbuf[i + 1] == L'\n') continue;
            wbuf[di++] = wbuf[i];
        }
        wbuf[di] = L'\0';
        out_chars = di;
    }

    BYTE *out_bytes = NULL;
    size_t out_len = 0;
    bool ok = false;

    if (g_enc == TV_ENC_UTF16LE || g_enc == TV_ENC_UTF16BE) {
        out_len = 2 + out_chars * 2;
        out_bytes = (BYTE *)VirtualAlloc(NULL, max((size_t)4, out_len), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (out_bytes) {
            if (g_enc == TV_ENC_UTF16LE) {
                out_bytes[0] = 0xFF; out_bytes[1] = 0xFE;
                if (out_chars > 0) memcpy(out_bytes + 2, wbuf, out_chars * 2);
            } else {
                out_bytes[0] = 0xFE; out_bytes[1] = 0xFF;
                for (size_t i = 0; i < out_chars; i++) {
                    wchar_t c = wbuf[i];
                    out_bytes[2 + i * 2]     = (BYTE)((c >> 8) & 0xFF);
                    out_bytes[2 + i * 2 + 1] = (BYTE)(c & 0xFF);
                }
            }
        }
    } else {
        UINT cp = (g_enc == TV_ENC_ANSI) ? CP_ACP : CP_UTF8;
        int need = (out_chars > 0)
                 ? WideCharToMultiByte(cp, 0, wbuf, (int)out_chars, NULL, 0, NULL, NULL)
                 : 0;
        size_t bom = (g_enc == TV_ENC_UTF8_BOM) ? 3 : 0;
        if (need >= 0) {
            out_len = bom + (size_t)need;
            out_bytes = (BYTE *)VirtualAlloc(NULL, max((size_t)4, out_len), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (out_bytes) {
                if (bom == 3) { out_bytes[0] = 0xEF; out_bytes[1] = 0xBB; out_bytes[2] = 0xBF; }
                if (need > 0) {
                    WideCharToMultiByte(cp, 0, wbuf, (int)out_chars, (char *)(out_bytes + bom), need, NULL, NULL);
                }
            }
        }
    }

    VirtualFree(wbuf, 0, MEM_RELEASE);

    if (out_bytes) {
        HANDLE hf = CreateFileW(g_cur_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            size_t written_tot = 0;
            ok = true;
            while (written_tot < out_len) {
                DWORD chunk = (DWORD)min(out_len - written_tot, (size_t)(1024 * 1024));
                DWORD wr = 0;
                if (!WriteFile(hf, out_bytes + written_tot, chunk, &wr, NULL) || wr == 0) {
                    ok = false;
                    break;
                }
                written_tot += wr;
            }
            CloseHandle(hf);
        }
        VirtualFree(out_bytes, 0, MEM_RELEASE);
    }

    if (ok) {
        g_modified = false;
        g_cur_file_size = (uint64_t)out_len;
        tv_update_window_title();
        tv_flash(L"Saved");
    } else {
        tv_flash(L"Could not save file (check permissions)");
    }
    return ok;
}

static bool tv_confirm_discard(HWND hwnd) {
    if (!g_modified) return true;
    wchar_t prompt[MAX_PATH + 96];
    _snwprintf_s(prompt, MAX_PATH + 96, _TRUNCATE,
                 L"Save changes to \"%ls\" before continuing?",
                 g_cur_name[0] ? g_cur_name : L"Untitled");
    int ans = MessageBoxW(hwnd, prompt, L"Unsaved Changes", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (ans == IDCANCEL) return false;
    if (ans == IDYES) return tv_save_file();
    return true;
}

static void tv_load_file(const wchar_t *path) {
    tv_free_doc_all();
    g_cur_path[0] = L'\0';
    g_cur_dir[0] = L'\0';
    g_cur_name[0] = L'\0';
    g_cur_file_size = 0;
    g_enc = TV_ENC_UTF8;
    g_use_crlf = true;
    g_modified = false;
    g_read_only = false;
    g_is_truncated = false;
    g_is_binary_guard = false;
    g_is_markdown = false;
    g_source_mode = false;
    g_find_match_idx = 0;
    g_find_match_total = 0;
    g_flash_msg[0] = L'\0';
    g_flash_until = 0;
    g_hover_toc = -1;
    g_hover_cb_blk = -1;
    g_hover_copy_blk = -1;
    g_hover_link = false;
    g_hover_url[0] = L'\0';

    if (!path || !path[0]) {
        wcsncpy_s(g_cur_name, MAX_PATH, L"Untitled.md", _TRUNCATE);
        g_is_markdown = true;
        g_source_mode = true;
        g_sidebar_open = false;
        tv_doc_set_text(L"", 0);
        tv_rebuild_mono_font();
        tv_sync_doc_to_edit(0, 0);
        tv_update_window_title();
        if (g_hTvWnd) {
            RECT cr; GetClientRect(g_hTvWnd, &cr);
            tv_update_layout(cr.right, cr.bottom);
            InvalidateRect(g_hTvWnd, NULL, FALSE);
        }
        return;
    }

    wcsncpy_s(g_cur_path, MAX_PATH, path, _TRUNCATE);
    wcsncpy_s(g_cur_dir, MAX_PATH, path, _TRUNCATE);
    PathRemoveFileSpecW(g_cur_dir);
    const wchar_t *fn = PathFindFileNameW(path);
    wcsncpy_s(g_cur_name, MAX_PATH, (fn && fn[0]) ? fn : path, _TRUNCATE);
    g_is_markdown = tv_is_md_ext(PathFindExtensionW(g_cur_name));
    g_source_mode = !g_is_markdown;

    DWORD attrs = GetFileAttributesW(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        g_read_only = true;
    }

    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        g_read_only = true;
        tv_doc_set_text(L"Unable to open this file.", 25);
        g_source_mode = true;
        tv_rebuild_mono_font();
        tv_sync_doc_to_edit(0, 0);
        tv_update_window_title();
        if (g_hTvWnd) {
            RECT cr; GetClientRect(g_hTvWnd, &cr);
            tv_update_layout(cr.right, cr.bottom);
            InvalidateRect(g_hTvWnd, NULL, FALSE);
        }
        return;
    }

    LARGE_INTEGER fsz;
    fsz.QuadPart = 0;
    GetFileSizeEx(hf, &fsz);
    g_cur_file_size = (fsz.QuadPart > 0) ? (uint64_t)fsz.QuadPart : 0;

    DWORD to_read = 0;
    if (g_cur_file_size > TV_MAX_EDIT_BYTES) {
        g_is_truncated = true;
        g_read_only = true;
        to_read = TV_PREVIEW_BYTES;
    } else {
        to_read = (DWORD)g_cur_file_size;
    }

    BYTE *raw = NULL;
    DWORD bytes_read = 0;
    if (to_read > 0) {
        raw = (BYTE *)VirtualAlloc(NULL, (SIZE_T)to_read + 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (raw) {
            while (bytes_read < to_read) {
                DWORD rd = 0;
                if (!ReadFile(hf, raw + bytes_read, to_read - bytes_read, &rd, NULL) || rd == 0) break;
                bytes_read += rd;
            }
        }
    }
    CloseHandle(hf);

    wchar_t *decoded = NULL;
    size_t dec_chars = 0;

    if (raw && bytes_read > 0) {
        if (bytes_read >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
            g_enc = TV_ENC_UTF16LE;
            dec_chars = (bytes_read - 2) / 2;
            decoded = (wchar_t *)VirtualAlloc(NULL, (dec_chars + 2) * sizeof(wchar_t),
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (decoded) {
                memcpy(decoded, raw + 2, dec_chars * 2);
                decoded[dec_chars] = L'\0';
            }
        } else if (bytes_read >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
            g_enc = TV_ENC_UTF16BE;
            dec_chars = (bytes_read - 2) / 2;
            decoded = (wchar_t *)VirtualAlloc(NULL, (dec_chars + 2) * sizeof(wchar_t),
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (decoded) {
                for (size_t i = 0; i < dec_chars; i++) {
                    decoded[i] = (wchar_t)(((wchar_t)raw[2 + i * 2] << 8) | (wchar_t)raw[2 + i * 2 + 1]);
                }
                decoded[dec_chars] = L'\0';
            }
        } else {
            size_t off = 0;
            if (bytes_read >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
                g_enc = TV_ENC_UTF8_BOM;
                off = 3;
            } else {
                g_enc = TV_ENC_UTF8;
            }

            size_t plen = bytes_read - off;
            if (g_is_truncated && plen > 3) {
                for (int k = 0; k < 4 && plen > 0; k++) {
                    BYTE last = raw[off + plen - 1];
                    if ((last & 0x80) == 0) break;
                    plen--;
                    if ((last & 0xC0) == 0xC0) break;
                }
            }

            for (size_t i = 0; i < plen; i++) {
                if (raw[off + i] == 0) {
                    raw[off + i] = ' ';
                    g_is_binary_guard = true;
                    g_read_only = true;
                }
            }

            UINT cp = CP_UTF8;
            int wlen = 0;
            if (plen > 0) {
                wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           (const char *)(raw + off), (int)plen, NULL, 0);
                if (wlen <= 0) {
                    cp = CP_ACP;
                    g_enc = TV_ENC_ANSI;
                    wlen = MultiByteToWideChar(CP_ACP, 0, (const char *)(raw + off), (int)plen, NULL, 0);
                }
            }
            if (wlen > 0) {
                decoded = (wchar_t *)VirtualAlloc(NULL, ((size_t)wlen + 2) * sizeof(wchar_t),
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (decoded) {
                    MultiByteToWideChar(cp, 0, (const char *)(raw + off), (int)plen, decoded, wlen);
                    decoded[wlen] = L'\0';
                    dec_chars = (size_t)wlen;
                }
            }
        }
    }

    if (raw) { VirtualFree(raw, 0, MEM_RELEASE); raw = NULL; }

    /* Detect CRLF vs LF and normalize to CRLF in g_doc_text */
    wchar_t *norm = NULL;
    size_t norm_len = 0;
    if (decoded && dec_chars > 0) {
        size_t crlf_n = 0, lf_only_n = 0, extra_cr = 0;
        for (size_t i = 0; i < dec_chars; i++) {
            if (decoded[i] == L'\0') {
                decoded[i] = L' ';
                g_is_binary_guard = true;
                g_read_only = true;
            } else if (decoded[i] == L'\r') {
                if (i + 1 < dec_chars && decoded[i + 1] == L'\n') { crlf_n++; i++; }
                else extra_cr++;
            } else if (decoded[i] == L'\n') {
                lf_only_n++;
                extra_cr++;
            }
        }
        g_use_crlf = (crlf_n >= lf_only_n);

        if (extra_cr == 0) {
            norm = decoded;
            norm_len = dec_chars;
            decoded = NULL;
        } else {
            norm = (wchar_t *)VirtualAlloc(NULL, (dec_chars + extra_cr + 4) * sizeof(wchar_t),
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (norm) {
                size_t di = 0;
                for (size_t i = 0; i < dec_chars; i++) {
                    if (decoded[i] == L'\r') {
                        norm[di++] = L'\r';
                        norm[di++] = L'\n';
                        if (i + 1 < dec_chars && decoded[i + 1] == L'\n') i++;
                    } else if (decoded[i] == L'\n') {
                        norm[di++] = L'\r';
                        norm[di++] = L'\n';
                    } else {
                        norm[di++] = decoded[i];
                    }
                }
                norm[di] = L'\0';
                norm_len = di;
            }
            VirtualFree(decoded, 0, MEM_RELEASE);
            decoded = NULL;
        }
    } else if (decoded) {
        VirtualFree(decoded, 0, MEM_RELEASE);
        decoded = NULL;
    }

    tv_doc_set_text(norm ? norm : L"", norm_len);
    if (norm) { VirtualFree(norm, 0, MEM_RELEASE); norm = NULL; }

    if (g_is_markdown) {
        tv_md_parse();
        /* The outline shows when there are headings */
        g_sidebar_open = (g_toc.count > 0);
    } else {
        g_sidebar_open = false;
    }

    tv_rebuild_mono_font();

    if (g_hTvWnd) {
        RECT cr;
        GetClientRect(g_hTvWnd, &cr);
        tv_update_layout(cr.right, cr.bottom);
        if (g_is_markdown && !g_source_mode) {
            tv_destroy_edit_control();
            tv_md_rebuild_layout(true);
            tv_update_active_toc();
            SetFocus(g_hTvWnd);
        } else {
            if (!g_hEdit) {
                tv_create_edit_control(g_word_wrap, g_doc_text ? g_doc_text : L"", 0, 0);
            } else {
                tv_sync_doc_to_edit(0, 0);
                ShowWindow(g_hEdit, SW_SHOW);
            }
            if (g_hEdit) {
                SendMessageW(g_hEdit, EM_EMPTYUNDOBUFFER, 0, 0);
                SetFocus(g_hEdit);
            }
        }
        tv_update_window_title();
        InvalidateRect(g_hTvWnd, NULL, FALSE);
    }
}

/* =========================================================================
 * Mode Toggles, Sidebar Toggle, Zoom, Word Wrap
 * ========================================================================= */
static void tv_toggle_source_mode(int jump_to_line) {
    if (!g_is_markdown || !g_hTvWnd) return;

    if (!g_source_mode) {
        /* Switch Rendered -> Source Mode */
        g_source_mode = true;
        tv_rebuild_mono_font();
        RECT cr;
        GetClientRect(g_hTvWnd, &cr);
        tv_update_layout(cr.right, cr.bottom);
        if (!g_hEdit) {
            tv_create_edit_control(g_word_wrap, g_doc_text ? g_doc_text : L"", 0, 0);
        } else {
            tv_sync_doc_to_edit(0, 0);
            ShowWindow(g_hEdit, SW_SHOW);
        }
        if (g_hEdit) {
            if (jump_to_line > 0) {
                tv_scroll_edit_to_line(jump_to_line);
            } else if (g_active_toc_idx >= 0 && (size_t)g_active_toc_idx < g_toc.count) {
                tv_scroll_edit_to_line(((const MdTocItem *)g_toc.ptr)[g_active_toc_idx].src_line);
            }
            SetFocus(g_hEdit);
        }
        tv_flash(L"Source Mode (Ctrl+E for Preview)");
    } else {
        /* Switch Source -> Rendered Mode */
        tv_sync_edit_to_doc();
        g_source_mode = false;
        tv_destroy_edit_control();
        tv_md_parse();
        RECT cr;
        GetClientRect(g_hTvWnd, &cr);
        tv_update_layout(cr.right, cr.bottom);
        tv_md_rebuild_layout(true);
        /* Jump near the line the user was editing */
        if (g_blocks.count > 0 && g_caret_line > 1) {
            const MdBlock *blks = (const MdBlock *)g_blocks.ptr;
            float target_y = 0.0f;
            for (size_t i = 0; i < g_blocks.count; i++) {
                if (blks[i].src_line <= g_caret_line) target_y = max(0.0f, blks[i].y - 36.0f);
                else break;
            }
            float view_h = max(60.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
            float max_sc = max(0.0f, g_doc_h - view_h);
            g_scroll_y = min(max_sc, target_y);
        }
        tv_update_active_toc();
        SetFocus(g_hTvWnd);
        tv_flash(L"Markdown Preview (Ctrl+E for Source)");
    }
    if (g_find_open && g_find_query[0]) tv_find_step(true, true);
    InvalidateRect(g_hTvWnd, NULL, FALSE);
}

static void tv_toggle_sidebar(void) {
    if (!g_is_markdown || !g_hTvWnd) return;
    g_sidebar_open = !g_sidebar_open;
    RECT cr;
    GetClientRect(g_hTvWnd, &cr);
    tv_update_layout(cr.right, cr.bottom);
    if (!g_source_mode) tv_md_rebuild_layout(false);
    InvalidateRect(g_hTvWnd, NULL, FALSE);
}

static void tv_toggle_word_wrap(void) {
    if (!g_hEdit || !g_hTvWnd) return;
    if (g_is_markdown && !g_source_mode) return;
    bool was_mod = g_modified;
    DWORD s0 = 0, s1 = 0;
    SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    tv_sync_edit_to_doc();
    g_word_wrap = !g_word_wrap;
    tv_create_edit_control(g_word_wrap, g_doc_text ? g_doc_text : L"", s0, s1);
    g_modified = was_mod;
    SetFocus(g_hEdit);
    tv_update_caret_info();
    tv_flash(g_word_wrap ? L"Word Wrap: On" : L"Word Wrap: Off");
}

static void tv_adjust_zoom(int delta) {
    if (g_is_markdown) {
        int np = g_body_px + delta;
        if (np < 12) np = 12;
        if (np > 26) np = 26;
        if (np == g_body_px) return;
        g_body_px = np;
        tv_rebuild_mono_font();
        if (!g_source_mode) tv_md_rebuild_layout(true);
        wchar_t msg[48];
        _snwprintf_s(msg, 48, _TRUNCATE, L"Zoom: %d%% (%d px)", (g_body_px * 100) / 16, g_body_px);
        tv_flash(msg);
    } else {
        int np = g_code_edit_px + delta;
        if (np < 10) np = 10;
        if (np > 26) np = 26;
        if (np == g_code_edit_px) return;
        g_code_edit_px = np;
        tv_rebuild_mono_font();
        wchar_t msg[48];
        _snwprintf_s(msg, 48, _TRUNCATE, L"Font size: %d px", g_code_edit_px);
        tv_flash(msg);
    }
}

static void tv_reset_zoom(void) {
    if (g_is_markdown) {
        g_body_px = 16;
        tv_rebuild_mono_font();
        if (!g_source_mode) tv_md_rebuild_layout(true);
        tv_flash(L"Zoom: 100% (16 px)");
    } else {
        g_code_edit_px = 14;
        tv_rebuild_mono_font();
        tv_flash(L"Font size: 14 px");
    }
}

/* =========================================================================
 * Layout Geometry Computation
 * ========================================================================= */
static void tv_update_layout(int w, int h) {
    /* 1. Windows 11 Caption Buttons (top-right inside 38px seamless titlebar) */
    int cap_w = 46;
    for (int i = 0; i < 3; i++) {
        int cx = w - (3 - i) * cap_w;
        g_rc_cap[i].left = cx; g_rc_cap[i].top = 0;
        g_rc_cap[i].right = cx + cap_w; g_rc_cap[i].bottom = TV_TITLE_H;
    }

    /* 2. Minimal Titlebar Controls (left: Outline + Mode/Wrap; right: Save, Find, Theme, External) */
    int ty = 6;
    int bh = 26;
    int lx = 10;

    if (g_is_markdown) {
        g_rc_tb[0].left = lx; g_rc_tb[0].top = ty; g_rc_tb[0].right = lx + 30; g_rc_tb[0].bottom = ty + bh;
        lx += 36;
        g_rc_tb[1].left = lx; g_rc_tb[1].top = ty; g_rc_tb[1].right = lx + 74; g_rc_tb[1].bottom = ty + bh;
    } else {
        ZeroMemory(&g_rc_tb[0], sizeof(RECT));
        g_rc_tb[1].left = lx; g_rc_tb[1].top = ty; g_rc_tb[1].right = lx + 64; g_rc_tb[1].bottom = ty + bh;
    }

    int rx = g_rc_cap[0].left - 8;
    /* 5: External App */
    g_rc_tb[5].right = rx; g_rc_tb[5].left = rx - 28; g_rc_tb[5].top = ty; g_rc_tb[5].bottom = ty + bh;
    rx -= 32;
    /* 4: Light/Dark Theme Toggle */
    g_rc_tb[4].right = rx; g_rc_tb[4].left = rx - 28; g_rc_tb[4].top = ty; g_rc_tb[4].bottom = ty + bh;
    rx -= 32;
    /* 3: Find (Ctrl+F) */
    g_rc_tb[3].right = rx; g_rc_tb[3].left = rx - 28; g_rc_tb[3].top = ty; g_rc_tb[3].bottom = ty + bh;
    rx -= 32;
    /* 2: Save (Ctrl+S) */
    g_rc_tb[2].right = rx; g_rc_tb[2].left = rx - 28; g_rc_tb[2].top = ty; g_rc_tb[2].bottom = ty + bh;

    int y = TV_TITLE_H;
    int content_bot = max(y + 40, h - TV_STATUS_H);

    /* 3. Collapsible Left Outline Sidebar (full height below titlebar down to status bar) */
    bool show_sb = (g_is_markdown && g_sidebar_open && w >= 560);
    if (show_sb) {
        g_rc_sidebar.left = 0;
        g_rc_sidebar.top = y;
        g_rc_sidebar.right = TV_SIDEBAR_W;
        g_rc_sidebar.bottom = content_bot;
    } else {
        ZeroMemory(&g_rc_sidebar, sizeof(g_rc_sidebar));
    }

    int main_left = show_sb ? TV_SIDEBAR_W : 0;

    /* 4. Optional Large-File / Binary Guard Banner */
    if (g_is_truncated || g_is_binary_guard) {
        g_rc_banner.left = main_left; g_rc_banner.top = y;
        g_rc_banner.right = w; g_rc_banner.bottom = y + TV_BANNER_H;
        g_rc_banner_btn.left = w - 122; g_rc_banner_btn.top = y + 4;
        g_rc_banner_btn.right = w - 12; g_rc_banner_btn.bottom = y + TV_BANNER_H - 4;
        y += TV_BANNER_H;
    } else {
        ZeroMemory(&g_rc_banner, sizeof(g_rc_banner));
        ZeroMemory(&g_rc_banner_btn, sizeof(g_rc_banner_btn));
    }

    /* 5. Optional Quick Find Bar (Ctrl+F) */
    if (g_find_open) {
        g_rc_find_bar.left = main_left; g_rc_find_bar.top = y;
        g_rc_find_bar.right = w; g_rc_find_bar.bottom = y + TV_FIND_BAR_H;

        int fb_w = min(300, max(140, (w - main_left) - 240));
        g_rc_find_box.left = main_left + 14; g_rc_find_box.top = y + 5;
        g_rc_find_box.right = g_rc_find_box.left + fb_w; g_rc_find_box.bottom = y + 33;

        int fx = g_rc_find_box.right + 8;
        int fbw[4] = { 28, 28, 32, 28 };
        for (int i = 0; i < 4; i++) {
            g_rc_find_btns[i].left = fx; g_rc_find_btns[i].top = y + 5;
            g_rc_find_btns[i].right = fx + fbw[i]; g_rc_find_btns[i].bottom = y + 33;
            fx += fbw[i] + 4;
        }
        g_rc_find_btns[3].left = w - 40;
        g_rc_find_btns[3].right = w - 12;

        if (g_hFindEdit) {
            MoveWindow(g_hFindEdit, g_rc_find_box.left + 26, g_rc_find_box.top + 5,
                       max(40, (int)(g_rc_find_box.right - g_rc_find_box.left) - 34), 18, TRUE);
            if (!IsWindowVisible(g_hFindEdit)) ShowWindow(g_hFindEdit, SW_SHOW);
        }
        y += TV_FIND_BAR_H;
    } else {
        ZeroMemory(&g_rc_find_bar, sizeof(g_rc_find_bar));
        if (g_hFindEdit && IsWindowVisible(g_hFindEdit)) ShowWindow(g_hFindEdit, SW_HIDE);
    }

    /* 6. Main Document / Editor Viewport */
    g_rc_doc.left = main_left;
    g_rc_doc.top = y;
    g_rc_doc.right = w;
    g_rc_doc.bottom = max(y + 40, content_bot);

    /* Centered column in Source Mode for Markdown; full width for code/config/log files (leave 14px right for custom scrollbar) */
    if (g_is_markdown) {
        int vw = max(260, (int)(g_rc_doc.right - g_rc_doc.left) - 16);
        int ew = min(TV_DOC_MAX_W + 40, max(240, vw - 48));
        int ex = g_rc_doc.left + (vw - ew) / 2;
        g_rc_edit_box.left = ex;
        g_rc_edit_box.top = g_rc_doc.top + 12;
        g_rc_edit_box.right = ex + ew;
        g_rc_edit_box.bottom = g_rc_doc.bottom;
    } else {
        g_rc_edit_box.left = g_rc_doc.left;
        g_rc_edit_box.top = g_rc_doc.top;
        g_rc_edit_box.right = max(g_rc_doc.left + 40, g_rc_doc.right - 14);
        g_rc_edit_box.bottom = g_rc_doc.bottom;
    }

    if (g_hEdit) {
        MoveWindow(g_hEdit, g_rc_edit_box.left, g_rc_edit_box.top,
                   max(40, (int)(g_rc_edit_box.right - g_rc_edit_box.left)),
                   max(40, (int)(g_rc_edit_box.bottom - g_rc_edit_box.top)), TRUE);
    }
}

/* =========================================================================
 * Lightweight Syntax-Tinted Code Block Line Painter (Zero Allocations)
 * ========================================================================= */
static bool tv_is_ident_ch(wchar_t c) {
    return iswalnum(c) || c == L'_';
}

static bool tv_is_code_kw(const wchar_t *s, size_t n) {
    static const wchar_t *kws[] = {
        L"const", L"static", L"void", L"int", L"bool", L"float", L"double", L"char", L"return",
        L"if", L"else", L"for", L"while", L"switch", L"case", L"break", L"continue", L"struct",
        L"typedef", L"enum", L"true", L"false", L"NULL", L"nullptr", L"def", L"class", L"import",
        L"from", L"function", L"let", L"var", L"async", L"await", L"export", L"fn", L"pub", L"mut",
        L"echo", L"set", L"if", L"then", L"fi", L"do", L"done", L"include", L"define"
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        if (wcslen(kws[i]) == n && wmemcmp(s, kws[i], n) == 0) return true;
    }
    return false;
}

static void tv_draw_code_line(GpGraphics *g, const wchar_t *txt, size_t len,
                              float x, float y, float lh, float px, const MtPalette *pal) {
    if (!txt || len == 0) return;
    /* Expand tabs cleanly to spaces in a stack buffer for crisp monospace rendering */
    wchar_t buf[512];
    size_t blen = 0;
    for (size_t i = 0; i < len && blen + 4 < 511; i++) {
        if (txt[i] == L'\t') {
            int sp = 4 - (int)(blen % 4);
            for (int s = 0; s < sp && blen < 510; s++) buf[blen++] = L' ';
        } else {
            buf[blen++] = txt[i];
        }
    }
    buf[blen] = L'\0';
    if (blen == 0) return;

    float cx = x;
    size_t i = 0;
    while (i < blen) {
        size_t s0 = i;
        ARGB col = pal->code_block_fg;

        /* Line comment (// or #) or C-style block comment */
        if ((buf[i] == L'/' && i + 1 < blen && buf[i + 1] == L'/') ||
            (buf[i] == L'#' && (i == 0 || buf[i - 1] == L' '))) {
            i = blen;
            col = pal->syn_com;
        } else if (buf[i] == L'/' && i + 1 < blen && buf[i + 1] == L'*') {
            i += 2;
            while (i + 1 < blen && !(buf[i] == L'*' && buf[i + 1] == L'/')) i++;
            i = (i + 1 < blen) ? (i + 2) : blen;
            col = pal->syn_com;
        } else if (buf[i] == L'"' || buf[i] == L'\'') {
            wchar_t q = buf[i++];
            while (i < blen) {
                if (buf[i] == L'\\' && i + 1 < blen) { i += 2; continue; }
                if (buf[i] == q) { i++; break; }
                i++;
            }
            col = pal->syn_str;
        } else if (iswdigit(buf[i]) && (i == 0 || !tv_is_ident_ch(buf[i - 1]))) {
            while (i < blen && (iswalnum(buf[i]) || buf[i] == L'.' || buf[i] == L'x' || buf[i] == L'X')) i++;
            col = pal->syn_num;
        } else if (iswalpha(buf[i]) || buf[i] == L'_') {
            while (i < blen && tv_is_ident_ch(buf[i])) i++;
            if (tv_is_code_kw(buf + s0, i - s0)) col = pal->syn_kw;
        } else {
            i++;
            while (i < blen && buf[i] != L'/' && buf[i] != L'#' && buf[i] != L'"' && buf[i] != L'\'' &&
                   !iswalnum(buf[i]) && buf[i] != L'_') {
                i++;
            }
        }

        size_t slen = i - s0;
        wchar_t saved = buf[i];
        buf[i] = L'\0';
        float w = tv_measure_run_exact(buf + s0, slen, true, px, GP_FONT_REGULAR);
        draw_text(g, buf + s0, g_mono_face, px, GP_FONT_REGULAR, col,
                  cx, y, w + 4.0f, lh, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        buf[i] = saved;
        cx += w;
    }
}

/* =========================================================================
 * Custom Chrome & Rich Pure-CPU Markdown Painting
 * ========================================================================= */
static void tv_draw_checkbox(GpGraphics *g, float x, float y, bool checked, bool hover, const MtPalette *pal) {
    /* circular 16px task checkbox */
    float sz = 16.0f;
    if (checked) {
        draw_circle_fill(g, x + sz * 0.5f, y + sz * 0.5f, sz * 0.5f, hover ? pal->accent_hover : pal->accent);
        /* Crisp checkmark stroke inside circle */
        GpPen *pen = NULL;
        if (GdipCreatePen1(pal->fg_on_accent, 2.0f, GP_UNIT_PIXEL, &pen) == GP_OK && pen) {
            GdipDrawLine(g, pen, x + 4.2f, y + 8.2f, x + 7.0f, y + 11.0f);
            GdipDrawLine(g, pen, x + 6.9f, y + 11.0f, x + 12.0f, y + 5.2f);
            GdipDeletePen(pen);
        }
    } else {
        draw_circle_fill(g, x + sz * 0.5f, y + sz * 0.5f, sz * 0.5f - 0.5f, hover ? pal->surface_alt : pal->bg);
        GpPen *pen = NULL;
        ARGB bdr = hover ? pal->accent : pal->muted;
        if (GdipCreatePen1(bdr, 1.8f, GP_UNIT_PIXEL, &pen) == GP_OK && pen) {
            GdipDrawEllipse(g, pen, x + 1.0f, y + 1.0f, sz - 2.0f, sz - 2.0f);
            GdipDeletePen(pen);
        }
    }
}

static void tv_paint_markdown_doc(GpGraphics *g, const MtPalette *pal) {
    int vx0 = g_rc_doc.left;
    int vy0 = g_rc_doc.top;
    int vw  = g_rc_doc.right - g_rc_doc.left;
    int vh  = g_rc_doc.bottom - g_rc_doc.top;
    if (vw <= 4 || vh <= 4) return;

    tv_md_rebuild_layout(false);

    GdipSetClipRect(g, (float)vx0, (float)vy0, (float)vw, (float)vh, GP_COMBINE_REPLACE);

    if (g_blocks.count == 0) {
        draw_text(g, L"Empty Markdown document — press Ctrl+E to start writing",
                  g_body_face, 14.0f, GP_FONT_ITALIC, pal->muted,
                  (float)vx0, (float)vy0 + 60.0f, (float)vw, 32.0f,
                  GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        GdipResetClip(g);
        return;
    }

    float vis_top = g_scroll_y - 40.0f;
    float vis_bot = g_scroll_y + (float)vh + 40.0f;
    float base_y  = (float)vy0 - g_scroll_y;

    const MdBlock *blks = (const MdBlock *)g_blocks.ptr;
    const MdLine  *larr = (const MdLine *)g_lines.ptr;
    const MdFrag  *farr = (const MdFrag *)g_frags.ptr;
    const MdTableCell *cells = (const MdTableCell *)g_cells.ptr;

    /* Pass 1: Block backgrounds, borders, dividers, bullets, checkboxes, table grids */
    for (size_t bi = 0; bi < g_blocks.count; bi++) {
        const MdBlock *b = &blks[bi];
        if (b->y + b->h < vis_top) continue;
        if (b->y > vis_bot) break;

        float sy = base_y + b->y;

        if (b->type == MDB_HEADING && b->divider_y > 0.0f) {
            float dy = base_y + b->divider_y;
            draw_line(g, (float)g_layout_col_x, dy, (float)(g_layout_col_x + g_layout_col_w), dy,
                      pal->divider, 1.0f);
        } else if (b->type == MDB_HR) {
            float hy = sy + b->h * 0.5f;
            /* crisp dashed/subtle thematic break */
            float x0 = (float)g_layout_col_x;
            float x1 = (float)(g_layout_col_x + g_layout_col_w);
            draw_line(g, x0, hy, x1, hy, pal->hr, 1.5f);
        } else if (b->type == MDB_BLOCKQUOTE) {
            ARGB bar_col = pal->quote_bar;
            const wchar_t *alert_lbl = NULL;
            if (b->alert_kind == MD_ALERT_NOTE) {
                bar_col = MAKE_ARGB(255, 0x3B, 0x82, 0xF6); alert_lbl = L"NOTE";
            } else if (b->alert_kind == MD_ALERT_TIP) {
                bar_col = MAKE_ARGB(255, 0x21, 0xB5, 0x6F); alert_lbl = L"TIP";
            } else if (b->alert_kind == MD_ALERT_IMPORTANT) {
                bar_col = MAKE_ARGB(255, 0x9B, 0x6D, 0xFF); alert_lbl = L"IMPORTANT";
            } else if (b->alert_kind == MD_ALERT_WARNING) {
                bar_col = MAKE_ARGB(255, 0xE5, 0xA0, 0x20); alert_lbl = L"WARNING";
            } else if (b->alert_kind == MD_ALERT_CAUTION) {
                bar_col = MAKE_ARGB(255, 0xE5, 0x48, 0x4D); alert_lbl = L"CAUTION";
            }

            if (b->alert_kind != MD_ALERT_NONE) {
                draw_round_rect_fill(g, (float)g_layout_col_x, sy, (float)g_layout_col_w, b->h, 5.0f, pal->surface_alt);
            }
            for (int d = 0; d < b->level; d++) {
                float bx = (float)g_layout_col_x + 4.0f + (float)d * 14.0f;
                draw_round_rect_fill(g, bx, sy + 2.0f, 3.0f, max(16.0f, b->h - 4.0f), 1.5f, bar_col);
            }
            if (alert_lbl) {
                float lx = (float)g_layout_col_x + 20.0f + (float)(b->level - 1) * 14.0f;
                draw_text(g, alert_lbl, g_body_face, 12.0f, GP_FONT_BOLD, bar_col,
                          lx, sy + 6.0f, 140.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
            }
        } else if (b->type == MDB_LIST_ITEM) {
            float l_indent = 28.0f + (float)b->level * 24.0f;
            float base_lh = (float)g_body_px * 1.60f;
            if (b->is_task) {
                tv_draw_checkbox(g, b->cb_x, base_y + b->cb_y, b->task_checked,
                                 (g_hover_cb_blk == (int)bi), pal);
            } else if (b->is_ordered) {
                wchar_t num_s[20];
                _snwprintf_s(num_s, 20, _TRUNCATE, L"%d.", b->ordinal);
                draw_text(g, num_s, g_body_face, (float)g_body_px * 0.94f, GP_FONT_REGULAR, pal->list_marker,
                          (float)g_layout_col_x + l_indent - 34.0f, sy, 28.0f, base_lh,
                          GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
            } else {
                float cx = (float)g_layout_col_x + l_indent - 13.0f;
                float cy = sy + base_lh * 0.5f;
                int dmod = b->level % 3;
                if (dmod == 0) {
                    draw_circle_fill(g, cx, cy, 3.1f, pal->list_marker);
                } else if (dmod == 1) {
                    GpPen *pen = NULL;
                    if (GdipCreatePen1(pal->list_marker, 1.4f, GP_UNIT_PIXEL, &pen) == GP_OK && pen) {
                        GdipDrawEllipse(g, pen, cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
                        GdipDeletePen(pen);
                    }
                } else {
                    draw_round_rect_fill(g, cx - 2.6f, cy - 2.6f, 5.2f, 5.2f, 1.0f, pal->list_marker);
                }
            }
        } else if (b->type == MDB_CODE_BLOCK) {
            draw_round_rect_fill(g, (float)g_layout_col_x, sy, (float)g_layout_col_w, b->h, 6.0f, pal->code_block_bg);
            draw_round_rect_stroke(g, (float)g_layout_col_x + 0.5f, sy + 0.5f,
                                   (float)g_layout_col_w - 1.0f, b->h - 1.0f, 6.0f, pal->code_block_border, 1.0f);
            if (b->lang[0]) {
                draw_text(g, b->lang, g_mono_face, 11.5f, GP_FONT_REGULAR, pal->muted,
                          (float)g_layout_col_x + 14.0f, sy + 6.0f, 180.0f, 18.0f,
                          GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
            }
            /* Subtle Copy button top-right */
            bool hov_cp = (g_hover_copy_blk == (int)bi);
            draw_round_rect_fill(g, b->copy_x, base_y + b->copy_y, 48.0f, 20.0f, 4.0f,
                                 hov_cp ? pal->accent_soft : pal->surface_alt);
            draw_text(g, L"Copy", g_body_face, 11.0f, GP_FONT_BOLD, hov_cp ? pal->accent : pal->muted,
                      b->copy_x, base_y + b->copy_y, 48.0f, 20.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        } else if (b->type == MDB_TABLE && b->table_cols > 0 && b->table_rows > 0) {
            int ncols = b->table_cols;
            int nrows = b->table_rows;
            float cell_pad_y = 7.0f;

            /* Compute row Y tops from cell lines */
            float row_top[256];
            int max_r = min(nrows, 255);
            for (int r = 0; r < max_r; r++) {
                float rmin = sy + b->h;
                bool any = false;
                for (int c = 0; c < ncols; c++) {
                    const MdTableCell *cell = &cells[b->first_cell + (uint32_t)(r * ncols + c)];
                    if (cell->line_count > 0) {
                        float ly = base_y + larr[cell->first_line].y - cell_pad_y;
                        if (!any || ly < rmin) { rmin = ly; any = true; }
                    }
                }
                row_top[r] = any ? rmin : (r == 0 ? sy : row_top[r - 1] + 30.0f);
            }
            row_top[max_r] = sy + b->h;

            /* Header row & alternating row subtle shading */
            if (max_r >= 1) {
                draw_round_rect_fill(g, (float)g_layout_col_x, row_top[0],
                                     (float)g_layout_col_w, row_top[1] - row_top[0], 0.0f, pal->table_header_bg);
            }
            for (int r = 2; r < max_r; r += 2) {
                draw_round_rect_fill(g, (float)g_layout_col_x, row_top[r],
                                     (float)g_layout_col_w, row_top[r + 1] - row_top[r], 0.0f, pal->table_alt_bg);
            }

            /* Horizontal row borders */
            for (int r = 1; r < max_r; r++) {
                draw_line(g, (float)g_layout_col_x, row_top[r],
                          (float)(g_layout_col_x + g_layout_col_w), row_top[r],
                          pal->table_border, (r == 1) ? 1.5f : 1.0f);
            }
            /* Vertical column borders */
            for (int c = 1; c < ncols; c++) {
                float cx = cells[b->first_cell + (uint32_t)c].box_x;
                draw_line(g, cx, sy, cx, sy + b->h, pal->table_border, 1.0f);
            }
            /* Outer table border */
            draw_round_rect_stroke(g, (float)g_layout_col_x + 0.5f, sy + 0.5f,
                                   (float)g_layout_col_w - 1.0f, b->h - 1.0f, 4.0f, pal->table_border, 1.0f);
        }
    }

    /* Pass 2: Find match highlights underneath text */
    if (g_find_open && g_md_matches.count > 0) {
        const MdFindMatch *marr = (const MdFindMatch *)g_md_matches.ptr;
        for (size_t mi = 0; mi < g_md_matches.count; mi++) {
            const MdFindMatch *m = &marr[mi];
            if (m->y + m->h < vis_top) continue;
            if (m->y > vis_bot) break;
            bool cur = ((int)mi + 1 == g_find_match_idx);
            draw_round_rect_fill(g, m->x - 1.0f, base_y + m->y, m->w + 2.0f, m->h, 3.0f,
                                 cur ? pal->find_cur : pal->find_hl);
            if (cur) {
                draw_round_rect_stroke(g, m->x - 1.0f, base_y + m->y, m->w + 2.0f, m->h, 3.0f, pal->accent, 1.2f);
            }
        }
    }

    /* Pass 3: Visible text fragments (Pure-CPU DirectWrite ClearType!) */
    wchar_t tmp[512];
    for (size_t li = 0; li < g_lines.count; li++) {
        const MdLine *ln = &larr[li];
        if (ln->y + ln->line_h < vis_top) continue;
        if (ln->y > vis_bot) continue; /* table cells can interleave Y across columns so continue instead of break */

        float sy = base_y + ln->y;
        const MdBlock *blk = (ln->block_idx >= 0 && (size_t)ln->block_idx < g_blocks.count)
                           ? &blks[ln->block_idx] : NULL;

        if (ln->is_code_line) {
            for (uint32_t fi = 0; fi < ln->frag_count; fi++) {
                const MdFrag *fr = &farr[ln->first_frag + fi];
                tv_draw_code_line(g, g_doc_text + fr->text_off, fr->text_len,
                                  fr->x, sy, ln->line_h, ln->font_px, pal);
            }
            continue;
        }

        bool is_heading = (blk && blk->type == MDB_HEADING);
        bool is_quote   = (blk && blk->type == MDB_BLOCKQUOTE);
        bool is_checked = (blk && blk->type == MDB_LIST_ITEM && blk->is_task && blk->task_checked);
        bool is_th      = (ln->cell_idx >= 0 && (size_t)ln->cell_idx < g_cells.count &&
                           cells[ln->cell_idx].row == 0);
        bool force_b    = is_heading || is_th;

        for (uint32_t fi = 0; fi < ln->frag_count; fi++) {
            const MdFrag *fr = &farr[ln->first_frag + fi];
            size_t n = min((size_t)fr->text_len, (size_t)511);
            if (n == 0) continue;
            memcpy(tmp, g_doc_text + fr->text_off, n * sizeof(wchar_t));
            tmp[n] = L'\0';

            bool is_code = (fr->flags & MD_SPAN_CODE) != 0;
            bool is_link = (fr->flags & MD_SPAN_LINK) != 0;
            bool is_img  = (fr->flags & MD_SPAN_IMAGE) != 0;
            bool is_strk = (fr->flags & MD_SPAN_STRIKE) != 0;
            bool is_bold = force_b || ((fr->flags & MD_SPAN_BOLD) != 0);
            bool is_ital = (fr->flags & MD_SPAN_ITALIC) != 0;

            if (is_code) {
                float cpx = max(10.5f, ln->font_px * 0.85f);
                float pill_h = min(ln->line_h - 2.0f, cpx + 7.0f);
                float pill_y = sy + (ln->line_h - pill_h) * 0.5f;
                draw_round_rect_fill(g, fr->x + 0.5f, pill_y, max(8.0f, fr->w - 1.0f), pill_h, 4.0f, pal->inline_code_bg);
                ARGB cfg = is_link ? pal->link : (is_checked ? pal->muted : pal->inline_code_fg);
                draw_text(g, tmp, g_mono_face, cpx, GP_FONT_REGULAR, cfg,
                          fr->x + 5.0f, sy + 0.5f, fr->w, ln->line_h,
                          GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
            } else {
                int st = md_span_style(fr->flags, force_b);
                ARGB col = pal->text;
                if (is_heading) {
                    col = (blk->level == 6) ? pal->heading6 : pal->heading;
                } else if (is_checked) {
                    col = pal->muted;
                } else if (is_quote) {
                    col = pal->quote_text;
                }
                if (is_bold && !is_heading && !is_checked && !is_quote) col = pal->strong;
                if (is_ital && !is_heading && !is_checked && !is_quote) col = pal->em;
                if (is_strk) col = pal->strike;
                if (is_link) col = pal->link;

                if (is_img) {
                    float pill_h = min(ln->line_h - 2.0f, ln->font_px + 6.0f);
                    float pill_y = sy + (ln->line_h - pill_h) * 0.5f;
                    draw_round_rect_fill(g, fr->x, pill_y, fr->w + 2.0f, pill_h, 4.0f, pal->accent_soft);
                }

                draw_text(g, tmp, g_body_face, ln->font_px, st, col,
                          fr->x, sy, fr->w + 4.0f, ln->line_h,
                          GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

                if (is_link) {
                    float uy = sy + ln->line_h * 0.5f + ln->font_px * 0.48f;
                    draw_line(g, fr->x, uy, fr->x + fr->w, uy, MAKE_ARGB(130, (pal->link >> 16) & 0xFF, (pal->link >> 8) & 0xFF, pal->link & 0xFF), 1.0f);
                }
            }

            if (is_strk) {
                float mid_y = sy + ln->line_h * 0.52f;
                draw_line(g, fr->x, mid_y, fr->x + fr->w, mid_y, pal->strike, 1.0f);
            }
        }
    }

    /* Pass 4: Thin subtle scrollbar on the far right */
    ZeroMemory(&g_rc_sb_thumb, sizeof(g_rc_sb_thumb));
    if (g_doc_h > (float)vh + 2.0f) {
        float track_h = (float)vh - 12.0f;
        float ratio = (float)vh / g_doc_h;
        float thumb_h = max(32.0f, track_h * ratio);
        float max_sc = max(1.0f, g_doc_h - (float)vh);
        float thumb_y = (float)vy0 + 6.0f + (g_scroll_y / max_sc) * (track_h - thumb_h);
        float sb_w = (g_hover_sb || g_sb_dragging) ? 7.0f : 5.0f;
        float sb_x = (float)g_rc_doc.right - sb_w - 3.0f;
        draw_round_rect_fill(g, sb_x, thumb_y, sb_w, thumb_h, sb_w * 0.5f,
                             (g_hover_sb || g_sb_dragging) ? pal->scroll_thumb_hov : pal->scroll_thumb);
        g_rc_sb_thumb.left   = g_rc_doc.right - 14;
        g_rc_sb_thumb.top    = (int)thumb_y;
        g_rc_sb_thumb.right  = g_rc_doc.right;
        g_rc_sb_thumb.bottom = (int)(thumb_y + thumb_h);
    }

    GdipResetClip(g);
}

static void tv_paint_sidebar(GpGraphics *g, const MtPalette *pal) {
    if (g_rc_sidebar.right <= g_rc_sidebar.left) return;
    int sx = g_rc_sidebar.left;
    int sy = g_rc_sidebar.top;
    int sw = g_rc_sidebar.right - g_rc_sidebar.left;
    int sh = g_rc_sidebar.bottom - g_rc_sidebar.top;

    draw_round_rect_fill(g, (float)sx, (float)sy, (float)sw, (float)sh, 0.0f, pal->sidebar_bg);
    draw_line(g, (float)g_rc_sidebar.right - 0.5f, (float)sy, (float)g_rc_sidebar.right - 0.5f, (float)g_rc_sidebar.bottom,
              pal->sidebar_border, 1.0f);

    /* Sidebar header: "OUTLINE" */
    draw_text(g, L"OUTLINE", g_body_face, 11.0f, GP_FONT_BOLD, pal->sidebar_muted,
              (float)sx + 16.0f, (float)sy + 8.0f, (float)sw - 32.0f, 22.0f,
              GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

    int list_top = sy + 36;
    int list_h = max(20, sh - 40);
    GdipSetClipRect(g, (float)sx, (float)list_top, (float)sw - 1.0f, (float)list_h, GP_COMBINE_REPLACE);

    if (g_toc.count == 0) {
        draw_text(g, L"No headings", g_body_face, 12.5f, GP_FONT_ITALIC, pal->sidebar_muted,
                  (float)sx + 16.0f, (float)list_top + 6.0f, (float)sw - 28.0f, 24.0f,
                  GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
    } else {
        const MdTocItem *tocs = (const MdTocItem *)g_toc.ptr;
        float row_h = 30.0f;
        for (size_t i = 0; i < g_toc.count; i++) {
            float ry = (float)list_top + (float)i * row_h - g_toc_scroll_y;
            if (ry + row_h < (float)list_top) continue;
            if (ry > (float)(list_top + list_h)) break;

            bool active = ((int)i == g_active_toc_idx);
            bool hov    = ((int)i == g_hover_toc);
            if (active) {
                draw_round_rect_fill(g, (float)sx + 8.0f, ry + 2.0f, (float)sw - 16.0f, row_h - 4.0f, 5.0f,
                                     pal->sidebar_active_bg);
                draw_round_rect_fill(g, (float)sx + 8.0f, ry + 6.0f, 2.5f, row_h - 12.0f, 1.2f, pal->accent);
            } else if (hov) {
                draw_round_rect_fill(g, (float)sx + 8.0f, ry + 2.0f, (float)sw - 16.0f, row_h - 4.0f, 5.0f,
                                     pal->sidebar_hover);
            }

            int depth = max(0, (int)tocs[i].level - g_toc_min_level);
            if (depth > 4) depth = 4;
            float tx = (float)sx + 16.0f + (float)depth * 13.0f;
            float tw = max(40.0f, (float)(sx + sw - 14) - tx);
            ARGB fg = active ? pal->accent : (depth == 0 ? pal->sidebar_title : pal->sidebar_text);
            int st = (active || depth == 0) ? GP_FONT_BOLD : GP_FONT_REGULAR;
            float fs = (depth == 0) ? 12.8f : 12.2f;
            draw_text(g, tocs[i].title, g_body_face, fs, st, fg,
                      tx, ry, tw, row_h, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        }
    }

    GdipResetClip(g);
}

static void tv_draw(LayerSurface *fb, int w, int h) {
    HWND hwnd = g_hTvWnd;
    (void)hwnd;
    tv_update_layout(w, h);

    MtPalette pal;
    tv_get_palette(&pal);
    {
        GpGraphics *g = fb->g;
        GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
        GdipSetTextRenderingHint(g, GP_TEXT_ANTIALIAS);

        /* 0. canvas fill */
        draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, (float)h, 0.0f, pal.bg);

        /* 1. Seamless 38px Custom Titlebar */
        if (g_rc_sidebar.right > g_rc_sidebar.left) {
            /* Match sidebar background seamlessly up into left titlebar region */
            draw_round_rect_fill(g, 0.0f, 0.0f, (float)g_rc_sidebar.right, (float)TV_TITLE_H, 0.0f, pal.sidebar_bg);
            draw_line(g, (float)g_rc_sidebar.right - 0.5f, 0.0f, (float)g_rc_sidebar.right - 0.5f, (float)TV_TITLE_H,
                      pal.sidebar_border, 1.0f);
        }
        draw_line(g, 0.0f, (float)TV_TITLE_H - 0.5f, (float)w, (float)TV_TITLE_H - 0.5f, pal.divider, 1.0f);

        /* Titlebar Left Controls: Outline toggle & Mode/Wrap pill */
        if (g_is_markdown && g_rc_tb[0].right > g_rc_tb[0].left) {
            const RECT *r = &g_rc_tb[0];
            bool act = g_sidebar_open;
            if (act || g_hover_tb == 0) {
                draw_round_rect_fill(g, (float)r->left, (float)r->top,
                                     (float)(r->right - r->left), (float)(r->bottom - r->top), 5.0f,
                                     act ? pal.accent_soft : pal.hover_bg);
            }
            draw_icon_glyph(g, 0xE8FD /* BulletedList / Outline */, 13.5f,
                            act ? pal.accent : pal.muted,
                            (float)r->left, (float)r->top,
                            (float)(r->right - r->left), (float)(r->bottom - r->top));
        }

        {
            const RECT *r = &g_rc_tb[1];
            bool act = g_is_markdown ? g_source_mode : g_word_wrap;
            if (act || g_hover_tb == 1) {
                draw_round_rect_fill(g, (float)r->left, (float)r->top,
                                     (float)(r->right - r->left), (float)(r->bottom - r->top), 5.0f,
                                     act ? pal.accent_soft : pal.hover_bg);
            }
            const wchar_t *lbl = g_is_markdown ? (g_source_mode ? L"Preview" : L"Source") : L"Wrap";
            wchar_t glyph = g_is_markdown ? (g_source_mode ? 0xE890 /* View */ : 0xE943 /* Code */) : 0xE751;
            ARGB fg = act ? pal.accent : pal.muted;
            draw_icon_glyph(g, glyph, 11.5f, fg, (float)r->left + 5.0f, (float)r->top, 16.0f, (float)(r->bottom - r->top));
            draw_text(g, lbl, g_body_face, 11.5f, GP_FONT_BOLD, fg,
                      (float)r->left + 22.0f, (float)r->top, (float)(r->right - r->left - 25), (float)(r->bottom - r->top),
                      GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        }

        /* Centered Document Title + Save Dot */
        {
            const wchar_t *disp = g_cur_name[0] ? g_cur_name : L"Untitled.md";
            int center_l = max(g_rc_tb[1].right + 14, g_rc_sidebar.right + 14);
            int center_r = g_rc_tb[2].left - 14;
            if (center_r > center_l + 60) {
                float tw = measure_text_width(g, disp, g_body_face, 13.0f, GP_FONT_BOLD);
                float max_tw = (float)(center_r - center_l) - (g_modified ? 18.0f : 0.0f) - (g_read_only ? 72.0f : 0.0f);
                if (tw > max_tw) tw = max(40.0f, max_tw);
                float total_w = tw + (g_modified ? 14.0f : 0.0f) + (g_read_only ? 68.0f : 0.0f);
                float sx = (float)center_l + ((float)(center_r - center_l) - total_w) * 0.5f;
                if (sx < (float)center_l) sx = (float)center_l;

                if (g_modified) {
                    /* 8px accent save dot */
                    draw_circle_fill(g, sx + 4.0f, (float)TV_TITLE_H * 0.5f, 4.0f, pal.accent);
                    sx += 14.0f;
                }
                draw_text(g, disp, g_body_face, 13.0f, GP_FONT_BOLD, pal.heading,
                          sx, 0.0f, tw + 2.0f, (float)TV_TITLE_H, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                if (g_read_only) {
                    float bx = sx + tw + 8.0f;
                    draw_round_rect_fill(g, bx, 10.0f, 60.0f, 18.0f, 4.0f, pal.surface_alt);
                    draw_text(g, L"Read-Only", g_body_face, 10.0f, GP_FONT_BOLD, pal.muted,
                              bx, 10.0f, 60.0f, 18.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
                }
            }
        }

        /* Titlebar Right Action Icons: Save, Find, Theme, External */
        {
            wchar_t r_glyphs[4] = {
                0xE74E, /* Save */
                0xE721, /* Find */
                pal.is_dark ? 0xE706 /* Sun */ : 0xE708 /* Moon */,
                0xE8A7  /* Open External */
            };
            for (int k = 0; k < 4; k++) {
                int idx = 2 + k;
                const RECT *r = &g_rc_tb[idx];
                int bw = r->right - r->left;
                int bh = r->bottom - r->top;
                bool dis = (idx == 2 && (g_read_only || !g_cur_path[0])) || (idx == 5 && !g_cur_path[0]);
                bool acc = (idx == 2 && g_modified && !dis);
                bool act = (idx == 3 && g_find_open);

                if (acc) {
                    draw_round_rect_fill(g, (float)r->left, (float)r->top, (float)bw, (float)bh, 5.0f,
                                         (g_hover_tb == idx) ? pal.accent_hover : pal.accent);
                } else if (act) {
                    draw_round_rect_fill(g, (float)r->left, (float)r->top, (float)bw, (float)bh, 5.0f, pal.accent_soft);
                } else if (g_hover_tb == idx && !dis) {
                    draw_round_rect_fill(g, (float)r->left, (float)r->top, (float)bw, (float)bh, 5.0f, pal.hover_bg);
                }

                ARGB fg = acc ? pal.fg_on_accent
                        : (dis ? pal.subtle : (act ? pal.accent : pal.muted));
                draw_icon_glyph(g, r_glyphs[k], 12.5f, fg, (float)r->left, (float)r->top, (float)bw, (float)bh);
            }
        }

        /* Windows 11 Caption Buttons */
        {
            bool zoomed = IsZoomed(hwnd) != 0;
            wchar_t cglyphs[3] = { 0xE921, zoomed ? 0xE923 : 0xE922, 0xE8BB };
            for (int c = 0; c < 3; c++) {
                const RECT *rc = &g_rc_cap[c];
                bool hov = (g_hover_cap == c + 1);
                if (hov) {
                    ARGB bg = (c == 2) ? MAKE_ARGB(255, 228, 79, 79) : pal.hover_bg;
                    draw_round_rect_fill(g, (float)rc->left, (float)rc->top,
                                         (float)(rc->right - rc->left), (float)(rc->bottom - rc->top), 0.0f, bg);
                }
                ARGB fg = (hov && c == 2) ? MAKE_ARGB(255, 255, 255, 255) : pal.heading;
                draw_icon_glyph(g, cglyphs[c], 10.0f, fg, (float)rc->left, (float)rc->top,
                                (float)(rc->right - rc->left), (float)(rc->bottom - rc->top));
            }
        }

        /* 2. Collapsible Left Outline Sidebar */
        tv_paint_sidebar(g, &pal);

        /* 3. Optional Large-File / Binary Guard Banner */
        if (g_rc_banner.bottom > g_rc_banner.top) {
            ARGB warn_bg = pal.is_dark ? MAKE_ARGB(255, 58, 44, 24) : MAKE_ARGB(255, 255, 244, 224);
            ARGB warn_fg = pal.is_dark ? MAKE_ARGB(255, 248, 200, 108) : MAKE_ARGB(255, 140, 82, 0);
            int bx = g_rc_banner.left;
            int bw = g_rc_banner.right - bx;
            draw_round_rect_fill(g, (float)bx, (float)g_rc_banner.top, (float)bw, (float)TV_BANNER_H, 0.0f, warn_bg);
            draw_line(g, (float)bx, (float)g_rc_banner.bottom - 0.5f, (float)g_rc_banner.right, (float)g_rc_banner.bottom - 0.5f, pal.divider, 1.0f);
            draw_icon_glyph(g, 0xE7BA, 13.0f, warn_fg, (float)bx + 12.0f, (float)g_rc_banner.top, 20.0f, (float)TV_BANNER_H);

            wchar_t bmsg[160];
            if (g_is_truncated) {
                wchar_t szs[32];
                tv_format_bytes(g_cur_file_size, szs, 32);
                _snwprintf_s(bmsg, 160, _TRUNCATE,
                             L"Large file (%ls) - Showing first 512 KB in read-only mode", szs);
            } else {
                wcsncpy_s(bmsg, 160, L"Binary bytes detected - Opened read-only to prevent file corruption", _TRUNCATE);
            }
            draw_text(g, bmsg, g_body_face, 11.5f, GP_FONT_BOLD, warn_fg,
                      (float)bx + 36.0f, (float)g_rc_banner.top, (float)(g_rc_banner_btn.left - bx - 44), (float)TV_BANNER_H,
                      GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

            draw_round_rect_fill(g, (float)g_rc_banner_btn.left, (float)g_rc_banner_btn.top,
                                 (float)(g_rc_banner_btn.right - g_rc_banner_btn.left), 24.0f, 5.0f,
                                 g_hover_banner_ext ? pal.accent : pal.input_bg);
            draw_text(g, L"Open External", g_body_face, 11.0f, GP_FONT_BOLD,
                      g_hover_banner_ext ? pal.fg_on_accent : pal.text,
                      (float)g_rc_banner_btn.left, (float)g_rc_banner_btn.top,
                      (float)(g_rc_banner_btn.right - g_rc_banner_btn.left), 24.0f,
                      GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        }

        /* 4. Optional Quick Find Bar (Ctrl+F) */
        if (g_find_open && g_rc_find_bar.bottom > g_rc_find_bar.top) {
            int fx0 = g_rc_find_bar.left;
            int fw  = g_rc_find_bar.right - fx0;
            draw_round_rect_fill(g, (float)fx0, (float)g_rc_find_bar.top, (float)fw, (float)TV_FIND_BAR_H, 0.0f, pal.surface_alt);
            draw_line(g, (float)fx0, (float)g_rc_find_bar.bottom - 0.5f, (float)g_rc_find_bar.right, (float)g_rc_find_bar.bottom - 0.5f, pal.divider, 1.0f);

            draw_round_rect_fill(g, (float)g_rc_find_box.left, (float)g_rc_find_box.top,
                                 (float)(g_rc_find_box.right - g_rc_find_box.left), 28.0f, 6.0f, pal.input_bg);
            draw_round_rect_stroke(g, (float)g_rc_find_box.left + 0.5f, (float)g_rc_find_box.top + 0.5f,
                                   (float)(g_rc_find_box.right - g_rc_find_box.left) - 1.0f, 27.0f, 6.0f, pal.divider, 1.0f);
            draw_icon_glyph(g, 0xE721, 11.5f, pal.muted, (float)(g_rc_find_box.left + 6), (float)g_rc_find_box.top, 18.0f, 28.0f);

            static const wchar_t fglyphs[4] = { 0xE70E, 0xE70D, 0, 0xE711 };
            for (int i = 0; i < 4; i++) {
                const RECT *r = &g_rc_find_btns[i];
                bool act = (i == 2 && g_find_case);
                if (act) {
                    draw_round_rect_fill(g, (float)r->left, (float)r->top, (float)(r->right - r->left), 28.0f, 5.0f, pal.accent_soft);
                } else if (g_hover_find == i) {
                    draw_round_rect_fill(g, (float)r->left, (float)r->top, (float)(r->right - r->left), 28.0f, 5.0f, pal.hover_bg);
                }
                ARGB fg = act ? pal.accent : pal.text;
                if (i == 2) {
                    draw_text(g, L"Aa", g_body_face, 11.5f, GP_FONT_BOLD, fg,
                              (float)r->left, (float)r->top, (float)(r->right - r->left), 28.0f,
                              GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
                } else {
                    draw_icon_glyph(g, fglyphs[i], 10.5f, fg, (float)r->left, (float)r->top,
                                    (float)(r->right - r->left), 28.0f);
                }
            }

            if (g_find_query[0]) {
                wchar_t cnt_s[48];
                if (g_find_match_total > 0) {
                    _snwprintf_s(cnt_s, 48, _TRUNCATE, L"%d of %d", g_find_match_idx, g_find_match_total);
                } else {
                    wcsncpy_s(cnt_s, 48, L"No matches", _TRUNCATE);
                }
                float cx = (float)(g_rc_find_btns[2].right + 10);
                float cw = (float)(g_rc_find_btns[3].left - 8) - cx;
                if (cw > 30.0f) {
                    draw_text(g, cnt_s, g_body_face, 11.5f, GP_FONT_REGULAR,
                              g_find_match_total > 0 ? pal.muted : MAKE_ARGB(255, 228, 79, 79),
                              cx, (float)g_rc_find_bar.top, cw, (float)TV_FIND_BAR_H, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                }
            }
        }

        /* 5. Main Document Area (Rendered Canvas in Preview Mode, or custom scrollbar for Source/Code Mode) */
        if (g_is_markdown && !g_source_mode) {
            tv_paint_markdown_doc(g, &pal);
        } else if (g_hEdit && IsWindowVisible(g_hEdit)) {
            /* Sleek custom vertical scrollbar synced to EDIT control */
            ZeroMemory(&g_rc_sb_thumb, sizeof(g_rc_sb_thumb));
            int total_lines = max(1, (int)SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0));
            int first_vis = max(0, (int)SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0));
            int edit_px = g_is_markdown ? max(11, g_body_px - 1) : g_code_edit_px;
            int line_h = max(12, edit_px + 4);
            int vh = max(40, (int)(g_rc_doc.bottom - g_rc_doc.top));
            int vis_lines = max(1, (int)(g_rc_edit_box.bottom - g_rc_edit_box.top) / line_h);
            if (total_lines > vis_lines) {
                float track_h = (float)vh - 12.0f;
                float ratio = (float)vis_lines / (float)total_lines;
                float thumb_h = max(32.0f, track_h * ratio);
                float max_top = (float)max(1, total_lines - vis_lines);
                float frac = min(1.0f, (float)first_vis / max_top);
                float thumb_y = (float)g_rc_doc.top + 6.0f + frac * (track_h - thumb_h);
                float sb_w = (g_hover_sb || g_sb_dragging) ? 7.0f : 5.0f;
                float sb_x = (float)g_rc_doc.right - sb_w - 3.0f;
                draw_round_rect_fill(g, sb_x, thumb_y, sb_w, thumb_h, sb_w * 0.5f,
                                     (g_hover_sb || g_sb_dragging) ? pal.scroll_thumb_hov : pal.scroll_thumb);
                g_rc_sb_thumb.left   = g_rc_doc.right - 14;
                g_rc_sb_thumb.top    = (int)thumb_y;
                g_rc_sb_thumb.right  = g_rc_doc.right;
                g_rc_sb_thumb.bottom = (int)(thumb_y + thumb_h);
            }
        }

        /* 6. Subtle Bottom Word & Character Count Status Bar (26px) */
        int sty = h - TV_STATUS_H;
        draw_round_rect_fill(g, 0.0f, (float)sty, (float)w, (float)TV_STATUS_H, 0.0f, pal.bg);
        draw_line(g, 0.0f, (float)sty + 0.5f, (float)w, (float)sty + 0.5f, pal.divider, 1.0f);

        static const wchar_t *tb_tips[6] = {
            L"Toggle Outline Sidebar (Ctrl+J)",
            L"Toggle Source / Preview Mode (Ctrl+E)",
            L"Save File (Ctrl+S)",
            L"Find in Document (Ctrl+F)",
            L"Switch Light / Dark Reading Theme (Ctrl+Shift+T)",
            L"Open with External App"
        };

        wchar_t left_st[256];
        if (g_flash_msg[0] && GetTickCount() < g_flash_until) {
            wcsncpy_s(left_st, 256, g_flash_msg, _TRUNCATE);
        } else if (g_hover_link && g_hover_url[0]) {
            _snwprintf_s(left_st, 256, _TRUNCATE, L"Link: %ls", g_hover_url);
        } else if (g_hover_tb >= 0 && g_hover_tb < 6) {
            if (g_hover_tb == 1 && !g_is_markdown) wcsncpy_s(left_st, 256, L"Toggle Word Wrap (Alt+Z)", _TRUNCATE);
            else wcsncpy_s(left_st, 256, tb_tips[g_hover_tb], _TRUNCATE);
        } else if (g_is_markdown) {
            int read_min = max(1, (g_stat_words + 199) / 200);
            _snwprintf_s(left_st, 256, _TRUNCATE,
                         L"Words %d   |   Characters %d   |   Lines %d   |   %d min read",
                         g_stat_words, g_stat_chars, g_stat_lines, read_min);
        } else {
            wchar_t szs[32];
            tv_format_bytes(g_cur_file_size, szs, 32);
            _snwprintf_s(left_st, 256, _TRUNCATE,
                         L"Ln %d, Col %d   |   %d %ls   |   %ls   |   %ls   |   %ls",
                         g_caret_line, g_caret_col, g_line_count, g_line_count == 1 ? L"line" : L"lines",
                         szs, tv_enc_label(g_enc), g_use_crlf ? L"CRLF" : L"LF");
        }

        draw_text(g, left_st, g_body_face, 11.2f, GP_FONT_REGULAR, pal.muted,
                  14.0f, (float)sty, (float)(w / 2 + 120), (float)TV_STATUS_H,
                  GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

        wchar_t right_st[128];
        if (g_is_markdown) {
            _snwprintf_s(right_st, 128, _TRUNCATE,
                         L"%ls   |   %ls   |   %ls   |   %d%%   |   Ctrl+E %ls",
                         g_source_mode ? L"Source" : L"Markdown",
                         tv_enc_label(g_enc), g_use_crlf ? L"CRLF" : L"LF",
                         (g_body_px * 100) / 16,
                         g_source_mode ? L"Preview" : L"Edit");
        } else {
            _snwprintf_s(right_st, 128, _TRUNCATE,
                         L"Wrap: %ls   |   %d px   |   Ctrl+S Save   |   Ctrl+F Find",
                         g_word_wrap ? L"On" : L"Off", g_code_edit_px);
        }
        draw_text(g, right_st, g_body_face, 11.0f, GP_FONT_REGULAR, pal.muted,
                  (float)(w / 2), (float)sty, (float)(w / 2 - 14), (float)TV_STATUS_H,
                  GP_ALIGN_FAR, GP_ALIGN_CENTER, true);
    }
}

static void tv_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        tv_draw(&fb, cr.right, cr.bottom);
        opaque_frame_end(hdc, &fb);
    }
    EndPaint(hwnd, &ps);
}

/* The editor drawn into a picture at its window's size, for checks. */
int textview_snapshot(const wchar_t *png_path) {
    if (!g_hTvWnd) return 0;
    RECT cr;
    GetClientRect(g_hTvWnd, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    int ok = 0;
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        tv_draw(&fb, cr.right, cr.bottom);
        ok = surface_save_png(&fb, png_path);
        surface_destroy(&fb);
    }
    return ok;
}

/* =========================================================================
 * Interactive Hit-Testing (Checkboxes, Links, Code Copy, Outline, Scrollbar)
 * ========================================================================= */
static void tv_copy_text_to_clipboard(HWND hwnd, const wchar_t *text, size_t len) {
    if (!text || len == 0) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
    if (hg) {
        wchar_t *dst = (wchar_t *)GlobalLock(hg);
        if (dst) {
            memcpy(dst, text, len * sizeof(wchar_t));
            dst[len] = L'\0';
            GlobalUnlock(hg);
            SetClipboardData(CF_UNICODETEXT, hg);
        } else {
            GlobalFree(hg);
        }
    }
    CloseClipboard();
}

static void tv_open_link_target(HWND hwnd, const wchar_t *url, size_t ulen) {
    if (!url || ulen == 0) return;
    wchar_t ubuf[512];
    size_t n = min(ulen, (size_t)511);
    memcpy(ubuf, url, n * sizeof(wchar_t));
    ubuf[n] = L'\0';

    /* Internal heading anchor #slug */
    if (ubuf[0] == L'#') {
        const wchar_t *slug = ubuf + 1;
        const MdTocItem *tocs = (const MdTocItem *)g_toc.ptr;
        for (size_t i = 0; i < g_toc.count; i++) {
            /* Loose slug match: compare alnum lowered */
            wchar_t norm[128];
            size_t di = 0;
            for (size_t k = 0; tocs[i].title[k] && di < 127; k++) {
                wchar_t c = (wchar_t)towlower(tocs[i].title[k]);
                if (iswalnum(c)) norm[di++] = c;
                else if (c == L' ' || c == L'-') norm[di++] = L'-';
            }
            norm[di] = L'\0';
            if (_wcsicmp(norm, slug) == 0 || wcsstr(norm, slug) != NULL) {
                float view_h = max(60.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
                float max_sc = max(0.0f, g_doc_h - view_h);
                g_scroll_y = min(max_sc, tocs[i].doc_y);
                tv_update_active_toc();
                InvalidateRect(hwnd, NULL, FALSE);
                return;
            }
        }
        return;
    }

    /* External web/mail URL */
    if (_wcsnicmp(ubuf, L"http://", 7) == 0 ||
        _wcsnicmp(ubuf, L"https://", 8) == 0 ||
        _wcsnicmp(ubuf, L"mailto:", 7) == 0) {
        ShellExecuteW(hwnd, L"open", ubuf, NULL, NULL, SW_SHOWNORMAL);
        return;
    }

    /* file:/// URL or relative local path */
    wchar_t local[MAX_PATH] = {0};
    if (_wcsnicmp(ubuf, L"file:///", 8) == 0) {
        wcsncpy_s(local, MAX_PATH, ubuf + 8, _TRUNCATE);
        for (size_t i = 0; local[i]; i++) if (local[i] == L'/') local[i] = L'\\';
    } else if (g_cur_dir[0]) {
        _snwprintf_s(local, MAX_PATH, _TRUNCATE, L"%ls\\%ls", g_cur_dir, ubuf);
        for (size_t i = 0; local[i]; i++) if (local[i] == L'/') local[i] = L'\\';
    }
    if (local[0]) {
        /* Strip #L123 fragment if present */
        wchar_t *hash = wcschr(local, L'#');
        if (hash) *hash = L'\0';
        DWORD at = GetFileAttributesW(local);
        if (at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY)) {
            if (textview_is_supported_ext(PathFindExtensionW(local))) {
                if (tv_confirm_discard(hwnd)) tv_load_file(local);
            } else {
                ShellExecuteW(hwnd, L"open", local, NULL, g_cur_dir, SW_SHOWNORMAL);
            }
        }
    }
}

static bool tv_hit_test_doc(POINT pt, int *out_cb_blk, int *out_copy_blk, const MdFrag **out_link_frag, int *out_src_line) {
    if (out_cb_blk) *out_cb_blk = -1;
    if (out_copy_blk) *out_copy_blk = -1;
    if (out_link_frag) *out_link_frag = NULL;
    if (out_src_line) *out_src_line = 0;
    if (!g_is_markdown || g_source_mode || !PtInRect(&g_rc_doc, pt)) return false;

    float doc_x = (float)pt.x;
    float doc_y = (float)(pt.y - g_rc_doc.top) + g_scroll_y;

    const MdBlock *blks = (const MdBlock *)g_blocks.ptr;
    for (size_t bi = 0; bi < g_blocks.count; bi++) {
        const MdBlock *b = &blks[bi];
        if (doc_y < b->y - 4.0f || doc_y > b->y + b->h + 4.0f) continue;

        if (out_src_line && *out_src_line == 0) *out_src_line = b->src_line;

        if (b->type == MDB_LIST_ITEM && b->is_task) {
            if (doc_x >= b->cb_x - 4.0f && doc_x <= b->cb_x + 20.0f &&
                doc_y >= b->cb_y - 4.0f && doc_y <= b->cb_y + 20.0f) {
                if (out_cb_blk) *out_cb_blk = (int)bi;
                return true;
            }
        }
        if (b->type == MDB_CODE_BLOCK) {
            if (doc_x >= b->copy_x - 2.0f && doc_x <= b->copy_x + 50.0f &&
                doc_y >= b->copy_y - 2.0f && doc_y <= b->copy_y + 22.0f) {
                if (out_copy_blk) *out_copy_blk = (int)bi;
                return true;
            }
        }
    }

    const MdLine *larr = (const MdLine *)g_lines.ptr;
    const MdFrag *farr = (const MdFrag *)g_frags.ptr;
    for (size_t li = 0; li < g_lines.count; li++) {
        const MdLine *ln = &larr[li];
        if (doc_y < ln->y || doc_y > ln->y + ln->line_h) continue;
        for (uint32_t fi = 0; fi < ln->frag_count; fi++) {
            const MdFrag *fr = &farr[ln->first_frag + fi];
            if ((fr->flags & MD_SPAN_LINK) && doc_x >= fr->x && doc_x <= fr->x + fr->w) {
                if (out_link_frag) *out_link_frag = fr;
                return true;
            }
        }
    }
    return false;
}

static int tv_hit_test_toc(POINT pt) {
    if (g_rc_sidebar.right <= g_rc_sidebar.left || g_toc.count == 0) return -1;
    int list_top = g_rc_sidebar.top + 36;
    if (pt.x < g_rc_sidebar.left || pt.x >= g_rc_sidebar.right ||
        pt.y < list_top || pt.y >= g_rc_sidebar.bottom) return -1;
    int idx = (int)(((float)(pt.y - list_top) + g_toc_scroll_y) / 30.0f);
    if (idx < 0 || (size_t)idx >= g_toc.count) return -1;
    return idx;
}

static void tv_scroll_doc_by(float delta_y) {
    if (!g_is_markdown || g_source_mode) return;
    float view_h = max(60.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
    float max_sc = max(0.0f, g_doc_h - view_h);
    float ns = g_scroll_y + delta_y;
    if (ns < 0.0f) ns = 0.0f;
    if (ns > max_sc) ns = max_sc;
    if (fabsf(ns - g_scroll_y) > 0.25f) {
        g_scroll_y = ns;
        tv_update_active_toc();
        if (g_hTvWnd) InvalidateRect(g_hTvWnd, NULL, FALSE);
    }
}

static void tv_toggle_theme_override(void) {
    if (g_theme_override == 0) g_theme_override = g_theme.is_dark ? 1 : 2;
    else g_theme_override = (g_theme_override == 1) ? 2 : 1;

    tv_refresh_brushes();
    if (g_hTvWnd) {
        BOOL dark = tv_is_dark() ? TRUE : FALSE;
        DwmSetWindowAttribute(g_hTvWnd, 20, &dark, sizeof(dark));
        InvalidateRect(g_hTvWnd, NULL, TRUE);
        if (g_hEdit) InvalidateRect(g_hEdit, NULL, TRUE);
        if (g_hFindEdit) InvalidateRect(g_hFindEdit, NULL, TRUE);
    }
    tv_flash(tv_is_dark() ? L"Dark" : L"Light");
}

/* =========================================================================
 * Subclassed Child Edit Controls & Main Window Procedure
 * ========================================================================= */
static void tv_invalidate_edit_scrollbar(void) {
    if (!g_hTvWnd) return;
    RECT sb = { max(0, g_rc_doc.right - 16), g_rc_doc.top, g_rc_doc.right, g_rc_doc.bottom };
    InvalidateRect(g_hTvWnd, &sb, FALSE);
}

static LRESULT CALLBACK tv_edit_sub_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_destroying || !g_orig_edit_proc) return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (msg == WM_MOUSEWHEEL) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (ctrl) {
            tv_adjust_zoom(delta > 0 ? 1 : -1);
            return 0;
        }
        int lines = (delta > 0) ? -3 : 3;
        SendMessageW(hwnd, EM_LINESCROLL, 0, (LPARAM)lines);
        tv_invalidate_edit_scrollbar();
        return 0;
    }

    if (msg == WM_KEYDOWN) {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && wParam == 'S') { tv_save_file(); return 0; }
        if (ctrl && wParam == 'F') { tv_open_find_bar(); return 0; }
        if (ctrl && wParam == 'E' && g_is_markdown) { tv_toggle_source_mode(0); return 0; }
        if (ctrl && wParam == 'J' && g_is_markdown) { tv_toggle_sidebar(); return 0; }
        if (ctrl && shift && wParam == 'T') { tv_toggle_theme_override(); return 0; }
        if (ctrl && wParam == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (ctrl && (wParam == VK_OEM_PLUS || wParam == VK_ADD))      { tv_adjust_zoom(1); return 0; }
        if (ctrl && (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT)){ tv_adjust_zoom(-1); return 0; }
        if (ctrl && (wParam == '0' || wParam == VK_NUMPAD0))          { tv_reset_zoom(); return 0; }
        if (wParam == VK_F3) {
            if (g_find_query[0]) tv_find_step(!shift, false);
            else tv_open_find_bar();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (g_find_open) tv_close_find_bar();
            else if (g_is_markdown) tv_toggle_source_mode(0);
            else if (g_hTvWnd) PostMessageW(g_hTvWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_TAB && !ctrl && !g_read_only) {
            SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)L"    ");
            return 0;
        }
    } else if (msg == WM_CHAR) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && (wParam == 1 || wParam == 5 || wParam == 6 || wParam == 10 || wParam == 19 || wParam == 20)) return 0;
    } else if (msg == WM_SYSKEYDOWN) {
        if (wParam == 'Z') { tv_toggle_word_wrap(); return 0; }
    } else if (msg == WM_SYSCHAR) {
        if (wParam == L'z' || wParam == L'Z') return 0;
    }

    LRESULT res = CallWindowProcW(g_orig_edit_proc, hwnd, msg, wParam, lParam);
    if (msg == WM_KEYUP || msg == WM_LBUTTONUP || msg == WM_VSCROLL || msg == WM_CHAR) {
        tv_update_caret_info();
        tv_invalidate_edit_scrollbar();
    }
    return res;
}

static LRESULT CALLBACK tv_find_sub_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_destroying || !g_orig_find_proc) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_KEYDOWN) {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wParam == VK_ESCAPE) { tv_close_find_bar(); return 0; }
        if (wParam == VK_RETURN || wParam == VK_F3) { tv_find_step(!shift, false); return 0; }
        if (ctrl && wParam == 'S') { tv_save_file(); return 0; }
        if (ctrl && wParam == 'E' && g_is_markdown) { tv_toggle_source_mode(0); return 0; }
    } else if (msg == WM_CHAR) {
        if (wParam == 13 || wParam == 27) return 0;
    } else if (msg == WM_SYSKEYDOWN && wParam == 'Z') {
        tv_toggle_word_wrap();
        return 0;
    } else if (msg == WM_SYSCHAR && (wParam == L'z' || wParam == L'Z')) {
        return 0;
    }
    return CallWindowProcW(g_orig_find_proc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK tv_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            NCCALCSIZE_PARAMS *np = (NCCALCSIZE_PARAMS *)lParam;
            if (IsZoomed(hwnd)) {
                int pad = GetSystemMetrics(SM_CXPADDEDBORDER);
                int fx = GetSystemMetrics(SM_CXSIZEFRAME) + pad;
                int fy = GetSystemMetrics(SM_CYSIZEFRAME) + pad;
                np->rgrc[0].left += fx; np->rgrc[0].top += fy;
                np->rgrc[0].right -= fx; np->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        return 0;

    case WM_NCHITTEST: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        RECT wr;
        GetWindowRect(hwnd, &wr);
        if (!IsZoomed(hwnd)) {
            const int b = 6;
            bool l = pt.x < wr.left + b, r = pt.x >= wr.right - b;
            bool t = pt.y < wr.top + b, bt = pt.y >= wr.bottom - b;
            if (t && l)  return HTTOPLEFT;
            if (t && r)  return HTTOPRIGHT;
            if (bt && l) return HTBOTTOMLEFT;
            if (bt && r) return HTBOTTOMRIGHT;
            if (l)  return HTLEFT;
            if (r)  return HTRIGHT;
            if (t)  return HTTOP;
            if (bt) return HTBOTTOM;
        }
        POINT cl = pt;
        ScreenToClient(hwnd, &cl);
        if (cl.y >= 0 && cl.y < TV_TITLE_H) {
            for (int c = 0; c < 3; c++) if (PtInRect(&g_rc_cap[c], cl)) return HTCLIENT;
            for (int b = 0; b < 6; b++) if (PtInRect(&g_rc_tb[b], cl))  return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (g_hover_link || g_hover_cb_blk >= 0 || g_hover_copy_blk >= 0 || g_hover_toc >= 0 || g_hover_tb >= 0) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_SIZE: {
        RECT cr;
        GetClientRect(hwnd, &cr);
        tv_update_layout(cr.right, cr.bottom);
        if (g_is_markdown && !g_source_mode) tv_md_rebuild_layout(false);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == ID_TV_EDIT && code == EN_CHANGE && !g_loading) {
            if (!g_modified) {
                g_modified = true;
                tv_update_window_title();
            }
            if (g_is_markdown && g_source_mode) {
                /* Live word count & Outline sync while editing Markdown in Source Mode */
                tv_sync_edit_to_doc();
                tv_md_parse();
                tv_update_active_toc();
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                RECT top_r = { 0, 0, 2200, TV_TITLE_H + 2 };
                InvalidateRect(hwnd, &top_r, FALSE);
            }
            tv_update_caret_info();
        } else if (id == ID_TV_FIND && code == EN_CHANGE) {
            tv_find_step(true, true);
        }
        return 0;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND ctrl = (HWND)lParam;
        MtPalette pal;
        tv_get_palette(&pal);
        if (ctrl == g_hFindEdit) {
            SetTextColor(hdc, pal.text_ref);
            SetBkColor(hdc, pal.input_ref);
            return (LRESULT)g_hFindBrush;
        }
        SetTextColor(hdc, pal.text_ref);
        SetBkColor(hdc, pal.bg_ref);
        return (LRESULT)g_hEditBrush;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
            tv_adjust_zoom(delta > 0 ? 1 : -1);
            return 0;
        }
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        if (g_rc_sidebar.right > g_rc_sidebar.left && PtInRect(&g_rc_sidebar, pt)) {
            float list_h = max(40.0f, (float)(g_rc_sidebar.bottom - g_rc_sidebar.top - 40));
            float max_ts = max(0.0f, g_toc_content_h - list_h);
            g_toc_scroll_y -= (float)delta * 0.45f;
            if (g_toc_scroll_y < 0.0f) g_toc_scroll_y = 0.0f;
            if (g_toc_scroll_y > max_ts) g_toc_scroll_y = max_ts;
            InvalidateRect(hwnd, &g_rc_sidebar, FALSE);
            return 0;
        }
        if (g_is_markdown && !g_source_mode) {
            tv_scroll_doc_by(-(float)delta * 0.65f);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };

        if (g_sb_dragging) {
            float vh = max(60.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
            float track_h = max(20.0f, vh - 12.0f);
            if (g_is_markdown && !g_source_mode) {
                float ratio = vh / max(vh, g_doc_h);
                float thumb_h = max(32.0f, track_h * ratio);
                float travel = max(1.0f, track_h - thumb_h);
                float max_sc = max(0.0f, g_doc_h - vh);
                float dy = (float)(pt.y - g_sb_drag_start_y);
                float ns = g_sb_drag_start_scroll + (dy / travel) * max_sc;
                if (ns < 0.0f) ns = 0.0f;
                if (ns > max_sc) ns = max_sc;
                g_scroll_y = ns;
                tv_update_active_toc();
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (g_hEdit && IsWindowVisible(g_hEdit)) {
                int total_lines = max(1, (int)SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0));
                int edit_px = g_is_markdown ? max(11, g_body_px - 1) : g_code_edit_px;
                int vis_lines = max(1, (int)(g_rc_edit_box.bottom - g_rc_edit_box.top) / max(12, edit_px + 4));
                int max_top = max(1, total_lines - vis_lines);
                float ratio = (float)vis_lines / (float)total_lines;
                float thumb_h = max(32.0f, track_h * ratio);
                float travel = max(1.0f, track_h - thumb_h);
                float dy = (float)(pt.y - g_sb_drag_start_y);
                int target = (int)(g_sb_drag_start_scroll + (dy / travel) * (float)max_top + 0.5f);
                if (target < 0) target = 0;
                if (target > max_top) target = max_top;
                int cur_first = (int)SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
                if (target != cur_first) {
                    SendMessageW(g_hEdit, EM_LINESCROLL, 0, (LPARAM)(target - cur_first));
                    tv_invalidate_edit_scrollbar();
                }
            }
            return 0;
        }

        int ncap = 0, ntb = -1, nf = -1;
        for (int c = 0; c < 3; c++) if (PtInRect(&g_rc_cap[c], pt)) { ncap = c + 1; break; }
        for (int b = 0; b < 6; b++) if (PtInRect(&g_rc_tb[b], pt))  { ntb = b; break; }
        if (g_find_open) {
            for (int i = 0; i < 4; i++) if (PtInRect(&g_rc_find_btns[i], pt)) { nf = i; break; }
        }
        bool nbext = (g_rc_banner.bottom > g_rc_banner.top && PtInRect(&g_rc_banner_btn, pt) != 0);
        int ntoc = tv_hit_test_toc(pt);
        int ncb = -1, ncp = -1;
        const MdFrag *nlink = NULL;
        tv_hit_test_doc(pt, &ncb, &ncp, &nlink, NULL);
        bool nhlink = (nlink != NULL);
        bool nsb = (g_rc_sb_thumb.bottom > g_rc_sb_thumb.top &&
                    pt.x >= g_rc_doc.right - 14 && pt.x <= g_rc_doc.right &&
                    pt.y >= g_rc_doc.top && pt.y <= g_rc_doc.bottom);

        wchar_t new_url[256] = {0};
        if (nlink && nlink->url_len > 0) {
            size_t n = min((size_t)nlink->url_len, (size_t)255);
            memcpy(new_url, g_doc_text + nlink->url_off, n * sizeof(wchar_t));
            new_url[n] = L'\0';
        }

        if (ncap != g_hover_cap || ntb != g_hover_tb || nf != g_hover_find ||
            nbext != g_hover_banner_ext || ntoc != g_hover_toc ||
            ncb != g_hover_cb_blk || ncp != g_hover_copy_blk ||
            nhlink != g_hover_link || nsb != g_hover_sb ||
            wcscmp(new_url, g_hover_url) != 0) {
            g_hover_cap = ncap;
            g_hover_tb = ntb;
            g_hover_find = nf;
            g_hover_banner_ext = nbext;
            g_hover_toc = ntoc;
            g_hover_cb_blk = ncb;
            g_hover_copy_blk = ncp;
            g_hover_link = nhlink;
            g_hover_sb = nsb;
            wcsncpy_s(g_hover_url, 256, new_url, _TRUNCATE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = false;
        g_hover_cap = 0;
        g_hover_tb = -1;
        g_hover_find = -1;
        g_hover_banner_ext = false;
        g_hover_toc = -1;
        g_hover_cb_blk = -1;
        g_hover_copy_blk = -1;
        g_hover_link = false;
        g_hover_sb = false;
        g_hover_url[0] = L'\0';
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP:
        if (g_sb_dragging) {
            g_sb_dragging = false;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        if (g_is_markdown && !g_source_mode && PtInRect(&g_rc_doc, pt)) {
            int cb = -1, cp = -1, line = 1;
            const MdFrag *lf = NULL;
            tv_hit_test_doc(pt, &cb, &cp, &lf, &line);
            if (cb < 0 && cp < 0 && !lf) {
                tv_toggle_source_mode(line > 0 ? line : 1);
                return 0;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };

        /* 1. Caption buttons */
        for (int c = 0; c < 3; c++) {
            if (PtInRect(&g_rc_cap[c], pt)) {
                if (c == 0) ShowWindow(hwnd, SW_MINIMIZE);
                else if (c == 1) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                else PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
        }

        /* 2. Titlebar action buttons */
        for (int b = 0; b < 6; b++) {
            if (PtInRect(&g_rc_tb[b], pt)) {
                if (b == 0)      tv_toggle_sidebar();
                else if (b == 1) { if (g_is_markdown) tv_toggle_source_mode(0); else tv_toggle_word_wrap(); }
                else if (b == 2) tv_save_file();
                else if (b == 3) { if (g_find_open) tv_close_find_bar(); else tv_open_find_bar(); }
                else if (b == 4) tv_toggle_theme_override();
                else if (b == 5 && g_cur_path[0]) {
                    ShellExecuteW(hwnd, L"open", g_cur_path, NULL, g_cur_dir, SW_SHOWNORMAL);
                }
                return 0;
            }
        }

        /* 3. Banner external button */
        if (g_rc_banner.bottom > g_rc_banner.top && PtInRect(&g_rc_banner_btn, pt) && g_cur_path[0]) {
            ShellExecuteW(hwnd, L"open", g_cur_path, NULL, g_cur_dir, SW_SHOWNORMAL);
            return 0;
        }

        /* 4. Find bar buttons */
        if (g_find_open) {
            for (int i = 0; i < 4; i++) {
                if (PtInRect(&g_rc_find_btns[i], pt)) {
                    if (i == 0)      tv_find_step(false, false);
                    else if (i == 1) tv_find_step(true, false);
                    else if (i == 2) { g_find_case = !g_find_case; tv_find_step(true, true); InvalidateRect(hwnd, NULL, FALSE); }
                    else if (i == 3) tv_close_find_bar();
                    return 0;
                }
            }
        }

        /* 5. Outline sidebar heading click */
        int toc_idx = tv_hit_test_toc(pt);
        if (toc_idx >= 0) {
            const MdTocItem *ti = &((const MdTocItem *)g_toc.ptr)[toc_idx];
            g_active_toc_idx = toc_idx;
            if (g_is_markdown && !g_source_mode) {
                float vh = max(60.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
                float max_sc = max(0.0f, g_doc_h - vh);
                g_scroll_y = min(max_sc, ti->doc_y);
            } else if (g_hEdit) {
                tv_scroll_edit_to_line(ti->src_line);
                SetFocus(g_hEdit);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* 6. Custom scrollbar drag (both Rendered Mode and Source/Code Mode) */
        if (g_rc_sb_thumb.bottom > g_rc_sb_thumb.top &&
            pt.x >= g_rc_doc.right - 14 && pt.x <= g_rc_doc.right &&
            pt.y >= g_rc_doc.top && pt.y <= g_rc_doc.bottom) {
            if (g_is_markdown && !g_source_mode) {
                if (PtInRect(&g_rc_sb_thumb, pt)) {
                    g_sb_dragging = true;
                    g_sb_drag_start_y = pt.y;
                    g_sb_drag_start_scroll = g_scroll_y;
                    SetCapture(hwnd);
                } else {
                    float vh = max(60.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
                    float max_sc = max(0.0f, g_doc_h - vh);
                    float frac = (float)(pt.y - g_rc_doc.top) / vh;
                    g_scroll_y = max(0.0f, min(max_sc, frac * max_sc));
                    tv_update_active_toc();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else if (g_hEdit && IsWindowVisible(g_hEdit)) {
                int cur_first = (int)SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
                if (PtInRect(&g_rc_sb_thumb, pt)) {
                    g_sb_dragging = true;
                    g_sb_drag_start_y = pt.y;
                    g_sb_drag_start_scroll = (float)cur_first;
                    SetCapture(hwnd);
                } else {
                    int total_lines = max(1, (int)SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0));
                    int edit_px = g_is_markdown ? max(11, g_body_px - 1) : g_code_edit_px;
                    int vis_lines = max(1, (int)(g_rc_edit_box.bottom - g_rc_edit_box.top) / max(12, edit_px + 4));
                    int max_top = max(1, total_lines - vis_lines);
                    float vh = max(60.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
                    float frac = max(0.0f, min(1.0f, (float)(pt.y - g_rc_doc.top) / vh));
                    int target = (int)(frac * (float)max_top + 0.5f);
                    SendMessageW(g_hEdit, EM_LINESCROLL, 0, (LPARAM)(target - cur_first));
                    tv_invalidate_edit_scrollbar();
                }
            }
            return 0;
        }

        /* 7. Interactive Markdown clicks: Task checkbox toggle, Code Copy, Clickable link */
        if (g_is_markdown && !g_source_mode && PtInRect(&g_rc_doc, pt)) {
            int cb_blk = -1, cp_blk = -1;
            const MdFrag *lf = NULL;
            tv_hit_test_doc(pt, &cb_blk, &cp_blk, &lf, NULL);

            if (cb_blk >= 0 && (size_t)cb_blk < g_blocks.count) {
                if (g_read_only) {
                    tv_flash(L"File is read-only");
                } else {
                    MdBlock *b = &((MdBlock *)g_blocks.ptr)[cb_blk];
                    if (g_doc_text && b->task_char_off < g_doc_len) {
                        b->task_checked = !b->task_checked;
                        g_doc_text[b->task_char_off] = b->task_checked ? L'x' : L' ';
                        g_modified = true;
                        tv_update_window_title();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                SetFocus(hwnd);
                return 0;
            }

            if (cp_blk >= 0 && (size_t)cp_blk < g_blocks.count) {
                const MdBlock *b = &((const MdBlock *)g_blocks.ptr)[cp_blk];
                if (g_doc_text && b->code_len > 0) {
                    tv_copy_text_to_clipboard(hwnd, g_doc_text + b->code_off, b->code_len);
                    tv_flash(L"Copied code block");
                }
                SetFocus(hwnd);
                return 0;
            }

            if (lf && lf->url_len > 0) {
                tv_open_link_target(hwnd, g_doc_text + lf->url_off, lf->url_len);
                SetFocus(hwnd);
                return 0;
            }

            SetFocus(hwnd);
            return 0;
        }

        if (g_hEdit && IsWindowVisible(g_hEdit)) SetFocus(g_hEdit);
        else SetFocus(hwnd);
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && wParam == 'S') { tv_save_file(); return 0; }
        if (ctrl && wParam == 'F') { tv_open_find_bar(); return 0; }
        if (ctrl && wParam == 'E' && g_is_markdown) { tv_toggle_source_mode(0); return 0; }
        if (ctrl && wParam == 'J' && g_is_markdown) { tv_toggle_sidebar(); return 0; }
        if (ctrl && shift && wParam == 'T') { tv_toggle_theme_override(); return 0; }
        if (ctrl && (wParam == VK_OEM_PLUS || wParam == VK_ADD))      { tv_adjust_zoom(1); return 0; }
        if (ctrl && (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT)){ tv_adjust_zoom(-1); return 0; }
        if (ctrl && (wParam == '0' || wParam == VK_NUMPAD0))          { tv_reset_zoom(); return 0; }
        if (wParam == VK_F3) {
            if (g_find_query[0]) tv_find_step(!shift, false);
            else tv_open_find_bar();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (g_find_open) tv_close_find_bar();
            else PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (g_is_markdown && !g_source_mode) {
            float vh = max(120.0f, (float)(g_rc_doc.bottom - g_rc_doc.top));
            if (wParam == VK_DOWN)       { tv_scroll_doc_by(56.0f); return 0; }
            if (wParam == VK_UP)         { tv_scroll_doc_by(-56.0f); return 0; }
            if (wParam == VK_NEXT || wParam == VK_SPACE) { tv_scroll_doc_by(vh * 0.85f); return 0; }
            if (wParam == VK_PRIOR)      { tv_scroll_doc_by(-vh * 0.85f); return 0; }
            if (wParam == VK_HOME)       { tv_scroll_doc_by(-g_doc_h); return 0; }
            if (wParam == VK_END)        { tv_scroll_doc_by(g_doc_h); return 0; }
            if (wParam == VK_F2)         { tv_toggle_source_mode(0); return 0; }
        }
        break;
    }

    case WM_SYSKEYDOWN:
        if (wParam == 'Z') { tv_toggle_word_wrap(); return 0; }
        break;

    case WM_SYSCHAR:
        if (wParam == L'z' || wParam == L'Z') return 0;
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        tv_paint(hwnd);
        return 0;

    case WM_CLOSE:
        if (!tv_confirm_discard(hwnd)) return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_destroying = true;
        if (g_sb_dragging) { g_sb_dragging = false; ReleaseCapture(); }
        if (g_hEdit) {
            if (g_orig_edit_proc) {
                SetWindowLongPtrW(g_hEdit, GWLP_WNDPROC, (LONG_PTR)g_orig_edit_proc);
                g_orig_edit_proc = NULL;
            }
            SetWindowTextW(g_hEdit, L"");
            SendMessageW(g_hEdit, EM_EMPTYUNDOBUFFER, 0, 0);
            g_hEdit = NULL;
        }
        if (g_hFindEdit) {
            if (g_orig_find_proc) {
                SetWindowLongPtrW(g_hFindEdit, GWLP_WNDPROC, (LONG_PTR)g_orig_find_proc);
                g_orig_find_proc = NULL;
            }
            SetWindowTextW(g_hFindEdit, L"");
            g_hFindEdit = NULL;
        }
        /* Free 100% of document, AST, layout, TOC, find, and DirectWrite format buffers immediately */
        tv_free_doc_all();
        tv_dw_clear_cache();
        g_hTvWnd = NULL;
        g_cur_path[0] = L'\0';
        g_cur_dir[0] = L'\0';
        g_cur_name[0] = L'\0';
        g_find_query[0] = L'\0';
        g_flash_msg[0] = L'\0';
        g_hover_url[0] = L'\0';
        g_cur_file_size = 0;
        g_modified = false;
        g_read_only = false;
        g_is_truncated = false;
        g_is_binary_guard = false;
        g_is_markdown = false;
        g_source_mode = false;
        g_find_open = false;
        g_find_match_idx = 0;
        g_find_match_total = 0;
        g_theme_override = 0;
        HeapCompact(GetProcessHeap(), 0);
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
        if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_WINDOWS_CHANGED, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* =========================================================================
 * The editor's lifecycle, as lares-view.exe calls it
 * ========================================================================= */
void textview_init(HINSTANCE hInst) {
    tv_dw_ensure_factory();
    g_hUiFont = render_create_ui_font(13, false);
    tv_rebuild_mono_font();
    tv_refresh_brushes();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = tv_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = TEXTVIEW_CLASS;
    RegisterClassExW(&wc);
}

void textview_open(const wchar_t *file_path) {
    /* --text-light and --text-dark pick the palette, for checks */
    const wchar_t *cmd = GetCommandLineW();
    if (cmd) {
        if (wcsstr(cmd, L"--text-light") != NULL) g_theme_override = 1;
        else if (wcsstr(cmd, L"--text-dark") != NULL) g_theme_override = 2;
    }

    if (g_hTvWnd && IsWindow(g_hTvWnd)) {
        if (!tv_confirm_discard(g_hTvWnd)) return;
        g_find_open = false;
        tv_refresh_brushes();
        tv_load_file(file_path);
        ShowWindow(g_hTvWnd, IsIconic(g_hTvWnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_hTvWnd);
        if (g_hEdit && IsWindowVisible(g_hEdit)) SetFocus(g_hEdit);
        else SetFocus(g_hTvWnd);
        return;
    }

    g_destroying = false;
    g_find_open = false;
    g_hover_cap = 0;
    g_hover_tb = -1;
    g_hover_find = -1;
    g_hover_toc = -1;
    g_hover_cb_blk = -1;
    g_hover_copy_blk = -1;
    g_hover_link = false;
    g_hover_sb = false;
    g_hover_banner_ext = false;
    tv_refresh_brushes();

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    /* spacious default window (1200x800 clamped nicely to work area) */
    int w = min(1180, (wa.right - wa.left) - 80);
    int h = min(780,  (wa.bottom - wa.top) - 70);
    if (w < 720) w = 720;
    if (h < 480) h = 480;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    /* A test copy's window sits far off the screen and out of Alt+Tab. */
    if (g_cfg.isolated) x = -30000;
    g_hTvWnd = CreateWindowExW(
        g_cfg.isolated ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW,
        TEXTVIEW_CLASS, L"Markdown",
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        NULL, NULL, g_hInst, NULL);
    if (!g_hTvWnd) return;

    BOOL dark = tv_is_dark() ? TRUE : FALSE;
    DWORD corner = 2; /* DWMWCP_ROUND */
    MARGINS m = { 0, 0, 1, 0 };
    DwmSetWindowAttribute(g_hTvWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_hTvWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    DwmExtendFrameIntoClientArea(g_hTvWnd, &m);
    SetWindowPos(g_hTvWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    RECT cr;
    GetClientRect(g_hTvWnd, &cr);
    tv_update_layout(cr.right, cr.bottom);
    tv_load_file(file_path);

    if (g_cfg.isolated) {
        ShowWindow(g_hTvWnd, SW_SHOWNOACTIVATE);
        return;
    }
    ShowWindow(g_hTvWnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hTvWnd);
    if (g_hEdit && IsWindowVisible(g_hEdit)) SetFocus(g_hEdit);
    else SetFocus(g_hTvWnd);
    if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_WINDOWS_CHANGED, 0, 0);
}

HWND textview_window(void) {
    return g_hTvWnd;
}

void textview_on_theme_changed(void) {
    g_theme_override = 0; /* Follow OS Light/Dark transition cleanly */
    tv_refresh_brushes();
    if (g_hTvWnd && IsWindow(g_hTvWnd)) {
        BOOL dark = tv_is_dark() ? TRUE : FALSE;
        DwmSetWindowAttribute(g_hTvWnd, 20, &dark, sizeof(dark));
        InvalidateRect(g_hTvWnd, NULL, TRUE);
        if (g_hEdit) InvalidateRect(g_hEdit, NULL, TRUE);
        if (g_hFindEdit) InvalidateRect(g_hFindEdit, NULL, TRUE);
    }
}

bool textview_is_open(void) {
    return (g_hTvWnd != NULL && IsWindow(g_hTvWnd));
}

void textview_shutdown(void) {
    if (g_hTvWnd && IsWindow(g_hTvWnd)) {
        g_modified = false;
        DestroyWindow(g_hTvWnd);
        g_hTvWnd = NULL;
    }
    tv_free_doc_all();
    tv_dw_clear_cache();
    if (g_tv_dw) {
        g_tv_dw->lpVtbl->Release(g_tv_dw);
        g_tv_dw = NULL;
    }
    if (g_hMonoFont)  { DeleteObject(g_hMonoFont);  g_hMonoFont = NULL; }
    if (g_hUiFont)    { DeleteObject(g_hUiFont);    g_hUiFont = NULL; }
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
    if (g_hFindBrush) { DeleteObject(g_hFindBrush); g_hFindBrush = NULL; }
}
