// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"
#pragma warning(push)
#pragma warning(disable: 4201)
#include <wincodec.h>
#pragma warning(pop)
#include <shlwapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Clipboard history (issue #148): Alt+V, or from the launcher.
 * - Windows tells the shell when the clipboard changes (AddClipboardFormatListener):
 *   no polling and no timers at rest, and only while the switch is on.
 * - Keeps up to 50 recent text clips and a few pictures, compressed as PNG, in
 *   memory, within 6 MB in all.
 * - Search, pin, delete, clear what is not pinned; Enter or a click pastes into
 *   the window that was in front. Pinned text is saved in Lares' settings folder,
 *   on this PC only (clipboard-pins.txt, or clipboard-pins-dry.txt in a dry run).
 * - Small pictures of the clips are made only while the list is open, and freed
 *   when it hides.
 */

#define CLIP_CLASS             L"LaresClipWnd"
#define ID_CLIP_SEARCH         6001
#define TIMER_CLIP_RETRY       6101
#define TIMER_CLIP_PASTE       6102

#define CLIP_WND_W             472
#define CLIP_WND_H             544
#define CLIP_MAX_TEXT          50
#define CLIP_MAX_IMAGES        10
#define CLIP_MAX_SLOTS         64
#define CLIP_BUDGET_BYTES      (6U * 1024U * 1024U)
#define CLIP_MAX_TEXT_CHARS    (128U * 1024U)     /* 256 KB max single text clip */
#define CLIP_MAX_IMAGE_BYTES   (1536U * 1024U)    /* 1.5 MB max compressed PNG per image clip */
#define CLIP_THUMB_W           68
#define CLIP_THUMB_H           44

typedef enum {
    CLIP_KIND_TEXT  = 0,
    CLIP_KIND_IMAGE = 1
} ClipKind;

typedef struct {
    ClipKind   kind;
    bool       pinned;
    uint64_t   hash;
    SYSTEMTIME captured_at;

    wchar_t   *text;
    size_t     text_chars;
    int        line_count;
    wchar_t    preview[160];

    BYTE      *png_data;
    size_t     png_size;
    int        img_w;
    int        img_h;

    size_t     mem_bytes;

    /* Popup-only preview thumbnail (allocated on demand while visible, freed on hide) */
    GpBitmap  *thumb_bmp;
    BYTE      *thumb_pixels;
    int        thumb_w;
    int        thumb_h;
} ClipItem;

static HWND     g_hClipWnd = NULL;
static HWND     g_hSearchEdit = NULL;
static WNDPROC  g_orig_edit_proc = NULL;
static HFONT    g_hEditFont = NULL;
static HBRUSH   g_hEditBrush = NULL;
static bool     g_listener_added = false;
static bool     g_destroying = false;

static ClipItem g_clips[CLIP_MAX_SLOTS];
static int      g_clip_count = 0;
static size_t   g_total_bytes = 0;
static DWORD    g_last_seq = 0;
static int      g_retry_left = 0;

/* Popup display & interaction state */
static int      g_vis[CLIP_MAX_SLOTS];
static int      g_vis_count = 0;
static wchar_t  g_query[128] = {0};
static int      g_sel_idx = 0;
static int      g_hover_idx = -1;
static int      g_hover_action = 0;   /* 1=pin, 2=delete */
static bool     g_hover_clear = false;
static bool     g_tracking = false;
static int      g_scroll_y = 0;
static int      g_content_h = 0;
static HWND     g_prev_fg_wnd = NULL;
static bool     g_pending_paste = false;

/* Cached layout rects */
static RECT     g_rc_clear_btn = {0};
static RECT     g_rc_search = {0};
static RECT     g_rc_list = {0};

static ARGB cl_argb(BYTE a, COLORREF c) {
    return MAKE_ARGB(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static uint64_t fnv1a_64(const void *data, size_t len) {
    const BYTE *p = (const BYTE *)data;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void clip_free_thumb(ClipItem *it) {
    if (!it) return;
    if (it->thumb_bmp) {
        GdipDisposeImage((GpImage *)it->thumb_bmp);
        it->thumb_bmp = NULL;
    }
    if (it->thumb_pixels) {
        HeapFree(GetProcessHeap(), 0, it->thumb_pixels);
        it->thumb_pixels = NULL;
    }
    it->thumb_w = 0;
    it->thumb_h = 0;
}

static void clip_free_all_thumbs(void) {
    for (int i = 0; i < g_clip_count; i++) {
        clip_free_thumb(&g_clips[i]);
    }
}

static void clip_free_item(ClipItem *it) {
    if (!it) return;
    clip_free_thumb(it);
    if (it->text) {
        HeapFree(GetProcessHeap(), 0, it->text);
        it->text = NULL;
    }
    if (it->png_data) {
        HeapFree(GetProcessHeap(), 0, it->png_data);
        it->png_data = NULL;
    }
    if (g_total_bytes >= it->mem_bytes) g_total_bytes -= it->mem_bytes;
    else g_total_bytes = 0;
    ZeroMemory(it, sizeof(*it));
}

static void clip_remove_at(int idx) {
    if (idx < 0 || idx >= g_clip_count) return;
    clip_free_item(&g_clips[idx]);
    if (idx + 1 < g_clip_count) {
        memmove(&g_clips[idx], &g_clips[idx + 1],
                (size_t)(g_clip_count - idx - 1) * sizeof(ClipItem));
    }
    g_clip_count--;
    ZeroMemory(&g_clips[g_clip_count], sizeof(ClipItem));
}

static void clip_build_text_preview(const wchar_t *src, size_t chars, wchar_t *out, size_t out_len, int *out_lines) {
    int lines = 1;
    for (size_t i = 0; i < chars; i++) {
        if (src[i] == L'\n') lines++;
    }
    if (out_lines) *out_lines = lines;

    size_t di = 0;
    bool prev_sp = true;
    for (size_t i = 0; i < chars && di + 1 < out_len; i++) {
        wchar_t c = src[i];
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        if (c < 0x20) continue;
        if (c == L' ') {
            if (prev_sp) continue;
            prev_sp = true;
        } else {
            prev_sp = false;
        }
        out[di++] = c;
    }
    while (di > 0 && out[di - 1] == L' ') di--;
    out[di] = L'\0';
    if (di == 0) {
        wcsncpy_s(out, out_len, L"(Whitespace)", _TRUNCATE);
    }
}

static int count_clips_of_kind(ClipKind k) {
    int n = 0;
    for (int i = 0; i < g_clip_count; i++) {
        if (g_clips[i].kind == k) n++;
    }
    return n;
}

static size_t sum_pinned_bytes(void) {
    size_t tot = 0;
    for (int i = 0; i < g_clip_count; i++) {
        if (g_clips[i].pinned) tot += g_clips[i].mem_bytes;
    }
    return tot;
}

static int find_oldest_unpinned(int kind_filter) {
    /* Clips are stored newest-first (0 = newest), so scan backward */
    for (int i = g_clip_count - 1; i >= 0; i--) {
        if (g_clips[i].pinned) continue;
        if (kind_filter >= 0 && (int)g_clips[i].kind != kind_filter) continue;
        return i;
    }
    return -1;
}

static void clip_enforce_budget(size_t incoming_bytes, ClipKind incoming_kind) {
    /* Keep room in slot array */
    while (g_clip_count >= CLIP_MAX_SLOTS) {
        int ev = find_oldest_unpinned(-1);
        if (ev < 0) break;
        clip_remove_at(ev);
    }

    /* Enforce per-kind caps */
    if (incoming_kind == CLIP_KIND_TEXT) {
        while (count_clips_of_kind(CLIP_KIND_TEXT) >= CLIP_MAX_TEXT) {
            int ev = find_oldest_unpinned(CLIP_KIND_TEXT);
            if (ev < 0) break;
            clip_remove_at(ev);
        }
    } else {
        while (count_clips_of_kind(CLIP_KIND_IMAGE) >= CLIP_MAX_IMAGES) {
            int ev = find_oldest_unpinned(CLIP_KIND_IMAGE);
            if (ev < 0) break;
            clip_remove_at(ev);
        }
    }

    /* Enforce strict 6 MB total memory cap */
    while (g_total_bytes + incoming_bytes > CLIP_BUDGET_BYTES) {
        int ev = find_oldest_unpinned(-1);
        if (ev < 0) break;
        clip_remove_at(ev);
    }
}

/* =========================================================================
 * Pinned clips, in Lares' settings folder
 * ========================================================================= */
static void clip_pins_path(wchar_t *out, size_t len) {
    wchar_t dir[MAX_PATH];
    if (!lares_shell_settings_dir(dir, MAX_PATH)) {
        out[0] = L'\0';
        return;
    }
    CreateDirectoryW(dir, NULL);
    _snwprintf_s(out, len, _TRUNCATE, L"%ls\\%ls", dir,
                 lares_shell_dry_run() ? L"clipboard-pins-dry.txt" : L"clipboard-pins.txt");
}

static void clip_save_pins(void) {
    wchar_t path[MAX_PATH];
    clip_pins_path(path, MAX_PATH);
    if (!path[0] || g_cfg.isolated) return;
    FILE *fp = NULL;
    if (_wfopen_s(&fp, path, L"wt, ccs=UTF-8") != 0 || !fp) return;

    fwprintf(fp, L"# Lares clipboard history: pinned clips\n\n");
    int pi = 0;
    for (int i = 0; i < g_clip_count; i++) {
        const ClipItem *it = &g_clips[i];
        if (!it->pinned || it->kind != CLIP_KIND_TEXT || !it->text) continue;
        fwprintf(fp, L"[pin_%d]\ntext=", pi++);
        for (size_t c = 0; c < it->text_chars; c++) {
            wchar_t ch = it->text[c];
            if (ch == L'\\')      fputws(L"\\\\", fp);
            else if (ch == L'\r') fputws(L"\\r", fp);
            else if (ch == L'\n') fputws(L"\\n", fp);
            else if (ch == L'\t') fputws(L"\\t", fp);
            else                  fputwc(ch, fp);
        }
        fputws(L"\n\n", fp);
    }
    fclose(fp);
}

static bool clip_add_text_internal(const wchar_t *src, size_t chars, bool pinned, bool save_if_pinned);

static void clip_load_pins(void) {
    wchar_t path[MAX_PATH];
    clip_pins_path(path, MAX_PATH);
    if (!path[0]) return;
    FILE *fp = NULL;
    if (_wfopen_s(&fp, path, L"rt, ccs=UTF-8") != 0 || !fp) return;

    size_t line_cap = 4096;
    wchar_t *line = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, line_cap * sizeof(wchar_t));
    if (!line) { fclose(fp); return; }

    /* Collect pins in file order first, then insert in reverse so pin_0 ends up first */
    wchar_t *loaded[CLIP_MAX_TEXT];
    size_t   loaded_len[CLIP_MAX_TEXT];
    int      loaded_cnt = 0;

    for (;;) {
        size_t len = 0;
        wint_t wc = WEOF;
        while ((wc = fgetwc(fp)) != WEOF) {
            if (wc == L'\r') continue;
            if (wc == L'\n') break;
            if (len + 2 >= line_cap) {
                if (line_cap >= CLIP_MAX_TEXT_CHARS * 2) continue;
                size_t ncap = line_cap * 2;
                wchar_t *nl = (wchar_t *)HeapReAlloc(GetProcessHeap(), 0, line, ncap * sizeof(wchar_t));
                if (!nl) break;
                line = nl;
                line_cap = ncap;
            }
            line[len++] = (wchar_t)wc;
        }
        line[len] = L'\0';
        if (len == 0 && wc == WEOF) break;

        if (wcsncmp(line, L"text=", 5) == 0 && loaded_cnt < CLIP_MAX_TEXT) {
            const wchar_t *s = line + 5;
            size_t slen = wcslen(s);
            wchar_t *dec = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (slen + 1) * sizeof(wchar_t));
            if (dec) {
                size_t di = 0;
                for (size_t i = 0; i < slen; i++) {
                    if (s[i] == L'\\' && i + 1 < slen) {
                        i++;
                        if (s[i] == L'n')      dec[di++] = L'\n';
                        else if (s[i] == L'r') dec[di++] = L'\r';
                        else if (s[i] == L't') dec[di++] = L'\t';
                        else if (s[i] == L'\\') dec[di++] = L'\\';
                        else                   dec[di++] = s[i];
                    } else {
                        dec[di++] = s[i];
                    }
                }
                dec[di] = L'\0';
                if (di > 0) {
                    loaded[loaded_cnt] = dec;
                    loaded_len[loaded_cnt] = di;
                    loaded_cnt++;
                } else {
                    HeapFree(GetProcessHeap(), 0, dec);
                }
            }
        }
        if (wc == WEOF) break;
    }

    for (int i = loaded_cnt - 1; i >= 0; i--) {
        clip_add_text_internal(loaded[i], loaded_len[i], true, false);
        HeapFree(GetProcessHeap(), 0, loaded[i]);
    }

    HeapFree(GetProcessHeap(), 0, line);
    fclose(fp);
}

/* =========================================================================
 * Add Text & Bounded PNG-Compressed Image Clips
 * ========================================================================= */
static bool clip_add_text_internal(const wchar_t *src, size_t chars, bool pinned, bool save_if_pinned) {
    if (!src || chars == 0) return false;
    if (chars > CLIP_MAX_TEXT_CHARS) chars = CLIP_MAX_TEXT_CHARS;

    uint64_t h = fnv1a_64(src, chars * sizeof(wchar_t));

    /* Deduplicate existing clip and promote to top */
    for (int i = 0; i < g_clip_count; i++) {
        if (g_clips[i].kind == CLIP_KIND_TEXT && g_clips[i].hash == h &&
            g_clips[i].text_chars == chars && g_clips[i].text &&
            wmemcmp(g_clips[i].text, src, chars) == 0) {
            ClipItem moved = g_clips[i];
            if (pinned) moved.pinned = true;
            GetLocalTime(&moved.captured_at);
            if (i > 0) {
                memmove(&g_clips[1], &g_clips[0], (size_t)i * sizeof(ClipItem));
                g_clips[0] = moved;
            } else {
                g_clips[0] = moved;
            }
            if (save_if_pinned && g_clips[0].pinned) clip_save_pins();
            return true;
        }
    }

    size_t need = (chars + 1) * sizeof(wchar_t);
    if (sum_pinned_bytes() + need > CLIP_BUDGET_BYTES) return false;

    clip_enforce_budget(need, CLIP_KIND_TEXT);
    if (g_clip_count >= CLIP_MAX_SLOTS || g_total_bytes + need > CLIP_BUDGET_BYTES) return false;

    wchar_t *copy = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, need);
    if (!copy) return false;
    memcpy(copy, src, chars * sizeof(wchar_t));
    copy[chars] = L'\0';

    if (g_clip_count > 0) {
        memmove(&g_clips[1], &g_clips[0], (size_t)g_clip_count * sizeof(ClipItem));
    }
    g_clip_count++;

    ClipItem *it = &g_clips[0];
    ZeroMemory(it, sizeof(*it));
    it->kind = CLIP_KIND_TEXT;
    it->pinned = pinned;
    it->hash = h;
    GetLocalTime(&it->captured_at);
    it->text = copy;
    it->text_chars = chars;
    it->mem_bytes = need;
    clip_build_text_preview(copy, chars, it->preview, 160, &it->line_count);
    g_total_bytes += need;

    if (save_if_pinned && pinned) clip_save_pins();
    return true;
}

static bool wic_encode_bgra_to_png(const BYTE *bgra, int w, int h, int max_edge,
                                   BYTE **out_png, size_t *out_size) {
    *out_png = NULL;
    *out_size = 0;

    IWICImagingFactory *fac = NULL;
    IWICBitmap *wbmp = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IStream *stm = NULL;
    bool ok = false;

    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&fac);
    if (FAILED(hr) || !fac) goto done;

    hr = IWICImagingFactory_CreateBitmapFromMemory(
        fac, (UINT)w, (UINT)h, &GUID_WICPixelFormat32bppBGRA,
        (UINT)(w * 4), (UINT)((size_t)w * (size_t)h * 4), (BYTE *)bgra, &wbmp);
    if (FAILED(hr) || !wbmp) goto done;

    UINT dw = (UINT)w, dh = (UINT)h;
    IWICBitmapSource *src = (IWICBitmapSource *)wbmp;
    if (max_edge > 0 && ((int)dw > max_edge || (int)dh > max_edge)) {
        if (dw >= dh) {
            dh = (UINT)max(1ULL, ((uint64_t)dh * (uint64_t)max_edge) / dw);
            dw = (UINT)max_edge;
        } else {
            dw = (UINT)max(1ULL, ((uint64_t)dw * (uint64_t)max_edge) / dh);
            dh = (UINT)max_edge;
        }
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(fac, &scaler)) && scaler) {
            if (SUCCEEDED(IWICBitmapScaler_Initialize(scaler, src, dw, dh, WICBitmapInterpolationModeFant))) {
                src = (IWICBitmapSource *)scaler;
            }
        }
    }

    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stm)) || !stm) goto done;
    if (FAILED(IWICImagingFactory_CreateEncoder(fac, &GUID_ContainerFormatPng, NULL, &enc)) || !enc) goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, stm, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, NULL)) || !frame) goto done;
    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, NULL))) goto done;
    if (FAILED(IWICBitmapFrameEncode_SetSize(frame, dw, dh))) goto done;

    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &pf))) goto done;
    if (FAILED(IWICBitmapFrameEncode_WriteSource(frame, src, NULL))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame))) goto done;
    if (FAILED(IWICBitmapEncoder_Commit(enc))) goto done;

    HGLOBAL hMem = NULL;
    if (SUCCEEDED(GetHGlobalFromStream(stm, &hMem)) && hMem) {
        STATSTG st;
        ZeroMemory(&st, sizeof(st));
        if (SUCCEEDED(IStream_Stat(stm, &st, STATFLAG_NONAME)) && st.cbSize.QuadPart > 0 &&
            st.cbSize.QuadPart <= (ULONGLONG)CLIP_BUDGET_BYTES) {
            size_t sz = (size_t)st.cbSize.QuadPart;
            void *p = GlobalLock(hMem);
            if (p) {
                BYTE *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, sz);
                if (buf) {
                    memcpy(buf, p, sz);
                    *out_png = buf;
                    *out_size = sz;
                    ok = true;
                }
                GlobalUnlock(hMem);
            }
        }
    }

done:
    if (frame)  IWICBitmapFrameEncode_Release(frame);
    if (enc)    IWICBitmapEncoder_Release(enc);
    if (stm)    IStream_Release(stm);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (wbmp)   IWICBitmap_Release(wbmp);
    if (fac)    IWICImagingFactory_Release(fac);
    return ok;
}

static bool clip_add_image_bgra(const BYTE *bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return false;

    BYTE *png = NULL;
    size_t png_sz = 0;
    if (!wic_encode_bgra_to_png(bgra, w, h, 1600, &png, &png_sz)) return false;

    /* If a noisy wallpaper PNG is still > 1.5 MB, bound to 1024px so budget stays healthy */
    if (png_sz > CLIP_MAX_IMAGE_BYTES) {
        HeapFree(GetProcessHeap(), 0, png);
        png = NULL;
        png_sz = 0;
        if (!wic_encode_bgra_to_png(bgra, w, h, 1024, &png, &png_sz) || png_sz > CLIP_MAX_IMAGE_BYTES) {
            if (png) HeapFree(GetProcessHeap(), 0, png);
            return false;
        }
    }

    uint64_t hash = fnv1a_64(png, png_sz);

    /* Deduplicate identical image */
    for (int i = 0; i < g_clip_count; i++) {
        if (g_clips[i].kind == CLIP_KIND_IMAGE && g_clips[i].hash == hash &&
            g_clips[i].png_size == png_sz && g_clips[i].png_data &&
            memcmp(g_clips[i].png_data, png, png_sz) == 0) {
            HeapFree(GetProcessHeap(), 0, png);
            ClipItem moved = g_clips[i];
            GetLocalTime(&moved.captured_at);
            if (i > 0) {
                memmove(&g_clips[1], &g_clips[0], (size_t)i * sizeof(ClipItem));
                g_clips[0] = moved;
            } else {
                g_clips[0] = moved;
            }
            return true;
        }
    }

    if (sum_pinned_bytes() + png_sz > CLIP_BUDGET_BYTES) {
        HeapFree(GetProcessHeap(), 0, png);
        return false;
    }

    clip_enforce_budget(png_sz, CLIP_KIND_IMAGE);
    if (g_clip_count >= CLIP_MAX_SLOTS || g_total_bytes + png_sz > CLIP_BUDGET_BYTES) {
        HeapFree(GetProcessHeap(), 0, png);
        return false;
    }

    if (g_clip_count > 0) {
        memmove(&g_clips[1], &g_clips[0], (size_t)g_clip_count * sizeof(ClipItem));
    }
    g_clip_count++;

    ClipItem *it = &g_clips[0];
    ZeroMemory(it, sizeof(*it));
    it->kind = CLIP_KIND_IMAGE;
    it->pinned = false;
    it->hash = hash;
    GetLocalTime(&it->captured_at);
    it->png_data = png;
    it->png_size = png_sz;
    it->img_w = w;
    it->img_h = h;
    it->mem_bytes = png_sz;
    _snwprintf_s(it->preview, 160, _TRUNCATE, L"Image (%d x %d)", w, h);
    g_total_bytes += png_sz;
    return true;
}

static bool clip_ensure_thumb(ClipItem *it) {
    if (!it || it->kind != CLIP_KIND_IMAGE || !it->png_data || it->png_size == 0) return false;
    if (it->thumb_bmp) return true;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, it->png_size);
    if (!hMem) return false;
    void *p = GlobalLock(hMem);
    if (!p) { GlobalFree(hMem); return false; }
    memcpy(p, it->png_data, it->png_size);
    GlobalUnlock(hMem);

    IStream *stm = NULL;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stm)) || !stm) {
        GlobalFree(hMem);
        return false;
    }

    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *conv = NULL;
    bool ok = false;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&fac)) || !fac) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(fac, stm, NULL,
                                                          WICDecodeMetadataCacheOnDemand, &dec)) || !dec) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) || !frame) goto done;

    UINT ow = 0, oh = 0;
    if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &ow, &oh)) || ow == 0 || oh == 0) goto done;

    float sx = (float)CLIP_THUMB_W / (float)ow;
    float sy = (float)CLIP_THUMB_H / (float)oh;
    float s = (sx < sy) ? sx : sy;
    if (s > 1.0f) s = 1.0f;
    UINT tw = (UINT)max(1, (int)((float)ow * s + 0.5f));
    UINT th = (UINT)max(1, (int)((float)oh * s + 0.5f));

    IWICBitmapSource *src = (IWICBitmapSource *)frame;
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(fac, &scaler)) && scaler) {
        if (SUCCEEDED(IWICBitmapScaler_Initialize(scaler, src, tw, th, WICBitmapInterpolationModeFant))) {
            src = (IWICBitmapSource *)scaler;
        }
    }
    if (FAILED(IWICImagingFactory_CreateFormatConverter(fac, &conv)) || !conv) goto done;
    if (FAILED(IWICFormatConverter_Initialize(conv, src, &GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeMedianCut))) goto done;

    UINT stride = tw * 4;
    size_t bytes = (size_t)stride * (size_t)th;
    BYTE *pix = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!pix) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, stride, (UINT)bytes, pix))) {
        HeapFree(GetProcessHeap(), 0, pix);
        goto done;
    }

    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromScan0((INT)tw, (INT)th, (INT)stride, GP_PIXELFORMAT_32BPP_PARGB, pix, &bmp) != GP_OK || !bmp) {
        HeapFree(GetProcessHeap(), 0, pix);
        goto done;
    }

    it->thumb_bmp = bmp;
    it->thumb_pixels = pix;
    it->thumb_w = (int)tw;
    it->thumb_h = (int)th;
    ok = true;

done:
    if (conv)   IWICFormatConverter_Release(conv);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (frame)  IWICBitmapFrameDecode_Release(frame);
    if (dec)    IWICBitmapDecoder_Release(dec);
    if (fac)    IWICImagingFactory_Release(fac);
    if (stm)    IStream_Release(stm);
    return ok;
}

static bool clip_capture_now(HWND hwnd) {
    if (!OpenClipboard(hwnd)) return false;

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        wchar_t *tmp = NULL;
        size_t chars = 0;
        if (hData) {
            const wchar_t *src = (const wchar_t *)GlobalLock(hData);
            if (src) {
                chars = wcsnlen(src, CLIP_MAX_TEXT_CHARS);
                if (chars > 0) {
                    tmp = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (chars + 1) * sizeof(wchar_t));
                    if (tmp) {
                        memcpy(tmp, src, chars * sizeof(wchar_t));
                        tmp[chars] = L'\0';
                    }
                }
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
        if (tmp) {
            clip_add_text_internal(tmp, chars, false, false);
            HeapFree(GetProcessHeap(), 0, tmp);
        }
        return true;
    }

    if (IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB) ||
        IsClipboardFormatAvailable(CF_DIBV5)) {
        HBITMAP hbm = (HBITMAP)GetClipboardData(CF_BITMAP);
        BYTE *pixels = NULL;
        int w = 0, h = 0;
        if (hbm) {
            BITMAP bm;
            ZeroMemory(&bm, sizeof(bm));
            if (GetObjectW(hbm, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight != 0) {
                w = bm.bmWidth;
                h = (bm.bmHeight < 0) ? -bm.bmHeight : bm.bmHeight;
                if (w <= 8192 && h <= 8192) {
                    SIZE_T sz = (SIZE_T)w * (SIZE_T)h * 4;
                    pixels = (BYTE *)VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                    if (pixels) {
                        BITMAPINFO bmi;
                        ZeroMemory(&bmi, sizeof(bmi));
                        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth = w;
                        bmi.bmiHeader.biHeight = -h;
                        bmi.bmiHeader.biPlanes = 1;
                        bmi.bmiHeader.biBitCount = 32;
                        bmi.bmiHeader.biCompression = BI_RGB;

                        HDC sdc = GetDC(NULL);
                        int got = GetDIBits(sdc, hbm, 0, (UINT)h, pixels, &bmi, DIB_RGB_COLORS);
                        ReleaseDC(NULL, sdc);
                        if (got <= 0) {
                            VirtualFree(pixels, 0, MEM_RELEASE);
                            pixels = NULL;
                        }
                    }
                }
            }
        }
        CloseClipboard();

        if (pixels) {
            /* Ensure opaque 24/32bpp DDBs (which leave alpha byte 0) encode cleanly */
            size_t npx = (size_t)w * (size_t)h;
            bool any_alpha = false;
            for (size_t i = 0; i < npx; i++) {
                if (pixels[i * 4 + 3] != 0) { any_alpha = true; break; }
            }
            if (!any_alpha) {
                for (size_t i = 0; i < npx; i++) pixels[i * 4 + 3] = 255;
            }
            clip_add_image_bgra(pixels, w, h);
            VirtualFree(pixels, 0, MEM_RELEASE);
        }
        return true;
    }

    CloseClipboard();
    return true;
}

/* =========================================================================
 * Filtered View, Paste Back, and Actions
 * ========================================================================= */
static void clip_rebuild_visible(void) {
    g_vis_count = 0;
    /* Pinned clips first, then unpinned clips newest-first */
    for (int pass = 0; pass < 2; pass++) {
        bool want_pin = (pass == 0);
        for (int i = 0; i < g_clip_count; i++) {
            const ClipItem *it = &g_clips[i];
            if (it->pinned != want_pin) continue;
            if (g_query[0]) {
                if (it->kind == CLIP_KIND_TEXT) {
                    if (!it->text || StrStrIW(it->text, g_query) == NULL) continue;
                } else {
                    if (StrStrIW(it->preview, g_query) == NULL &&
                        StrStrIW(L"image screenshot picture png", g_query) == NULL) continue;
                }
            }
            g_vis[g_vis_count++] = i;
        }
    }
    if (g_sel_idx >= g_vis_count) g_sel_idx = max(0, g_vis_count - 1);
    if (g_sel_idx < 0) g_sel_idx = 0;
}

static bool clip_write_to_clipboard(const ClipItem *it) {
    if (!it) return false;
    if (it->kind == CLIP_KIND_TEXT) {
        if (!it->text || it->text_chars == 0) return false;
        size_t bytes = (it->text_chars + 1) * sizeof(wchar_t);
        HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!hm) return false;
        void *dst = GlobalLock(hm);
        if (!dst) { GlobalFree(hm); return false; }
        memcpy(dst, it->text, bytes);
        GlobalUnlock(hm);

        if (!OpenClipboard(g_hClipWnd)) { GlobalFree(hm); return false; }
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, hm)) {
            GlobalFree(hm);
            CloseClipboard();
            return false;
        }
        CloseClipboard();
        g_last_seq = GetClipboardSequenceNumber();
        return true;
    }

    /* Decode PNG back to bottom-up 32bpp CF_DIB for maximum paste compatibility */
    if (!it->png_data || it->png_size == 0) return false;
    HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, it->png_size);
    if (!hPng) return false;
    void *pp = GlobalLock(hPng);
    if (!pp) { GlobalFree(hPng); return false; }
    memcpy(pp, it->png_data, it->png_size);
    GlobalUnlock(hPng);

    IStream *stm = NULL;
    if (FAILED(CreateStreamOnHGlobal(hPng, TRUE, &stm)) || !stm) {
        GlobalFree(hPng);
        return false;
    }

    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    BYTE *top_pix = NULL;
    bool ok = false;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&fac)) || !fac) goto img_done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(fac, stm, NULL,
                                                          WICDecodeMetadataCacheOnDemand, &dec)) || !dec) goto img_done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) || !frame) goto img_done;

    UINT dw = 0, dh = 0;
    if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &dw, &dh)) || dw == 0 || dh == 0) goto img_done;

    if (FAILED(IWICImagingFactory_CreateFormatConverter(fac, &conv)) || !conv) goto img_done;
    if (FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                                              &GUID_WICPixelFormat32bppBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeMedianCut))) goto img_done;

    UINT stride = dw * 4;
    SIZE_T img_sz = (SIZE_T)stride * (SIZE_T)dh;
    top_pix = (BYTE *)VirtualAlloc(NULL, img_sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!top_pix) goto img_done;
    if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, stride, (UINT)img_sz, top_pix))) goto img_done;

    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + img_sz);
    if (!hDib) goto img_done;
    BYTE *dst = (BYTE *)GlobalLock(hDib);
    if (!dst) { GlobalFree(hDib); goto img_done; }

    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)dst;
    ZeroMemory(bih, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = (LONG)dw;
    bih->biHeight = (LONG)dh; /* bottom-up standard CF_DIB */
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)img_sz;

    BYTE *bits = dst + sizeof(BITMAPINFOHEADER);
    for (UINT y = 0; y < dh; y++) {
        memcpy(bits + (size_t)y * stride, top_pix + (size_t)(dh - 1 - y) * stride, stride);
    }
    GlobalUnlock(hDib);

    if (OpenClipboard(g_hClipWnd)) {
        EmptyClipboard();
        if (SetClipboardData(CF_DIB, hDib)) {
            ok = true;
        } else {
            GlobalFree(hDib);
        }
        CloseClipboard();
        if (ok) g_last_seq = GetClipboardSequenceNumber();
    } else {
        GlobalFree(hDib);
    }

img_done:
    if (top_pix) VirtualFree(top_pix, 0, MEM_RELEASE);
    if (conv)    IWICFormatConverter_Release(conv);
    if (frame)   IWICBitmapFrameDecode_Release(frame);
    if (dec)     IWICBitmapDecoder_Release(dec);
    if (fac)     IWICImagingFactory_Release(fac);
    if (stm)     IStream_Release(stm);
    return ok;
}

static void clip_send_ctrl_v(void) {
    /* Ensure Alt is not stuck down if user quickly pasted after Alt+V */
    INPUT inp[6];
    ZeroMemory(inp, sizeof(inp));
    int n = 0;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) {
        inp[n].type = INPUT_KEYBOARD;
        inp[n].ki.wVk = VK_MENU;
        inp[n].ki.dwFlags = KEYEVENTF_KEYUP;
        n++;
    }
    inp[n].type = INPUT_KEYBOARD; inp[n].ki.wVk = VK_CONTROL; n++;
    inp[n].type = INPUT_KEYBOARD; inp[n].ki.wVk = 'V'; n++;
    inp[n].type = INPUT_KEYBOARD; inp[n].ki.wVk = 'V'; inp[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    inp[n].type = INPUT_KEYBOARD; inp[n].ki.wVk = VK_CONTROL; inp[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    SendInput((UINT)n, inp, sizeof(INPUT));
}

static void clip_paste_vis_idx(int vis_idx) {
    if (vis_idx < 0 || vis_idx >= g_vis_count) return;
    int ci = g_vis[vis_idx];
    if (ci < 0 || ci >= g_clip_count) return;

    if (!clip_write_to_clipboard(&g_clips[ci])) return;

    HWND target = g_prev_fg_wnd;
    clipboard_hide();

    if (target && IsWindow(target) && target != g_hClipWnd) {
        if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
        SetForegroundWindow(target);
        g_pending_paste = true;
        SetTimer(g_hClipWnd, TIMER_CLIP_PASTE, 70, NULL);
    }
}

static void clip_toggle_pin_vis(int vis_idx) {
    if (vis_idx < 0 || vis_idx >= g_vis_count) return;
    int ci = g_vis[vis_idx];
    if (ci < 0 || ci >= g_clip_count) return;

    g_clips[ci].pinned = !g_clips[ci].pinned;
    if (g_clips[ci].kind == CLIP_KIND_TEXT) {
        clip_save_pins();
    }
    clip_rebuild_visible();
    if (g_hClipWnd) InvalidateRect(g_hClipWnd, NULL, FALSE);
}

static void clip_delete_vis(int vis_idx) {
    if (vis_idx < 0 || vis_idx >= g_vis_count) return;
    int ci = g_vis[vis_idx];
    if (ci < 0 || ci >= g_clip_count) return;

    bool was_pinned_text = (g_clips[ci].pinned && g_clips[ci].kind == CLIP_KIND_TEXT);
    clip_remove_at(ci);
    if (was_pinned_text) clip_save_pins();
    clip_rebuild_visible();
    if (g_hClipWnd) InvalidateRect(g_hClipWnd, NULL, FALSE);
}

static void clip_clear_unpinned(void) {
    for (int i = g_clip_count - 1; i >= 0; i--) {
        if (!g_clips[i].pinned) {
            clip_remove_at(i);
        }
    }
    g_scroll_y = 0;
    g_sel_idx = 0;
    clip_rebuild_visible();
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
    if (g_hClipWnd) InvalidateRect(g_hClipWnd, NULL, FALSE);
}

/* =========================================================================
 * Layout & Pure-CPU DirectWrite / GDI+ Rendering
 * ========================================================================= */
#define CLIP_CARD_H    58
#define CLIP_CARD_GAP  6
#define CLIP_ROW_STEP  (CLIP_CARD_H + CLIP_CARD_GAP)

static void clip_update_layout(int w, int h) {
    g_rc_clear_btn.left = w - 122;
    g_rc_clear_btn.top = 10;
    g_rc_clear_btn.right = w - 16;
    g_rc_clear_btn.bottom = 36;

    g_rc_search.left = 16;
    g_rc_search.top = 46;
    g_rc_search.right = w - 16;
    g_rc_search.bottom = 80;

    if (g_hSearchEdit) {
        MoveWindow(g_hSearchEdit, g_rc_search.left + 30, g_rc_search.top + 8,
                   (g_rc_search.right - g_rc_search.left) - 42, 18, TRUE);
    }

    g_rc_list.left = 16;
    g_rc_list.top = 90;
    g_rc_list.right = w - 16;
    g_rc_list.bottom = h - 30;

    g_content_h = g_vis_count * CLIP_ROW_STEP + 4;
    int view_h = g_rc_list.bottom - g_rc_list.top;
    int max_s = max(0, g_content_h - view_h);
    if (g_scroll_y > max_s) g_scroll_y = max_s;
    if (g_scroll_y < 0) g_scroll_y = 0;
}

static void clip_ensure_sel_visible(void) {
    int view_h = g_rc_list.bottom - g_rc_list.top;
    if (view_h <= 0) return;
    int top_y = g_sel_idx * CLIP_ROW_STEP;
    int bot_y = top_y + CLIP_CARD_H;
    if (top_y < g_scroll_y) g_scroll_y = top_y;
    else if (bot_y > g_scroll_y + view_h) g_scroll_y = bot_y - view_h;
    int max_s = max(0, g_content_h - view_h);
    if (g_scroll_y > max_s) g_scroll_y = max_s;
    if (g_scroll_y < 0) g_scroll_y = 0;
}

static void clip_card_action_rects(int card_y, RECT *rc_pin, RECT *rc_del) {
    int right = g_rc_list.right - 10;
    rc_del->left   = right - 26;
    rc_del->top    = card_y + 16;
    rc_del->right  = right;
    rc_del->bottom = card_y + 42;

    rc_pin->left   = rc_del->left - 30;
    rc_pin->top    = card_y + 16;
    rc_pin->right  = rc_del->left - 4;
    rc_pin->bottom = card_y + 42;
}

static void clip_draw(LayerSurface *fb, int w, int h) {
    clip_update_layout(w, h);
    {
        GpGraphics *g = fb->g;
        GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
        GdipSetTextRenderingHint(g, GP_TEXT_ANTIALIAS);
        GdipSetInterpolationMode(g, GP_INTERP_HIGHQUALITY_BICUBIC);

        ARGB c_bg    = cl_argb(255, g_theme.files_bg);
        ARGB c_tb    = cl_argb(255, g_theme.files_toolbar);
        ARGB c_card  = cl_argb(255, g_theme.files_alt);
        ARGB c_addr  = cl_argb(255, g_theme.files_addrbar);
        ARGB c_bdr   = cl_argb(255, g_theme.files_border);
        ARGB c_txt   = cl_argb(255, g_theme.files_text);
        ARGB c_muted = cl_argb(255, g_theme.files_text_muted);
        ARGB c_hov   = cl_argb(255, g_theme.files_hover_bg);

        /* Outer surface */
        draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, (float)h, 0.0f, c_bg);
        draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, 86.0f, 0.0f, c_tb);
        draw_line(g, 0.0f, 85.5f, (float)w, 85.5f, c_bdr, 1.0f);

        /* Header: icon + title + count/budget readout + Clear Unpinned button */
        draw_icon_glyph(g, 0xE77F, 15.0f, g_theme.accent, 16.0f, 10.0f, 22.0f, 26.0f);
        draw_text(g, L"Clipboard History", L"Segoe UI", 13.5f, GP_FONT_BOLD, c_txt,
                  42.0f, 10.0f, 148.0f, 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, false);

        wchar_t stat_s[64];
        double mb = (double)g_total_bytes / (1024.0 * 1024.0);
        _snwprintf_s(stat_s, 64, _TRUNCATE, L"%d %ls  ·  %.2f / 6 MB",
                     g_clip_count, g_clip_count == 1 ? L"clip" : L"clips", mb);
        draw_text(g, stat_s, L"Segoe UI", 11.0f, GP_FONT_REGULAR, c_muted,
                  186.0f, 10.0f, 158.0f, 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

        bool has_unpinned = (find_oldest_unpinned(-1) >= 0);
        ARGB clr_bg = (g_hover_clear && has_unpinned) ? c_hov : c_addr;
        draw_round_rect_fill(g, (float)g_rc_clear_btn.left, (float)g_rc_clear_btn.top,
                             (float)(g_rc_clear_btn.right - g_rc_clear_btn.left), 26.0f, 6.0f, clr_bg);
        draw_round_rect_stroke(g, (float)g_rc_clear_btn.left + 0.5f, (float)g_rc_clear_btn.top + 0.5f,
                               (float)(g_rc_clear_btn.right - g_rc_clear_btn.left) - 1.0f, 25.0f, 6.0f, c_bdr, 1.0f);
        draw_text(g, L"Clear Unpinned", L"Segoe UI", 11.0f, GP_FONT_BOLD,
                  has_unpinned ? c_txt : c_muted,
                  (float)g_rc_clear_btn.left, (float)g_rc_clear_btn.top,
                  (float)(g_rc_clear_btn.right - g_rc_clear_btn.left), 26.0f,
                  GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);

        /* Search Pill */
        draw_round_rect_fill(g, (float)g_rc_search.left, (float)g_rc_search.top,
                             (float)(g_rc_search.right - g_rc_search.left), 34.0f, 8.0f, c_addr);
        draw_round_rect_stroke(g, (float)g_rc_search.left + 0.5f, (float)g_rc_search.top + 0.5f,
                               (float)(g_rc_search.right - g_rc_search.left) - 1.0f, 33.0f, 8.0f, c_bdr, 1.0f);
        draw_icon_glyph(g, 0xE721, 12.5f, c_muted, (float)(g_rc_search.left + 8), (float)g_rc_search.top, 18.0f, 34.0f);

        /* Scrollable Clip List */
        int lx = g_rc_list.left;
        int ly0 = g_rc_list.top;
        int lw = g_rc_list.right - g_rc_list.left;
        int lh = g_rc_list.bottom - g_rc_list.top;

        GdipSetClipRect(g, (float)lx, (float)ly0, (float)(lw + 12), (float)lh, GP_COMBINE_REPLACE);

        if (g_vis_count == 0) {
            const wchar_t *empty_msg = g_query[0]
                ? L"No matching clips found"
                : L"Clipboard history is empty. Copy text or images to see them here.";
            draw_icon_glyph(g, 0xE77F, 28.0f, c_muted, (float)lx, (float)(ly0 + lh / 2 - 42), (float)lw, 34.0f);
            draw_text(g, empty_msg, L"Segoe UI", 12.5f, GP_FONT_REGULAR, c_muted,
                      (float)(lx + 16), (float)(ly0 + lh / 2), (float)(lw - 32), 24.0f,
                      GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);
        } else {
            int first = max(0, g_scroll_y / CLIP_ROW_STEP);
            int vis_n = (lh / CLIP_ROW_STEP) + 2;
            int last  = min(g_vis_count, first + vis_n);
            int card_w = lw - ((g_content_h > lh) ? 10 : 0);

            for (int v = first; v < last; v++) {
                ClipItem *it = &g_clips[g_vis[v]];
                int cy = ly0 + v * CLIP_ROW_STEP - g_scroll_y;
                bool is_sel = (v == g_sel_idx);
                bool is_hov = (v == g_hover_idx);

                draw_round_rect_fill(g, (float)lx, (float)cy, (float)card_w, (float)CLIP_CARD_H, 9.0f, c_card);
                if (is_sel) {
                    draw_round_rect_fill(g, (float)lx, (float)cy, (float)card_w, (float)CLIP_CARD_H, 9.0f,
                                         g_theme.accent_overlay_selected);
                    draw_round_rect_stroke(g, (float)lx + 0.5f, (float)cy + 0.5f,
                                           (float)card_w - 1.0f, (float)CLIP_CARD_H - 1.0f, 9.0f,
                                           g_theme.accent, 1.5f);
                } else if (is_hov) {
                    draw_round_rect_fill(g, (float)lx, (float)cy, (float)card_w, (float)CLIP_CARD_H, 9.0f, c_hov);
                    draw_round_rect_stroke(g, (float)lx + 0.5f, (float)cy + 0.5f,
                                           (float)card_w - 1.0f, (float)CLIP_CARD_H - 1.0f, 9.0f, c_bdr, 1.0f);
                } else {
                    draw_round_rect_stroke(g, (float)lx + 0.5f, (float)cy + 0.5f,
                                           (float)card_w - 1.0f, (float)CLIP_CARD_H - 1.0f, 9.0f, c_bdr, 1.0f);
                }

                /* Left Accent Stripe for pinned items */
                if (it->pinned) {
                    draw_round_rect_fill(g, (float)(lx + 2), (float)(cy + 10), 3.0f, (float)(CLIP_CARD_H - 20), 1.5f,
                                         g_theme.accent);
                }

                int text_x = lx + 14;
                int right_pad = 72; /* space for pin + delete icons */

                if (it->kind == CLIP_KIND_IMAGE) {
                    int bx = lx + 10, by = cy + 7;
                    draw_round_rect_fill(g, (float)bx, (float)by, (float)CLIP_THUMB_W, (float)CLIP_THUMB_H, 6.0f, c_bg);
                    if (clip_ensure_thumb(it) && it->thumb_bmp) {
                        int tx = bx + (CLIP_THUMB_W - it->thumb_w) / 2;
                        int ty = by + (CLIP_THUMB_H - it->thumb_h) / 2;
                        GdipDrawImageRectI(g, (GpImage *)it->thumb_bmp, tx, ty, it->thumb_w, it->thumb_h);
                    } else {
                        draw_icon_glyph(g, 0xEB9F, 18.0f, g_theme.accent, (float)bx, (float)by,
                                        (float)CLIP_THUMB_W, (float)CLIP_THUMB_H);
                    }
                    draw_round_rect_stroke(g, (float)bx + 0.5f, (float)by + 0.5f,
                                           (float)(CLIP_THUMB_W - 1), (float)(CLIP_THUMB_H - 1), 6.0f, c_bdr, 1.0f);
                    text_x = bx + CLIP_THUMB_W + 10;

                    draw_text(g, it->preview, L"Segoe UI", 12.5f, GP_FONT_BOLD, c_txt,
                              (float)text_x, (float)(cy + 9), (float)(lx + card_w - text_x - right_pad), 20.0f,
                              GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

                    wchar_t meta[96];
                    _snwprintf_s(meta, 96, _TRUNCATE, L"%02u:%02u  ·  %.1f KB PNG%ls",
                                 it->captured_at.wHour, it->captured_at.wMinute,
                                 (double)it->png_size / 1024.0,
                                 it->pinned ? L"  ·  Pinned" : L"");
                    draw_text(g, meta, L"Segoe UI", 11.0f, GP_FONT_REGULAR, c_muted,
                              (float)text_x, (float)(cy + 31), (float)(lx + card_w - text_x - right_pad), 18.0f,
                              GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                } else {
                    draw_text(g, it->preview, L"Segoe UI", 12.5f, is_sel ? GP_FONT_BOLD : GP_FONT_REGULAR, c_txt,
                              (float)text_x, (float)(cy + 8), (float)(lx + card_w - text_x - right_pad), 22.0f,
                              GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

                    wchar_t meta[96];
                    if (it->line_count > 1) {
                        _snwprintf_s(meta, 96, _TRUNCATE, L"%02u:%02u  ·  %d lines  ·  %llu chars%ls",
                                     it->captured_at.wHour, it->captured_at.wMinute,
                                     it->line_count, (unsigned long long)it->text_chars,
                                     it->pinned ? L"  ·  Pinned" : L"");
                    } else {
                        _snwprintf_s(meta, 96, _TRUNCATE, L"%02u:%02u  ·  %llu %ls%ls",
                                     it->captured_at.wHour, it->captured_at.wMinute,
                                     (unsigned long long)it->text_chars,
                                     it->text_chars == 1 ? L"char" : L"chars",
                                     it->pinned ? L"  ·  Pinned" : L"");
                    }
                    draw_text(g, meta, L"Segoe UI", 11.0f, GP_FONT_REGULAR, c_muted,
                              (float)text_x, (float)(cy + 32), (float)(lx + card_w - text_x - right_pad), 18.0f,
                              GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
                }

                /* Inline Pin & Delete Buttons */
                RECT rc_pin, rc_del;
                clip_card_action_rects(cy, &rc_pin, &rc_del);
                if (g_content_h > lh) {
                    OffsetRect(&rc_pin, -10, 0);
                    OffsetRect(&rc_del, -10, 0);
                }
                bool pin_hov = (is_hov && g_hover_action == 1);
                bool del_hov = (is_hov && g_hover_action == 2);

                if (pin_hov || it->pinned) {
                    draw_round_rect_fill(g, (float)rc_pin.left, (float)rc_pin.top, 26.0f, 26.0f, 6.0f,
                                         pin_hov ? c_hov : g_theme.accent_overlay_selected);
                }
                draw_icon_glyph(g, it->pinned ? 0xE840 : 0xE718, 12.0f,
                                it->pinned ? g_theme.accent : (is_hov || is_sel ? c_txt : c_muted),
                                (float)rc_pin.left, (float)rc_pin.top, 26.0f, 26.0f);

                if (del_hov) {
                    draw_round_rect_fill(g, (float)rc_del.left, (float)rc_del.top, 26.0f, 26.0f, 6.0f,
                                         MAKE_ARGB(55, 232, 72, 72));
                }
                draw_icon_glyph(g, 0xE74D, 12.0f,
                                del_hov ? g_theme.danger : (is_hov || is_sel ? c_txt : c_muted),
                                (float)rc_del.left, (float)rc_del.top, 26.0f, 26.0f);
            }

            /* Slim Scrollbar */
            if (g_content_h > lh && lh > 24) {
                float ratio = (float)lh / (float)g_content_h;
                float th = max(28.0f, (float)lh * ratio);
                float max_s = (float)(g_content_h - lh);
                float ty = (float)ly0 + ((float)g_scroll_y / max_s) * ((float)lh - th);
                draw_round_rect_fill(g, (float)(g_rc_list.right - 5), ty, 5.0f, th, 2.5f, c_bdr);
            }
        }
        GdipResetClip(g);

        /* Footer */
        int fy = h - 26;
        draw_round_rect_fill(g, 0.0f, (float)fy, (float)w, 26.0f, 0.0f, c_tb);
        draw_line(g, 0.0f, (float)fy + 0.5f, (float)w, (float)fy + 0.5f, c_bdr, 1.0f);
        draw_text(g, L"Enter / Click: Paste  |  Ctrl+P: Pin  |  Ctrl+Del: Delete  |  Esc: Close",
                  L"Segoe UI", 10.5f, GP_FONT_REGULAR, c_muted,
                  12.0f, (float)fy, (float)(w - 24), 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, true);
    }
}

static void clip_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        clip_draw(&fb, cr.right, cr.bottom);
        opaque_frame_end(hdc, &fb);
    }
    EndPaint(hwnd, &ps);
}

/* The list drawn into a picture, for checks. A test copy never listens to
 * the clipboard, so it shows the made-up clips a check puts in. */
int clipboard_snapshot(const wchar_t *png_path) {
    if (!g_hClipWnd) return 0;
    g_scroll_y = 0;
    g_sel_idx = 0;
    g_query[0] = L'\0';
    clip_rebuild_visible();
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    int ok = 0;
    if (opaque_frame_begin(&fb, CLIP_WND_W, CLIP_WND_H)) {
        clip_draw(&fb, CLIP_WND_W, CLIP_WND_H);
        ok = surface_save_png(&fb, png_path);
        surface_destroy(&fb);
    }
    clip_free_all_thumbs();
    return ok;
}

/* Made-up clips for a test copy's pictures: never the PC's own clipboard. */
void clipboard_add_sample(const wchar_t *text, bool pinned) {
    clip_add_text_internal(text, wcslen(text), pinned, false);
}

static int clip_hit_test(int mx, int my, int *out_action) {
    if (out_action) *out_action = 0;
    if (mx < g_rc_list.left || mx > g_rc_list.right ||
        my < g_rc_list.top || my > g_rc_list.bottom) {
        return -1;
    }
    int rel_y = my - g_rc_list.top + g_scroll_y;
    if (rel_y < 0) return -1;
    int idx = rel_y / CLIP_ROW_STEP;
    int off = rel_y % CLIP_ROW_STEP;
    if (idx < 0 || idx >= g_vis_count || off >= CLIP_CARD_H) return -1;

    if (out_action) {
        int card_y = g_rc_list.top + idx * CLIP_ROW_STEP - g_scroll_y;
        RECT rc_pin, rc_del;
        clip_card_action_rects(card_y, &rc_pin, &rc_del);
        int lh = g_rc_list.bottom - g_rc_list.top;
        if (g_content_h > lh) {
            OffsetRect(&rc_pin, -10, 0);
            OffsetRect(&rc_del, -10, 0);
        }
        POINT pt = { mx, my };
        if (PtInRect(&rc_pin, pt)) *out_action = 1;
        else if (PtInRect(&rc_del, pt)) *out_action = 2;
    }
    return idx;
}

static LRESULT CALLBACK clip_edit_sub_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_destroying || !g_orig_edit_proc) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_MOUSEWHEEL && g_hClipWnd) {
        SendMessageW(g_hClipWnd, WM_MOUSEWHEEL, wParam, lParam);
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wParam == VK_ESCAPE) {
            if (g_query[0]) {
                SetWindowTextW(hwnd, L"");
            } else {
                clipboard_hide();
            }
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (g_vis_count > 0) clip_paste_vis_idx(g_sel_idx);
            return 0;
        }
        if (wParam == VK_DOWN && g_vis_count > 0) {
            g_sel_idx = min(g_vis_count - 1, g_sel_idx + 1);
            clip_ensure_sel_visible();
            InvalidateRect(g_hClipWnd, NULL, FALSE);
            return 0;
        }
        if (wParam == VK_UP && g_vis_count > 0) {
            g_sel_idx = max(0, g_sel_idx - 1);
            clip_ensure_sel_visible();
            InvalidateRect(g_hClipWnd, NULL, FALSE);
            return 0;
        }
        if (ctrl && wParam == 'P' && g_vis_count > 0) {
            clip_toggle_pin_vis(g_sel_idx);
            return 0;
        }
        if (wParam == VK_DELETE && g_vis_count > 0 && (ctrl || !g_query[0])) {
            clip_delete_vis(g_sel_idx);
            return 0;
        }
    }
    return CallWindowProcW(g_orig_edit_proc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK clip_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLIPBOARDUPDATE: {
        DWORD seq = GetClipboardSequenceNumber();
        if (seq != g_last_seq) {
            g_last_seq = seq;
            if (!clip_capture_now(hwnd)) {
                g_retry_left = 3;
                SetTimer(hwnd, TIMER_CLIP_RETRY, 45, NULL);
            } else if (IsWindowVisible(hwnd)) {
                clip_rebuild_visible();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_CLIP_RETRY) {
            KillTimer(hwnd, TIMER_CLIP_RETRY);
            if (clip_capture_now(hwnd)) {
                g_retry_left = 0;
                if (IsWindowVisible(hwnd)) {
                    clip_rebuild_visible();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else if (--g_retry_left > 0) {
                SetTimer(hwnd, TIMER_CLIP_RETRY, 55, NULL);
            }
        } else if (wParam == TIMER_CLIP_PASTE) {
            KillTimer(hwnd, TIMER_CLIP_PASTE);
            if (g_pending_paste) {
                g_pending_paste = false;
                clip_send_ctrl_v();
            }
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible(hwnd)) {
            clipboard_hide();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_CLIP_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            GetWindowTextW(g_hSearchEdit, g_query, 128);
            g_scroll_y = 0;
            g_sel_idx = 0;
            clip_rebuild_visible();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_theme.files_text);
        SetBkColor(hdc, g_theme.files_addrbar);
        return (LRESULT)g_hEditBrush;
    }

    case WM_MOUSEWHEEL: {
        short d = GET_WHEEL_DELTA_WPARAM(wParam);
        g_scroll_y -= (d / 120) * CLIP_ROW_STEP;
        if (g_scroll_y < 0) g_scroll_y = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT pt = { mx, my };
        bool nh_clr = PtInRect(&g_rc_clear_btn, pt) != 0;
        int nact = 0;
        int nh = clip_hit_test(mx, my, &nact);
        if (nh != g_hover_idx || nact != g_hover_action || nh_clr != g_hover_clear) {
            g_hover_idx = nh;
            g_hover_action = nact;
            g_hover_clear = nh_clr;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = false;
        g_hover_idx = -1;
        g_hover_action = 0;
        g_hover_clear = false;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP: {
        int mx = (int)(short)LOWORD(lParam);
        int my = (int)(short)HIWORD(lParam);
        POINT pt = { mx, my };
        if (PtInRect(&g_rc_clear_btn, pt)) {
            clip_clear_unpinned();
            return 0;
        }
        int act = 0;
        int hit = clip_hit_test(mx, my, &act);
        if (hit >= 0) {
            if (act == 1)      clip_toggle_pin_vis(hit);
            else if (act == 2) clip_delete_vis(hit);
            else               clip_paste_vis_idx(hit);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        clip_paint(hwnd);
        return 0;

    case WM_DESTROY:
        g_destroying = true;
        KillTimer(hwnd, TIMER_CLIP_RETRY);
        KillTimer(hwnd, TIMER_CLIP_PASTE);
        if (g_listener_added) {
            RemoveClipboardFormatListener(hwnd);
            g_listener_added = false;
        }
        if (g_hSearchEdit && g_orig_edit_proc) {
            SetWindowLongPtrW(g_hSearchEdit, GWLP_WNDPROC, (LONG_PTR)g_orig_edit_proc);
            g_orig_edit_proc = NULL;
        }
        g_hSearchEdit = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void refresh_clip_theme_objects(void) {
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
    g_hEditBrush = CreateSolidBrush(g_theme.files_addrbar);
    if (g_hClipWnd) {
        BOOL dark = g_theme.is_dark ? TRUE : FALSE;
        DwmSetWindowAttribute(g_hClipWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    }
}

void clipboard_init(HINSTANCE hInst) {
    g_destroying = false;
    g_hEditFont = render_create_ui_font(14, false);
    refresh_clip_theme_objects();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = clip_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = CLIP_CLASS;
    RegisterClassExW(&wc);

    g_hClipWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        CLIP_CLASS, L"Clipboard History",
        WS_POPUP | WS_BORDER,
        100, 100, CLIP_WND_W, CLIP_WND_H,
        NULL, NULL, hInst, NULL);
    if (!g_hClipWnd) return;

    DWORD corner = 2; /* DWMWCP_ROUND */
    DwmSetWindowAttribute(g_hClipWnd, 33, &corner, sizeof(corner));
    refresh_clip_theme_objects();

    g_hSearchEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        46, 54, CLIP_WND_W - 74, 18,
        g_hClipWnd, (HMENU)(INT_PTR)ID_CLIP_SEARCH, hInst, NULL);
    SendMessageW(g_hSearchEdit, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);
    SendMessageW(g_hSearchEdit, 0x1501 /* EM_SETCUEBANNER */, (WPARAM)TRUE,
                 (LPARAM)L"Search clipboard history...");
    g_orig_edit_proc = (WNDPROC)SetWindowLongPtrW(g_hSearchEdit, GWLP_WNDPROC, (LONG_PTR)clip_edit_sub_proc);

    /* A test copy reads no pins and never listens to the PC's clipboard. */
    if (g_cfg.isolated) return;
    clip_load_pins();
    g_last_seq = GetClipboardSequenceNumber();
    if (AddClipboardFormatListener(g_hClipWnd)) {
        g_listener_added = true;
    }
}

void clipboard_toggle(void) {
    if (!g_hClipWnd) return;
    if (IsWindowVisible(g_hClipWnd)) {
        clipboard_hide();
        return;
    }

    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);
    if (fg && fg_pid != GetCurrentProcessId()) {
        g_prev_fg_wnd = fg;
    } else {
        HWND lfg = launcher_get_prev_fg();
        if (lfg && IsWindow(lfg)) {
            g_prev_fg_wnd = lfg;
        } else {
            HWND pfg = popups_get_last_fg_window();
            if (pfg && IsWindow(pfg)) g_prev_fg_wnd = pfg;
        }
    }

    if (g_cfg.isolated) return; /* a test copy shows nothing */
    POINT cur = { 0, 0 };
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;

    int x = wa.left + ((wa.right - wa.left) - CLIP_WND_W) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - CLIP_WND_H) / 3;
    if (x < wa.left + 12) x = wa.left + 12;
    if (y < wa.top + 12)  y = wa.top + 12;
    if (x + CLIP_WND_W > wa.right - 12)  x = wa.right - CLIP_WND_W - 12;
    if (y + CLIP_WND_H > wa.bottom - 12) y = wa.bottom - CLIP_WND_H - 12;

    g_scroll_y = 0;
    g_sel_idx = 0;
    g_hover_idx = -1;
    g_hover_action = 0;
    g_hover_clear = false;
    g_query[0] = L'\0';
    SetWindowTextW(g_hSearchEdit, L"");
    clip_rebuild_visible();

    SetWindowPos(g_hClipWnd, HWND_TOPMOST, x, y, CLIP_WND_W, CLIP_WND_H, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hClipWnd);
    SetFocus(g_hSearchEdit);
    InvalidateRect(g_hClipWnd, NULL, FALSE);
}

void clipboard_hide(void) {
    if (g_hClipWnd && IsWindowVisible(g_hClipWnd)) {
        ShowWindow(g_hClipWnd, SW_HIDE);
        clip_free_all_thumbs();
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
    }
}

bool clipboard_is_open(void) {
    return (g_hClipWnd != NULL && IsWindowVisible(g_hClipWnd));
}

void clipboard_on_theme_changed(void) {
    refresh_clip_theme_objects();
    if (g_hClipWnd && IsWindowVisible(g_hClipWnd)) {
        InvalidateRect(g_hClipWnd, NULL, TRUE);
    }
}

void clipboard_shutdown(void) {
    if (g_listener_added && g_hClipWnd) {
        RemoveClipboardFormatListener(g_hClipWnd);
        g_listener_added = false;
    }
    if (g_hClipWnd && IsWindow(g_hClipWnd)) {
        DestroyWindow(g_hClipWnd);
        g_hClipWnd = NULL;
    }
    while (g_clip_count > 0) {
        clip_remove_at(g_clip_count - 1);
    }
    g_total_bytes = 0;
    if (g_hEditFont)  { DeleteObject(g_hEditFont);  g_hEditFont = NULL; }
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
}
