// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"
#include "programs.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <wincodec.h>
#pragma warning(pop)
#include <stdio.h>
#include <stdlib.h>

/*
 * Files, in lares-files.exe (issue #153).
 * - A light and a dark palette, following Windows' mode.
 * - A folder is read one level at a time, on a worker thread: nothing is
 *   ever searched through recursively.
 * - A thumbnail cache of at most 48 pictures of up to 120 x 120 (under
 *   3 MB), read through Windows Imaging Component.
 * - Its own icons and menus: no other program's context menu or icon
 *   handler is loaded into it.
 * - Tabs, a sidebar with places, bookmarks and drives, breadcrumbs, an
 *   editable path, a filter, list and grid views, an info pane, rename (F2),
 *   copy, cut and paste (CF_HDROP), and delete to the Recycle Bin.
 * - Quick Look (Space): a preview panel that does not take the focus, for
 *   pictures, Markdown and text; Up and Down step through the neighbours,
 *   Enter opens the viewer or editor, Space or Esc closes it and frees all
 *   of it. Quick Look is a trademark of Apple Inc.; Lares is not
 *   affiliated with Apple.
 * - Drag and drop inside Files onto folders, bookmarks and drives (move,
 *   Ctrl copies) through Windows' own file operations, a resizable sidebar
 *   and list columns, and buttons that act when released.
 */

#define FILES_CLASS      L"LaresFilesWnd"
#define ID_FILES_ADDR    4001
#define ID_FILES_FILTER  4002
#define ID_FILES_RENAME  4003

#define MAX_FILES_WINS   8
#define MAX_FILES_TABS   8
#define MAX_HIST          32
#define MAX_DRIVES        16
#define MAX_BOOKMARKS     16
#define MAX_SIDEBAR_ITEMS 40
#define MAX_THUMB_SLOTS   48
#define THUMB_DIM         120

typedef enum {
    SORT_NAME = 0,
    SORT_DATE = 1,
    SORT_SIZE = 2,
    SORT_TYPE = 3
} SortCol;

typedef struct {
    wchar_t  name[MAX_PATH];
    wchar_t  name_lower[MAX_PATH];
    wchar_t  ext[24];
    uint64_t size;
    FILETIME mtime;
    DWORD    attrs;
    bool     is_dir;
    bool     is_image;
    bool     is_text;
    bool     selected;
} FileEntry;

typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t title[96];
    wchar_t hist[MAX_HIST][MAX_PATH];
    int     hist_count;
    int     hist_pos;
    int     scroll_y;
} FilesTab;

typedef struct {
    wchar_t  label[64];
    wchar_t  path[MAX_PATH];
    wchar_t  icon_rel[MAX_PATH];
    uint64_t free_bytes;
    uint64_t total_bytes;
    bool     is_drive;
    bool     is_bookmark;
    int      bookmark_idx;
} SidebarItem;

typedef struct {
    wchar_t name[64];
    wchar_t path[MAX_PATH];
} FilesBookmark;

typedef struct FilesWindow {
    HWND        hwnd;
    HWND        hAddrEdit;
    HWND        hFilterEdit;
    HWND        hRenameEdit;
    WNDPROC     orig_addr_proc;
    WNDPROC     orig_filter_proc;
    WNDPROC     orig_rename_proc;
    bool        editing_addr;
    bool        destroying;
    int         renaming_vis_idx;

    FilesTab   tabs[MAX_FILES_TABS];
    int         tab_count;
    int         active_tab;

    /* Loaded entries for the active tab (heap-allocated, freed on navigation/close) */
    FileEntry  *entries;
    int         entry_count;
    int        *visible; /* filtered/sorted indices into entries */
    int         visible_count;
    bool        loading;
    LONG        load_gen;
    wchar_t     filter_text[128];
    wchar_t     pending_select_name[MAX_PATH];

    bool        grid_view;
    bool        show_hidden;
    bool        show_info_pane;
    SortCol     sort_col;
    bool        sort_asc;
    int         focus_vis_idx;
    int         hover_vis_idx;
    int         hover_sidebar_idx;
    int         hover_toolbar_btn;
    int         hover_crumb_idx;
    int         hover_caption_btn; /* 0=none, 1=min, 2=max, 3=close */
    bool        hover_tab_add;
    int         hover_tab_close_idx;
    bool        tracking_mouse;
    int         scroll_y;
    int         content_h;
    int         hover_tab_idx;

    SidebarItem sidebar[MAX_SIDEBAR_ITEMS];
    int         sidebar_count;
    uint64_t    cur_drive_free;
    uint64_t    cur_drive_total;
    wchar_t     cur_drive_label[64];
    bool        cur_drive_valid;

    /* Cached layout rects */
    RECT rc_tabs[MAX_FILES_TABS];
    RECT rc_tab_close[MAX_FILES_TABS];
    RECT rc_tab_add;
    RECT rc_cap_btns[3]; /* Min, Max/Restore, Close */
    RECT rc_tb_btns[9];  /* Back, Fwd, Up, Refresh, NewFolder, View, Hidden, Info, Term */
    RECT rc_addr_bar;
    RECT rc_filter_bar;
    RECT rc_sidebar_items[MAX_SIDEBAR_ITEMS];
    RECT rc_col_hdr[4];
    RECT rc_file_area;
    RECT rc_info_pane;
    RECT rc_info_btns[4]; /* Open, Term, CopyPath, Delete */
    RECT rc_crumbs[12];
    wchar_t crumb_paths[12][MAX_PATH];
    int  crumb_count;

    /* Resizable sidebar + List view columns (Name flexes; Date/Size/Type are user sized) */
    int  sidebar_w;
    int  col_w[4];
    int  resize_kind;        /* 0 = none, 1 = sidebar, 2..4 = column divider 1..3 */
    int  resize_start_x;
    int  resize_start_val[4];
    int  hover_divider;      /* -1 = none, 0 = sidebar, 1..3 = column dividers */

    /* press-and-release feedback for toolbar / caption / tab / action buttons */
    int  pressed_btn;
    bool pressed_inside;
    int  hover_info_btn;

    /* Drag source arming + deferred click selection (so multi-selections can be dragged) */
    bool  drag_armed;
    POINT drag_origin;
    int   deferred_select_vis;
    int   deferred_toggle_vis;
} FilesWindow;

/* Pressable button ids (pressed_btn) */
#define PB_TB_BASE        100  /* + 0..8 toolbar */
#define PB_CAP_BASE       200  /* + 0..2 caption */
#define PB_TAB_ADD        300
#define PB_TAB_CLOSE_BASE 400  /* + tab index */
#define PB_INFO_BASE      500  /* + 0..3 info pane actions */

#define SIDEBAR_MIN_W     150
#define SIDEBAR_MAX_W     360
#define COL_MIN_NAME      120
static const int g_col_min_w[4] = { COL_MIN_NAME, 80, 56, 52 };

/* Last sizes chosen by the user, inherited by new Files windows (in memory only) */
static int g_last_sidebar_w = 200;
static int g_last_col_w[4] = { 0, 136, 84, 76 };

/* In-process drag & drop between Files windows (capture based, no OLE drop target needed) */
enum { DROP_NONE = 0, DROP_FOLDER_ITEM = 1, DROP_SIDEBAR = 2, DROP_WINDOW_DIR = 3 };

typedef struct {
    bool          active;
    struct FilesWindow *src;
    wchar_t      *paths;        /* double-null-terminated source list (heap, freed on end) */
    int           count;
    wchar_t       src_dir[MAX_PATH];
    wchar_t       first_name[MAX_PATH];
    struct FilesWindow *over;  /* Files window under the cursor (may be src) */
    int           target_kind;
    int           target_idx;
    wchar_t       target_path[MAX_PATH];
    wchar_t       target_label[96];
    bool          valid;
    bool          copy;
    POINT         cursor;       /* client coords inside 'over' */
} FilesDrag;

static FilesDrag g_drag;

static bool files_drag_is_over(const struct FilesWindow *fw) {
    return g_drag.active && g_drag.over == fw && g_drag.valid;
}

/* ---------- Quick Look panel state (one at a time, freed completely on close) ---------- */
#define QL_CLASS        L"LaresQuickLookWnd"
#define QL_SHADOW       26
#define QL_TITLE_H      52
#define QL_TEXT_CAP     (256 * 1024)
#define QL_MD_CAP       (160 * 1024)
#define QL_MAX_BLOCKS   6000

enum { QL_KIND_GENERIC = 0, QL_KIND_IMAGE = 1, QL_KIND_MARKDOWN = 2, QL_KIND_TEXT = 3 };
enum { QB_PARA = 0, QB_H1, QB_H2, QB_H3, QB_H4, QB_H5, QB_H6, QB_BULLET, QB_NUMBER, QB_QUOTE,
       QB_CODE, QB_RULE, QB_TASK_OPEN, QB_TASK_DONE, QB_MONO, QB_BLANK };

typedef struct {
    int     start;   /* offset into QuickLook.disp */
    int     len;
    uint8_t type;
    uint8_t indent;
    uint8_t code_first;
    uint8_t code_last;
    float   y;
    float   h;
} QlBlock;

typedef struct {
    HWND         hwnd;
    struct FilesWindow *owner;
    LayerSurface surf;
    int          kind;
    int          vis_idx;
    wchar_t      path[MAX_PATH];
    wchar_t      name[MAX_PATH];
    wchar_t      meta[160];
    bool         is_dir;
    /* image (VirtualAlloc backed, decoded straight to display size) */
    BYTE        *img_px;
    GpBitmap    *img;
    int          img_w, img_h;
    /* text / markdown (VirtualAlloc backed) */
    wchar_t     *disp;
    SIZE_T       disp_bytes;
    QlBlock     *blocks;
    SIZE_T       blocks_bytes;
    int          block_count;
    bool         truncated;
    float        content_h;
    int          scroll_y;
    /* geometry: panel size + window position (window = panel + shadow margin) */
    int          panel_w, panel_h;
    int          win_x, win_y;
    int          hover_btn;  /* 1 = close, 2 = open */
    int          press_btn;
} QuickLook;

static QuickLook g_ql;

static bool ql_is_open_for(const struct FilesWindow *fw) {
    return g_ql.hwnd != NULL && g_ql.owner == fw;
}

static void ql_close(void);

/* Strict <= 4 MB global LRU thumbnail cache shared across Files windows */
typedef struct {
    wchar_t   path[MAX_PATH];
    GpBitmap *bmp;
    uint32_t  tick;
} ThumbSlot;

static ThumbSlot     g_thumbs[MAX_THUMB_SLOTS];
static int           g_thumb_count = 0;
static uint32_t      g_thumb_tick = 1;
static FilesWindow  *g_wins[MAX_FILES_WINS];
static int            g_win_count = 0;
static FilesBookmark g_bookmarks[MAX_BOOKMARKS];
static int            g_bookmark_count = 0;
static bool           g_bookmarks_loaded = false;
static HFONT          g_hEditFont = NULL;
static HBRUSH         g_hEditBrush = NULL;

static bool paths_equal_norm(const wchar_t *a, const wchar_t *b);

/* Background loader payload */
typedef struct {
    HWND       hwnd;
    LONG       gen;
    wchar_t    path[MAX_PATH];
    FileEntry *entries;
    int        count;
} LoadResult;

static ARGB argb_from_cref(BYTE a, COLORREF c) {
    return MAKE_ARGB(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static bool is_image_ext(const wchar_t *ext) {
    return viewer_is_supported_ext(ext);
}

static void str_lower_f(const wchar_t *src, wchar_t *dst, size_t max_len) {
    size_t i = 0;
    for (; src[i] && i + 1 < max_len; i++) dst[i] = (wchar_t)towlower(src[i]);
    dst[i] = L'\0';
}

static void format_bytes(uint64_t bytes, wchar_t *out, size_t len) {
    if (bytes < 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        _snwprintf_s(out, len, _TRUNCATE, L"%.2f TB", (double)bytes / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    }
}

static void format_filetime(const FILETIME *ft, wchar_t *out, size_t len) {
    FILETIME lft;
    SYSTEMTIME st;
    if (FileTimeToLocalFileTime(ft, &lft) && FileTimeToSystemTime(&lft, &st)) {
        _snwprintf_s(out, len, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    } else {
        wcsncpy_s(out, len, L"-", _TRUNCATE);
    }
}

static void thumb_cache_clear(void) {
    for (int i = 0; i < g_thumb_count; i++) {
        if (g_thumbs[i].bmp) {
            GdipDisposeImage((GpImage *)g_thumbs[i].bmp);
            g_thumbs[i].bmp = NULL;
        }
    }
    g_thumb_count = 0;
}

GpStatus WINAPI GdipGetImageGraphicsContext(GpImage *image, GpGraphics **graphics);

static GpBitmap *thumb_cache_get(const wchar_t *full_path) {
    for (int i = 0; i < g_thumb_count; i++) {
        if (_wcsicmp(g_thumbs[i].path, full_path) == 0) {
            g_thumbs[i].tick = ++g_thumb_tick;
            return g_thumbs[i].bmp;
        }
    }

    /* Guard against giant files (> 24 MB) to keep RAM strictly bounded */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(full_path, GetFileExInfoStandard, &fad)) return NULL;
    ULARGE_INTEGER fsz;
    fsz.LowPart = fad.nFileSizeLow;
    fsz.HighPart = fad.nFileSizeHigh;
    if (fsz.QuadPart > 24ULL * 1024ULL * 1024ULL) return NULL;

    GpBitmap *src = NULL;
    if (GdipCreateBitmapFromFile(full_path, &src) != GP_OK || !src) return NULL;

    UINT sw = 0, sh = 0;
    GdipGetImageWidth((GpImage *)src, &sw);
    GdipGetImageHeight((GpImage *)src, &sh);
    if (sw == 0 || sh == 0) {
        GdipDisposeImage((GpImage *)src);
        return NULL;
    }

    int dw = THUMB_DIM, dh = THUMB_DIM;
    if (sw > sh) {
        dh = (int)((sh * THUMB_DIM) / sw);
        if (dh < 1) dh = 1;
    } else {
        dw = (int)((sw * THUMB_DIM) / sh);
        if (dw < 1) dw = 1;
    }

    GpBitmap *scaled = NULL;
    if (GdipCreateBitmapFromScan0(dw, dh, 0, GP_PIXELFORMAT_32BPP_ARGB, NULL, &scaled) == GP_OK && scaled) {
        GpGraphics *g = NULL;
        if (GdipGetImageGraphicsContext((GpImage *)scaled, &g) == GP_OK && g) {
            GdipSetInterpolationMode(g, GP_INTERP_HIGHQUALITY_BICUBIC);
            GdipDrawImageRectI(g, (GpImage *)src, 0, 0, dw, dh);
            GdipDeleteGraphics(g);
        }
    }
    GdipDisposeImage((GpImage *)src);
    if (!scaled) return NULL;

    int slot = g_thumb_count;
    if (slot >= MAX_THUMB_SLOTS) {
        slot = 0;
        uint32_t oldest = g_thumbs[0].tick;
        for (int i = 1; i < MAX_THUMB_SLOTS; i++) {
            if (g_thumbs[i].tick < oldest) { oldest = g_thumbs[i].tick; slot = i; }
        }
        if (g_thumbs[slot].bmp) {
            GdipDisposeImage((GpImage *)g_thumbs[slot].bmp);
            g_thumbs[slot].bmp = NULL;
        }
    } else {
        g_thumb_count++;
    }

    wcsncpy_s(g_thumbs[slot].path, MAX_PATH, full_path, _TRUNCATE);
    g_thumbs[slot].bmp = scaled;
    g_thumbs[slot].tick = ++g_thumb_tick;
    return scaled;
}

/* Bookmarks live in Lares' own settings (shell.ini, through shell_prefs.h). */
static void bookmarks_load(void) {
    g_bookmark_count = 0;
    g_bookmarks_loaded = true;
    static LaresBookmark marks[MAX_BOOKMARKS];
    int n = lares_shell_bookmarks_load(marks, MAX_BOOKMARKS);
    for (int i = 0; i < n; i++) {
        if (!marks[i].path[0]) continue;
        wchar_t name[64];
        wcsncpy_s(name, 64, marks[i].name, _TRUNCATE);
        if (!name[0]) {
            wchar_t tmp[MAX_PATH];
            wcsncpy_s(tmp, MAX_PATH, marks[i].path, _TRUNCATE);
            PathRemoveBackslashW(tmp);
            const wchar_t *fn = PathFindFileNameW(tmp);
            wcsncpy_s(name, 64, (fn && fn[0]) ? fn : tmp, _TRUNCATE);
        }
        wcsncpy_s(g_bookmarks[g_bookmark_count].name, 64, name, _TRUNCATE);
        wcsncpy_s(g_bookmarks[g_bookmark_count].path, MAX_PATH, marks[i].path, _TRUNCATE);
        g_bookmark_count++;
    }
}

static void bookmarks_save(void) {
    if (g_cfg.isolated) return;
    static LaresBookmark marks[MAX_BOOKMARKS];
    for (int i = 0; i < g_bookmark_count; i++) {
        wcsncpy_s(marks[i].name, LARES_BOOKMARK_NAME, g_bookmarks[i].name, _TRUNCATE);
        wcsncpy_s(marks[i].path, LARES_PIN_PATH, g_bookmarks[i].path, _TRUNCATE);
    }
    lares_shell_bookmarks_save(marks, g_bookmark_count);
}

static int bookmarks_find_idx(const wchar_t *path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < g_bookmark_count; i++) {
        if (paths_equal_norm(g_bookmarks[i].path, path)) return i;
    }
    return -1;
}

static bool is_builtin_or_drive_sidebar_path(const FilesWindow *fw, const wchar_t *path) {
    if (!fw || !path || !path[0]) return false;
    for (int i = 0; i < fw->sidebar_count; i++) {
        if (!fw->sidebar[i].is_bookmark && paths_equal_norm(fw->sidebar[i].path, path)) {
            return true;
        }
    }
    return false;
}

static void build_sidebar(FilesWindow *fw);
static void refresh_all_sidebars(void) {
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i] && g_wins[i]->hwnd) {
            build_sidebar(g_wins[i]);
            InvalidateRect(g_wins[i]->hwnd, NULL, FALSE);
        }
    }
}

static bool bookmarks_toggle_path(const FilesWindow *fw, const wchar_t *raw_path) {
    if (!raw_path || !raw_path[0]) return false;
    wchar_t clean[MAX_PATH];
    wcsncpy_s(clean, MAX_PATH, raw_path, _TRUNCATE);
    size_t n = wcslen(clean);
    while (n > 3 && clean[n - 1] == L'\\') clean[--n] = L'\0';

    int existing = bookmarks_find_idx(clean);
    if (existing >= 0) {
        memmove(&g_bookmarks[existing], &g_bookmarks[existing + 1],
                (size_t)(g_bookmark_count - existing - 1) * sizeof(FilesBookmark));
        g_bookmark_count--;
        bookmarks_save();
        refresh_all_sidebars();
        return false;
    }

    if (is_builtin_or_drive_sidebar_path(fw, clean)) return false;
    DWORD attr = GetFileAttributesW(clean);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (g_bookmark_count >= MAX_BOOKMARKS) return false;

    const wchar_t *fn = PathFindFileNameW(clean);
    wcsncpy_s(g_bookmarks[g_bookmark_count].name, 64, (fn && fn[0]) ? fn : clean, _TRUNCATE);
    wcsncpy_s(g_bookmarks[g_bookmark_count].path, MAX_PATH, clean, _TRUNCATE);
    g_bookmark_count++;
    bookmarks_save();
    refresh_all_sidebars();
    return true;
}

static void bookmarks_remove_idx(int idx) {
    if (idx < 0 || idx >= g_bookmark_count) return;
    memmove(&g_bookmarks[idx], &g_bookmarks[idx + 1],
            (size_t)(g_bookmark_count - idx - 1) * sizeof(FilesBookmark));
    g_bookmark_count--;
    bookmarks_save();
    refresh_all_sidebars();
}

static void bookmarks_move_idx(int idx, int delta) {
    int dst = idx + delta;
    if (idx < 0 || idx >= g_bookmark_count || dst < 0 || dst >= g_bookmark_count) return;
    FilesBookmark tmp = g_bookmarks[idx];
    g_bookmarks[idx] = g_bookmarks[dst];
    g_bookmarks[dst] = tmp;
    bookmarks_save();
    refresh_all_sidebars();
}

static void build_sidebar(FilesWindow *fw) {
    if (!g_bookmarks_loaded) bookmarks_load();
    fw->sidebar_count = 0;

    /* The user's own folders where Windows keeps them (they may have been
     * moved, to another drive or a synced folder): never a fixed path. */
    typedef struct { const wchar_t *lbl; const KNOWNFOLDERID *id; } FavDir;
    static const FavDir dirs[] = {
        { L"Home",      &FOLDERID_Profile },
        { L"Desktop",   &FOLDERID_Desktop },
        { L"Documents", &FOLDERID_Documents },
        { L"Downloads", &FOLDERID_Downloads },
        { L"Pictures",  &FOLDERID_Pictures },
        { L"Music",     &FOLDERID_Music },
        { L"Videos",    &FOLDERID_Videos }
    };

    for (int i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])) && fw->sidebar_count < MAX_SIDEBAR_ITEMS; i++) {
        PWSTR known = NULL;
        if (FAILED(SHGetKnownFolderPath(dirs[i].id, 0, NULL, &known)) || !known) {
            if (known) CoTaskMemFree(known);
            continue;
        }
        SidebarItem *s = &fw->sidebar[fw->sidebar_count++];
        ZeroMemory(s, sizeof(*s));
        wcsncpy_s(s->label, 64, dirs[i].lbl, _TRUNCATE);
        wcsncpy_s(s->path, MAX_PATH, known, _TRUNCATE);
        CoTaskMemFree(known);
        s->is_drive = false;
        s->is_bookmark = false;
        s->bookmark_idx = -1;
    }

    for (int i = 0; i < g_bookmark_count && fw->sidebar_count < MAX_SIDEBAR_ITEMS; i++) {
        SidebarItem *s = &fw->sidebar[fw->sidebar_count++];
        ZeroMemory(s, sizeof(*s));
        wcsncpy_s(s->label, 64, g_bookmarks[i].name, _TRUNCATE);
        wcsncpy_s(s->path, MAX_PATH, g_bookmarks[i].path, _TRUNCATE);
        s->is_drive = false;
        s->is_bookmark = true;
        s->bookmark_idx = i;
    }

    wchar_t drv_buf[256] = {0};
    DWORD n = GetLogicalDriveStringsW(255, drv_buf);
    const wchar_t *p = drv_buf;
    while (p < drv_buf + n && *p && fw->sidebar_count < MAX_SIDEBAR_ITEMS) {
        UINT dt = GetDriveTypeW(p);
        if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE || dt == DRIVE_REMOTE) {
            SidebarItem *s = &fw->sidebar[fw->sidebar_count++];
            ZeroMemory(s, sizeof(*s));
            wcsncpy_s(s->path, MAX_PATH, p, _TRUNCATE);
            wchar_t vol[64] = {0};
            GetVolumeInformationW(p, vol, 64, NULL, NULL, NULL, NULL, 0);
            if (vol[0]) _snwprintf_s(s->label, 64, _TRUNCATE, L"%ls (%c:)", vol, p[0]);
            else        _snwprintf_s(s->label, 64, _TRUNCATE, L"Local Disk (%c:)", p[0]);
            ULARGE_INTEGER avail, total, freeb;
            if (GetDiskFreeSpaceExW(p, &avail, &total, &freeb)) {
                s->free_bytes = avail.QuadPart;
                s->total_bytes = total.QuadPart;
            }
            s->is_drive = true;
            s->is_bookmark = false;
            s->bookmark_idx = -1;
        }
        p += wcslen(p) + 1;
    }
}

/* Lazy single-directory worker thread (NEVER recurses!) */
static DWORD WINAPI dir_load_worker(LPVOID lp) {
    LoadResult *res = (LoadResult *)lp;
    wchar_t pat[MAX_PATH];
    size_t plen = wcslen(res->path);
    if (plen > 0 && res->path[plen - 1] == L'\\') {
        _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls*", res->path);
    } else {
        _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\*", res->path);
    }

    int cap = 128;
    int cnt = 0;
    FileEntry *arr = (FileEntry *)HeapAlloc(GetProcessHeap(), 0, (size_t)cap * sizeof(FileEntry));
    if (arr) {
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (cnt >= cap) {
                    if (cap >= 32768) break; /* safety ceiling for extreme directories */
                    cap *= 2;
                    FileEntry *grown = (FileEntry *)HeapReAlloc(GetProcessHeap(), 0, arr, (size_t)cap * sizeof(FileEntry));
                    if (!grown) break;
                    arr = grown;
                }
                FileEntry *e = &arr[cnt++];
                ZeroMemory(e, sizeof(*e));
                wcsncpy_s(e->name, MAX_PATH, fd.cFileName, _TRUNCATE);
                str_lower_f(fd.cFileName, e->name_lower, MAX_PATH);
                e->attrs = fd.dwFileAttributes;
                e->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e->mtime = fd.ftLastWriteTime;
                ULARGE_INTEGER sz;
                sz.LowPart = fd.nFileSizeLow;
                sz.HighPart = fd.nFileSizeHigh;
                e->size = sz.QuadPart;
                if (!e->is_dir) {
                    const wchar_t *ext = PathFindExtensionW(fd.cFileName);
                    if (ext && ext[0]) {
                        wcsncpy_s(e->ext, 24, ext, _TRUNCATE);
                        e->is_image = is_image_ext(ext);
                        e->is_text  = textview_is_supported_ext(ext);
                    }
                }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    }

    res->entries = arr;
    res->count = cnt;
    if (!PostMessageW(res->hwnd, WM_LARES_FILES_LOADED, 0, (LPARAM)res)) {
        if (arr) HeapFree(GetProcessHeap(), 0, arr);
        HeapFree(GetProcessHeap(), 0, res);
    }
    return 0;
}

static wchar_t g_crumb_root[MAX_PATH];

static FilesWindow *g_sort_ctx = NULL;
static int cmp_visible_idx(const void *pa, const void *pb) {
    const FileEntry *a = &g_sort_ctx->entries[*(const int *)pa];
    const FileEntry *b = &g_sort_ctx->entries[*(const int *)pb];
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;

    int cmp = 0;
    switch (g_sort_ctx->sort_col) {
    case SORT_DATE:
        cmp = CompareFileTime(&a->mtime, &b->mtime);
        break;
    case SORT_SIZE:
        cmp = (a->size < b->size) ? -1 : (a->size > b->size ? 1 : 0);
        break;
    case SORT_TYPE:
        cmp = _wcsicmp(a->ext, b->ext);
        if (cmp == 0) cmp = StrCmpLogicalW(a->name, b->name);
        break;
    case SORT_NAME:
    default:
        cmp = StrCmpLogicalW(a->name, b->name);
        break;
    }
    return g_sort_ctx->sort_asc ? cmp : -cmp;
}

static void rebuild_visible(FilesWindow *fw) {
    if (fw->visible) {
        HeapFree(GetProcessHeap(), 0, fw->visible);
        fw->visible = NULL;
    }
    fw->visible_count = 0;
    if (!fw->entries || fw->entry_count == 0) return;

    fw->visible = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)fw->entry_count * sizeof(int));
    if (!fw->visible) return;

    wchar_t flow[128];
    str_lower_f(fw->filter_text, flow, 128);

    for (int i = 0; i < fw->entry_count; i++) {
        const FileEntry *e = &fw->entries[i];
        if (!fw->show_hidden &&
            ((e->attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) || e->name[0] == L'.')) {
            continue;
        }
        if (flow[0] && wcsstr(e->name_lower, flow) == NULL) {
            continue;
        }
        fw->visible[fw->visible_count++] = i;
    }

    if (fw->visible_count > 1) {
        g_sort_ctx = fw;
        qsort(fw->visible, (size_t)fw->visible_count, sizeof(int), cmp_visible_idx);
        g_sort_ctx = NULL;
    }
    if (fw->focus_vis_idx >= fw->visible_count) fw->focus_vis_idx = fw->visible_count - 1;
}

static void update_tab_title(FilesTab *tab) {
    wchar_t clean[MAX_PATH];
    wcsncpy_s(clean, MAX_PATH, tab->path, _TRUNCATE);
    size_t n = wcslen(clean);
    if (n > 3 && clean[n - 1] == L'\\') clean[n - 1] = L'\0';
    const wchar_t *fn = PathFindFileNameW(clean);
    if (fn && fn[0] && wcscmp(fn, clean) != 0) {
        wcsncpy_s(tab->title, 96, fn, _TRUNCATE);
    } else {
        wcsncpy_s(tab->title, 96, clean, _TRUNCATE);
    }
}

static void refresh_drive_space(FilesWindow *fw) {
    if (!fw) return;
    /* Refresh live free/total space for every sidebar drive */
    for (int i = 0; i < fw->sidebar_count; i++) {
        if (!fw->sidebar[i].is_drive) continue;
        ULARGE_INTEGER avail, total, freeb;
        if (GetDiskFreeSpaceExW(fw->sidebar[i].path, &avail, &total, &freeb)) {
            fw->sidebar[i].free_bytes = avail.QuadPart;
            fw->sidebar[i].total_bytes = total.QuadPart;
        }
    }

    /* Resolve live capacity/available space for the active tab's volume */
    fw->cur_drive_valid = false;
    fw->cur_drive_free = 0;
    fw->cur_drive_total = 0;
    fw->cur_drive_label[0] = L'\0';
    if (fw->tab_count <= 0) return;

    const wchar_t *tp = fw->tabs[fw->active_tab].path;
    if (!tp || !tp[0]) return;

    wchar_t root[MAX_PATH] = {0};
    if (tp[0] && tp[1] == L':') {
        root[0] = tp[0]; root[1] = L':'; root[2] = L'\\'; root[3] = L'\0';
    } else {
        wcsncpy_s(root, MAX_PATH, tp, _TRUNCATE);
    }

    ULARGE_INTEGER avail, total, freeb;
    if (GetDiskFreeSpaceExW(root, &avail, &total, &freeb)) {
        fw->cur_drive_free = avail.QuadPart;
        fw->cur_drive_total = total.QuadPart;
        fw->cur_drive_valid = (total.QuadPart > 0);
        for (int i = 0; i < fw->sidebar_count; i++) {
            if (fw->sidebar[i].is_drive && _wcsicmp(fw->sidebar[i].path, root) == 0) {
                wcsncpy_s(fw->cur_drive_label, 64, fw->sidebar[i].label, _TRUNCATE);
                break;
            }
        }
        if (!fw->cur_drive_label[0]) {
            wcsncpy_s(fw->cur_drive_label, 64, root, _TRUNCATE);
        }
    }
}

static void start_async_load(FilesWindow *fw) {
    FilesTab *tab = &fw->tabs[fw->active_tab];
    if (ql_is_open_for(fw)) ql_close(); /* entries are about to be replaced */
    refresh_drive_space(fw);
    fw->loading = true;
    LONG gen = InterlockedIncrement(&fw->load_gen);
    fw->scroll_y = tab->scroll_y;
    fw->focus_vis_idx = -1;
    fw->hover_vis_idx = -1;

    wchar_t wtitle[160];
    _snwprintf_s(wtitle, 160, _TRUNCATE, L"%ls - Files", tab->title);
    SetWindowTextW(fw->hwnd, wtitle);
    if (fw->hFilterEdit) {
        wchar_t cue[128];
        _snwprintf_s(cue, 128, _TRUNCATE, L"Search %ls...", tab->title);
        SendMessageW(fw->hFilterEdit, 0x1501 /* EM_SETCUEBANNER */, (WPARAM)TRUE, (LPARAM)cue);
    }

    LoadResult *req = (LoadResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LoadResult));
    if (!req) return;
    req->hwnd = fw->hwnd;
    req->gen = gen;
    wcsncpy_s(req->path, MAX_PATH, tab->path, _TRUNCATE);

    HANDLE h = CreateThread(NULL, 0, dir_load_worker, req, 0, NULL);
    if (h) CloseHandle(h);
    else { HeapFree(GetProcessHeap(), 0, req); fw->loading = false; }

    InvalidateRect(fw->hwnd, NULL, FALSE);
}

static void navigate_to(FilesWindow *fw, const wchar_t *raw_path, bool push_hist) {
    if (!raw_path || !raw_path[0]) return;
    wchar_t norm[MAX_PATH];
    wcsncpy_s(norm, MAX_PATH, raw_path, _TRUNCATE);
    /* Expand environment variables if user typed e.g. %USERPROFILE% */
    if (wcschr(norm, L'%')) {
        wchar_t exp[MAX_PATH];
        if (ExpandEnvironmentStringsW(norm, exp, MAX_PATH) > 0) {
            wcsncpy_s(norm, MAX_PATH, exp, _TRUNCATE);
        }
    }
    DWORD attr = GetFileAttributesW(norm);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;

    FilesTab *tab = &fw->tabs[fw->active_tab];
    tab->scroll_y = 0;
    wcsncpy_s(tab->path, MAX_PATH, norm, _TRUNCATE);
    update_tab_title(tab);

    if (push_hist) {
        if (tab->hist_pos + 1 < MAX_HIST) {
            tab->hist_pos++;
            tab->hist_count = tab->hist_pos + 1;
        } else {
            memmove(tab->hist[0], tab->hist[1], (MAX_HIST - 1) * sizeof(tab->hist[0]));
            tab->hist_pos = MAX_HIST - 1;
            tab->hist_count = MAX_HIST;
        }
        wcsncpy_s(tab->hist[tab->hist_pos], MAX_PATH, norm, _TRUNCATE);
    }

    fw->filter_text[0] = L'\0';
    if (fw->hFilterEdit) SetWindowTextW(fw->hFilterEdit, L"");
    start_async_load(fw);
}

static void build_full_entry_path(const FilesWindow *fw, const FileEntry *e, wchar_t *out, size_t len) {
    const wchar_t *dir = fw->tabs[fw->active_tab].path;
    size_t n = wcslen(dir);
    if (n > 0 && dir[n - 1] == L'\\') _snwprintf_s(out, len, _TRUNCATE, L"%ls%ls", dir, e->name);
    else _snwprintf_s(out, len, _TRUNCATE, L"%ls\\%ls", dir, e->name);
}

static void select_only(FilesWindow *fw, int vis_idx) {
    for (int i = 0; i < fw->entry_count; i++) fw->entries[i].selected = false;
    if (vis_idx >= 0 && vis_idx < fw->visible_count) {
        fw->entries[fw->visible[vis_idx]].selected = true;
        fw->focus_vis_idx = vis_idx;
    } else {
        fw->focus_vis_idx = -1;
    }
}

static int count_selected(const FilesWindow *fw, uint64_t *out_bytes, int *first_vis) {
    int cnt = 0;
    uint64_t tot = 0;
    if (first_vis) *first_vis = -1;
    for (int v = 0; v < fw->visible_count; v++) {
        const FileEntry *e = &fw->entries[fw->visible[v]];
        if (e->selected) {
            if (cnt == 0 && first_vis) *first_vis = v;
            cnt++;
            if (!e->is_dir) tot += e->size;
        }
    }
    if (out_bytes) *out_bytes = tot;
    return cnt;
}

static void copy_selection_to_clipboard(FilesWindow *fw, bool is_cut) {
    /* Build double-null-terminated CF_HDROP path list */
    size_t total_chars = 1;
    int sel_cnt = 0;
    for (int v = 0; v < fw->visible_count; v++) {
        const FileEntry *e = &fw->entries[fw->visible[v]];
        if (!e->selected) continue;
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, e, full, MAX_PATH);
        total_chars += wcslen(full) + 1;
        sel_cnt++;
    }
    if (sel_cnt == 0) return;

    size_t hdrop_sz = sizeof(DROPFILES) + total_chars * sizeof(wchar_t);
    HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, hdrop_sz);
    if (!hDrop) return;

    DROPFILES *df = (DROPFILES *)GlobalLock(hDrop);
    if (!df) { GlobalFree(hDrop); return; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t *dst = (wchar_t *)((BYTE *)df + sizeof(DROPFILES));
    for (int v = 0; v < fw->visible_count; v++) {
        const FileEntry *e = &fw->entries[fw->visible[v]];
        if (!e->selected) continue;
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, e, full, MAX_PATH);
        size_t l = wcslen(full) + 1;
        memcpy(dst, full, l * sizeof(wchar_t));
        dst += l;
    }
    *dst = L'\0';
    GlobalUnlock(hDrop);

    if (OpenClipboard(fw->hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_HDROP, hDrop);

        UINT cf_eff = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        HGLOBAL hEff = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hEff) {
            DWORD *pe = (DWORD *)GlobalLock(hEff);
            if (pe) {
                *pe = is_cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
                GlobalUnlock(hEff);
                SetClipboardData(cf_eff, hEff);
            } else {
                GlobalFree(hEff);
            }
        }
        CloseClipboard();
    } else {
        GlobalFree(hDrop);
    }
}

static void paste_from_clipboard(FilesWindow *fw) {
    if (!OpenClipboard(fw->hwnd)) return;
    HANDLE hData = GetClipboardData(CF_HDROP);
    if (!hData) { CloseClipboard(); return; }

    bool is_move = false;
    UINT cf_eff = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HANDLE hEff = GetClipboardData(cf_eff);
    if (hEff) {
        DWORD *pe = (DWORD *)GlobalLock(hEff);
        if (pe) {
            if (*pe & DROPEFFECT_MOVE) is_move = true;
            GlobalUnlock(hEff);
        }
    }

    HDROP hDrop = (HDROP)hData;
    UINT nfiles = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    if (nfiles == 0) { CloseClipboard(); return; }

    size_t cap = (size_t)nfiles * (MAX_PATH + 2) + 4;
    wchar_t *from_buf = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(wchar_t));
    if (from_buf) {
        wchar_t *p = from_buf;
        for (UINT i = 0; i < nfiles; i++) {
            wchar_t f[MAX_PATH] = {0};
            if (DragQueryFileW(hDrop, i, f, MAX_PATH) > 0) {
                size_t l = wcslen(f) + 1;
                memcpy(p, f, l * sizeof(wchar_t));
                p += l;
            }
        }
        *p = L'\0';
    }
    CloseClipboard();

    if (!from_buf) return;
    wchar_t to_buf[MAX_PATH + 2];
    ZeroMemory(to_buf, sizeof(to_buf));
    wcsncpy_s(to_buf, MAX_PATH, fw->tabs[fw->active_tab].path, _TRUNCATE);

    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.hwnd = fw->hwnd;
    op.wFunc = is_move ? FO_MOVE : FO_COPY;
    op.pFrom = from_buf;
    op.pTo = to_buf;
    op.fFlags = FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION;
    SHFileOperationW(&op);

    HeapFree(GetProcessHeap(), 0, from_buf);
    start_async_load(fw);
}

static void delete_selection(FilesWindow *fw, bool permanent) {
    size_t cap = 4;
    int cnt = 0;
    for (int v = 0; v < fw->visible_count; v++) {
        if (fw->entries[fw->visible[v]].selected) {
            cap += MAX_PATH + 2;
            cnt++;
        }
    }
    if (cnt == 0) return;

    wchar_t *from_buf = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(wchar_t));
    if (!from_buf) return;
    wchar_t *p = from_buf;
    for (int v = 0; v < fw->visible_count; v++) {
        const FileEntry *e = &fw->entries[fw->visible[v]];
        if (!e->selected) continue;
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, e, full, MAX_PATH);
        size_t l = wcslen(full) + 1;
        memcpy(p, full, l * sizeof(wchar_t));
        p += l;
    }
    *p = L'\0';

    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.hwnd = fw->hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from_buf;
    op.fFlags = permanent ? 0 : FOF_ALLOWUNDO;
    SHFileOperationW(&op);

    HeapFree(GetProcessHeap(), 0, from_buf);
    start_async_load(fw);
}

static void begin_inline_rename(FilesWindow *fw, int vis_idx);

static void create_new_folder(FilesWindow *fw) {
    const wchar_t *dir = fw->tabs[fw->active_tab].path;
    wchar_t cand[MAX_PATH];
    for (int n = 0; n < 99; n++) {
        if (n == 0) _snwprintf_s(cand, MAX_PATH, _TRUNCATE, L"%ls\\New Folder", dir);
        else        _snwprintf_s(cand, MAX_PATH, _TRUNCATE, L"%ls\\New Folder (%d)", dir, n + 1);
        if (GetFileAttributesW(cand) == INVALID_FILE_ATTRIBUTES) {
            if (CreateDirectoryW(cand, NULL)) {
                start_async_load(fw);
            }
            return;
        }
    }
}

/* A terminal in the folder, found the way Windows finds it (programs.h). */
static void open_terminal_at(const wchar_t *path) {
    wchar_t exe[MAX_PATH];
    if (!lares_find_terminal(exe, MAX_PATH, NULL, 0)) return;
    wchar_t args[MAX_PATH + 16] = L"";
    if (_wcsicmp(PathFindFileNameW(exe), L"wt.exe") == 0)
        _snwprintf_s(args, MAX_PATH + 16, _TRUNCATE, L"-d \"%ls\"", path);
    ShellExecuteW(NULL, L"open", exe, args[0] ? args : NULL, path, SW_SHOWNORMAL);
}

/* A picture or a text file in Lares' viewers when their switch is on
 * (lares-view.exe, next to this program), else the way Windows opens it. */
static void open_in_viewer(bool image, const wchar_t *full, const wchar_t *dir) {
    LaresShellPrefs p;
    lares_shell_prefs_load(&p);
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (p.viewers && n > 0 && n < MAX_PATH && PathRemoveFileSpecW(exe) &&
        wcscat_s(exe, MAX_PATH, L"\\lares-view.exe") == 0 && PathFileExistsW(exe)) {
        wchar_t args[MAX_PATH + 16];
        _snwprintf_s(args, MAX_PATH + 16, _TRUNCATE, image ? L"--view \"%ls\"" : L"--text \"%ls\"", full);
        if ((INT_PTR)ShellExecuteW(NULL, L"open", exe, args, NULL, SW_SHOWNORMAL) > 32) return;
    }
    ShellExecuteW(NULL, L"open", full, NULL, dir, SW_SHOWNORMAL);
}

static void copy_path_clipboard(HWND hwnd, const wchar_t *path) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (wcslen(path) + 1) * sizeof(wchar_t);
    HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hm) {
        void *p = GlobalLock(hm);
        if (p) { memcpy(p, path, bytes); GlobalUnlock(hm); SetClipboardData(CF_UNICODETEXT, hm); }
    }
    CloseClipboard();
}

static void open_vis_item(FilesWindow *fw, int vis_idx) {
    if (vis_idx < 0 || vis_idx >= fw->visible_count) return;
    const FileEntry *e = &fw->entries[fw->visible[vis_idx]];
    wchar_t full[MAX_PATH];
    build_full_entry_path(fw, e, full, MAX_PATH);
    if (e->is_dir) {
        navigate_to(fw, full, true);
    } else if (e->is_image) {
        open_in_viewer(true, full, fw->tabs[fw->active_tab].path);
    } else if (e->is_text) {
        open_in_viewer(false, full, fw->tabs[fw->active_tab].path);
    } else {
        ShellExecuteW(fw->hwnd, L"open", full, NULL, fw->tabs[fw->active_tab].path, SW_SHOWNORMAL);
    }
}

static void add_new_tab(FilesWindow *fw, const wchar_t *path) {
    if (fw->tab_count >= MAX_FILES_TABS) return;
    int idx = fw->tab_count++;
    FilesTab *t = &fw->tabs[idx];
    ZeroMemory(t, sizeof(*t));
    if (path && path[0]) {
        wcsncpy_s(t->path, MAX_PATH, path, _TRUNCATE);
    } else if (fw->sidebar_count > 0) {
        wcsncpy_s(t->path, MAX_PATH, fw->sidebar[0].path, _TRUNCATE);
    } else {
        /* The root of the drive Windows is on, whichever letter it has. */
        wchar_t win[MAX_PATH];
        if (GetWindowsDirectoryW(win, MAX_PATH) && PathStripToRootW(win)) wcsncpy_s(t->path, MAX_PATH, win, _TRUNCATE);
    }
    update_tab_title(t);
    wcsncpy_s(t->hist[0], MAX_PATH, t->path, _TRUNCATE);
    t->hist_count = 1;
    t->hist_pos = 0;
    fw->active_tab = idx;
    start_async_load(fw);
}

static void close_tab(FilesWindow *fw, int idx) {
    if (fw->tab_count <= 1) {
        PostMessageW(fw->hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (idx < 0 || idx >= fw->tab_count) return;
    memmove(&fw->tabs[idx], &fw->tabs[idx + 1], (size_t)(fw->tab_count - idx - 1) * sizeof(FilesTab));
    fw->tab_count--;
    if (fw->active_tab >= fw->tab_count) fw->active_tab = fw->tab_count - 1;
    start_async_load(fw);
}

static void update_layout(FilesWindow *fw, int w, int h) {
    int tab_h = 38;
    int tb_h  = 44;
    int info_w = fw->show_info_pane ? 236 : 0;
    if (fw->sidebar_w < SIDEBAR_MIN_W) fw->sidebar_w = SIDEBAR_MIN_W;
    if (fw->sidebar_w > SIDEBAR_MAX_W) fw->sidebar_w = SIDEBAR_MAX_W;
    /* Keep at least ~260px for the file list on narrow windows */
    if (w - info_w - fw->sidebar_w < 260) fw->sidebar_w = max(SIDEBAR_MIN_W, w - info_w - 260);
    int sb_w  = fw->sidebar_w;
    int stat_h = 26;

    /* Integrated Windows 11 caption controls on far right of unified title+tab bar */
    int cap_w = 46;
    for (int i = 0; i < 3; i++) {
        int cx = w - (3 - i) * cap_w;
        fw->rc_cap_btns[i].left   = cx;
        fw->rc_cap_btns[i].top    = 0;
        fw->rc_cap_btns[i].right  = cx + cap_w;
        fw->rc_cap_btns[i].bottom = tab_h;
    }

    /* Tabs integrated seamlessly into top 38px titlebar */
    int tx = 10;
    int avail_tabs_w = (w - cap_w * 3 - 54);
    if (avail_tabs_w < 120) avail_tabs_w = 120;
    int max_tw = avail_tabs_w / max(1, fw->tab_count);
    if (max_tw > 210) max_tw = 210;
    if (max_tw < 100) max_tw = 100;
    for (int i = 0; i < fw->tab_count; i++) {
        fw->rc_tabs[i].left   = tx;
        fw->rc_tabs[i].top    = 6;
        fw->rc_tabs[i].right  = tx + max_tw;
        fw->rc_tabs[i].bottom = tab_h;
        fw->rc_tab_close[i].left   = tx + max_tw - 26;
        fw->rc_tab_close[i].top    = 11;
        fw->rc_tab_close[i].right  = tx + max_tw - 6;
        fw->rc_tab_close[i].bottom = tab_h - 7;
        tx += max_tw + 4;
    }
    fw->rc_tab_add.left   = tx + 2;
    fw->rc_tab_add.top    = 8;
    fw->rc_tab_add.right  = tx + 30;
    fw->rc_tab_add.bottom = tab_h - 4;

    /* Toolbar buttons: Left = [0:Back, 1:Fwd, 2:Up, 3:Refresh], Right = [4:NewFolder, 5:View, 6:Hidden, 7:Info, 8:Terminal] */
    int ty = tab_h + 7;
    int bx = 10;
    for (int b = 0; b < 4; b++) {
        fw->rc_tb_btns[b].left   = bx;
        fw->rc_tb_btns[b].top    = ty;
        fw->rc_tb_btns[b].right  = bx + 30;
        fw->rc_tb_btns[b].bottom = ty + 30;
        bx += 32;
    }

    int rx = w - 10;
    for (int b = 8; b >= 4; b--) {
        rx -= 30;
        fw->rc_tb_btns[b].left   = rx;
        fw->rc_tb_btns[b].top    = ty;
        fw->rc_tb_btns[b].right  = rx + 30;
        fw->rc_tb_btns[b].bottom = ty + 30;
        rx -= 4;
    }

    int filter_w = 192;
    rx -= filter_w + 8;
    fw->rc_filter_bar.left   = rx;
    fw->rc_filter_bar.top    = ty;
    fw->rc_filter_bar.right  = rx + filter_w;
    fw->rc_filter_bar.bottom = ty + 30;

    fw->rc_addr_bar.left   = bx + 6;
    fw->rc_addr_bar.top    = ty;
    fw->rc_addr_bar.right  = fw->rc_filter_bar.left - 10;
    fw->rc_addr_bar.bottom = ty + 30;
    if (fw->rc_addr_bar.right < fw->rc_addr_bar.left + 80) fw->rc_addr_bar.right = fw->rc_addr_bar.left + 80;

    if (fw->hFilterEdit) {
        MoveWindow(fw->hFilterEdit, fw->rc_filter_bar.left + 28, fw->rc_filter_bar.top + 6,
                   filter_w - 36, 18, TRUE);
    }
    if (fw->hAddrEdit && fw->editing_addr) {
        MoveWindow(fw->hAddrEdit, fw->rc_addr_bar.left + 10, fw->rc_addr_bar.top + 6,
                   (fw->rc_addr_bar.right - fw->rc_addr_bar.left) - 20, 18, TRUE);
    }

    int body_y = tab_h + tb_h;
    int body_h = h - body_y - stat_h;
    if (body_h < 60) body_h = 60;

    fw->rc_file_area.left = sb_w;
    fw->rc_file_area.top = body_y;
    fw->rc_file_area.right = w - info_w;
    fw->rc_file_area.bottom = body_y + body_h;

    fw->rc_info_pane.left = w - info_w;
    fw->rc_info_pane.top = body_y;
    fw->rc_info_pane.right = w;
    fw->rc_info_pane.bottom = body_y + body_h;

    /* Details column headers (Name flexes, Date/Size/Type keep their user-resized widths) */
    int fa_w = fw->rc_file_area.right - fw->rc_file_area.left;
    for (int c = 1; c < 4; c++) {
        if (fw->col_w[c] < g_col_min_w[c]) fw->col_w[c] = g_col_min_w[c];
    }
    int c_date_w = fw->col_w[1], c_size_w = fw->col_w[2], c_type_w = fw->col_w[3];
    int c_name_w = fa_w - c_date_w - c_size_w - c_type_w - 16;
    if (c_name_w < COL_MIN_NAME) c_name_w = COL_MIN_NAME;
    fw->col_w[0] = c_name_w;
    int cx = fw->rc_file_area.left;
    int cols_w[4] = { c_name_w, c_date_w, c_size_w, c_type_w };
    for (int c = 0; c < 4; c++) {
        fw->rc_col_hdr[c].left = cx; fw->rc_col_hdr[c].top = body_y;
        fw->rc_col_hdr[c].right = cx + cols_w[c]; fw->rc_col_hdr[c].bottom = body_y + 28;
        cx += cols_w[c];
    }

    /* Compute content height for scrolling */
    if (fw->grid_view) {
        int cols = max(1, (fa_w - 24) / 112);
        int rows = (fw->visible_count + cols - 1) / cols;
        fw->content_h = rows * 116 + 20;
    } else {
        fw->content_h = fw->visible_count * 30 + 16;
    }

    int view_h = body_h - (fw->grid_view ? 0 : 28);
    int max_scroll = max(0, fw->content_h - view_h);
    if (fw->scroll_y > max_scroll) fw->scroll_y = max_scroll;
    if (fw->scroll_y < 0) fw->scroll_y = 0;
}

/* Crisp antialiased blue vector folder icon (no monochrome/blurry ICO artifacts) */
static void draw_folder_icon(GpGraphics *g, float x, float y, float sz) {
    float pad = sz * 0.08f;
    float fx = x + pad;
    float fy = y + sz * 0.14f;
    float fw = sz - pad * 2.0f;
    float fh = sz * 0.72f;
    float r  = max(1.5f, sz * 0.12f);

    /* Back tab */
    draw_round_rect_fill(g, fx, fy, fw * 0.46f, fh * 0.36f, r, MAKE_ARGB(255, 62, 162, 245));
    /* Back upper rim */
    draw_round_rect_fill(g, fx, fy + fh * 0.14f, fw, fh * 0.34f, r, MAKE_ARGB(255, 74, 174, 250));
    /* Front vibrant folder body */
    draw_round_rect_fill(g, fx, fy + fh * 0.24f, fw, fh * 0.76f, r, MAKE_ARGB(255, 92, 192, 255));
    /* Subtle top edge specular highlight */
    draw_line(g, fx + r * 0.7f, fy + fh * 0.27f, fx + fw - r * 0.7f, fy + fh * 0.27f,
              MAKE_ARGB(110, 255, 255, 255), max(1.0f, sz * 0.04f));
}

static void draw_entry_icon(GpGraphics *g, const FilesWindow *fw, const FileEntry *e, int x, int y, int sz) {
    if (e->is_dir) {
        draw_folder_icon(g, (float)x, (float)y, (float)sz);
        return;
    }

    if (fw->grid_view && e->is_image) {
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, e, full, MAX_PATH);
        GpBitmap *th = thumb_cache_get(full);
        if (th) {
            UINT tw = 0, thh = 0;
            GdipGetImageWidth((GpImage *)th, &tw);
            GdipGetImageHeight((GpImage *)th, &thh);
            int dw = sz, dh = sz;
            if (tw > thh && tw > 0) dh = max(1, (int)((thh * (UINT)sz) / tw));
            else if (thh > 0)       dw = max(1, (int)((tw * (UINT)sz) / thh));
            GdipDrawImageRectI(g, (GpImage *)th, x + (sz - dw) / 2, y + (sz - dh) / 2, dw, dh);
            return;
        }
    }

    /* Fast zero-shell-extension glyph by file type */
    wchar_t glyph = 0xE8A5; /* generic document */
    ARGB col = argb_from_cref(255, g_theme.files_text_muted);
    if (e->is_image) { glyph = 0xEB9F; col = MAKE_ARGB(255, 92, 184, 245); }
    else if (_wcsicmp(e->ext, L".mp4") == 0 || _wcsicmp(e->ext, L".mkv") == 0 || _wcsicmp(e->ext, L".mov") == 0) {
        glyph = 0xE714; col = MAKE_ARGB(255, 198, 128, 248);
    } else if (_wcsicmp(e->ext, L".mp3") == 0 || _wcsicmp(e->ext, L".wav") == 0 || _wcsicmp(e->ext, L".flac") == 0) {
        glyph = 0xE8D6; col = MAKE_ARGB(255, 245, 122, 146);
    } else if (_wcsicmp(e->ext, L".zip") == 0 || _wcsicmp(e->ext, L".7z") == 0 || _wcsicmp(e->ext, L".rar") == 0) {
        glyph = 0xF012; col = MAKE_ARGB(255, 232, 178, 72);
    } else if (_wcsicmp(e->ext, L".exe") == 0 || _wcsicmp(e->ext, L".msi") == 0 || _wcsicmp(e->ext, L".bat") == 0) {
        glyph = 0xE756; col = MAKE_ARGB(255, 92, 206, 134);
    } else if (_wcsicmp(e->ext, L".c") == 0 || _wcsicmp(e->ext, L".h") == 0 || _wcsicmp(e->ext, L".py") == 0 ||
               _wcsicmp(e->ext, L".js") == 0 || _wcsicmp(e->ext, L".ts") == 0 || _wcsicmp(e->ext, L".json") == 0) {
        glyph = 0xE943; col = MAKE_ARGB(255, 118, 196, 255);
    }
    draw_icon_glyph(g, glyph, (float)sz * 0.80f, col, (float)x, (float)y, (float)sz, (float)sz);
}

static void draw_files(FilesWindow *fw, LayerSurface *fb, int w, int h) {
    update_layout(fw, w, h);
    {
        GpGraphics *g = fb->g;
        GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
        GdipSetTextRenderingHint(g, GP_TEXT_ANTIALIAS);
        GdipSetInterpolationMode(g, GP_INTERP_HIGHQUALITY_BICUBIC);

        ARGB c_sb     = argb_from_cref(255, g_theme.files_sidebar);
        ARGB c_tb     = argb_from_cref(255, g_theme.files_toolbar);
        ARGB c_addr   = argb_from_cref(255, g_theme.files_addrbar);
        ARGB c_bg     = argb_from_cref(255, g_theme.files_bg);
        ARGB c_alt    = argb_from_cref(255, g_theme.files_alt);
        ARGB c_info   = argb_from_cref(255, g_theme.files_info_bg);
        ARGB c_bdr    = argb_from_cref(255, g_theme.files_border);
        ARGB c_txt    = argb_from_cref(255, g_theme.files_text);
        ARGB c_muted  = argb_from_cref(255, g_theme.files_text_muted);
        ARGB c_sel    = argb_from_cref(255, g_theme.files_sel_bg);
        ARGB c_hov    = argb_from_cref(255, g_theme.files_hover_bg);

        /* 1. Unified Seamless Titlebar + Tab Strip (38px) + Toolbar (44px) */
        draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, 38.0f, 0.0f, c_sb);
        draw_round_rect_fill(g, 0.0f, 38.0f, (float)w, 44.0f, 0.0f, c_tb);
        draw_line(g, 0.0f, 38.0f, (float)w, 38.0f, c_bdr, 1.0f);
        draw_line(g, 0.0f, 81.5f, (float)w, 81.5f, c_bdr, 1.0f);

        /* Integrated Tabs (active tab seamlessly connects to toolbar below!) */
        for (int i = 0; i < fw->tab_count; i++) {
            const RECT *tr = &fw->rc_tabs[i];
            int tw = tr->right - tr->left;
            int th = tr->bottom - tr->top;
            bool act = (i == fw->active_tab);
            if (act) {
                /* Active connected tab card */
                draw_round_rect_fill(g, (float)tr->left, (float)tr->top, (float)tw, (float)(th + 6), 8.0f, c_tb);
                draw_round_rect_stroke(g, (float)tr->left + 0.5f, (float)tr->top + 0.5f,
                                       (float)tw - 1.0f, (float)(th + 6), 8.0f, c_bdr, 1.0f);
                /* Erase bottom border seam so active tab merges cleanly into toolbar */
                draw_round_rect_fill(g, (float)(tr->left + 1), 37.0f, (float)(tw - 2), 4.0f, 0.0f, c_tb);
            } else if (i == fw->hover_tab_idx) {
                draw_round_rect_fill(g, (float)tr->left, (float)(tr->top + 1), (float)tw, (float)(th - 4), 7.0f, c_hov);
            }
            draw_folder_icon(g, (float)(tr->left + 9), (float)(tr->top + 7), 16.0f);
            draw_text(g, fw->tabs[i].title, L"Segoe UI", 12.0f, act ? GP_FONT_BOLD : GP_FONT_REGULAR,
                      act ? c_txt : c_muted, (float)(tr->left + 30), (float)tr->top,
                      (float)(tw - 58), (float)th, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
            bool close_pressed = (fw->pressed_btn == PB_TAB_CLOSE_BASE + i && fw->pressed_inside);
            if (fw->hover_tab_close_idx == i || close_pressed) {
                draw_round_rect_fill(g, (float)fw->rc_tab_close[i].left, (float)fw->rc_tab_close[i].top,
                                     20.0f, 20.0f, 5.0f, c_hov);
                if (close_pressed) {
                    draw_round_rect_fill(g, (float)fw->rc_tab_close[i].left, (float)fw->rc_tab_close[i].top,
                                         20.0f, 20.0f, 5.0f,
                                         g_theme.is_dark ? MAKE_ARGB(46, 255, 255, 255) : MAKE_ARGB(30, 0, 0, 0));
                }
            }
            draw_icon_glyph(g, 0xE711, 9.0f, act ? c_txt : c_muted,
                            (float)fw->rc_tab_close[i].left, (float)fw->rc_tab_close[i].top, 20.0f, 20.0f);
        }
        {
            bool add_pressed = (fw->pressed_btn == PB_TAB_ADD && fw->pressed_inside);
            if (fw->hover_tab_add || add_pressed) {
                draw_round_rect_fill(g, (float)fw->rc_tab_add.left, (float)fw->rc_tab_add.top, 28.0f, 26.0f, 6.0f, c_hov);
            }
            if (add_pressed) {
                draw_round_rect_fill(g, (float)fw->rc_tab_add.left, (float)fw->rc_tab_add.top, 28.0f, 26.0f, 6.0f,
                                     g_theme.is_dark ? MAKE_ARGB(46, 255, 255, 255) : MAKE_ARGB(30, 0, 0, 0));
            }
        }
        draw_icon_glyph(g, 0xE710, 11.5f, c_txt, (float)fw->rc_tab_add.left, (float)fw->rc_tab_add.top, 28.0f, 26.0f);

        /* Integrated Windows 11 Caption Controls (_, [], X) */
        {
            bool zoomed = IsZoomed(fw->hwnd) != 0;
            wchar_t cglyphs[3] = { 0xE921, zoomed ? 0xE923 : 0xE922, 0xE8BB };
            for (int c = 0; c < 3; c++) {
                const RECT *cr2 = &fw->rc_cap_btns[c];
                bool cap_pressed = (fw->pressed_btn == PB_CAP_BASE + c && fw->pressed_inside);
                bool hov = (fw->hover_caption_btn == c + 1) || cap_pressed;
                if (hov) {
                    ARGB hbg = (c == 2) ? (cap_pressed ? MAKE_ARGB(255, 196, 14, 30) : MAKE_ARGB(255, 232, 17, 35)) : c_hov;
                    draw_round_rect_fill(g, (float)cr2->left, (float)cr2->top,
                                         (float)(cr2->right - cr2->left), (float)(cr2->bottom - cr2->top), 0.0f, hbg);
                    if (cap_pressed && c != 2) {
                        draw_round_rect_fill(g, (float)cr2->left, (float)cr2->top,
                                             (float)(cr2->right - cr2->left), (float)(cr2->bottom - cr2->top), 0.0f,
                                             g_theme.is_dark ? MAKE_ARGB(40, 255, 255, 255) : MAKE_ARGB(26, 0, 0, 0));
                    }
                }
                ARGB fg = (hov && c == 2) ? MAKE_ARGB(255, 255, 255, 255) : c_txt;
                draw_icon_glyph(g, cglyphs[c], 10.0f, fg, (float)cr2->left, (float)cr2->top,
                                (float)(cr2->right - cr2->left), (float)(cr2->bottom - cr2->top));
            }
        }

        /* Toolbar buttons: [0:Back, 1:Fwd, 2:Up, 3:Refresh | 4:NewFolder, 5:View, 6:Hidden, 7:Info, 8:Terminal] */
        static const wchar_t tb_glyphs[9] = {
            0xE72B, 0xE72A, 0xE74A, 0xE72C, 0xE8F4, 0xF0E2, 0xE7B3, 0xE946, 0xE756
        };
        const FilesTab *cur_tab = &fw->tabs[fw->active_tab];
        for (int b = 0; b < 9; b++) {
            const RECT *r = &fw->rc_tb_btns[b];
            bool active_state = (b == 5 && fw->grid_view) || (b == 6 && fw->show_hidden) || (b == 7 && fw->show_info_pane);
            bool disabled = (b == 0 && cur_tab->hist_pos <= 0) || (b == 1 && cur_tab->hist_pos + 1 >= cur_tab->hist_count);
            bool pressed = !disabled && fw->pressed_btn == PB_TB_BASE + b && fw->pressed_inside;
            if (active_state) {
                draw_round_rect_fill(g, (float)r->left, (float)r->top, 30.0f, 30.0f, 6.0f, g_theme.accent_overlay_selected);
            } else if ((fw->hover_toolbar_btn == b || pressed) && !disabled) {
                draw_round_rect_fill(g, (float)r->left, (float)r->top, 30.0f, 30.0f, 6.0f, c_hov);
            }
            if (pressed) {
                /* press: darker well while the mouse button is held */
                draw_round_rect_fill(g, (float)r->left, (float)r->top, 30.0f, 30.0f, 6.0f,
                                     g_theme.is_dark ? MAKE_ARGB(46, 255, 255, 255) : MAKE_ARGB(30, 0, 0, 0));
            }
            wchar_t gl = tb_glyphs[b];
            if (b == 5) gl = fw->grid_view ? 0xE8FD : 0xF0E2;
            ARGB gc = disabled ? MAKE_ARGB(90, GetRValue(g_theme.files_text_muted), GetGValue(g_theme.files_text_muted), GetBValue(g_theme.files_text_muted))
                    : (active_state ? g_theme.accent : c_txt);
            if (pressed) gc = (gc & 0x00FFFFFFu) | 0xB4000000u;
            draw_icon_glyph(g, gl, pressed ? 13.0f : 13.5f, gc, (float)r->left, (float)r->top + (pressed ? 0.5f : 0.0f), 30.0f, 30.0f);
        }

        /* Breadcrumb Address Bar (sleek rounded pill with per-segment hover highlight) */
        draw_round_rect_fill(g, (float)fw->rc_addr_bar.left, (float)fw->rc_addr_bar.top,
                             (float)(fw->rc_addr_bar.right - fw->rc_addr_bar.left), 30.0f, 8.0f, c_addr);
        draw_round_rect_stroke(g, (float)fw->rc_addr_bar.left + 0.5f, (float)fw->rc_addr_bar.top + 0.5f,
                               (float)(fw->rc_addr_bar.right - fw->rc_addr_bar.left) - 1.0f, 29.0f, 8.0f, c_bdr, 1.0f);

        fw->crumb_count = 0;
        if (!fw->editing_addr) {
            draw_folder_icon(g, (float)(fw->rc_addr_bar.left + 8), (float)(fw->rc_addr_bar.top + 7), 16.0f);
            /* A test copy shows the path from its made-up folder on, never
             * the folders above it (files_set_crumb_root). */
            const wchar_t *shown = cur_tab->path;
            wchar_t acc[MAX_PATH] = {0};
            size_t root_len = wcslen(g_crumb_root);
            if (root_len && _wcsnicmp(shown, g_crumb_root, root_len) == 0 && shown[root_len] == L'\\') {
                wcsncpy_s(acc, MAX_PATH, g_crumb_root, _TRUNCATE);
                shown += root_len + 1;
            }
            wchar_t copy[MAX_PATH];
            wcsncpy_s(copy, MAX_PATH, shown, _TRUNCATE);
            int cx = fw->rc_addr_bar.left + 28;
            int max_x = fw->rc_addr_bar.right - 10;
            wchar_t *ctx = NULL;
            wchar_t *tok = wcstok_s(copy, L"\\", &ctx);
            while (tok && fw->crumb_count < 12 && cx < max_x - 24) {
                if (fw->crumb_count > 0) {
                    draw_icon_glyph(g, 0xE76C, 8.5f, c_muted, (float)cx, (float)fw->rc_addr_bar.top, 14.0f, 30.0f);
                    cx += 14;
                }
                if (acc[0]) {
                    wcsncat_s(acc, MAX_PATH, L"\\", _TRUNCATE);
                    wcsncat_s(acc, MAX_PATH, tok, _TRUNCATE);
                } else {
                    _snwprintf_s(acc, MAX_PATH, _TRUNCATE, L"%ls\\", tok);
                }
                float tw = measure_text_width(g, tok, L"Segoe UI", 12.0f, GP_FONT_REGULAR);
                int cw = (int)tw + 12;
                if (cx + cw > max_x) cw = max_x - cx;
                int ci = fw->crumb_count++;
                fw->rc_crumbs[ci].left = cx; fw->rc_crumbs[ci].top = fw->rc_addr_bar.top + 3;
                fw->rc_crumbs[ci].right = cx + cw; fw->rc_crumbs[ci].bottom = fw->rc_addr_bar.bottom - 3;
                wcsncpy_s(fw->crumb_paths[ci], MAX_PATH, acc, _TRUNCATE);
                if (fw->hover_crumb_idx == ci) {
                    draw_round_rect_fill(g, (float)cx, (float)(fw->rc_addr_bar.top + 3),
                                         (float)cw, 24.0f, 5.0f, c_hov);
                }
                draw_text(g, tok, L"Segoe UI", 12.0f, GP_FONT_REGULAR, c_txt,
                          (float)(cx + 5), (float)fw->rc_addr_bar.top, (float)(cw - 6), 30.0f,
                          GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                cx += cw;
                tok = wcstok_s(NULL, L"\\", &ctx);
            }
        }

        /* Filter / Search bar container */
        draw_round_rect_fill(g, (float)fw->rc_filter_bar.left, (float)fw->rc_filter_bar.top,
                             (float)(fw->rc_filter_bar.right - fw->rc_filter_bar.left), 30.0f, 8.0f, c_addr);
        draw_round_rect_stroke(g, (float)fw->rc_filter_bar.left + 0.5f, (float)fw->rc_filter_bar.top + 0.5f,
                               (float)(fw->rc_filter_bar.right - fw->rc_filter_bar.left) - 1.0f, 29.0f, 8.0f, c_bdr, 1.0f);
        draw_icon_glyph(g, 0xE721, 12.0f, c_muted, (float)(fw->rc_filter_bar.left + 7), (float)fw->rc_filter_bar.top, 18.0f, 30.0f);

        /* 2. Left Sidebar (Favorites + Custom Bookmarks + Drives) */
        int body_y = fw->rc_file_area.top;
        int body_h = fw->rc_file_area.bottom - fw->rc_file_area.top;
        float sbw = (float)fw->sidebar_w;
        bool drop_here = files_drag_is_over(fw);
        draw_round_rect_fill(g, 0.0f, (float)body_y, sbw, (float)body_h, 0.0f, c_sb);
        {
            /* Sidebar divider: accent while hovered or being dragged (resize cursor on hover) */
            bool hot = (fw->hover_divider == 0 || fw->resize_kind == 1);
            draw_line(g, sbw - 0.5f, (float)body_y, sbw - 0.5f, (float)(body_y + body_h),
                      hot ? g_theme.accent : c_bdr, hot ? 2.0f : 1.0f);
        }
        GdipSetClipRect(g, 0.0f, (float)body_y, sbw - 1.0f, (float)body_h, GP_COMBINE_REPLACE);

        int sy = body_y + 10;
        draw_text(g, L"FAVORITES", L"Segoe UI", 10.5f, GP_FONT_BOLD, c_muted,
                  14.0f, (float)sy, sbw - 28.0f, 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
        sy += 22;

        static const wchar_t fav_glyphs[7] = {
            0xE80F, /* Home */
            0xE7F4, /* Desktop */
            0xE8A5, /* Documents */
            0xE896, /* Downloads */
            0xEB9F, /* Pictures */
            0xE8D6, /* Music */
            0xE714  /* Videos */
        };
        ARGB mac_blue = g_theme.is_dark ? MAKE_ARGB(255, 92, 182, 255) : MAKE_ARGB(255, 0, 122, 255);

        bool drew_loc_hdr = false;
        for (int i = 0; i < fw->sidebar_count; i++) {
            const SidebarItem *si = &fw->sidebar[i];
            if (si->is_drive && !drew_loc_hdr) {
                sy += 8;
                draw_text(g, L"LOCATIONS", L"Segoe UI", 10.5f, GP_FONT_BOLD, c_muted,
                          14.0f, (float)sy, sbw - 28.0f, 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                sy += 22;
                drew_loc_hdr = true;
            }
            int row_h = si->is_drive ? 50 : 30;
            fw->rc_sidebar_items[i].left = 8; fw->rc_sidebar_items[i].top = sy;
            fw->rc_sidebar_items[i].right = fw->sidebar_w - 8; fw->rc_sidebar_items[i].bottom = sy + row_h;

            bool is_cur = paths_equal_norm(cur_tab->path, si->path);
            bool is_drop = drop_here && g_drag.target_kind == DROP_SIDEBAR && g_drag.target_idx == i;
            if (is_drop) {
                draw_round_rect_fill(g, 8.0f, (float)sy, sbw - 16.0f, (float)row_h, 7.0f, g_theme.accent_overlay_selected);
                draw_round_rect_stroke(g, 8.5f, (float)sy + 0.5f, sbw - 17.0f, (float)row_h - 1.0f, 7.0f, g_theme.accent, 2.0f);
            } else if (is_cur) {
                draw_round_rect_fill(g, 8.0f, (float)sy, sbw - 16.0f, (float)row_h, 7.0f, c_sel);
            } else if (i == fw->hover_sidebar_idx && !g_drag.active) {
                draw_round_rect_fill(g, 8.0f, (float)sy, sbw - 16.0f, (float)row_h, 7.0f, c_hov);
            }

            if (si->is_drive) {
                draw_icon_glyph(g, 0xEDA2, 14.5f, mac_blue, 14.0f, (float)(sy + 2), 22.0f, 22.0f);
                draw_text(g, si->label, L"Segoe UI", 12.0f, is_cur ? GP_FONT_BOLD : GP_FONT_REGULAR, c_txt,
                          38.0f, (float)(sy + 3), sbw - 52.0f, 18.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                if (si->total_bytes > 0) {
                    float pct = (float)(si->total_bytes - si->free_bytes) / (float)si->total_bytes;
                    if (pct < 0.0f) pct = 0.0f;
                    if (pct > 1.0f) pct = 1.0f;
                    float bar_w = sbw - 56.0f;
                    draw_round_rect_fill(g, 38.0f, (float)(sy + 24), bar_w, 4.0f, 2.0f, c_addr);
                    draw_round_rect_fill(g, 38.0f, (float)(sy + 24), max(4.0f, bar_w * pct), 4.0f, 2.0f,
                                         pct > 0.90f ? g_theme.danger : mac_blue);
                    wchar_t f_str[32], t_str[32], cap_line[80];
                    format_bytes(si->free_bytes, f_str, 32);
                    format_bytes(si->total_bytes, t_str, 32);
                    _snwprintf_s(cap_line, 80, _TRUNCATE, L"%ls free of %ls", f_str, t_str);
                    draw_text(g, cap_line, L"Segoe UI", 10.5f, GP_FONT_REGULAR, is_cur ? c_txt : c_muted,
                              38.0f, (float)(sy + 31), sbw - 52.0f, 15.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                }
            } else {
                wchar_t fg = (!si->is_bookmark && i < 7) ? fav_glyphs[i] : 0xE8B7;
                draw_icon_glyph(g, fg, 14.5f, mac_blue, 14.0f, (float)sy, 22.0f, 30.0f);
                draw_text(g, si->label, L"Segoe UI", 12.5f, is_cur ? GP_FONT_BOLD : GP_FONT_REGULAR, c_txt,
                          38.0f, (float)sy, sbw - 52.0f, 30.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
            }
            sy += row_h + 2;
        }
        GdipResetClip(g);

        /* 3. Main File Area */
        int fa_x = fw->rc_file_area.left;
        int fa_y = fw->rc_file_area.top;
        int fa_w = fw->rc_file_area.right - fw->rc_file_area.left;
        int fa_h = fw->rc_file_area.bottom - fw->rc_file_area.top;
        draw_round_rect_fill(g, (float)fa_x, (float)fa_y, (float)fa_w, (float)fa_h, 0.0f, c_bg);

        int list_top = fa_y;
        if (!fw->grid_view) {
            static const wchar_t *col_names[4] = { L"Name", L"Date Modified", L"Size", L"Type" };
            draw_round_rect_fill(g, (float)fa_x, (float)fa_y, (float)fa_w, 28.0f, 0.0f, c_alt);
            draw_line(g, (float)fa_x, (float)(fa_y + 27.5f), (float)(fa_x + fa_w), (float)(fa_y + 27.5f), c_bdr, 1.0f);
            for (int c = 0; c < 4; c++) {
                const RECT *hr = &fw->rc_col_hdr[c];
                bool sorted = (fw->sort_col == (SortCol)c);
                draw_text(g, col_names[c], L"Segoe UI", 11.5f, GP_FONT_BOLD,
                          sorted ? c_txt : c_muted,
                          (float)(hr->left + 10), (float)hr->top, (float)(hr->right - hr->left - 24), 28.0f,
                          (c == 2) ? GP_ALIGN_FAR : GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                if (sorted) {
                    float tw = measure_text_width(g, col_names[c], L"Segoe UI", 11.5f, GP_FONT_BOLD);
                    float chx = (c == 2) ? (float)(hr->right - 16) : (float)(hr->left + 12 + (int)tw);
                    draw_icon_glyph(g, fw->sort_asc ? 0xE70E : 0xE70D, 8.5f, c_muted, chx, (float)hr->top, 14.0f, 28.0f);
                }
                if (c < 3) {
                    /* Column divider (resize cursor + accent while hovered/dragged) */
                    bool hot = (fw->hover_divider == c + 1 || fw->resize_kind == c + 2);
                    float dx = (float)hr->right - 0.5f;
                    draw_line(g, dx, (float)(hr->top + (hot ? 3 : 7)), dx, (float)(hr->bottom - (hot ? 3 : 7)),
                              hot ? g_theme.accent : c_bdr, hot ? 2.0f : 1.0f);
                }
            }
            list_top += 28;
        }

        GdipSetClipRect(g, (float)fa_x, (float)list_top, (float)fa_w, (float)(fa_y + fa_h - list_top), GP_COMBINE_REPLACE);

        if (fw->loading) {
            draw_text(g, L"Loading folder...", L"Segoe UI", 13.0f, GP_FONT_REGULAR, c_muted,
                      (float)fa_x, (float)(list_top + 40), (float)fa_w, 30.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        } else if (fw->visible_count == 0) {
            draw_text(g, fw->filter_text[0] ? L"No items match filter" : L"This folder is empty",
                      L"Segoe UI", 13.0f, GP_FONT_REGULAR, c_muted,
                      (float)fa_x, (float)(list_top + 40), (float)fa_w, 30.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        } else if (!fw->grid_view) {
            int row_h = 30;
            int first_row = max(0, fw->scroll_y / row_h);
            int view_rows = ((fa_y + fa_h - list_top) / row_h) + 2;
            int last_row = min(fw->visible_count, first_row + view_rows);

            for (int v = first_row; v < last_row; v++) {
                const FileEntry *e = &fw->entries[fw->visible[v]];
                int ry = list_top + v * row_h - fw->scroll_y;

                if (drop_here && g_drag.target_kind == DROP_FOLDER_ITEM && g_drag.target_idx == v) {
                    draw_round_rect_fill(g, (float)(fa_x + 6), (float)(ry + 1), (float)(fa_w - 18), (float)(row_h - 2), 6.0f,
                                         g_theme.accent_overlay_selected);
                    draw_round_rect_stroke(g, (float)(fa_x + 6.5f), (float)(ry + 1.5f), (float)(fa_w - 19), (float)(row_h - 3), 6.0f,
                                           g_theme.accent, 2.0f);
                } else if (e->selected) {
                    draw_round_rect_fill(g, (float)(fa_x + 6), (float)(ry + 1), (float)(fa_w - 18), (float)(row_h - 2), 6.0f, c_sel);
                } else if (v == fw->hover_vis_idx && !g_drag.active) {
                    draw_round_rect_fill(g, (float)(fa_x + 6), (float)(ry + 1), (float)(fa_w - 18), (float)(row_h - 2), 6.0f, c_hov);
                } else if ((v & 1) == 1) {
                    draw_round_rect_fill(g, (float)(fa_x + 6), (float)ry, (float)(fa_w - 18), (float)row_h, 0.0f, c_alt);
                }

                draw_entry_icon(g, fw, e, fa_x + 14, ry + 6, 18);
                draw_text(g, e->name, L"Segoe UI", 12.5f, GP_FONT_REGULAR, c_txt,
                          (float)(fa_x + 38), (float)ry, (float)(fw->rc_col_hdr[0].right - (fa_x + 42)), (float)row_h,
                          GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

                wchar_t dt[32];
                format_filetime(&e->mtime, dt, 32);
                draw_text(g, dt, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                          (float)(fw->rc_col_hdr[1].left + 8), (float)ry,
                          (float)(fw->rc_col_hdr[1].right - fw->rc_col_hdr[1].left - 12), (float)row_h,
                          GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

                wchar_t szs[32] = L"--";
                if (!e->is_dir) format_bytes(e->size, szs, 32);
                draw_text(g, szs, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                          (float)(fw->rc_col_hdr[2].left + 4), (float)ry,
                          (float)(fw->rc_col_hdr[2].right - fw->rc_col_hdr[2].left - 12), (float)row_h,
                          GP_ALIGN_FAR, GP_ALIGN_CENTER, false);

                const wchar_t *typ = e->is_dir ? L"Folder" : (e->ext[0] ? e->ext + 1 : L"File");
                draw_text(g, typ, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                          (float)(fw->rc_col_hdr[3].left + 8), (float)ry,
                          (float)(fw->rc_col_hdr[3].right - fw->rc_col_hdr[3].left - 14), (float)row_h,
                          GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
            }
        } else {
            /* Grid View */
            int cols = max(1, (fa_w - 24) / 112);
            int cell_w = 104, cell_h = 108, gap = 8;
            int first_row = max(0, fw->scroll_y / (cell_h + gap));
            int vis_rows = ((fa_y + fa_h - list_top) / (cell_h + gap)) + 2;
            int start_v = first_row * cols;
            int end_v = min(fw->visible_count, (first_row + vis_rows) * cols);

            for (int v = start_v; v < end_v; v++) {
                const FileEntry *e = &fw->entries[fw->visible[v]];
                int r = v / cols, c = v % cols;
                int gx = fa_x + 12 + c * (cell_w + gap);
                int gy = list_top + 10 + r * (cell_h + gap) - fw->scroll_y;
                if (drop_here && g_drag.target_kind == DROP_FOLDER_ITEM && g_drag.target_idx == v) {
                    draw_round_rect_fill(g, (float)gx, (float)gy, (float)cell_w, (float)cell_h, 10.0f, g_theme.accent_overlay_selected);
                    draw_round_rect_stroke(g, (float)gx + 0.5f, (float)gy + 0.5f, (float)cell_w - 1.0f, (float)cell_h - 1.0f,
                                           10.0f, g_theme.accent, 2.0f);
                } else if (e->selected) {
                    draw_round_rect_fill(g, (float)gx, (float)gy, (float)cell_w, (float)cell_h, 10.0f, c_sel);
                } else if (v == fw->hover_vis_idx && !g_drag.active) {
                    draw_round_rect_fill(g, (float)gx, (float)gy, (float)cell_w, (float)cell_h, 10.0f, c_hov);
                }
                draw_entry_icon(g, fw, e, gx + 22, gy + 10, 60);
                draw_text(g, e->name, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_txt,
                          (float)(gx + 6), (float)(gy + 74), (float)(cell_w - 12), 28.0f,
                          GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);
            }
        }

        /* Crisp slim scrollbar */
        int view_h = fa_y + fa_h - list_top;
        if (fw->content_h > view_h && view_h > 20) {
            float ratio = (float)view_h / (float)fw->content_h;
            float thumb_h = max(28.0f, (float)view_h * ratio);
            float max_s = (float)(fw->content_h - view_h);
            float thumb_y = (float)list_top + ((float)fw->scroll_y / max_s) * ((float)view_h - thumb_h);
            draw_round_rect_fill(g, (float)(fa_x + fa_w - 8), thumb_y, 5.0f, thumb_h, 2.5f, c_bdr);
        }
        GdipResetClip(g);

        /* 4. Right Info & Preview Pane */
        if (fw->show_info_pane) {
            int ix = fw->rc_info_pane.left;
            int iw = fw->rc_info_pane.right - fw->rc_info_pane.left;
            draw_round_rect_fill(g, (float)ix, (float)body_y, (float)iw, (float)body_h, 0.0f, c_info);
            draw_line(g, (float)ix + 0.5f, (float)body_y, (float)ix + 0.5f, (float)(body_y + body_h), c_bdr, 1.0f);

            int first_sel = -1;
            uint64_t sel_bytes = 0;
            int sel_n = count_selected(fw, &sel_bytes, &first_sel);

            if (sel_n == 1 && first_sel >= 0) {
                const FileEntry *e = &fw->entries[fw->visible[first_sel]];
                wchar_t full[MAX_PATH];
                build_full_entry_path(fw, e, full, MAX_PATH);

                /* Preview Box */
                draw_round_rect_fill(g, (float)(ix + 16), (float)(body_y + 16), (float)(iw - 32), 130.0f, 10.0f, c_bg);
                bool drew_prev = false;
                if (e->is_image) {
                    GpBitmap *th = thumb_cache_get(full);
                    if (th) {
                        UINT tw = 0, thh = 0;
                        GdipGetImageWidth((GpImage *)th, &tw);
                        GdipGetImageHeight((GpImage *)th, &thh);
                        int box_w = iw - 44, box_h = 114;
                        int dw = box_w, dh = box_h;
                        if (tw > 0 && thh > 0) {
                            float s = min((float)box_w / (float)tw, (float)box_h / (float)thh);
                            dw = max(1, (int)((float)tw * s));
                            dh = max(1, (int)((float)thh * s));
                        }
                        GdipDrawImageRectI(g, (GpImage *)th, ix + 16 + (iw - 32 - dw) / 2,
                                           body_y + 16 + (130 - dh) / 2, dw, dh);
                        drew_prev = true;
                    }
                }
                if (!drew_prev) {
                    draw_entry_icon(g, fw, e, ix + (iw - 64) / 2, body_y + 49, 64);
                }

                draw_text(g, e->name, L"Segoe UI", 13.5f, GP_FONT_BOLD, c_txt,
                          (float)(ix + 16), (float)(body_y + 156), (float)(iw - 32), 24.0f,
                          GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);

                wchar_t szs[32], dts[32];
                if (e->is_dir) wcsncpy_s(szs, 32, L"Folder", _TRUNCATE);
                else format_bytes(e->size, szs, 32);
                format_filetime(&e->mtime, dts, 32);

                int py = body_y + 190;
                draw_text(g, L"Kind", L"Segoe UI", 11.0f, GP_FONT_BOLD, c_muted, (float)(ix + 16), (float)py, 64.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                draw_text(g, e->is_dir ? L"Folder" : (e->ext[0] ? e->ext : L"File"), L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_txt, (float)(ix + 82), (float)py, (float)(iw - 96), 20.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, true);
                py += 24;
                draw_text(g, L"Size", L"Segoe UI", 11.0f, GP_FONT_BOLD, c_muted, (float)(ix + 16), (float)py, 64.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                draw_text(g, szs, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_txt, (float)(ix + 82), (float)py, (float)(iw - 96), 20.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, true);
                py += 24;
                draw_text(g, L"Modified", L"Segoe UI", 11.0f, GP_FONT_BOLD, c_muted, (float)(ix + 16), (float)py, 64.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                draw_text(g, dts, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_txt, (float)(ix + 82), (float)py, (float)(iw - 96), 20.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, true);

                static const wchar_t *blbls[4] = { L"Open", L"Open in Terminal", L"Copy Path", L"Delete" };
                py += 38;
                for (int b = 0; b < 4; b++) {
                    fw->rc_info_btns[b].left = ix + 16; fw->rc_info_btns[b].top = py;
                    fw->rc_info_btns[b].right = ix + iw - 16; fw->rc_info_btns[b].bottom = py + 28;
                    draw_round_rect_fill(g, (float)(ix + 16), (float)py, (float)(iw - 32), 28.0f, 7.0f,
                                         b == 0 ? g_theme.accent : c_addr);
                    bool ib_pressed = (fw->pressed_btn == PB_INFO_BASE + b && fw->pressed_inside);
                    if (ib_pressed) {
                        draw_round_rect_fill(g, (float)(ix + 16), (float)py, (float)(iw - 32), 28.0f, 7.0f,
                                             MAKE_ARGB(b == 0 ? 56 : (g_theme.is_dark ? 40 : 28), 0, 0, 0));
                    } else if (fw->hover_info_btn == b) {
                        draw_round_rect_fill(g, (float)(ix + 16), (float)py, (float)(iw - 32), 28.0f, 7.0f,
                                             b == 0 ? MAKE_ARGB(34, 255, 255, 255)
                                                    : (g_theme.is_dark ? MAKE_ARGB(22, 255, 255, 255) : MAKE_ARGB(14, 0, 0, 0)));
                    }
                    draw_text(g, blbls[b], L"Segoe UI", 11.5f, GP_FONT_BOLD,
                              b == 0 ? g_theme.fg_on_accent : (b == 3 ? g_theme.danger : c_txt),
                              (float)(ix + 16), (float)py, (float)(iw - 32), 28.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
                    py += 34;
                }
            } else {
                ZeroMemory(fw->rc_info_btns, sizeof(fw->rc_info_btns));
                bool at_drive_root = (wcslen(cur_tab->path) <= 3 && cur_tab->path[1] == L':');

                /* Preview Box with Drive or Folder icon */
                draw_round_rect_fill(g, (float)(ix + 16), (float)(body_y + 16), (float)(iw - 32), 116.0f, 10.0f, c_bg);
                if (at_drive_root) {
                    draw_icon_glyph(g, 0xEDA2, 48.0f, mac_blue, (float)(ix + 16), (float)(body_y + 16), (float)(iw - 32), 116.0f);
                } else {
                    draw_folder_icon(g, (float)(ix + (iw - 64) / 2), (float)(body_y + 42), 64.0f);
                }

                const wchar_t *hdr_name = (at_drive_root && fw->cur_drive_label[0]) ? fw->cur_drive_label : cur_tab->title;
                draw_text(g, hdr_name, L"Segoe UI", 13.5f, GP_FONT_BOLD, c_txt,
                          (float)(ix + 16), (float)(body_y + 142), (float)(iw - 32), 22.0f,
                          GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);

                wchar_t sub[96];
                if (sel_n > 1) {
                    wchar_t sb[32]; format_bytes(sel_bytes, sb, 32);
                    _snwprintf_s(sub, 96, _TRUNCATE, L"%d items selected (%ls)", sel_n, sb);
                } else {
                    _snwprintf_s(sub, 96, _TRUNCATE, L"%d %ls", fw->visible_count,
                                 fw->visible_count == 1 ? L"item" : L"items");
                }
                draw_text(g, sub, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                          (float)(ix + 16), (float)(body_y + 164), (float)(iw - 32), 18.0f,
                          GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);

                if (fw->cur_drive_valid && fw->cur_drive_total > 0) {
                    uint64_t used_b = (fw->cur_drive_total >= fw->cur_drive_free)
                                    ? (fw->cur_drive_total - fw->cur_drive_free) : 0;
                    float pct = (float)used_b / (float)fw->cur_drive_total;
                    if (pct < 0.0f) pct = 0.0f;
                    if (pct > 1.0f) pct = 1.0f;
                    int pct_i = (int)(pct * 100.0f + 0.5f);

                    int py = body_y + 196;
                    draw_line(g, (float)(ix + 16), (float)py, (float)(ix + iw - 16), (float)py, c_bdr, 1.0f);
                    py += 14;

                    draw_text(g, L"STORAGE", L"Segoe UI", 10.5f, GP_FONT_BOLD, c_muted,
                              (float)(ix + 16), (float)py, (float)(iw - 32), 16.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                    py += 22;

                    float bar_w = (float)(iw - 32);
                    draw_round_rect_fill(g, (float)(ix + 16), (float)py, bar_w, 6.0f, 3.0f, c_addr);
                    draw_round_rect_fill(g, (float)(ix + 16), (float)py, max(6.0f, bar_w * pct), 6.0f, 3.0f,
                                         pct > 0.90f ? g_theme.danger : mac_blue);
                    py += 16;

                    wchar_t cap_s[32], avail_s[32], used_s[48];
                    format_bytes(fw->cur_drive_total, cap_s, 32);
                    format_bytes(fw->cur_drive_free, avail_s, 32);
                    wchar_t u_raw[32]; format_bytes(used_b, u_raw, 32);
                    _snwprintf_s(used_s, 48, _TRUNCATE, L"%ls (%d%%)", u_raw, pct_i);

                    draw_text(g, L"Capacity", L"Segoe UI", 11.0f, GP_FONT_BOLD, c_muted,
                              (float)(ix + 16), (float)py, 72.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                    draw_text(g, cap_s, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_txt,
                              (float)(ix + 88), (float)py, (float)(iw - 104), 20.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, true);
                    py += 24;

                    draw_text(g, L"Available", L"Segoe UI", 11.0f, GP_FONT_BOLD, c_muted,
                              (float)(ix + 16), (float)py, 72.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                    draw_text(g, avail_s, L"Segoe UI", 11.5f, GP_FONT_BOLD, c_txt,
                              (float)(ix + 88), (float)py, (float)(iw - 104), 20.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, true);
                    py += 24;

                    draw_text(g, L"Used", L"Segoe UI", 11.0f, GP_FONT_BOLD, c_muted,
                              (float)(ix + 16), (float)py, 72.0f, 20.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);
                    draw_text(g, used_s, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_txt,
                              (float)(ix + 88), (float)py, (float)(iw - 104), 20.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, true);
                }
            }
        }

        /* 5. Bottom Status Bar (item count + live drive available space) */
        int sty = h - 26;
        draw_round_rect_fill(g, 0.0f, (float)sty, (float)w, 26.0f, 0.0f, c_tb);
        draw_line(g, 0.0f, (float)sty + 0.5f, (float)w, (float)sty + 0.5f, c_bdr, 1.0f);

        static const wchar_t *tb_tips[9] = {
            L"Back (Alt+Left)", L"Forward (Alt+Right)", L"Up One Folder (Alt+Up)", L"Refresh (F5)",
            L"New Folder (Ctrl+Shift+N)", L"Switch List / Grid View", L"Show / Hide Hidden Files (Ctrl+H)",
            L"Toggle Preview Pane (Alt+P)", L"Open in Terminal"
        };
        uint64_t sel_b = 0;
        int sel_n = count_selected(fw, &sel_b, NULL);
        wchar_t st_txt[192];
        wchar_t avail_tail[64] = {0};
        if (fw->cur_drive_valid) {
            wchar_t av[32];
            format_bytes(fw->cur_drive_free, av, 32);
            _snwprintf_s(avail_tail, 64, _TRUNCATE, L", %ls available", av);
        }
        if (fw->hover_toolbar_btn >= 0 && fw->hover_toolbar_btn < 9) {
            wcsncpy_s(st_txt, 192, tb_tips[fw->hover_toolbar_btn], _TRUNCATE);
        } else if (sel_n > 0) {
            wchar_t sb[32]; format_bytes(sel_b, sb, 32);
            _snwprintf_s(st_txt, 192, _TRUNCATE, L"%d of %d selected (%ls)%ls",
                         sel_n, fw->visible_count, sb, avail_tail);
        } else {
            _snwprintf_s(st_txt, 192, _TRUNCATE, L"%d %ls%ls",
                         fw->visible_count, fw->visible_count == 1 ? L"item" : L"items", avail_tail);
        }
        draw_text(g, st_txt, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                  14.0f, (float)sty, (float)(w - 28), 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);

        /* 6. Drag badge next to the cursor ("Move 3 items to Downloads" / Ctrl = "Copy ...") */
        if (g_drag.active && g_drag.over == fw) {
            wchar_t badge[200];
            wchar_t what[MAX_PATH + 16];
            if (g_drag.count == 1) _snwprintf_s(what, MAX_PATH + 16, _TRUNCATE, L"\"%ls\"", g_drag.first_name);
            else _snwprintf_s(what, MAX_PATH + 16, _TRUNCATE, L"%d items", g_drag.count);
            if (g_drag.valid) {
                _snwprintf_s(badge, 200, _TRUNCATE, L"%ls %ls to %ls", g_drag.copy ? L"Copy" : L"Move", what, g_drag.target_label);
            } else {
                _snwprintf_s(badge, 200, _TRUNCATE, L"%ls %ls", g_drag.copy ? L"Copy" : L"Move", what);
            }
            float bw = measure_text_width(g, badge, L"Segoe UI", 11.5f, GP_FONT_BOLD) + 40.0f;
            if (bw > 360.0f) bw = 360.0f;
            float bx = (float)g_drag.cursor.x + 16.0f;
            float by = (float)g_drag.cursor.y + 18.0f;
            if (bx + bw > (float)w - 6.0f) bx = (float)w - 6.0f - bw;
            if (by + 26.0f > (float)h - 4.0f) by = (float)g_drag.cursor.y - 32.0f;
            draw_round_rect_fill(g, bx + 1.0f, by + 2.0f, bw, 26.0f, 13.0f, MAKE_ARGB(60, 0, 0, 0));
            draw_round_rect_fill(g, bx, by, bw, 26.0f, 13.0f, g_drag.valid ? g_theme.accent : c_tb);
            draw_round_rect_stroke(g, bx + 0.5f, by + 0.5f, bw - 1.0f, 25.0f, 13.0f, g_drag.valid ? g_theme.accent : c_bdr, 1.0f);
            draw_icon_glyph(g, g_drag.copy ? 0xE710 : 0xE8DE, 10.5f, g_drag.valid ? g_theme.fg_on_accent : c_muted,
                            bx + 6.0f, by, 20.0f, 26.0f);
            draw_text(g, badge, L"Segoe UI", 11.5f, GP_FONT_BOLD, g_drag.valid ? g_theme.fg_on_accent : c_muted,
                      bx + 28.0f, by, bw - 38.0f, 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        }
    }
}

static void paint_files(FilesWindow *fw) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(fw->hwnd, &ps);
    RECT cr;
    GetClientRect(fw->hwnd, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        draw_files(fw, &fb, cr.right, cr.bottom);
        opaque_frame_end(hdc, &fb);
    }
    EndPaint(fw->hwnd, &ps);
}

static int hit_test_file_item(const FilesWindow *fw, int mx, int my) {
    if (!PtInRect(&fw->rc_file_area, (POINT){ mx, my })) return -1;
    int list_top = fw->rc_file_area.top + (fw->grid_view ? 0 : 28);
    if (my < list_top) return -1;

    if (!fw->grid_view) {
        int idx = (my - list_top + fw->scroll_y) / 30;
        return (idx >= 0 && idx < fw->visible_count) ? idx : -1;
    } else {
        int fa_w = fw->rc_file_area.right - fw->rc_file_area.left;
        int cols = max(1, (fa_w - 24) / 112);
        int col = (mx - (fw->rc_file_area.left + 12)) / 112;
        int row = (my - (list_top + 10) + fw->scroll_y) / 116;
        if (col < 0 || col >= cols || row < 0) return -1;
        int idx = row * cols + col;
        return (idx >= 0 && idx < fw->visible_count) ? idx : -1;
    }
}

static void finish_inline_rename(FilesWindow *fw, bool commit) {
    if (!fw || !fw->hRenameEdit || fw->renaming_vis_idx < 0) return;
    int vi = fw->renaming_vis_idx;
    fw->renaming_vis_idx = -1;
    wchar_t new_name[MAX_PATH] = {0};
    GetWindowTextW(fw->hRenameEdit, new_name, MAX_PATH);
    ShowWindow(fw->hRenameEdit, SW_HIDE);
    if (!fw->destroying && IsWindow(fw->hwnd)) SetFocus(fw->hwnd);

    if (commit && !fw->destroying && new_name[0] && vi < fw->visible_count) {
        const FileEntry *e = &fw->entries[fw->visible[vi]];
        if (wcscmp(new_name, e->name) != 0) {
            wchar_t old_p[MAX_PATH], new_p[MAX_PATH];
            build_full_entry_path(fw, e, old_p, MAX_PATH);
            _snwprintf_s(new_p, MAX_PATH, _TRUNCATE, L"%ls\\%ls", fw->tabs[fw->active_tab].path, new_name);
            if (MoveFileExW(old_p, new_p, 0)) {
                start_async_load(fw);
            }
        }
    }
}

static void begin_inline_rename(FilesWindow *fw, int vis_idx) {
    if (!fw->hRenameEdit || vis_idx < 0 || vis_idx >= fw->visible_count) return;
    const FileEntry *e = &fw->entries[fw->visible[vis_idx]];
    fw->renaming_vis_idx = vis_idx;

    int x, y, w, h = 22;
    if (!fw->grid_view) {
        x = fw->rc_file_area.left + 36;
        y = fw->rc_file_area.top + 28 + vis_idx * 30 - fw->scroll_y + 4;
        w = max(120, (fw->rc_col_hdr[0].right - x) - 8);
    } else {
        int fa_w = fw->rc_file_area.right - fw->rc_file_area.left;
        int cols = max(1, (fa_w - 24) / 112);
        int r = vis_idx / cols, c = vis_idx % cols;
        x = fw->rc_file_area.left + 12 + c * 112;
        y = fw->rc_file_area.top + 10 + r * 116 - fw->scroll_y + 74;
        w = 104;
    }

    SetWindowTextW(fw->hRenameEdit, e->name);
    MoveWindow(fw->hRenameEdit, x, y, w, h, TRUE);
    ShowWindow(fw->hRenameEdit, SW_SHOW);
    SetFocus(fw->hRenameEdit);
    SendMessageW(fw->hRenameEdit, EM_SETSEL, 0, -1);
}

static void begin_addr_edit(FilesWindow *fw) {
    fw->editing_addr = true;
    SetWindowTextW(fw->hAddrEdit, fw->tabs[fw->active_tab].path);
    MoveWindow(fw->hAddrEdit, fw->rc_addr_bar.left + 8, fw->rc_addr_bar.top + 5,
               (fw->rc_addr_bar.right - fw->rc_addr_bar.left) - 16, 18, TRUE);
    ShowWindow(fw->hAddrEdit, SW_SHOW);
    SetFocus(fw->hAddrEdit);
    SendMessageW(fw->hAddrEdit, EM_SETSEL, 0, -1);
    InvalidateRect(fw->hwnd, NULL, FALSE);
}

static void end_addr_edit(FilesWindow *fw, bool commit) {
    if (!fw || !fw->editing_addr) return;
    wchar_t p[MAX_PATH] = {0};
    GetWindowTextW(fw->hAddrEdit, p, MAX_PATH);
    fw->editing_addr = false;
    ShowWindow(fw->hAddrEdit, SW_HIDE);
    if (!fw->destroying && IsWindow(fw->hwnd)) {
        SetFocus(fw->hwnd);
        if (commit && p[0]) navigate_to(fw, p, true);
        InvalidateRect(fw->hwnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK files_sub_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HWND parent = GetParent(hwnd);
    FilesWindow *fw = parent ? (FilesWindow *)GetWindowLongPtrW(parent, GWLP_USERDATA) : NULL;
    if (!fw) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    int id = GetDlgCtrlID(hwnd);
    WNDPROC orig = (id == ID_FILES_ADDR) ? fw->orig_addr_proc
                 : (id == ID_FILES_FILTER) ? fw->orig_filter_proc : fw->orig_rename_proc;
    if (!orig) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (!fw->destroying) {
        if (msg == WM_KEYDOWN) {
            if (id == ID_FILES_ADDR) {
                if (wParam == VK_RETURN) { end_addr_edit(fw, true); return 0; }
                if (wParam == VK_ESCAPE) { end_addr_edit(fw, false); return 0; }
            } else if (id == ID_FILES_RENAME) {
                if (wParam == VK_RETURN) { finish_inline_rename(fw, true); return 0; }
                if (wParam == VK_ESCAPE) { finish_inline_rename(fw, false); return 0; }
            } else if (id == ID_FILES_FILTER && wParam == VK_ESCAPE) {
                SetWindowTextW(hwnd, L"");
                SetFocus(fw->hwnd);
                return 0;
            }
        } else if (msg == WM_KILLFOCUS) {
            if (id == ID_FILES_ADDR) end_addr_edit(fw, false);
            else if (id == ID_FILES_RENAME) finish_inline_rename(fw, false);
        }
    }
    return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
}

static void show_sidebar_context_menu(FilesWindow *fw, int sb_idx, int sx, int sy) {
    if (!fw || sb_idx < 0 || sb_idx >= fw->sidebar_count) return;
    SidebarItem si = fw->sidebar[sb_idx];
    HMENU m = CreatePopupMenu();
    if (!m) return;

    AppendMenuW(m, MF_STRING, 1, L"Open in New Tab");
    AppendMenuW(m, MF_STRING, 2, L"Open in Terminal");
    if (si.is_bookmark) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        if (si.bookmark_idx > 0) AppendMenuW(m, MF_STRING, 3, L"Move Up");
        if (si.bookmark_idx + 1 < g_bookmark_count) AppendMenuW(m, MF_STRING, 4, L"Move Down");
        AppendMenuW(m, MF_STRING, 5, L"Remove from Favorites");
    }

    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, fw->hwnd, NULL);
    DestroyMenu(m);

    if (cmd == 1) add_new_tab(fw, si.path);
    else if (cmd == 2) open_terminal_at(si.path);
    else if (cmd == 3 && si.is_bookmark) bookmarks_move_idx(si.bookmark_idx, -1);
    else if (cmd == 4 && si.is_bookmark) bookmarks_move_idx(si.bookmark_idx, 1);
    else if (cmd == 5 && si.is_bookmark) bookmarks_remove_idx(si.bookmark_idx);
}

static void show_files_context_menu(FilesWindow *fw, int vis_idx, int sx, int sy) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    if (vis_idx >= 0 && vis_idx < fw->visible_count) {
        const FileEntry *e = &fw->entries[fw->visible[vis_idx]];
        AppendMenuW(m, MF_STRING, 1,
                    e->is_image ? L"Open in Image Viewer"
                                : (e->is_text ? L"Open in Text Editor" : L"Open"));
        if (e->is_dir) {
            AppendMenuW(m, MF_STRING, 2, L"Open in New Tab");
            wchar_t full[MAX_PATH];
            build_full_entry_path(fw, e, full, MAX_PATH);
            if (!is_builtin_or_drive_sidebar_path(fw, full)) {
                bool is_bm = (bookmarks_find_idx(full) >= 0);
                AppendMenuW(m, MF_STRING, 14, is_bm ? L"Remove from Favorites" : L"Add to Favorites\tCtrl+D");
            }
        }
        if (!e->is_dir && (e->is_image || e->is_text)) AppendMenuW(m, MF_STRING, 13, L"Open with External App");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, 3, L"Copy\tCtrl+C");
        AppendMenuW(m, MF_STRING, 4, L"Cut\tCtrl+X");
        AppendMenuW(m, MF_STRING, 5, L"Paste\tCtrl+V");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, 6, L"Rename\tF2");
        AppendMenuW(m, MF_STRING, 7, L"Copy Path");
        AppendMenuW(m, MF_STRING, 8, L"Move to Recycle Bin\tDel");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    } else {
        AppendMenuW(m, MF_STRING, 5, L"Paste\tCtrl+V");
        AppendMenuW(m, MF_STRING, 9, L"New Folder\tCtrl+Shift+N");
        const wchar_t *curp = fw->tabs[fw->active_tab].path;
        if (!is_builtin_or_drive_sidebar_path(fw, curp)) {
            bool is_bm = (bookmarks_find_idx(curp) >= 0);
            AppendMenuW(m, MF_STRING, 15, is_bm ? L"Remove Current Folder from Favorites\tCtrl+D"
                                                : L"Add Current Folder to Favorites\tCtrl+D");
        }
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, 10, L"Open in Terminal");
    AppendMenuW(m, MF_STRING, 11, fw->show_hidden ? L"Hide Hidden Files" : L"Show Hidden Files");
    AppendMenuW(m, MF_STRING, 12, L"Refresh\tF5");

    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, fw->hwnd, NULL);
    DestroyMenu(m);

    if (cmd == 1) open_vis_item(fw, vis_idx);
    else if (cmd == 2 && vis_idx >= 0) {
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, &fw->entries[fw->visible[vis_idx]], full, MAX_PATH);
        add_new_tab(fw, full);
    }
    else if (cmd == 3) copy_selection_to_clipboard(fw, false);
    else if (cmd == 4) copy_selection_to_clipboard(fw, true);
    else if (cmd == 5) paste_from_clipboard(fw);
    else if (cmd == 6) begin_inline_rename(fw, vis_idx);
    else if (cmd == 7 && vis_idx >= 0) {
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, &fw->entries[fw->visible[vis_idx]], full, MAX_PATH);
        copy_path_clipboard(fw->hwnd, full);
    }
    else if (cmd == 8) delete_selection(fw, false);
    else if (cmd == 9) create_new_folder(fw);
    else if (cmd == 10) {
        if (vis_idx >= 0 && fw->entries[fw->visible[vis_idx]].is_dir) {
            wchar_t full[MAX_PATH];
            build_full_entry_path(fw, &fw->entries[fw->visible[vis_idx]], full, MAX_PATH);
            open_terminal_at(full);
        } else {
            open_terminal_at(fw->tabs[fw->active_tab].path);
        }
    }
    else if (cmd == 11) {
        fw->show_hidden = !fw->show_hidden;
        rebuild_visible(fw);
        InvalidateRect(fw->hwnd, NULL, FALSE);
    }
    else if (cmd == 12) start_async_load(fw);
    else if (cmd == 13 && vis_idx >= 0) {
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, &fw->entries[fw->visible[vis_idx]], full, MAX_PATH);
        ShellExecuteW(fw->hwnd, L"open", full, NULL, fw->tabs[fw->active_tab].path, SW_SHOWNORMAL);
    }
    else if (cmd == 14 && vis_idx >= 0) {
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, &fw->entries[fw->visible[vis_idx]], full, MAX_PATH);
        bookmarks_toggle_path(fw, full);
    }
    else if (cmd == 15) {
        bookmarks_toggle_path(fw, fw->tabs[fw->active_tab].path);
    }
}

/* ======================================================================================
 * Shared helpers: keep the focused row visible, find Files windows, safe shell file ops
 * ====================================================================================== */

static void ensure_visible(FilesWindow *fw, int vis) {
    if (!fw || vis < 0 || vis >= fw->visible_count) return;
    int fa_w = fw->rc_file_area.right - fw->rc_file_area.left;
    int fa_h = fw->rc_file_area.bottom - fw->rc_file_area.top;
    if (fa_h <= 0) return;
    if (!fw->grid_view) {
        int view_h = fa_h - 28;
        int ry = vis * 30;
        if (ry < fw->scroll_y) fw->scroll_y = ry;
        else if (ry + 30 > fw->scroll_y + view_h) fw->scroll_y = ry + 30 - view_h;
    } else {
        int cols = max(1, (fa_w - 24) / 112);
        int ry = 10 + (vis / cols) * 116;
        if (ry < fw->scroll_y) fw->scroll_y = max(0, ry - 10);
        else if (ry + 116 > fw->scroll_y + fa_h) fw->scroll_y = ry + 116 - fa_h;
    }
    if (fw->scroll_y < 0) fw->scroll_y = 0;
    fw->tabs[fw->active_tab].scroll_y = fw->scroll_y;
}

static FilesWindow *files_from_hwnd(HWND h) {
    if (!h) return NULL;
    HWND root = GetAncestor(h, GA_ROOT);
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i] && g_wins[i]->hwnd == root) return g_wins[i];
    }
    return NULL;
}

/* true when 'target' is 'src' itself or lives somewhere below it */
static bool path_is_same_or_inside(const wchar_t *target, const wchar_t *src) {
    wchar_t s[MAX_PATH];
    wcsncpy_s(s, MAX_PATH, src, _TRUNCATE);
    size_t n = wcslen(s);
    while (n > 3 && s[n - 1] == L'\\') s[--n] = L'\0';
    if (_wcsnicmp(target, s, n) != 0) return false;
    return target[n] == L'\0' || target[n] == L'\\' || (n > 0 && s[n - 1] == L'\\');
}

/* Validates a drop: target must be an existing folder, never one of the sources (or inside one),
 * and a plain move into the folder the items already live in is a no-op. */
static bool drop_target_valid(const wchar_t *target, const wchar_t *from_list, const wchar_t *src_dir, bool copy) {
    if (!target || !target[0] || !from_list || !from_list[0]) return false;
    DWORD a = GetFileAttributesW(target);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (!copy && src_dir && src_dir[0] && paths_equal_norm(target, src_dir)) return false;
    for (const wchar_t *p = from_list; *p; p += wcslen(p) + 1) {
        if (path_is_same_or_inside(target, p)) return false;
    }
    return true;
}

static void refresh_windows_showing(const wchar_t *a, const wchar_t *b) {
    for (int i = 0; i < g_win_count; i++) {
        FilesWindow *w = g_wins[i];
        if (!w || !w->hwnd || w->tab_count <= 0) continue;
        const wchar_t *cur = w->tabs[w->active_tab].path;
        if ((a && a[0] && paths_equal_norm(cur, a)) || (b && b[0] && paths_equal_norm(cur, b))) {
            start_async_load(w);
        } else {
            refresh_drive_space(w);
            InvalidateRect(w->hwnd, NULL, FALSE);
        }
    }
}

/* Move (default) or copy (Ctrl) through the Windows shell: undo-able, shows the standard
 * progress / name-conflict dialogs, never overwrites silently. */
static void shell_move_or_copy(HWND owner, const wchar_t *from_list, const wchar_t *target_dir, bool copy, bool same_dir) {
    wchar_t to_buf[MAX_PATH + 2];
    ZeroMemory(to_buf, sizeof(to_buf));
    wcsncpy_s(to_buf, MAX_PATH, target_dir, _TRUNCATE);
    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.hwnd = owner;
    op.wFunc = copy ? FO_COPY : FO_MOVE;
    op.pFrom = from_list;
    op.pTo = to_buf;
    op.fFlags = FOF_ALLOWUNDO | (same_dir ? FOF_RENAMEONCOLLISION : 0);
    SHFileOperationW(&op);
}

/* Finds a drop target in Files window 'ow' at client point 'cp'.
 * Sidebar items (Favorites / bookmarks / Locations), folder rows/cells, or (optionally) the window's folder. */
static int files_drop_target_at(FilesWindow *ow, POINT cp, bool skip_selected, bool allow_window_dir,
                                 wchar_t *out_path, wchar_t *out_label, size_t label_len, int *out_idx) {
    *out_idx = -1;
    out_path[0] = L'\0';
    out_label[0] = L'\0';
    int body_top = ow->rc_file_area.top, body_bot = ow->rc_file_area.bottom;
    if (cp.x >= 0 && cp.x < ow->sidebar_w && cp.y >= body_top && cp.y < body_bot) {
        for (int i = 0; i < ow->sidebar_count; i++) {
            if (PtInRect(&ow->rc_sidebar_items[i], cp)) {
                wcsncpy_s(out_path, MAX_PATH, ow->sidebar[i].path, _TRUNCATE);
                wcsncpy_s(out_label, label_len, ow->sidebar[i].label, _TRUNCATE);
                *out_idx = i;
                return DROP_SIDEBAR;
            }
        }
        return DROP_NONE;
    }
    int hit = hit_test_file_item(ow, cp.x, cp.y);
    if (hit >= 0) {
        const FileEntry *e = &ow->entries[ow->visible[hit]];
        if (e->is_dir && !(skip_selected && e->selected)) {
            build_full_entry_path(ow, e, out_path, MAX_PATH);
            wcsncpy_s(out_label, label_len, e->name, _TRUNCATE);
            *out_idx = hit;
            return DROP_FOLDER_ITEM;
        }
    }
    if (allow_window_dir && PtInRect(&ow->rc_file_area, cp) && ow->tab_count > 0) {
        wcsncpy_s(out_path, MAX_PATH, ow->tabs[ow->active_tab].path, _TRUNCATE);
        wcsncpy_s(out_label, label_len, ow->tabs[ow->active_tab].title, _TRUNCATE);
        return DROP_WINDOW_DIR;
    }
    return DROP_NONE;
}

/* ======================================================================================
 * Drag & drop (in-process, mouse-capture based)
 * ====================================================================================== */

static void drag_free(void) {
    if (g_drag.paths) HeapFree(GetProcessHeap(), 0, g_drag.paths);
    ZeroMemory(&g_drag, sizeof(g_drag));
}

static void drag_update(POINT screen_pt) {
    FilesWindow *prev = g_drag.over;
    int prev_kind = g_drag.target_kind, prev_idx = g_drag.target_idx;
    bool prev_valid = g_drag.valid, prev_copy = g_drag.copy;

    g_drag.copy = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    g_drag.target_kind = DROP_NONE;
    g_drag.target_idx = -1;
    g_drag.valid = false;
    g_drag.target_path[0] = L'\0';
    g_drag.target_label[0] = L'\0';

    HWND under = WindowFromPoint(screen_pt);
    FilesWindow *ow = files_from_hwnd(under);
    g_drag.over = ow;
    if (ow) {
        POINT cp = screen_pt;
        ScreenToClient(ow->hwnd, &cp);
        g_drag.cursor = cp;
        int idx = -1;
        int kind = files_drop_target_at(ow, cp, ow == g_drag.src, ow != g_drag.src,
                                         g_drag.target_path, g_drag.target_label, 96, &idx);
        g_drag.target_kind = kind;
        g_drag.target_idx = idx;
        if (kind != DROP_NONE) {
            g_drag.valid = drop_target_valid(g_drag.target_path, g_drag.paths, g_drag.src_dir, g_drag.copy);
        }
    }
    SetCursor(LoadCursorW(NULL, g_drag.valid ? IDC_ARROW : IDC_NO));

    if (prev && prev != ow && IsWindow(prev->hwnd)) InvalidateRect(prev->hwnd, NULL, FALSE);
    if (ow) {
        /* The badge follows the cursor, so repaint every move; cheap thanks to the CPU back buffer */
        (void)prev_kind; (void)prev_idx; (void)prev_valid; (void)prev_copy;
        InvalidateRect(ow->hwnd, NULL, FALSE);
    }
}

static bool drag_begin(FilesWindow *fw) {
    int cnt = 0;
    for (int v = 0; v < fw->visible_count; v++) {
        if (fw->entries[fw->visible[v]].selected) cnt++;
    }
    if (cnt == 0) return false;
    size_t cap = (size_t)cnt * (MAX_PATH + 1) + 2;
    wchar_t *buf = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(wchar_t));
    if (!buf) return false;

    drag_free();
    wchar_t *p = buf;
    for (int v = 0; v < fw->visible_count; v++) {
        const FileEntry *e = &fw->entries[fw->visible[v]];
        if (!e->selected) continue;
        wchar_t full[MAX_PATH];
        build_full_entry_path(fw, e, full, MAX_PATH);
        size_t l = wcslen(full) + 1;
        memcpy(p, full, l * sizeof(wchar_t));
        p += l;
        if (g_drag.count == 0) wcsncpy_s(g_drag.first_name, MAX_PATH, e->name, _TRUNCATE);
        g_drag.count++;
    }
    *p = L'\0';
    g_drag.paths = buf;
    g_drag.src = fw;
    wcsncpy_s(g_drag.src_dir, MAX_PATH, fw->tabs[fw->active_tab].path, _TRUNCATE);
    g_drag.active = true;
    fw->drag_armed = false;
    fw->deferred_select_vis = -1;
    fw->deferred_toggle_vis = -1;
    if (ql_is_open_for(fw)) ql_close();

    POINT sp;
    GetCursorPos(&sp);
    drag_update(sp);
    return true;
}

static void drag_finish(bool do_drop) {
    if (!g_drag.active) return;
    FilesWindow *src = g_drag.src;
    FilesWindow *over = g_drag.over;
    bool run = do_drop && g_drag.valid && g_drag.paths;
    bool copy = g_drag.copy;
    wchar_t target[MAX_PATH], src_dir[MAX_PATH];
    wcsncpy_s(target, MAX_PATH, g_drag.target_path, _TRUNCATE);
    wcsncpy_s(src_dir, MAX_PATH, g_drag.src_dir, _TRUNCATE);
    wchar_t *paths = g_drag.paths;
    g_drag.paths = NULL;
    drag_free(); /* clears 'active' first so WM_CAPTURECHANGED below is a no-op */

    if (src && IsWindow(src->hwnd) && GetCapture() == src->hwnd) ReleaseCapture();
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    if (run) {
        shell_move_or_copy(src ? src->hwnd : NULL, paths, target, copy, paths_equal_norm(target, src_dir));
        refresh_windows_showing(src_dir, target);
    }
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (src && IsWindow(src->hwnd)) InvalidateRect(src->hwnd, NULL, FALSE);
    if (over && over != src && IsWindow(over->hwnd)) InvalidateRect(over->hwnd, NULL, FALSE);
}

/* Files dropped from Explorer / other apps (WM_DROPFILES): same rules, Ctrl = copy, else move */
static void handle_external_drop(FilesWindow *fw, HDROP hd) {
    POINT cp = { 0, 0 };
    DragQueryPoint(hd, &cp);
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
    if (n == 0) return;
    size_t cap = (size_t)n * (MAX_PATH + 1) + 2;
    wchar_t *buf = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(wchar_t));
    if (!buf) return;
    wchar_t *p = buf;
    wchar_t src_dir[MAX_PATH] = {0};
    for (UINT i = 0; i < n; i++) {
        wchar_t f[MAX_PATH] = {0};
        if (DragQueryFileW(hd, i, f, MAX_PATH) == 0) continue;
        if (!src_dir[0]) {
            wcsncpy_s(src_dir, MAX_PATH, f, _TRUNCATE);
            PathRemoveFileSpecW(src_dir);
        }
        size_t l = wcslen(f) + 1;
        memcpy(p, f, l * sizeof(wchar_t));
        p += l;
    }
    *p = L'\0';

    wchar_t target[MAX_PATH], label[96];
    int idx = -1;
    int kind = files_drop_target_at(fw, cp, false, true, target, label, 96, &idx);
    if (kind == DROP_NONE && fw->tab_count > 0) {
        wcsncpy_s(target, MAX_PATH, fw->tabs[fw->active_tab].path, _TRUNCATE);
    }
    bool copy = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (drop_target_valid(target, buf, src_dir, copy)) {
        shell_move_or_copy(fw->hwnd, buf, target, copy, paths_equal_norm(target, src_dir));
        refresh_windows_showing(src_dir, target);
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

/* ======================================================================================
 * Resizable sidebar + List view column dividers
 * ====================================================================================== */

static int hit_test_divider(const FilesWindow *fw, int mx, int my) {
    if (my >= fw->rc_file_area.top && my < fw->rc_file_area.bottom && abs(mx - fw->sidebar_w) <= 4) return 0;
    if (!fw->grid_view && my >= fw->rc_col_hdr[0].top && my < fw->rc_col_hdr[0].bottom) {
        for (int c = 0; c < 3; c++) {
            if (abs(mx - fw->rc_col_hdr[c].right) <= 4) return c + 1;
        }
    }
    return -1;
}

static void apply_resize_drag(FilesWindow *fw, int mx) {
    int d = mx - fw->resize_start_x;
    int fa_w = fw->rc_file_area.right - fw->rc_file_area.left;
    if (fw->resize_kind == 1) {
        int nw = fw->resize_start_val[0] + d;
        if (nw < SIDEBAR_MIN_W) nw = SIDEBAR_MIN_W;
        if (nw > SIDEBAR_MAX_W) nw = SIDEBAR_MAX_W;
        fw->sidebar_w = nw;
        g_last_sidebar_w = nw;
    } else if (fw->resize_kind >= 2 && fw->resize_kind <= 4) {
        int date0 = fw->resize_start_val[1], size0 = fw->resize_start_val[2], type0 = fw->resize_start_val[3];
        int date = date0, size = size0, type = type0;
        if (fw->resize_kind == 2) {
            /* Name | Date: Name grows to the right, Date gives the space back */
            int max_date = fa_w - size0 - type0 - 16 - COL_MIN_NAME;
            date = date0 - d;
            if (date > max_date) date = max_date;
            if (date < g_col_min_w[1]) date = g_col_min_w[1];
        } else if (fw->resize_kind == 3) {
            int dd = d;
            if (date0 + dd < g_col_min_w[1]) dd = g_col_min_w[1] - date0;
            if (size0 - dd < g_col_min_w[2]) dd = size0 - g_col_min_w[2];
            date = date0 + dd;
            size = size0 - dd;
        } else {
            int dd = d;
            if (size0 + dd < g_col_min_w[2]) dd = g_col_min_w[2] - size0;
            if (type0 - dd < g_col_min_w[3]) dd = type0 - g_col_min_w[3];
            size = size0 + dd;
            type = type0 - dd;
        }
        fw->col_w[1] = date; fw->col_w[2] = size; fw->col_w[3] = type;
        g_last_col_w[1] = date; g_last_col_w[2] = size; g_last_col_w[3] = type;
    }
    InvalidateRect(fw->hwnd, NULL, FALSE);
}

/* ======================================================================================
 * Press-and-release buttons (toolbar, caption, tabs, info pane actions)
 * ====================================================================================== */

static int pressable_hit(const FilesWindow *fw, POINT pt) {
    for (int c = 0; c < 3; c++) if (PtInRect(&fw->rc_cap_btns[c], pt)) return PB_CAP_BASE + c;
    if (PtInRect(&fw->rc_tab_add, pt)) return PB_TAB_ADD;
    for (int t = 0; t < fw->tab_count; t++) if (PtInRect(&fw->rc_tab_close[t], pt)) return PB_TAB_CLOSE_BASE + t;
    for (int b = 0; b < 9; b++) if (PtInRect(&fw->rc_tb_btns[b], pt)) return PB_TB_BASE + b;
    if (fw->show_info_pane) {
        for (int b = 0; b < 4; b++) {
            const RECT *r = &fw->rc_info_btns[b];
            if (r->right > r->left && PtInRect(r, pt)) return PB_INFO_BASE + b;
        }
    }
    return 0;
}

static void go_up_one(FilesWindow *fw) {
    wchar_t up[MAX_PATH];
    wcsncpy_s(up, MAX_PATH, fw->tabs[fw->active_tab].path, _TRUNCATE);
    PathRemoveBackslashW(up);
    if (PathRemoveFileSpecW(up) && up[0]) {
        if (wcslen(up) == 2 && up[1] == L':') wcsncat_s(up, MAX_PATH, L"\\", _TRUNCATE);
        navigate_to(fw, up, true);
    }
}

static void run_pressable(FilesWindow *fw, int id) {
    HWND hwnd = fw->hwnd;
    if (id >= PB_CAP_BASE && id < PB_CAP_BASE + 3) {
        int c = id - PB_CAP_BASE;
        if (c == 0) ShowWindow(hwnd, SW_MINIMIZE);
        else if (c == 1) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        else PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else if (id == PB_TAB_ADD) {
        add_new_tab(fw, fw->tabs[fw->active_tab].path);
    } else if (id >= PB_TAB_CLOSE_BASE && id < PB_TAB_CLOSE_BASE + MAX_FILES_TABS) {
        close_tab(fw, id - PB_TAB_CLOSE_BASE);
    } else if (id >= PB_TB_BASE && id < PB_TB_BASE + 9) {
        int b = id - PB_TB_BASE;
        FilesTab *ct = &fw->tabs[fw->active_tab];
        if (b == 0 && ct->hist_pos > 0) {
            ct->hist_pos--;
            navigate_to(fw, ct->hist[ct->hist_pos], false);
        } else if (b == 1 && ct->hist_pos + 1 < ct->hist_count) {
            ct->hist_pos++;
            navigate_to(fw, ct->hist[ct->hist_pos], false);
        } else if (b == 2) {
            go_up_one(fw);
        } else if (b == 3) {
            start_async_load(fw);
        } else if (b == 4) {
            create_new_folder(fw);
        } else if (b == 5) {
            fw->grid_view = !fw->grid_view;
            fw->scroll_y = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (b == 6) {
            fw->show_hidden = !fw->show_hidden;
            rebuild_visible(fw);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (b == 7) {
            fw->show_info_pane = !fw->show_info_pane;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (b == 8) {
            open_terminal_at(ct->path);
        }
    } else if (id >= PB_INFO_BASE && id < PB_INFO_BASE + 4) {
        int first_sel = -1;
        if (count_selected(fw, NULL, &first_sel) == 1 && first_sel >= 0) {
            int b = id - PB_INFO_BASE;
            const FileEntry *e = &fw->entries[fw->visible[first_sel]];
            wchar_t full[MAX_PATH];
            build_full_entry_path(fw, e, full, MAX_PATH);
            if (b == 0) open_vis_item(fw, first_sel);
            else if (b == 1) open_terminal_at(e->is_dir ? full : fw->tabs[fw->active_tab].path);
            else if (b == 2) copy_path_clipboard(hwnd, full);
            else if (b == 3) delete_selection(fw, false);
        }
    }
}

/* ======================================================================================
 * Quick Look (Space)
 * Non-activating layered panel owned by the Files window. Images are decoded by WIC straight
 * to display size, text/Markdown is read with a size cap; every buffer is VirtualAlloc backed so
 * closing returns 100% of the preview memory to Windows immediately.
 * ====================================================================================== */

static void ql_free_content(void) {
    if (g_ql.img)    { GdipDisposeImage((GpImage *)g_ql.img); g_ql.img = NULL; }
    if (g_ql.img_px) { VirtualFree(g_ql.img_px, 0, MEM_RELEASE); g_ql.img_px = NULL; }
    if (g_ql.disp)   { VirtualFree(g_ql.disp, 0, MEM_RELEASE); g_ql.disp = NULL; }
    if (g_ql.blocks) { VirtualFree(g_ql.blocks, 0, MEM_RELEASE); g_ql.blocks = NULL; }
    g_ql.img_w = g_ql.img_h = 0;
    g_ql.disp_bytes = 0;
    g_ql.blocks_bytes = 0;
    g_ql.block_count = 0;
    g_ql.content_h = 0.0f;
    g_ql.scroll_y = 0;
    g_ql.truncated = false;
    g_ql.meta[0] = L'\0';
}

static void ql_close(void) {
    if (!g_ql.hwnd) return;
    HWND h = g_ql.hwnd;
    FilesWindow *owner = g_ql.owner;
    g_ql.hwnd = NULL;
    DestroyWindow(h);
    ql_free_content();
    surface_destroy(&g_ql.surf);
    ZeroMemory(&g_ql, sizeof(g_ql));
    if (owner && IsWindow(owner->hwnd)) InvalidateRect(owner->hwnd, NULL, FALSE);
}

static bool ql_decode_image(const wchar_t *path, int max_w, int max_h, int *orig_w, int *orig_h) {
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *conv = NULL;
    bool ok = false;
    *orig_w = *orig_h = 0;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&fac)) || !fac) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, &dec)) || !dec) goto done;

    UINT best = 0;
    const wchar_t *ext = PathFindExtensionW(path);
    if (ext && _wcsicmp(ext, L".ico") == 0) {
        UINT fcnt = 0, best_area = 0;
        if (SUCCEEDED(IWICBitmapDecoder_GetFrameCount(dec, &fcnt))) {
            for (UINT i = 0; i < fcnt && i < 32; i++) {
                IWICBitmapFrameDecode *tf = NULL;
                if (SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, i, &tf)) && tf) {
                    UINT tw = 0, th = 0;
                    if (SUCCEEDED(IWICBitmapFrameDecode_GetSize(tf, &tw, &th)) && tw * th > best_area) {
                        best_area = tw * th;
                        best = i;
                    }
                    IWICBitmapFrameDecode_Release(tf);
                }
            }
        }
    }
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, best, &frame)) || !frame) goto done;
    UINT ow = 0, oh = 0;
    if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &ow, &oh)) || ow == 0 || oh == 0) goto done;
    *orig_w = (int)ow;
    *orig_h = (int)oh;

    double s = 1.0;
    if ((int)ow > max_w) s = (double)max_w / (double)ow;
    if ((double)oh * s > (double)max_h) s = (double)max_h / (double)oh;
    UINT dw = (UINT)max(1.0, (double)ow * s + 0.5);
    UINT dh = (UINT)max(1.0, (double)oh * s + 0.5);

    IWICBitmapSource *src = (IWICBitmapSource *)frame;
    if (dw != ow || dh != oh) {
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(fac, &scaler)) && scaler &&
            SUCCEEDED(IWICBitmapScaler_Initialize(scaler, src, dw, dh, WICBitmapInterpolationModeFant))) {
            src = (IWICBitmapSource *)scaler;
        } else {
            goto done;
        }
    }
    if (FAILED(IWICImagingFactory_CreateFormatConverter(fac, &conv)) || !conv) goto done;
    if (FAILED(IWICFormatConverter_Initialize(conv, src, &GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeMedianCut))) goto done;

    UINT stride = dw * 4;
    SIZE_T bytes = (SIZE_T)stride * dh;
    BYTE *px = (BYTE *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!px) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, stride, (UINT)bytes, px))) {
        VirtualFree(px, 0, MEM_RELEASE);
        goto done;
    }
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromScan0((INT)dw, (INT)dh, (INT)stride, GP_PIXELFORMAT_32BPP_PARGB, px, &bmp) != GP_OK || !bmp) {
        VirtualFree(px, 0, MEM_RELEASE);
        goto done;
    }
    g_ql.img_px = px;
    g_ql.img = bmp;
    g_ql.img_w = (int)dw;
    g_ql.img_h = (int)dh;
    ok = true;

done:
    if (conv)   IWICFormatConverter_Release(conv);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (frame)  IWICBitmapFrameDecode_Release(frame);
    if (dec)    IWICBitmapDecoder_Release(dec);
    if (fac)    IWICImagingFactory_Release(fac);
    return ok;
}

/* Reads up to cap bytes and decodes UTF-8 / UTF-8 BOM / UTF-16 LE/BE / ANSI into a VirtualAlloc'd buffer */
static wchar_t *ql_read_text(const wchar_t *path, SIZE_T cap, int *out_len, bool *out_trunc, SIZE_T *out_alloc) {
    *out_len = 0;
    *out_trunc = false;
    *out_alloc = 0;
    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER fsz;
    if (!GetFileSizeEx(hf, &fsz)) { CloseHandle(hf); return NULL; }
    SIZE_T want = (SIZE_T)min((ULONGLONG)fsz.QuadPart, (ULONGLONG)cap);
    *out_trunc = ((ULONGLONG)fsz.QuadPart > (ULONGLONG)cap);
    BYTE *raw = (BYTE *)VirtualAlloc(NULL, want + 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!raw) { CloseHandle(hf); return NULL; }
    DWORD got = 0;
    BOOL rd = want ? ReadFile(hf, raw, (DWORD)want, &got, NULL) : TRUE;
    CloseHandle(hf);
    if (!rd) { VirtualFree(raw, 0, MEM_RELEASE); return NULL; }

    const BYTE *p = raw;
    int n = (int)got;
    wchar_t *out = NULL;
    int wlen = 0;
    if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
        bool be = (p[0] == 0xFE);
        int chars = (n - 2) / 2;
        SIZE_T bytes = (SIZE_T)(chars + 1) * sizeof(wchar_t);
        out = (wchar_t *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (out) {
            for (int i = 0; i < chars; i++) {
                BYTE a = p[2 + i * 2], b = p[3 + i * 2];
                out[i] = be ? (wchar_t)((a << 8) | b) : (wchar_t)((b << 8) | a);
            }
            wlen = chars;
            *out_alloc = bytes;
        }
    } else {
        int skip = (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) ? 3 : 0;
        const char *s = (const char *)p + skip;
        int sn = n - skip;
        /* Drop a UTF-8 sequence cut in half by the size cap */
        if (*out_trunc) {
            int back = 0;
            while (back < 3 && sn - 1 - back >= 0 && (((BYTE)s[sn - 1 - back]) & 0xC0) == 0x80) back++;
            if (sn - 1 - back >= 0 && ((BYTE)s[sn - 1 - back]) >= 0xC0) sn -= back + 1;
        }
        UINT cp = CP_UTF8;
        int need = sn > 0 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, sn, NULL, 0) : 0;
        if (sn > 0 && need <= 0) {
            cp = CP_ACP;
            need = MultiByteToWideChar(CP_ACP, 0, s, sn, NULL, 0);
        }
        SIZE_T bytes = (SIZE_T)(max(need, 0) + 1) * sizeof(wchar_t);
        out = (wchar_t *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (out) {
            wlen = (sn > 0) ? MultiByteToWideChar(cp, 0, s, sn, out, need) : 0;
            if (wlen < 0) wlen = 0;
            *out_alloc = bytes;
        }
    }
    VirtualFree(raw, 0, MEM_RELEASE);
    if (out) out[wlen] = L'\0';
    *out_len = wlen;
    return out;
}

/* Strips inline Markdown markers (**bold**, `code`, [text](url), ![alt](url)) for the preview */
static int ql_md_inline(const wchar_t *s, int n, wchar_t *out) {
    int o = 0;
    int close_at = -1; /* matching single * or _ of an emphasis span */
    for (int i = 0; i < n; i++) {
        wchar_t c = s[i];
        if (i == close_at) { close_at = -1; continue; }
        if ((c == L'*' || c == L'_' || c == L'~') && i + 1 < n && s[i + 1] == c) { i++; continue; }
        if ((c == L'*' || c == L'_') && close_at < 0 && (i == 0 || !iswalnum(s[i - 1])) &&
            i + 1 < n && !iswspace(s[i + 1])) {
            /* *emphasis* / _emphasis_: drop both markers when a proper closing one exists */
            for (int j = i + 2; j < n; j++) {
                if (s[j] == c && !iswspace(s[j - 1]) && (j + 1 == n || !iswalnum(s[j + 1]))) { close_at = j; break; }
            }
            if (close_at >= 0) continue;
        }
        if (c == L'`') continue;
        if (c == L'!' && i + 1 < n && s[i + 1] == L'[') continue;
        if (c == L'[') {
            int j = i + 1;
            while (j < n && s[j] != L']') j++;
            if (j + 1 < n && s[j] == L']' && s[j + 1] == L'(') {
                int k = j + 2;
                while (k < n && s[k] != L')') k++;
                if (k < n) {
                    for (int t = i + 1; t < j; t++) {
                        if (s[t] == L'*' || s[t] == L'`') continue;
                        out[o++] = s[t];
                    }
                    i = k;
                    continue;
                }
            }
        }
        out[o++] = c;
    }
    return o;
}

static QlBlock *ql_add_block(int type, int indent) {
    if (g_ql.block_count >= QL_MAX_BLOCKS) { g_ql.truncated = true; return NULL; }
    QlBlock *b = &g_ql.blocks[g_ql.block_count++];
    ZeroMemory(b, sizeof(*b));
    b->type = (uint8_t)type;
    b->indent = (uint8_t)min(indent, 6);
    return b;
}

static void ql_build_blocks(const wchar_t *src, int src_len, bool markdown) {
    g_ql.disp_bytes = (SIZE_T)(src_len + 64) * sizeof(wchar_t);
    g_ql.disp = (wchar_t *)VirtualAlloc(NULL, g_ql.disp_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_ql.blocks_bytes = (SIZE_T)QL_MAX_BLOCKS * sizeof(QlBlock);
    g_ql.blocks = (QlBlock *)VirtualAlloc(NULL, g_ql.blocks_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_ql.disp || !g_ql.blocks) return;

    int dpos = 0;
    bool in_code = false;
    int code_start = -1;
    int i = 0;
    while (i <= src_len && g_ql.block_count < QL_MAX_BLOCKS) {
        if (i == src_len && i > 0 && (src[i - 1] == L'\n')) break;
        int ls = i;
        while (i < src_len && src[i] != L'\n') i++;
        int le = i;
        if (le > ls && src[le - 1] == L'\r') le--;
        i++; /* skip '\n' */
        const wchar_t *line = src + ls;
        int ll = le - ls;

        if (!markdown) {
            QlBlock *b = ql_add_block(QB_CODE, 0);
            if (!b) break;
            b->start = dpos;
            for (int k = 0; k < ll; k++) g_ql.disp[dpos++] = (line[k] == L'\t') ? L' ' : line[k];
            b->len = dpos - b->start;
            if (ls >= src_len) break;
            continue;
        }

        int ind = 0;
        while (ind < ll && (line[ind] == L' ' || line[ind] == L'\t')) ind += 1;
        const wchar_t *t = line + ind;
        int tl = ll - ind;
        while (tl > 0 && (t[tl - 1] == L' ' || t[tl - 1] == L'\t')) tl--;

        bool fence = (tl >= 3 && ((t[0] == L'`' && t[1] == L'`' && t[2] == L'`') ||
                                  (t[0] == L'~' && t[1] == L'~' && t[2] == L'~')));
        if (fence) {
            if (!in_code) { in_code = true; code_start = g_ql.block_count; }
            else {
                in_code = false;
                if (g_ql.block_count > code_start && code_start >= 0) {
                    g_ql.blocks[code_start].code_first = 1;
                    g_ql.blocks[g_ql.block_count - 1].code_last = 1;
                }
                code_start = -1;
            }
            if (ls >= src_len) break;
            continue;
        }
        if (in_code) {
            QlBlock *b = ql_add_block(QB_CODE, 0);
            if (!b) break;
            b->start = dpos;
            for (int k = 0; k < ll; k++) g_ql.disp[dpos++] = (line[k] == L'\t') ? L' ' : line[k];
            b->len = dpos - b->start;
            if (ls >= src_len) break;
            continue;
        }

        QlBlock *prev = g_ql.block_count > 0 ? &g_ql.blocks[g_ql.block_count - 1] : NULL;
        if (tl == 0) {
            if (prev && prev->type != QB_BLANK) ql_add_block(QB_BLANK, 0);
            if (ls >= src_len) break;
            continue;
        }

        int type = QB_PARA, skip = 0;
        int hashes = 0;
        while (hashes < tl && hashes < 7 && t[hashes] == L'#') hashes++;
        bool all_rule = tl >= 3;
        wchar_t rc = t[0];
        int rule_cnt = 0;
        for (int k = 0; k < tl && all_rule; k++) {
            if (t[k] == L' ') continue;
            if (t[k] != rc || (rc != L'-' && rc != L'*' && rc != L'_')) all_rule = false;
            else rule_cnt++;
        }
        if (hashes >= 1 && hashes <= 6 && (hashes == tl || t[hashes] == L' ')) {
            type = QB_H1 + hashes - 1;
            skip = min(tl, hashes + 1);
        } else if (all_rule && rule_cnt >= 3) {
            type = QB_RULE;
        } else if (t[0] == L'>') {
            type = QB_QUOTE;
            skip = 1;
            while (skip < tl && (t[skip] == L'>' || t[skip] == L' ')) skip++;
        } else if (tl >= 6 && (t[0] == L'-' || t[0] == L'*' || t[0] == L'+') && t[1] == L' ' && t[2] == L'[' &&
                   (t[3] == L' ' || t[3] == L'x' || t[3] == L'X') && t[4] == L']') {
            type = (t[3] == L' ') ? QB_TASK_OPEN : QB_TASK_DONE;
            skip = 5;
            while (skip < tl && t[skip] == L' ') skip++;
        } else if (tl >= 2 && (t[0] == L'-' || t[0] == L'*' || t[0] == L'+') && t[1] == L' ') {
            type = QB_BULLET;
            skip = 2;
        } else if (iswdigit(t[0])) {
            int k = 0;
            while (k < tl && k < 9 && iswdigit(t[k])) k++;
            if (k < tl - 1 && (t[k] == L'.' || t[k] == L')') && t[k + 1] == L' ') type = QB_NUMBER;
        } else if (t[0] == L'|') {
            bool sep = true;
            for (int k = 0; k < tl; k++) {
                if (t[k] != L'|' && t[k] != L'-' && t[k] != L':' && t[k] != L' ') { sep = false; break; }
            }
            if (sep) { if (ls >= src_len) break; continue; }
            type = QB_MONO;
        }

        /* Lazy continuation lines join the previous paragraph / list item / quote */
        bool can_join = prev && type == QB_PARA &&
                        (prev->type == QB_PARA || ((prev->type == QB_BULLET || prev->type == QB_NUMBER ||
                                                    prev->type == QB_TASK_OPEN || prev->type == QB_TASK_DONE) && ind >= 2));
        if (type == QB_QUOTE && prev && prev->type == QB_QUOTE) can_join = true;
        if (can_join && prev->start + prev->len == dpos) {
            g_ql.disp[dpos++] = L' ';
            dpos += ql_md_inline(t + skip, tl - skip, g_ql.disp + dpos);
            prev->len = dpos - prev->start;
            if (ls >= src_len) break;
            continue;
        }

        QlBlock *b = ql_add_block(type, ind / 2);
        if (!b) break;
        b->start = dpos;
        if (type == QB_MONO) {
            for (int k = 0; k < tl; k++) g_ql.disp[dpos++] = t[k];
        } else if (type != QB_RULE) {
            int body = tl - skip;
            if (type >= QB_H1 && type <= QB_H6) {
                while (body > 0 && (t[skip + body - 1] == L'#' || t[skip + body - 1] == L' ')) body--;
            }
            dpos += ql_md_inline(t + skip, body, g_ql.disp + dpos);
        }
        b->len = dpos - b->start;
        if (ls >= src_len) break;
    }
    if (in_code && code_start >= 0 && g_ql.block_count > code_start) {
        g_ql.blocks[code_start].code_first = 1;
        g_ql.blocks[g_ql.block_count - 1].code_last = 1;
    }
    if (dpos < (int)(g_ql.disp_bytes / sizeof(wchar_t))) g_ql.disp[dpos] = L'\0';
}

static void ql_md_style(int type, float *size, int *style, const wchar_t **font) {
    *font = L"Segoe UI";
    *style = GP_FONT_REGULAR;
    *size = 14.0f;
    switch (type) {
    case QB_H1: *size = 24.0f; *style = GP_FONT_BOLD; break;
    case QB_H2: *size = 20.0f; *style = GP_FONT_BOLD; break;
    case QB_H3: *size = 17.0f; *style = GP_FONT_BOLD; break;
    case QB_H4: *size = 15.0f; *style = GP_FONT_BOLD; break;
    case QB_H5: *size = 14.0f; *style = GP_FONT_BOLD; break;
    case QB_H6: *size = 13.0f; *style = GP_FONT_BOLD; break;
    case QB_QUOTE: *style = GP_FONT_ITALIC; break;
    case QB_CODE: case QB_MONO: *font = L"Consolas"; *size = 12.5f; break;
    default: break;
    }
}

static float ql_md_indent(const QlBlock *b) {
    float base = (float)b->indent * 18.0f;
    switch (b->type) {
    case QB_BULLET: return base + 20.0f;
    case QB_NUMBER: return base + 4.0f;
    case QB_TASK_OPEN: case QB_TASK_DONE: return base + 26.0f;
    case QB_QUOTE: return 16.0f;
    case QB_CODE: return 14.0f;
    default: return 0.0f;
    }
}

static void ql_layout_blocks(float col_w) {
    float y = 0.0f;
    if (g_ql.kind == QL_KIND_TEXT) {
        for (int i = 0; i < g_ql.block_count; i++) {
            g_ql.blocks[i].y = y;
            g_ql.blocks[i].h = 18.0f;
            y += 18.0f;
        }
        g_ql.content_h = y + 12.0f;
        return;
    }
    for (int i = 0; i < g_ql.block_count; i++) {
        QlBlock *b = &g_ql.blocks[i];
        float size; int style; const wchar_t *font;
        ql_md_style(b->type, &size, &style, &font);
        float before = 0.0f, after = 4.0f;
        if (b->type >= QB_H1 && b->type <= QB_H3) { before = (i == 0) ? 0.0f : 14.0f; after = (b->type <= QB_H2) ? 14.0f : 6.0f; }
        else if (b->type >= QB_H4 && b->type <= QB_H6) { before = (i == 0) ? 0.0f : 10.0f; after = 4.0f; }
        else if (b->type == QB_CODE || b->type == QB_MONO) { before = b->code_first ? 8.0f : 0.0f; after = b->code_last ? 10.0f : 0.0f; }
        else if (b->type == QB_BULLET || b->type == QB_NUMBER || b->type == QB_TASK_OPEN || b->type == QB_TASK_DONE) after = 3.0f;
        float h;
        if (b->type == QB_BLANK) h = 6.0f;
        else if (b->type == QB_RULE) h = 18.0f;
        else if (b->type == QB_CODE || b->type == QB_MONO) h = 18.0f;
        else {
            float ind = ql_md_indent(b);
            h = (b->len > 0) ? draw_text_wrapped(NULL, g_ql.disp + b->start, b->len, font, size, style, 0, 0.0f, 0.0f,
                                                 col_w - ind, 0.0f, false) : 0.0f;
            if (h < size * 1.3f) h = size * 1.3f;
        }
        b->y = y + before;
        b->h = h;
        y = b->y + h + after;
    }
    g_ql.content_h = y + 24.0f;
}

static void ql_format_kind(const FileEntry *e, wchar_t *out, size_t len) {
    if (e->is_dir) { wcsncpy_s(out, len, L"Folder", _TRUNCATE); return; }
    if (!e->ext[0]) { wcsncpy_s(out, len, L"File", _TRUNCATE); return; }
    wchar_t up[24];
    int k = 0;
    for (int i = 1; e->ext[i] && k < 22; i++) up[k++] = (wchar_t)towupper(e->ext[i]);
    up[k] = L'\0';
    _snwprintf_s(out, len, _TRUNCATE, L"%ls %ls", up, e->is_image ? L"image" : (e->is_text ? L"document" : L"file"));
}

static void ql_load_entry(FilesWindow *fw, int vis) {
    ql_free_content();
    if (vis < 0 || vis >= fw->visible_count) return;
    const FileEntry *e = &fw->entries[fw->visible[vis]];
    build_full_entry_path(fw, e, g_ql.path, MAX_PATH);
    wcsncpy_s(g_ql.name, MAX_PATH, e->name, _TRUNCATE);
    g_ql.vis_idx = vis;
    g_ql.is_dir = e->is_dir;
    g_ql.owner = fw;

    /* Bounds: up to ~86% of the Files window, never larger than the monitor work area */
    RECT wr;
    GetWindowRect(fw->hwnd, &wr);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(fw->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int work_w = mi.rcWork.right - mi.rcWork.left - QL_SHADOW * 2 - 16;
    int work_h = mi.rcWork.bottom - mi.rcWork.top - QL_SHADOW * 2 - 16;
    int max_pw = min(1100, min(work_w, max(420, (int)((wr.right - wr.left) * 0.86f))));
    int max_ph = min(820, min(work_h, max(320, (int)((wr.bottom - wr.top) * 0.86f))));

    wchar_t pos[32], szs[32], kind[48];
    _snwprintf_s(pos, 32, _TRUNCATE, L"%d of %d", vis + 1, fw->visible_count);
    format_bytes(e->size, szs, 32);
    ql_format_kind(e, kind, 48);

    g_ql.kind = QL_KIND_GENERIC;
    if (e->is_image) {
        int ow = 0, oh = 0;
        if (ql_decode_image(g_ql.path, max_pw - 28, max_ph - QL_TITLE_H - 28, &ow, &oh)) {
            g_ql.kind = QL_KIND_IMAGE;
            g_ql.panel_w = max(380, g_ql.img_w + 28);
            g_ql.panel_h = max(260, g_ql.img_h + QL_TITLE_H + 28);
            _snwprintf_s(g_ql.meta, 160, _TRUNCATE, L"%ls   |   %d x %d   |   %ls", pos, ow, oh, szs);
        }
    } else if (e->is_text) {
        bool md = (_wcsicmp(e->ext, L".md") == 0 || _wcsicmp(e->ext, L".markdown") == 0 || _wcsicmp(e->ext, L".mkd") == 0);
        int tlen = 0;
        bool trunc = false;
        SIZE_T alloc = 0;
        wchar_t *txt = ql_read_text(g_ql.path, md ? QL_MD_CAP : QL_TEXT_CAP, &tlen, &trunc, &alloc);
        if (txt) {
            g_ql.kind = md ? QL_KIND_MARKDOWN : QL_KIND_TEXT;
            g_ql.panel_w = min(max_pw, md ? 860 : 900);
            g_ql.panel_h = max_ph;
            ql_build_blocks(txt, tlen, md);
            VirtualFree(txt, 0, MEM_RELEASE);
            g_ql.truncated = g_ql.truncated || trunc;
            float col_w = (g_ql.kind == QL_KIND_MARKDOWN) ? (float)min(g_ql.panel_w - 72, 720) : (float)(g_ql.panel_w - 80);
            if (g_ql.blocks) ql_layout_blocks(col_w);
            _snwprintf_s(g_ql.meta, 160, _TRUNCATE, L"%ls   |   %ls   |   %ls%ls", pos, kind, szs,
                         g_ql.truncated ? L"   |   preview shows the beginning" : L"");
        }
    }
    if (g_ql.kind == QL_KIND_GENERIC) {
        g_ql.panel_w = 440;
        g_ql.panel_h = 320;
        if (e->is_dir) {
            /* Single-level item count only (never recursive) */
            int items = 0;
            wchar_t pat[MAX_PATH];
            _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\*", g_ql.path);
            WIN32_FIND_DATAW fd;
            HANDLE hf = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) items++;
                } while (items < 100000 && FindNextFileW(hf, &fd));
                FindClose(hf);
            }
            _snwprintf_s(g_ql.meta, 160, _TRUNCATE, L"%ls   |   Folder   |   %d %ls", pos, items, items == 1 ? L"item" : L"items");
        } else {
            _snwprintf_s(g_ql.meta, 160, _TRUNCATE, L"%ls   |   %ls   |   %ls", pos, kind, szs);
        }
    }
}

static void ql_position_centered(int cx, int cy) {
    int W = g_ql.panel_w + QL_SHADOW * 2;
    int H = g_ql.panel_h + QL_SHADOW * 2;
    int x = cx - W / 2, y = cy - H / 2;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint((POINT){ cx, cy }, MONITOR_DEFAULTTONEAREST), &mi);
    if (x + W > mi.rcWork.right + QL_SHADOW) x = mi.rcWork.right + QL_SHADOW - W;
    if (y + H > mi.rcWork.bottom + QL_SHADOW) y = mi.rcWork.bottom + QL_SHADOW - H;
    if (x < mi.rcWork.left - QL_SHADOW) x = mi.rcWork.left - QL_SHADOW;
    if (y < mi.rcWork.top - QL_SHADOW) y = mi.rcWork.top - QL_SHADOW;
    g_ql.win_x = x;
    g_ql.win_y = y;
}

static void ql_get_btn_rects(RECT *close_rc, RECT *open_rc) {
    int ox = QL_SHADOW, oy = QL_SHADOW;
    close_rc->left = ox + 12; close_rc->top = oy + 14; close_rc->right = ox + 36; close_rc->bottom = oy + 38;
    open_rc->left = ox + g_ql.panel_w - 128; open_rc->top = oy + 13;
    open_rc->right = ox + g_ql.panel_w - 14; open_rc->bottom = oy + 39;
}

static int ql_max_scroll(void) {
    int view_h = g_ql.panel_h - QL_TITLE_H - 16;
    int ms = (int)g_ql.content_h - view_h;
    return ms > 0 ? ms : 0;
}

static void ql_paint(void) {
    if (!g_ql.hwnd) return;
    int pw = g_ql.panel_w, ph = g_ql.panel_h;
    int W = pw + QL_SHADOW * 2, H = ph + QL_SHADOW * 2;
    if (!surface_ensure(&g_ql.surf, W, H)) return;
    GpGraphics *g = g_ql.surf.g;
    GdipGraphicsClear(g, 0);

    bool dark = g_theme.is_dark;
    ARGB c_panel = dark ? MAKE_ARGB(250, 32, 32, 35) : MAKE_ARGB(252, 251, 251, 252);
    ARGB c_title = dark ? MAKE_ARGB(255, 44, 44, 48) : MAKE_ARGB(255, 238, 238, 241);
    ARGB c_bdr   = dark ? MAKE_ARGB(46, 255, 255, 255) : MAKE_ARGB(40, 0, 0, 0);
    ARGB c_txt   = dark ? MAKE_ARGB(255, 234, 234, 238) : MAKE_ARGB(255, 28, 28, 30);
    ARGB c_mut   = dark ? MAKE_ARGB(255, 150, 150, 160) : MAKE_ARGB(255, 112, 112, 120);
    ARGB c_code  = dark ? MAKE_ARGB(255, 46, 46, 51) : MAKE_ARGB(255, 241, 241, 244);
    ARGB c_hov   = dark ? MAKE_ARGB(40, 255, 255, 255) : MAKE_ARGB(22, 0, 0, 0);
    float ox = (float)QL_SHADOW, oy = (float)QL_SHADOW;

    /* Soft layered drop shadow */
    for (int k = 9; k >= 1; k--) {
        float sp = (float)k * 2.6f;
        draw_round_rect_fill(g, ox - sp, oy - sp + 8.0f, (float)pw + sp * 2.0f, (float)ph + sp * 2.0f,
                             14.0f + sp, MAKE_ARGB(dark ? 9 : 6, 0, 0, 0));
    }
    /* Panel: title tint on top, content color below with rounded bottom corners */
    draw_round_rect_fill(g, ox, oy, (float)pw, (float)ph, 12.0f, c_title);
    draw_round_rect_fill(g, ox, oy + QL_TITLE_H, (float)pw, (float)(ph - QL_TITLE_H) - 12.0f, 0.0f, c_panel);
    draw_round_rect_fill(g, ox, oy + (float)ph - 26.0f, (float)pw, 26.0f, 12.0f, c_panel);
    draw_line(g, ox, oy + QL_TITLE_H - 0.5f, ox + (float)pw, oy + QL_TITLE_H - 0.5f, c_bdr, 1.0f);
    draw_round_rect_stroke(g, ox + 0.5f, oy + 0.5f, (float)pw - 1.0f, (float)ph - 1.0f, 12.0f, c_bdr, 1.0f);

    /* Title bar: close, file name + meta, Open button */
    RECT rc_close, rc_open;
    ql_get_btn_rects(&rc_close, &rc_open);
    bool cp_ = (g_ql.press_btn == 1 && g_ql.hover_btn == 1);
    draw_circle_fill(g, (float)(rc_close.left + 12), (float)(rc_close.top + 12), 10.0f,
                     g_ql.hover_btn == 1 ? (cp_ ? MAKE_ARGB(255, 200, 58, 50) : MAKE_ARGB(255, 255, 95, 87))
                                         : (dark ? MAKE_ARGB(60, 255, 255, 255) : MAKE_ARGB(30, 0, 0, 0)));
    draw_icon_glyph(g, 0xE711, 8.0f, g_ql.hover_btn == 1 ? MAKE_ARGB(220, 70, 10, 8) : c_mut,
                    (float)rc_close.left, (float)rc_close.top, 24.0f, 24.0f);

    const wchar_t *open_lbl = g_ql.is_dir ? L"Open Folder"
                            : g_ql.kind == QL_KIND_IMAGE ? L"Open in Viewer"
                            : (g_ql.kind == QL_KIND_MARKDOWN || g_ql.kind == QL_KIND_TEXT) ? L"Open in Editor" : L"Open";
    draw_round_rect_fill(g, (float)rc_open.left, (float)rc_open.top, (float)(rc_open.right - rc_open.left),
                         (float)(rc_open.bottom - rc_open.top), 7.0f, g_theme.accent);
    if (g_ql.hover_btn == 2) {
        draw_round_rect_fill(g, (float)rc_open.left, (float)rc_open.top, (float)(rc_open.right - rc_open.left),
                             (float)(rc_open.bottom - rc_open.top), 7.0f,
                             g_ql.press_btn == 2 ? MAKE_ARGB(60, 0, 0, 0) : MAKE_ARGB(34, 255, 255, 255));
    }
    draw_text(g, open_lbl, L"Segoe UI", 11.5f, GP_FONT_BOLD, g_theme.fg_on_accent,
              (float)rc_open.left, (float)rc_open.top, (float)(rc_open.right - rc_open.left),
              (float)(rc_open.bottom - rc_open.top), GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

    float tx = ox + 48.0f, tw = (float)pw - 48.0f - 140.0f;
    draw_text(g, g_ql.name, L"Segoe UI", 13.0f, GP_FONT_BOLD, c_txt, tx, oy + 8.0f, tw, 20.0f,
              GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);
    draw_text(g, g_ql.meta, L"Segoe UI", 11.0f, GP_FONT_REGULAR, c_mut, tx, oy + 27.0f, tw, 18.0f,
              GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);

    /* Content */
    float cx0 = ox, cy0 = oy + QL_TITLE_H, cw = (float)pw, ch = (float)(ph - QL_TITLE_H);
    GdipSetClipRect(g, cx0 + 1.0f, cy0, cw - 2.0f, ch - 6.0f, GP_COMBINE_REPLACE);

    if (g_ql.kind == QL_KIND_IMAGE && g_ql.img) {
        float ix = cx0 + (cw - (float)g_ql.img_w) * 0.5f;
        float iy = cy0 + (ch - (float)g_ql.img_h) * 0.5f;
        GdipDrawImageRectI(g, (GpImage *)g_ql.img, (INT)ix, (INT)iy, g_ql.img_w, g_ql.img_h);
    } else if ((g_ql.kind == QL_KIND_MARKDOWN || g_ql.kind == QL_KIND_TEXT) && g_ql.blocks && g_ql.disp) {
        bool md = (g_ql.kind == QL_KIND_MARKDOWN);
        float col_w = md ? (float)min(pw - 72, 720) : (float)(pw - 80);
        float base_x = md ? cx0 + (cw - col_w) * 0.5f : cx0 + 62.0f;
        float base_y = cy0 + 18.0f - (float)g_ql.scroll_y;
        float view_top = cy0, view_bot = cy0 + ch;
        wchar_t num[16];
        if (md) {
            /* Pass 1: fenced code backgrounds (one rounded card per code block) */
            for (int i = 0; i < g_ql.block_count; i++) {
                const QlBlock *b = &g_ql.blocks[i];
                float by = base_y + b->y;
                if (by > view_bot) break;
                if (b->type != QB_CODE || by + b->h + 12.0f < view_top) continue;
                float top = by - (b->code_first ? 6.0f : 0.0f);
                float hh = b->h + (b->code_first ? 6.0f : 0.0f) + (b->code_last ? 6.0f : 0.0f);
                if (b->code_first && b->code_last)  draw_round_rect_fill(g, base_x, top, col_w, hh, 6.0f, c_code);
                else if (b->code_first)             draw_round_rect_fill(g, base_x, top, col_w, hh + 10.0f, 6.0f, c_code);
                else if (b->code_last)              draw_round_rect_fill(g, base_x, top - 10.0f, col_w, hh + 10.0f, 6.0f, c_code);
                else                                draw_round_rect_fill(g, base_x, top, col_w, hh, 0.0f, c_code);
            }
        }
        for (int i = 0; i < g_ql.block_count; i++) {
            const QlBlock *b = &g_ql.blocks[i];
            float by = base_y + b->y;
            if (by > view_bot) break;
            if (by + b->h + 12.0f < view_top) continue;
            const wchar_t *s = g_ql.disp + b->start;
            if (!md) {
                _snwprintf_s(num, 16, _TRUNCATE, L"%d", i + 1);
                draw_text(g, num, L"Consolas", 11.5f, GP_FONT_REGULAR, c_mut, cx0 + 6.0f, by, 44.0f, 18.0f,
                          GP_ALIGN_FAR, GP_ALIGN_CENTER, false);
                if (b->len > 0) {
                    wchar_t line[512];
                    int n = min(b->len, 511);
                    memcpy(line, s, (size_t)n * sizeof(wchar_t));
                    line[n] = L'\0';
                    draw_text(g, line, L"Consolas", 12.5f, GP_FONT_REGULAR, c_txt, base_x, by, col_w, 18.0f,
                              GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                }
                continue;
            }
            float size; int style; const wchar_t *font;
            ql_md_style(b->type, &size, &style, &font);
            float ind = ql_md_indent(b);
            ARGB col = c_txt;
            if (b->type == QB_H6 || b->type == QB_QUOTE || b->type == QB_TASK_DONE) col = c_mut;

            if (b->type == QB_CODE || b->type == QB_MONO) {
                if (b->len > 0) {
                    wchar_t line[512];
                    int n = min(b->len, 511);
                    memcpy(line, s, (size_t)n * sizeof(wchar_t));
                    line[n] = L'\0';
                    draw_text(g, line, font, size, style, col, base_x + ind, by, col_w - ind - 8.0f, 18.0f,
                              GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                }
                continue;
            }
            if (b->type == QB_RULE) {
                draw_line(g, base_x, by + 9.0f, base_x + col_w, by + 9.0f, c_bdr, 1.0f);
                continue;
            }
            if (b->type == QB_BLANK) continue;
            if (b->type == QB_BULLET) {
                draw_circle_fill(g, base_x + ind - 11.0f, by + size * 0.68f, 2.6f, c_txt);
            } else if (b->type == QB_TASK_OPEN || b->type == QB_TASK_DONE) {
                float bx = base_x + ind - 22.0f, byy = by + size * 0.68f - 7.0f;
                if (b->type == QB_TASK_DONE) {
                    draw_round_rect_fill(g, bx, byy, 14.0f, 14.0f, 4.0f, g_theme.accent);
                    draw_icon_glyph(g, 0xE73E, 9.0f, g_theme.fg_on_accent, bx, byy, 14.0f, 14.0f);
                } else {
                    draw_round_rect_stroke(g, bx + 0.5f, byy + 0.5f, 13.0f, 13.0f, 4.0f, c_mut, 1.2f);
                }
            } else if (b->type == QB_QUOTE) {
                draw_round_rect_fill(g, base_x + 2.0f, by, 3.0f, b->h, 1.5f, c_bdr);
            }
            if (b->len > 0) {
                draw_text_wrapped(g, s, b->len, font, size, style, col, base_x + ind, by, col_w - ind, b->h + 4.0f, true);
            }
            if (b->type == QB_H1 || b->type == QB_H2) {
                draw_line(g, base_x, by + b->h + 6.0f, base_x + col_w, by + b->h + 6.0f, c_bdr, 1.0f);
            }
        }
        /* Slim scrollbar */
        int ms = ql_max_scroll();
        if (ms > 0) {
            float view_h = ch - 16.0f;
            float th = max(28.0f, view_h * view_h / (float)g_ql.content_h);
            float ty = cy0 + 8.0f + ((float)g_ql.scroll_y / (float)ms) * (view_h - th);
            draw_round_rect_fill(g, cx0 + cw - 9.0f, ty, 5.0f, th, 2.5f, c_bdr);
        }
    } else {
        /* Generic card: big icon + name + kind */
        float icx = cx0 + (cw - 96.0f) * 0.5f, icy = cy0 + ch * 0.5f - 86.0f;
        if (g_ql.is_dir) {
            draw_folder_icon(g, icx, icy, 96.0f);
        } else if (g_ql.owner && g_ql.vis_idx >= 0 && g_ql.vis_idx < g_ql.owner->visible_count) {
            const FileEntry *e = &g_ql.owner->entries[g_ql.owner->visible[g_ql.vis_idx]];
            draw_entry_icon(g, g_ql.owner, e, (int)icx, (int)icy, 96);
        }
        draw_text(g, g_ql.name, L"Segoe UI", 15.0f, GP_FONT_BOLD, c_txt, cx0 + 20.0f, icy + 108.0f, cw - 40.0f, 24.0f,
                  GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);
        if (g_ql.owner && g_ql.vis_idx >= 0 && g_ql.vis_idx < g_ql.owner->visible_count) {
            const FileEntry *e = &g_ql.owner->entries[g_ql.owner->visible[g_ql.vis_idx]];
            wchar_t dt[32], line2[96];
            format_filetime(&e->mtime, dt, 32);
            _snwprintf_s(line2, 96, _TRUNCATE, L"Modified %ls", dt);
            draw_text(g, line2, L"Segoe UI", 12.0f, GP_FONT_REGULAR, c_mut, cx0 + 20.0f, icy + 134.0f, cw - 40.0f, 20.0f,
                      GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);
        }
    }
    GdipResetClip(g);
    (void)c_hov;
    surface_present(g_ql.hwnd, &g_ql.surf, g_ql.win_x, g_ql.win_y, 255);
}

static void ql_scroll_by(int dy) {
    if (g_ql.kind != QL_KIND_MARKDOWN && g_ql.kind != QL_KIND_TEXT) return;
    int ms = ql_max_scroll();
    int ns = g_ql.scroll_y + dy;
    if (ns < 0) ns = 0;
    if (ns > ms) ns = ms;
    if (ns != g_ql.scroll_y) {
        g_ql.scroll_y = ns;
        ql_paint();
    }
}

static void ql_open_owner_item(void) {
    FilesWindow *fw = g_ql.owner;
    int v = g_ql.vis_idx;
    ql_close();
    if (fw && IsWindow(fw->hwnd)) open_vis_item(fw, v);
}

static LRESULT CALLBACK ql_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT panel = { QL_SHADOW, QL_SHADOW, QL_SHADOW + g_ql.panel_w, QL_SHADOW + g_ql.panel_h };
        if (!PtInRect(&panel, pt)) return HTTRANSPARENT;
        RECT rc_close, rc_open;
        ql_get_btn_rects(&rc_close, &rc_open);
        if (PtInRect(&rc_close, pt) || PtInRect(&rc_open, pt)) return HTCLIENT;
        if (pt.y < QL_SHADOW + QL_TITLE_H) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS *wp = (const WINDOWPOS *)lParam;
        if (wp && !(wp->flags & SWP_NOMOVE)) {
            g_ql.win_x = wp->x;
            g_ql.win_y = wp->y;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        RECT rc_close, rc_open;
        ql_get_btn_rects(&rc_close, &rc_open);
        int hv = PtInRect(&rc_close, pt) ? 1 : (PtInRect(&rc_open, pt) ? 2 : 0);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        if (hv != g_ql.hover_btn) { g_ql.hover_btn = hv; ql_paint(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_ql.hover_btn && !g_ql.press_btn) { g_ql.hover_btn = 0; ql_paint(); }
        return 0;
    case WM_LBUTTONDOWN:
        if (g_ql.hover_btn) {
            g_ql.press_btn = g_ql.hover_btn;
            SetCapture(hwnd);
            ql_paint();
        }
        return 0;
    case WM_LBUTTONUP: {
        int pressed = g_ql.press_btn;
        g_ql.press_btn = 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (pressed && pressed == g_ql.hover_btn) {
            if (pressed == 1) {
                FilesWindow *owner = g_ql.owner;
                ql_close();
                if (owner && IsWindow(owner->hwnd)) SetFocus(owner->hwnd);
            } else {
                ql_open_owner_item();
            }
            return 0;
        }
        if (g_ql.hwnd) ql_paint();
        return 0;
    }
    case WM_MOUSEWHEEL:
        ql_scroll_by(-(GET_WHEEL_DELTA_WPARAM(wParam) / 120) * 54);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ql_open_for(FilesWindow *fw, int vis) {
    if (!fw || vis < 0 || vis >= fw->visible_count) return;
    if (g_ql.hwnd && g_ql.owner != fw) ql_close();
    bool fresh = (g_ql.hwnd == NULL);
    int cx, cy;
    if (fresh) {
        RECT wr;
        GetWindowRect(fw->hwnd, &wr);
        cx = (wr.left + wr.right) / 2;
        cy = (wr.top + wr.bottom) / 2;
    } else {
        cx = g_ql.win_x + (g_ql.panel_w + QL_SHADOW * 2) / 2;
        cy = g_ql.win_y + (g_ql.panel_h + QL_SHADOW * 2) / 2;
    }
    ql_load_entry(fw, vis);
    ql_position_centered(cx, cy);
    if (fresh) {
        g_ql.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    QL_CLASS, L"Quick Look", WS_POPUP,
                                    g_ql.win_x, g_ql.win_y,
                                    g_ql.panel_w + QL_SHADOW * 2, g_ql.panel_h + QL_SHADOW * 2,
                                    fw->hwnd, NULL, g_hInst, NULL);
        if (!g_ql.hwnd) {
            ql_free_content();
            ZeroMemory(&g_ql, sizeof(g_ql));
            return;
        }
        g_ql.owner = fw;
    }
    ql_paint();
    if (fresh) ShowWindow(g_ql.hwnd, SW_SHOWNOACTIVATE);
}

static void ql_step(int delta) {
    FilesWindow *fw = g_ql.owner;
    if (!fw || fw->visible_count <= 0) return;
    int nv = g_ql.vis_idx + delta;
    if (nv < 0) nv = 0;
    if (nv >= fw->visible_count) nv = fw->visible_count - 1;
    if (nv == g_ql.vis_idx) return;
    select_only(fw, nv);
    ensure_visible(fw, nv);
    InvalidateRect(fw->hwnd, NULL, FALSE);
    ql_open_for(fw, nv);
}

static LRESULT CALLBACK files_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    FilesWindow *fw = (FilesWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    if (!fw) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            NCCALCSIZE_PARAMS *np = (NCCALCSIZE_PARAMS *)lParam;
            if (IsZoomed(hwnd)) {
                int pad = GetSystemMetrics(SM_CXPADDEDBORDER);
                int fx = GetSystemMetrics(SM_CXSIZEFRAME) + pad;
                int fy = GetSystemMetrics(SM_CYSIZEFRAME) + pad;
                np->rgrc[0].left   += fx;
                np->rgrc[0].top    += fy;
                np->rgrc[0].right  -= fx;
                np->rgrc[0].bottom -= fy;
            }
            return 0; /* Removes default Windows caption so our unified tab+title bar owns y=0..38 cleanly */
        }
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        RECT wr;
        GetWindowRect(hwnd, &wr);
        if (!IsZoomed(hwnd)) {
            const int b = 6;
            bool left   = (pt.x < wr.left + b);
            bool right  = (pt.x >= wr.right - b);
            bool top    = (pt.y < wr.top + b);
            bool bottom = (pt.y >= wr.bottom - b);
            if (top && left)     return HTTOPLEFT;
            if (top && right)    return HTTOPRIGHT;
            if (bottom && left)  return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left)   return HTLEFT;
            if (right)  return HTRIGHT;
            if (top)    return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        POINT cl = pt;
        ScreenToClient(hwnd, &cl);
        if (cl.y >= 0 && cl.y < 38) {
            for (int c = 0; c < 3; c++) {
                if (PtInRect(&fw->rc_cap_btns[c], cl)) return HTCLIENT;
            }
            if (PtInRect(&fw->rc_tab_add, cl)) return HTCLIENT;
            for (int t = 0; t < fw->tab_count; t++) {
                if (PtInRect(&fw->rc_tabs[t], cl)) return HTCLIENT;
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_LARES_FILES_LOADED: {
        LoadResult *res = (LoadResult *)lParam;
        if (res) {
            if (res->gen == fw->load_gen) {
                if (fw->entries) HeapFree(GetProcessHeap(), 0, fw->entries);
                fw->entries = res->entries;
                fw->entry_count = res->count;
                fw->loading = false;
                rebuild_visible(fw);
                if (fw->pending_select_name[0]) {
                    int found_v = -1;
                    for (int v = 0; v < fw->visible_count; v++) {
                        if (_wcsicmp(fw->entries[fw->visible[v]].name, fw->pending_select_name) == 0) {
                            found_v = v;
                            break;
                        }
                    }
                    if (found_v < 0 && !fw->show_hidden) {
                        bool exists_hidden = false;
                        for (int i = 0; i < fw->entry_count; i++) {
                            if (_wcsicmp(fw->entries[i].name, fw->pending_select_name) == 0) {
                                exists_hidden = true;
                                break;
                            }
                        }
                        if (exists_hidden) {
                            fw->show_hidden = true;
                            rebuild_visible(fw);
                            for (int v = 0; v < fw->visible_count; v++) {
                                if (_wcsicmp(fw->entries[fw->visible[v]].name, fw->pending_select_name) == 0) {
                                    found_v = v;
                                    break;
                                }
                            }
                        }
                    }
                    if (found_v >= 0) {
                        select_only(fw, found_v);
                        if (!fw->grid_view) {
                            int target_y = found_v * 30 - 90;
                            if (target_y < 0) target_y = 0;
                            fw->scroll_y = target_y;
                            fw->tabs[fw->active_tab].scroll_y = target_y;
                        }
                    }
                    fw->pending_select_name[0] = L'\0';
                }
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                if (res->entries) HeapFree(GetProcessHeap(), 0, res->entries);
            }
            HeapFree(GetProcessHeap(), 0, res);
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_FILES_FILTER && HIWORD(wParam) == EN_CHANGE) {
            GetWindowTextW(fw->hFilterEdit, fw->filter_text, 128);
            rebuild_visible(fw);
            fw->scroll_y = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_theme.files_text);
        SetBkColor(hdc, g_theme.files_addrbar);
        return (LRESULT)g_hEditBrush;
    }

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && ql_is_open_for(fw)) ql_close();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (g_drag.active) {
                SetCursor(LoadCursorW(NULL, g_drag.valid ? IDC_ARROW : IDC_NO));
                return TRUE;
            }
            POINT cp;
            GetCursorPos(&cp);
            ScreenToClient(hwnd, &cp);
            if (fw->resize_kind || hit_test_divider(fw, cp.x, cp.y) >= 0) {
                SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;

    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (ql_is_open_for(fw)) {
            /* Wheel over the Quick Look panel scrolls the preview, not the list */
            POINT sp = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            RECT qr = { g_ql.win_x + QL_SHADOW, g_ql.win_y + QL_SHADOW,
                        g_ql.win_x + QL_SHADOW + g_ql.panel_w, g_ql.win_y + QL_SHADOW + g_ql.panel_h };
            if (PtInRect(&qr, sp)) {
                ql_scroll_by(-(delta / 120) * 54);
                return 0;
            }
        }
        fw->scroll_y -= (delta / 120) * 60;
        if (fw->scroll_y < 0) fw->scroll_y = 0;
        fw->tabs[fw->active_tab].scroll_y = fw->scroll_y;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!fw->tracking_mouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            fw->tracking_mouse = true;
        }
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT pt = { mx, my };

        /* Divider resizing */
        if (fw->resize_kind) {
            SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
            apply_resize_drag(fw, mx);
            return 0;
        }
        /* Active drag: track the target under the cursor (any Files window) */
        if (g_drag.active && g_drag.src == fw) {
            POINT sp = pt;
            ClientToScreen(hwnd, &sp);
            drag_update(sp);
            return 0;
        }
        /* Armed drag: start once the mouse travels past the system drag threshold */
        if (fw->drag_armed && (wParam & MK_LBUTTON)) {
            int dx = abs(mx - fw->drag_origin.x), dy = abs(my - fw->drag_origin.y);
            if (dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG)) {
                if (drag_begin(fw)) return 0;
                fw->drag_armed = false;
            }
        }
        /* Press-and-release: show the pressed state only while still over the button */
        if (fw->pressed_btn) {
            bool inside = (pressable_hit(fw, pt) == fw->pressed_btn);
            if (inside != fw->pressed_inside) {
                fw->pressed_inside = inside;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }

        int nh_div = hit_test_divider(fw, mx, my);
        int nh_ib = -1;
        if (fw->show_info_pane) {
            for (int b = 0; b < 4; b++) {
                const RECT *r = &fw->rc_info_btns[b];
                if (r->right > r->left && PtInRect(r, pt)) { nh_ib = b; break; }
            }
        }
        if (nh_div != fw->hover_divider || nh_ib != fw->hover_info_btn) {
            fw->hover_divider = nh_div;
            fw->hover_info_btn = nh_ib;
            InvalidateRect(hwnd, NULL, FALSE);
        }

        int nh_sb = -1;
        for (int i = 0; i < fw->sidebar_count; i++) {
            if (PtInRect(&fw->rc_sidebar_items[i], pt)) { nh_sb = i; break; }
        }
        int nh_tb = -1;
        for (int b = 0; b < 9; b++) {
            if (PtInRect(&fw->rc_tb_btns[b], pt)) { nh_tb = b; break; }
        }
        int nh_cap = 0;
        for (int c = 0; c < 3; c++) {
            if (PtInRect(&fw->rc_cap_btns[c], pt)) { nh_cap = c + 1; break; }
        }
        int nh_tab = -1, nh_tclose = -1;
        for (int t = 0; t < fw->tab_count; t++) {
            if (PtInRect(&fw->rc_tab_close[t], pt)) nh_tclose = t;
            if (PtInRect(&fw->rc_tabs[t], pt)) { nh_tab = t; break; }
        }
        bool nh_tadd = PtInRect(&fw->rc_tab_add, pt) != 0;
        int nh_crumb = -1;
        for (int c = 0; c < fw->crumb_count; c++) {
            if (PtInRect(&fw->rc_crumbs[c], pt)) { nh_crumb = c; break; }
        }
        int nh_fi = hit_test_file_item(fw, mx, my);

        if (nh_sb != fw->hover_sidebar_idx || nh_tb != fw->hover_toolbar_btn ||
            nh_fi != fw->hover_vis_idx || nh_tab != fw->hover_tab_idx ||
            nh_cap != fw->hover_caption_btn || nh_crumb != fw->hover_crumb_idx ||
            nh_tadd != fw->hover_tab_add || nh_tclose != fw->hover_tab_close_idx) {
            fw->hover_sidebar_idx = nh_sb;
            fw->hover_toolbar_btn = nh_tb;
            fw->hover_vis_idx = nh_fi;
            fw->hover_tab_idx = nh_tab;
            fw->hover_caption_btn = nh_cap;
            fw->hover_crumb_idx = nh_crumb;
            fw->hover_tab_add = nh_tadd;
            fw->hover_tab_close_idx = nh_tclose;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        fw->tracking_mouse = false;
        fw->hover_sidebar_idx = -1;
        fw->hover_toolbar_btn = -1;
        fw->hover_vis_idx = -1;
        fw->hover_tab_idx = -1;
        fw->hover_caption_btn = 0;
        fw->hover_crumb_idx = -1;
        fw->hover_tab_add = false;
        fw->hover_tab_close_idx = -1;
        fw->hover_divider = -1;
        fw->hover_info_btn = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT pt = { mx, my };
        SetFocus(hwnd);

        /* Sidebar / column divider resize */
        int div = hit_test_divider(fw, mx, my);
        if (div >= 0) {
            fw->resize_kind = div + 1;
            fw->resize_start_x = mx;
            fw->resize_start_val[0] = fw->sidebar_w;
            for (int c = 1; c < 4; c++) fw->resize_start_val[c] = fw->col_w[c];
            SetCapture(hwnd);
            SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Caption / tab / toolbar / info-pane buttons act on release (press-and-release) */
        int pb = pressable_hit(fw, pt);
        if (pb) {
            fw->pressed_btn = pb;
            fw->pressed_inside = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Tabs */
        for (int t = 0; t < fw->tab_count; t++) {
            if (PtInRect(&fw->rc_tabs[t], pt)) {
                if (t != fw->active_tab) {
                    fw->tabs[fw->active_tab].scroll_y = fw->scroll_y;
                    fw->active_tab = t;
                    start_async_load(fw);
                }
                return 0;
            }
        }

        /* Breadcrumbs or Address bar */
        if (PtInRect(&fw->rc_addr_bar, pt)) {
            for (int c = 0; c < fw->crumb_count; c++) {
                if (PtInRect(&fw->rc_crumbs[c], pt)) {
                    navigate_to(fw, fw->crumb_paths[c], true);
                    return 0;
                }
            }
            begin_addr_edit(fw);
            return 0;
        }

        /* Sidebar */
        for (int i = 0; i < fw->sidebar_count; i++) {
            if (PtInRect(&fw->rc_sidebar_items[i], pt)) {
                navigate_to(fw, fw->sidebar[i].path, true);
                return 0;
            }
        }

        /* Column headers */
        if (!fw->grid_view) {
            for (int c = 0; c < 4; c++) {
                if (PtInRect(&fw->rc_col_hdr[c], pt)) {
                    if (fw->sort_col == (SortCol)c) fw->sort_asc = !fw->sort_asc;
                    else { fw->sort_col = (SortCol)c; fw->sort_asc = true; }
                    rebuild_visible(fw);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
        }

        /* File item selection (+ arm a drag; multi-selection collapse / Ctrl un-toggle wait for release) */
        int hit = hit_test_file_item(fw, mx, my);
        fw->deferred_select_vis = -1;
        fw->deferred_toggle_vis = -1;
        if (hit >= 0) {
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool was_sel = fw->entries[fw->visible[hit]].selected;
            if (shift && fw->focus_vis_idx >= 0) {
                int lo = min(fw->focus_vis_idx, hit);
                int hi = max(fw->focus_vis_idx, hit);
                for (int v = lo; v <= hi; v++) fw->entries[fw->visible[v]].selected = true;
            } else if (ctrl) {
                if (was_sel) fw->deferred_toggle_vis = hit;
                else fw->entries[fw->visible[hit]].selected = true;
                fw->focus_vis_idx = hit;
            } else if (was_sel && count_selected(fw, NULL, NULL) > 1) {
                fw->deferred_select_vis = hit;
                fw->focus_vis_idx = hit;
            } else {
                select_only(fw, hit);
            }
            if (fw->entries[fw->visible[hit]].selected) {
                fw->drag_armed = true;
                fw->drag_origin = pt;
                SetCapture(hwnd);
            }
        } else if (PtInRect(&fw->rc_file_area, pt)) {
            select_only(fw, -1);
        }
        if (ql_is_open_for(fw) && fw->focus_vis_idx >= 0 && fw->focus_vis_idx != g_ql.vis_idx &&
            fw->deferred_select_vis < 0 && fw->deferred_toggle_vis < 0) {
            ql_open_for(fw, fw->focus_vis_idx); /* Quick Look follows the clicked item */
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT pt = { mx, my };
        if (fw->resize_kind) {
            fw->resize_kind = 0;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_drag.active && g_drag.src == fw) {
            POINT sp = pt;
            ClientToScreen(hwnd, &sp);
            drag_update(sp);
            drag_finish(true);
            return 0;
        }
        if (fw->pressed_btn) {
            int id = fw->pressed_btn;
            bool inside = (pressable_hit(fw, pt) == id);
            fw->pressed_btn = 0;
            fw->pressed_inside = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            if (inside) run_pressable(fw, id);
            return 0;
        }
        if (fw->drag_armed) {
            fw->drag_armed = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            /* Plain click without dragging: apply the deferred selection change now */
            if (fw->deferred_select_vis >= 0 && fw->deferred_select_vis < fw->visible_count) {
                select_only(fw, fw->deferred_select_vis);
            } else if (fw->deferred_toggle_vis >= 0 && fw->deferred_toggle_vis < fw->visible_count) {
                FileEntry *e = &fw->entries[fw->visible[fw->deferred_toggle_vis]];
                e->selected = !e->selected;
            }
            fw->deferred_select_vis = -1;
            fw->deferred_toggle_vis = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if ((HWND)lParam != hwnd) {
            if (g_drag.active && g_drag.src == fw) drag_finish(false);
            if (fw->pressed_btn) { fw->pressed_btn = 0; fw->pressed_inside = false; InvalidateRect(hwnd, NULL, FALSE); }
            if (fw->resize_kind) { fw->resize_kind = 0; InvalidateRect(hwnd, NULL, FALSE); }
            fw->drag_armed = false;
        }
        return 0;

    case WM_DROPFILES:
        handle_external_drop(fw, (HDROP)wParam);
        DragFinish((HDROP)wParam);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && ql_is_open_for(fw)) {
            HWND to = (HWND)lParam;
            if (to != g_ql.hwnd) ql_close(); /* hides Quick Look when the app loses focus */
        }
        break;

    case WM_LBUTTONDBLCLK: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT dpt = { mx, my };
        if (hit_test_divider(fw, mx, my) >= 0 || pressable_hit(fw, dpt)) {
            /* Fast repeated clicks on buttons/dividers behave like normal presses */
            return files_wnd_proc(hwnd, WM_LBUTTONDOWN, wParam, lParam);
        }
        int hit = hit_test_file_item(fw, mx, my);
        if (hit >= 0) open_vis_item(fw, hit);
        return 0;
    }

    case WM_RBUTTONUP: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT pt = { mx, my };
        for (int i = 0; i < fw->sidebar_count; i++) {
            if (PtInRect(&fw->rc_sidebar_items[i], pt)) {
                ClientToScreen(hwnd, &pt);
                show_sidebar_context_menu(fw, i, pt.x, pt.y);
                return 0;
            }
        }
        if (PtInRect(&fw->rc_file_area, pt)) {
            int hit = hit_test_file_item(fw, mx, my);
            if (hit >= 0 && !fw->entries[fw->visible[hit]].selected) {
                select_only(fw, hit);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            ClientToScreen(hwnd, &pt);
            show_files_context_menu(fw, hit, pt.x, pt.y);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;

        /* Drag in progress: Esc cancels, Ctrl flips Move <-> Copy live */
        if (g_drag.active) {
            if (wParam == VK_ESCAPE) { drag_finish(false); return 0; }
            if (wParam == VK_CONTROL) { POINT sp; GetCursorPos(&sp); drag_update(sp); return 0; }
            return 0;
        }

        /* Quick Look: Space toggles, arrows step, Enter opens, Esc closes */
        if (wParam == VK_SPACE && !ctrl && !alt) {
            if (lParam & 0x40000000) return 0; /* ignore key auto-repeat */
            if (ql_is_open_for(fw)) ql_close();
            else if (fw->focus_vis_idx >= 0 && fw->focus_vis_idx < fw->visible_count) ql_open_for(fw, fw->focus_vis_idx);
            return 0;
        }
        if (ql_is_open_for(fw)) {
            if (wParam == VK_ESCAPE) { ql_close(); return 0; }
            if (wParam == VK_UP || wParam == VK_LEFT)    { ql_step(-1); return 0; }
            if (wParam == VK_DOWN || wParam == VK_RIGHT) { ql_step(1); return 0; }
            if (wParam == VK_RETURN) { ql_open_owner_item(); return 0; }
            if (wParam == VK_PRIOR) { ql_scroll_by(-(g_ql.panel_h - QL_TITLE_H - 60)); return 0; }
            if (wParam == VK_NEXT)  { ql_scroll_by(g_ql.panel_h - QL_TITLE_H - 60); return 0; }
            if (wParam == VK_HOME)  { ql_scroll_by(-1000000); return 0; }
            if (wParam == VK_END)   { ql_scroll_by(1000000); return 0; }
        }

        if (ctrl && wParam == 'L') { begin_addr_edit(fw); return 0; }
        if (ctrl && wParam == 'F') { SetFocus(fw->hFilterEdit); return 0; }
        if (ctrl && wParam == 'T') { add_new_tab(fw, fw->tabs[fw->active_tab].path); return 0; }
        if (ctrl && wParam == 'W') { close_tab(fw, fw->active_tab); return 0; }
        if (ctrl && wParam == 'N' && shift) { create_new_folder(fw); return 0; }
        if (ctrl && wParam == 'N') { files_open(fw->tabs[fw->active_tab].path); return 0; }
        if (ctrl && wParam == 'D') {
            int first_sel = -1;
            if (count_selected(fw, NULL, &first_sel) == 1 && first_sel >= 0 &&
                fw->entries[fw->visible[first_sel]].is_dir) {
                wchar_t full[MAX_PATH];
                build_full_entry_path(fw, &fw->entries[fw->visible[first_sel]], full, MAX_PATH);
                bookmarks_toggle_path(fw, full);
            } else {
                bookmarks_toggle_path(fw, fw->tabs[fw->active_tab].path);
            }
            return 0;
        }
        if (ctrl && wParam == 'A') {
            for (int v = 0; v < fw->visible_count; v++) fw->entries[fw->visible[v]].selected = true;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (ctrl && wParam == 'C') { copy_selection_to_clipboard(fw, false); return 0; }
        if (ctrl && wParam == 'X') { copy_selection_to_clipboard(fw, true); return 0; }
        if (ctrl && wParam == 'V') { paste_from_clipboard(fw); return 0; }
        if (ctrl && wParam == 'H') {
            fw->show_hidden = !fw->show_hidden;
            rebuild_visible(fw);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (alt && wParam == 'P') {
            fw->show_info_pane = !fw->show_info_pane;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wParam == VK_F5) { start_async_load(fw); return 0; }
        if (wParam == VK_F2 && fw->focus_vis_idx >= 0) { begin_inline_rename(fw, fw->focus_vis_idx); return 0; }
        if (wParam == VK_DELETE) { delete_selection(fw, shift); return 0; }
        if (wParam == VK_RETURN && fw->focus_vis_idx >= 0) { open_vis_item(fw, fw->focus_vis_idx); return 0; }
        if (wParam == VK_BACK || (alt && wParam == VK_UP)) {
            wchar_t up[MAX_PATH];
            wcsncpy_s(up, MAX_PATH, fw->tabs[fw->active_tab].path, _TRUNCATE);
            PathRemoveBackslashW(up);
            if (PathRemoveFileSpecW(up) && up[0]) {
                if (wcslen(up) == 2 && up[1] == L':') wcsncat_s(up, MAX_PATH, L"\\", _TRUNCATE);
                navigate_to(fw, up, true);
            }
            return 0;
        }
        if (alt && wParam == VK_LEFT && fw->tabs[fw->active_tab].hist_pos > 0) {
            FilesTab *ct = &fw->tabs[fw->active_tab];
            ct->hist_pos--;
            navigate_to(fw, ct->hist[ct->hist_pos], false);
            return 0;
        }
        if (alt && wParam == VK_RIGHT && fw->tabs[fw->active_tab].hist_pos + 1 < fw->tabs[fw->active_tab].hist_count) {
            FilesTab *ct = &fw->tabs[fw->active_tab];
            ct->hist_pos++;
            navigate_to(fw, ct->hist[ct->hist_pos], false);
            return 0;
        }
        if (wParam == VK_DOWN && fw->visible_count > 0) {
            int nxt = min(fw->visible_count - 1, fw->focus_vis_idx + 1);
            select_only(fw, nxt);
            ensure_visible(fw, nxt);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wParam == VK_UP && fw->visible_count > 0) {
            int prv = max(0, fw->focus_vis_idx - 1);
            select_only(fw, prv);
            ensure_visible(fw, prv);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_KEYUP:
        if (g_drag.active && wParam == VK_CONTROL) {
            POINT sp;
            GetCursorPos(&sp);
            drag_update(sp);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint_files(fw);
        return 0;

    case WM_DESTROY:
        fw->destroying = true;
        if (ql_is_open_for(fw)) ql_close();
        if (g_drag.active && g_drag.src == fw) drag_free();
        if (g_drag.over == fw) g_drag.over = NULL;
        /* Restore original EDIT window procs BEFORE child controls receive WM_DESTROY/WM_NCDESTROY */
        if (fw->hAddrEdit && fw->orig_addr_proc) {
            SetWindowLongPtrW(fw->hAddrEdit, GWLP_WNDPROC, (LONG_PTR)fw->orig_addr_proc);
            fw->orig_addr_proc = NULL;
        }
        if (fw->hFilterEdit && fw->orig_filter_proc) {
            SetWindowLongPtrW(fw->hFilterEdit, GWLP_WNDPROC, (LONG_PTR)fw->orig_filter_proc);
            fw->orig_filter_proc = NULL;
        }
        if (fw->hRenameEdit && fw->orig_rename_proc) {
            SetWindowLongPtrW(fw->hRenameEdit, GWLP_WNDPROC, (LONG_PTR)fw->orig_rename_proc);
            fw->orig_rename_proc = NULL;
        }
        if (fw->entries) { HeapFree(GetProcessHeap(), 0, fw->entries); fw->entries = NULL; }
        if (fw->visible) { HeapFree(GetProcessHeap(), 0, fw->visible); fw->visible = NULL; }
        for (int i = 0; i < g_win_count; i++) {
            if (g_wins[i] == fw) {
                memmove(&g_wins[i], &g_wins[i + 1], (size_t)(g_win_count - i - 1) * sizeof(FilesWindow *));
                g_win_count--;
                break;
            }
        }
        if (g_win_count == 0) {
            thumb_cache_clear();
            SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
        }
        if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_WINDOWS_CHANGED, 0, 0);
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        HeapFree(GetProcessHeap(), 0, fw);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void refresh_files_theme_brush(void) {
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
    g_hEditBrush = CreateSolidBrush(g_theme.files_addrbar);
}

void files_init(HINSTANCE hInst) {
    bookmarks_load();
    g_hEditFont = render_create_ui_font(13, false);
    refresh_files_theme_brush();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = files_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = FILES_CLASS;
    RegisterClassExW(&wc);

    WNDCLASSEXW qc;
    ZeroMemory(&qc, sizeof(qc));
    qc.cbSize = sizeof(qc);
    qc.lpfnWndProc = ql_wnd_proc;
    qc.hInstance = hInst;
    qc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    qc.lpszClassName = QL_CLASS;
    RegisterClassExW(&qc);
}

static bool paths_equal_norm(const wchar_t *a, const wchar_t *b) {
    wchar_t na[MAX_PATH], nb[MAX_PATH];
    wcsncpy_s(na, MAX_PATH, a ? a : L"", _TRUNCATE);
    wcsncpy_s(nb, MAX_PATH, b ? b : L"", _TRUNCATE);
    size_t la = wcslen(na);
    size_t lb = wcslen(nb);
    while (la > 3 && (na[la - 1] == L'\\' || na[la - 1] == L'/')) na[--la] = L'\0';
    while (lb > 3 && (nb[lb - 1] == L'\\' || nb[lb - 1] == L'/')) nb[--lb] = L'\0';
    return _wcsicmp(na, nb) == 0;
}

static void resolve_target_path(const wchar_t *raw_path, const wchar_t *select_name,
                                wchar_t *out_dir, size_t dir_len,
                                wchar_t *out_sel, size_t sel_len) {
    out_dir[0] = L'\0';
    out_sel[0] = L'\0';
    if (select_name && select_name[0]) {
        wcsncpy_s(out_sel, sel_len, select_name, _TRUNCATE);
    }
    if (!raw_path || !raw_path[0]) return;

    wchar_t clean[MAX_PATH];
    wcsncpy_s(clean, MAX_PATH, raw_path, _TRUNCATE);
    for (size_t i = 0; clean[i]; i++) {
        if (clean[i] == L'/') clean[i] = L'\\';
    }
    size_t n = wcslen(clean);
    while (n > 0 && (clean[n - 1] == L'"' || clean[n - 1] == L' ')) clean[--n] = L'\0';
    if (n == 2 && clean[1] == L':') {
        clean[2] = L'\\';
        clean[3] = L'\0';
    }

    DWORD attr = GetFileAttributesW(clean);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        const wchar_t *fn = PathFindFileNameW(clean);
        if (fn && fn[0] && !out_sel[0]) {
            wcsncpy_s(out_sel, sel_len, fn, _TRUNCATE);
        }
        PathRemoveFileSpecW(clean);
        if (wcslen(clean) == 2 && clean[1] == L':') {
            wcsncat_s(clean, MAX_PATH, L"\\", _TRUNCATE);
        }
        wcsncpy_s(out_dir, dir_len, clean, _TRUNCATE);
    } else if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wcsncpy_s(out_dir, dir_len, clean, _TRUNCATE);
    }
}

void files_open_ex(const wchar_t *initial_path, const wchar_t *select_name, bool reuse_window) {
    wchar_t dir_path[MAX_PATH];
    wchar_t sel_name[MAX_PATH];
    resolve_target_path(initial_path, select_name, dir_path, MAX_PATH, sel_name, MAX_PATH);
    const wchar_t *target_dir = dir_path[0] ? dir_path : NULL;

    if (reuse_window && g_win_count > 0 && g_wins[g_win_count - 1] && g_wins[g_win_count - 1]->hwnd) {
        FilesWindow *fw = g_wins[g_win_count - 1];
        if (sel_name[0]) {
            wcsncpy_s(fw->pending_select_name, MAX_PATH, sel_name, _TRUNCATE);
        } else {
            fw->pending_select_name[0] = L'\0';
        }

        const wchar_t *effective_dir = target_dir;
        if (!effective_dir && fw->sidebar_count > 0) effective_dir = fw->sidebar[0].path;

        int match_tab = -1;
        if (effective_dir) {
            for (int t = 0; t < fw->tab_count; t++) {
                if (paths_equal_norm(fw->tabs[t].path, effective_dir)) {
                    match_tab = t;
                    break;
                }
            }
        }

        if (match_tab >= 0) {
            fw->tabs[fw->active_tab].scroll_y = fw->scroll_y;
            fw->active_tab = match_tab;
            start_async_load(fw);
        } else if (fw->tab_count < MAX_FILES_TABS) {
            add_new_tab(fw, effective_dir);
        } else if (effective_dir) {
            navigate_to(fw, effective_dir, true);
        }

        /* A test copy never takes the focus from the user's windows. */
        if (g_cfg.isolated) return;
        if (IsIconic(fw->hwnd)) ShowWindow(fw->hwnd, SW_RESTORE);
        SetForegroundWindow(fw->hwnd);
        return;
    }

    if (g_win_count >= MAX_FILES_WINS) {
        if (g_wins[0] && g_wins[0]->hwnd && !g_cfg.isolated) {
            if (IsIconic(g_wins[0]->hwnd)) ShowWindow(g_wins[0]->hwnd, SW_RESTORE);
            SetForegroundWindow(g_wins[0]->hwnd);
        }
        return;
    }

    FilesWindow *fw = (FilesWindow *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FilesWindow));
    if (!fw) return;
    fw->show_info_pane = true;
    fw->sort_col = SORT_NAME;
    fw->sort_asc = true;
    fw->focus_vis_idx = -1;
    fw->hover_vis_idx = -1;
    fw->hover_sidebar_idx = -1;
    fw->hover_toolbar_btn = -1;
    fw->hover_crumb_idx = -1;
    fw->hover_tab_idx = -1;
    fw->hover_tab_close_idx = -1;
    fw->renaming_vis_idx = -1;
    fw->hover_divider = -1;
    fw->hover_info_btn = -1;
    fw->deferred_select_vis = -1;
    fw->deferred_toggle_vis = -1;
    fw->sidebar_w = g_last_sidebar_w;
    for (int c = 0; c < 4; c++) fw->col_w[c] = g_last_col_w[c];
    if (sel_name[0]) {
        wcsncpy_s(fw->pending_select_name, MAX_PATH, sel_name, _TRUNCATE);
    }
    build_sidebar(fw);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = min(1120, sw - 80);
    int wh = min(700, sh - 100);
    int wx = (sw - ww) / 2 + g_win_count * 26;
    int wy = (sh - wh) / 2 + g_win_count * 22;
    /* A test copy's window sits far off the screen and out of Alt+Tab. */
    if (g_cfg.isolated) wx = -30000;

    fw->hwnd = CreateWindowExW(
        g_cfg.isolated ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW,
        FILES_CLASS, L"Files",
        WS_OVERLAPPEDWINDOW,
        wx, wy, ww, wh,
        NULL, NULL, g_hInst, fw);
    if (!fw->hwnd) {
        HeapFree(GetProcessHeap(), 0, fw);
        return;
    }

    /* Windows 11 rounded corners + soft DWM drop shadow + seamless custom titlebar */
    BOOL dark = g_theme.is_dark ? TRUE : FALSE;
    DWORD corner = 2; /* DWMWCP_ROUND */
    MARGINS m = { 0, 0, 1, 0 };
    DwmSetWindowAttribute(fw->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DwmSetWindowAttribute(fw->hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    DwmExtendFrameIntoClientArea(fw->hwnd, &m);
    SetWindowPos(fw->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    /* Windows' own folder icon, for Alt+Tab and the dock */
    SHSTOCKICONINFO sii;
    ZeroMemory(&sii, sizeof(sii));
    sii.cbSize = sizeof(sii);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_LARGEICON, &sii)) && sii.hIcon) {
        SendMessageW(fw->hwnd, WM_SETICON, ICON_BIG, (LPARAM)sii.hIcon);
        SendMessageW(fw->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sii.hIcon);
    }

    fw->hAddrEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                    0, 0, 200, 18, fw->hwnd, (HMENU)(INT_PTR)ID_FILES_ADDR, g_hInst, NULL);
    fw->hFilterEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 140, 18, fw->hwnd, (HMENU)(INT_PTR)ID_FILES_FILTER, g_hInst, NULL);
    fw->hRenameEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                      0, 0, 140, 22, fw->hwnd, (HMENU)(INT_PTR)ID_FILES_RENAME, g_hInst, NULL);

    SendMessageW(fw->hAddrEdit, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);
    SendMessageW(fw->hFilterEdit, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);
    SendMessageW(fw->hRenameEdit, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);
    DragAcceptFiles(fw->hwnd, TRUE); /* files dropped from Explorer / other apps */

    fw->orig_addr_proc   = (WNDPROC)SetWindowLongPtrW(fw->hAddrEdit, GWLP_WNDPROC, (LONG_PTR)files_sub_edit_proc);
    fw->orig_filter_proc = (WNDPROC)SetWindowLongPtrW(fw->hFilterEdit, GWLP_WNDPROC, (LONG_PTR)files_sub_edit_proc);
    fw->orig_rename_proc = (WNDPROC)SetWindowLongPtrW(fw->hRenameEdit, GWLP_WNDPROC, (LONG_PTR)files_sub_edit_proc);

    g_wins[g_win_count++] = fw;
    add_new_tab(fw, target_dir);

    if (g_cfg.isolated) {
        ShowWindow(fw->hwnd, SW_SHOWNOACTIVATE);
        return;
    }
    ShowWindow(fw->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(fw->hwnd);
    if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_WINDOWS_CHANGED, 0, 0);
}

/* Test copies: the path bar leaves out the folders above ROOT, so a picture
 * never shows where the made-up folder was made. */
void files_set_crumb_root(const wchar_t *root) {
    wcsncpy_s(g_crumb_root, MAX_PATH, root ? root : L"", _TRUNCATE);
}

/* The first Files window drawn into a picture at its size, for checks. */
int files_snapshot(const wchar_t *png_path) {
    if (g_win_count < 1 || !g_wins[0] || !g_wins[0]->hwnd) return 0;
    FilesWindow *fw = g_wins[0];
    RECT cr;
    GetClientRect(fw->hwnd, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    int ok = 0;
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        draw_files(fw, &fb, cr.right, cr.bottom);
        ok = surface_save_png(&fb, png_path);
        surface_destroy(&fb);
    }
    return ok;
}

/* The first window's folder has been read (a worker thread reads it). */
bool files_loaded(void) {
    return g_win_count > 0 && g_wins[0] && !g_wins[0]->loading;
}

void files_open(const wchar_t *initial_path) {
    files_open_ex(initial_path, NULL, false);
}

void files_on_theme_changed(void) {
    refresh_files_theme_brush();
    BOOL dark = g_theme.is_dark ? TRUE : FALSE;
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i] && g_wins[i]->hwnd) {
            DwmSetWindowAttribute(g_wins[i]->hwnd, 20, &dark, sizeof(dark));
            InvalidateRect(g_wins[i]->hwnd, NULL, TRUE);
        }
    }
}

bool files_has_open_windows(void) {
    return g_win_count > 0;
}

int files_get_window_count(void) {
    return g_win_count;
}

HWND files_get_first_window(void) {
    return (g_win_count > 0 && g_wins[0]) ? g_wins[0]->hwnd : NULL;
}

void files_shutdown(void) {
    ql_close();
    drag_free();
    while (g_win_count > 0) {
        HWND h = g_wins[0]->hwnd;
        DestroyWindow(h);
    }
    thumb_cache_clear();
    if (g_hEditFont)  { DeleteObject(g_hEditFont);  g_hEditFont = NULL; }
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
}
