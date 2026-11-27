// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"
#pragma warning(push)
#pragma warning(disable: 4201)
#include <wincodec.h>
#pragma warning(pop)
#include <shlwapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/*
 * The image viewer in lares-view.exe (issue #151).
 * - Opened from Files, the launcher and the screenshot preview.
 * - PNG, JPG, JPEG, WEBP, BMP, GIF and ICO through Windows Imaging Component, on the processor.
 * - Caps decoded bitmap longest edge to 2048 px (<= ~16 MB) using VirtualAlloc so total RAM
 * stays strictly under 50 MB even on 6K/8K wallpapers, and VirtualFree returns 100% of
 * decoded pixel memory back to Windows the instant the viewer closes or switches images.
 */

#define VIEWER_CLASS       L"LaresViewerWnd"
#define VIEWER_MAX_DIM     2048
#define VIEWER_MAX_FOLDER  8192

typedef struct {
    wchar_t name[MAX_PATH];
} ViewerFileItem;

static HWND            g_hViewer = NULL;
static wchar_t         g_cur_path[MAX_PATH];
static wchar_t         g_cur_dir[MAX_PATH];
static wchar_t         g_cur_name[MAX_PATH];
static uint64_t        g_cur_file_size = 0;

/* Owned decoded image buffer (VirtualAlloc so VirtualFree immediately releases Private RAM) */
static BYTE           *g_img_pixels = NULL;
static SIZE_T          g_img_bytes = 0;
static GpBitmap       *g_img_bmp = NULL;
static int             g_dec_w = 0;
static int             g_dec_h = 0;
static int             g_orig_w = 0;
static int             g_orig_h = 0;
static bool            g_was_downscaled = false;
static bool            g_load_failed = false;

/* Folder browsing list (single directory only, zero recursion) */
static ViewerFileItem *g_folder_items = NULL;
static int             g_folder_count = 0;
static int             g_folder_idx = -1;

/* View transform & interaction state */
static bool            g_fit_mode = true;
static float           g_zoom = 1.0f;       /* scale applied to g_dec_w x g_dec_h */
static float           g_pan_x = 0.0f;      /* offset from viewport center in screen px */
static float           g_pan_y = 0.0f;
static bool            g_dragging = false;
static POINT           g_drag_start = {0, 0};
static float           g_drag_pan_x = 0.0f;
static float           g_drag_pan_y = 0.0f;

static int             g_hover_cap = 0;     /* 0=none, 1=min, 2=max, 3=close */
static int             g_hover_tb = -1;     /* 0..8 toolbar buttons */
static int             g_hover_nav = 0;     /* -1=left chevron, 1=right chevron */
static bool            g_tracking_mouse = false;
static wchar_t         g_status_flash[96];
static DWORD           g_status_flash_until = 0;

/* Cached layout rectangles */
static RECT            g_rc_cap[3];
static RECT            g_rc_tb[9];          /* Prev, Next, ZoomOut, ZoomIn, Fit, 1:1, Rotate, Copy, External */
static RECT            g_rc_view;
static RECT            g_rc_nav_l;
static RECT            g_rc_nav_r;

static ARGB v_argb(BYTE a, COLORREF c) {
    return MAKE_ARGB(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

bool viewer_is_supported_ext(const wchar_t *ext) {
    if (!ext || !ext[0]) return false;
    return (_wcsicmp(ext, L".png")  == 0 ||
            _wcsicmp(ext, L".jpg")  == 0 ||
            _wcsicmp(ext, L".jpeg") == 0 ||
            _wcsicmp(ext, L".webp") == 0 ||
            _wcsicmp(ext, L".bmp")  == 0 ||
            _wcsicmp(ext, L".gif")  == 0 ||
            _wcsicmp(ext, L".ico")  == 0);
}

static void viewer_format_bytes(uint64_t bytes, wchar_t *out, size_t len) {
    if (bytes < 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        _snwprintf_s(out, len, _TRUNCATE, L"%.1f KB", (double)bytes / 1024.0);
    } else {
        _snwprintf_s(out, len, _TRUNCATE, L"%.2f MB", (double)bytes / (1024.0 * 1024.0));
    }
}

static void viewer_free_image(void) {
    if (g_img_bmp) {
        GdipDisposeImage((GpImage *)g_img_bmp);
        g_img_bmp = NULL;
    }
    if (g_img_pixels) {
        VirtualFree(g_img_pixels, 0, MEM_RELEASE);
        g_img_pixels = NULL;
        g_img_bytes = 0;
    }
    g_dec_w = 0;
    g_dec_h = 0;
    g_orig_w = 0;
    g_orig_h = 0;
    g_was_downscaled = false;
    g_load_failed = false;
}

static void viewer_free_folder_list(void) {
    if (g_folder_items) {
        HeapFree(GetProcessHeap(), 0, g_folder_items);
        g_folder_items = NULL;
    }
    g_folder_count = 0;
    g_folder_idx = -1;
}

static int cmp_folder_item(const void *pa, const void *pb) {
    const ViewerFileItem *a = (const ViewerFileItem *)pa;
    const ViewerFileItem *b = (const ViewerFileItem *)pb;
    return StrCmpLogicalW(a->name, b->name);
}

static void viewer_scan_folder(const wchar_t *dir, const wchar_t *active_name) {
    viewer_free_folder_list();
    if (!dir || !dir[0]) return;

    wchar_t pat[MAX_PATH];
    size_t dlen = wcslen(dir);
    if (dlen > 0 && dir[dlen - 1] == L'\\') {
        _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls*", dir);
    } else {
        _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\*", dir);
    }

    int cap = 64;
    int cnt = 0;
    ViewerFileItem *arr = (ViewerFileItem *)HeapAlloc(GetProcessHeap(), 0, (size_t)cap * sizeof(ViewerFileItem));
    if (!arr) return;

    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const wchar_t *ext = PathFindExtensionW(fd.cFileName);
            if (!viewer_is_supported_ext(ext)) continue;
            if (cnt >= cap) {
                if (cap >= VIEWER_MAX_FOLDER) break;
                cap *= 2;
                if (cap > VIEWER_MAX_FOLDER) cap = VIEWER_MAX_FOLDER;
                ViewerFileItem *grown = (ViewerFileItem *)HeapReAlloc(GetProcessHeap(), 0, arr,
                                                                      (size_t)cap * sizeof(ViewerFileItem));
                if (!grown) break;
                arr = grown;
            }
            wcsncpy_s(arr[cnt].name, MAX_PATH, fd.cFileName, _TRUNCATE);
            cnt++;
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }

    if (cnt > 1) {
        qsort(arr, (size_t)cnt, sizeof(ViewerFileItem), cmp_folder_item);
    }

    g_folder_items = arr;
    g_folder_count = cnt;
    g_folder_idx = -1;
    if (active_name && active_name[0]) {
        for (int i = 0; i < cnt; i++) {
            if (_wcsicmp(arr[i].name, active_name) == 0) {
                g_folder_idx = i;
                break;
            }
        }
    }
}

/* Pure CPU WIC decode with 2048px longest-edge RAM safety cap and VirtualAlloc backing */
static bool viewer_decode_wic(const wchar_t *path) {
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *conv = NULL;
    bool ok = false;

    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&fac);
    if (FAILED(hr) || !fac) goto done;

    hr = IWICImagingFactory_CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr) || !dec) goto done;

    /* For multi-size ICO files, pick the sharpest/largest frame instead of 16x16 */
    UINT best_idx = 0;
    const wchar_t *ext = PathFindExtensionW(path);
    if (ext && _wcsicmp(ext, L".ico") == 0) {
        UINT fcnt = 0;
        if (SUCCEEDED(IWICBitmapDecoder_GetFrameCount(dec, &fcnt)) && fcnt > 1) {
            UINT best_area = 0;
            for (UINT i = 0; i < fcnt; i++) {
                IWICBitmapFrameDecode *tf = NULL;
                if (SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, i, &tf)) && tf) {
                    UINT tw = 0, th = 0;
                    if (SUCCEEDED(IWICBitmapFrameDecode_GetSize(tf, &tw, &th))) {
                        if (tw * th > best_area) {
                            best_area = tw * th;
                            best_idx = i;
                        }
                    }
                    IWICBitmapFrameDecode_Release(tf);
                }
            }
        }
    }

    hr = IWICBitmapDecoder_GetFrame(dec, best_idx, &frame);
    if (FAILED(hr) || !frame) goto done;

    UINT ow = 0, oh = 0;
    hr = IWICBitmapFrameDecode_GetSize(frame, &ow, &oh);
    if (FAILED(hr) || ow == 0 || oh == 0) goto done;

    UINT dw = ow, dh = oh;
    bool scaled = false;
    if (ow > VIEWER_MAX_DIM || oh > VIEWER_MAX_DIM) {
        scaled = true;
        if (ow >= oh) {
            dw = VIEWER_MAX_DIM;
            dh = (UINT)max(1ULL, ((uint64_t)oh * VIEWER_MAX_DIM) / ow);
        } else {
            dh = VIEWER_MAX_DIM;
            dw = (UINT)max(1ULL, ((uint64_t)ow * VIEWER_MAX_DIM) / oh);
        }
    }

    IWICBitmapSource *src = (IWICBitmapSource *)frame;
    if (scaled) {
        hr = IWICImagingFactory_CreateBitmapScaler(fac, &scaler);
        if (SUCCEEDED(hr) && scaler) {
            hr = IWICBitmapScaler_Initialize(scaler, src, dw, dh, WICBitmapInterpolationModeFant);
            if (SUCCEEDED(hr)) {
                src = (IWICBitmapSource *)scaler;
            } else {
                IWICBitmapScaler_Release(scaler);
                scaler = NULL;
                scaled = false;
                dw = ow;
                dh = oh;
            }
        }
    }

    hr = IWICImagingFactory_CreateFormatConverter(fac, &conv);
    if (FAILED(hr) || !conv) goto done;

    hr = IWICFormatConverter_Initialize(conv, src, &GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) goto done;

    UINT stride = dw * 4;
    SIZE_T total_bytes = (SIZE_T)stride * (SIZE_T)dh;
    BYTE *pixels = (BYTE *)VirtualAlloc(NULL, total_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pixels) goto done;

    hr = IWICFormatConverter_CopyPixels(conv, NULL, stride, (UINT)total_bytes, pixels);
    if (FAILED(hr)) {
        VirtualFree(pixels, 0, MEM_RELEASE);
        goto done;
    }

    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromScan0((INT)dw, (INT)dh, (INT)stride, GP_PIXELFORMAT_32BPP_PARGB, pixels, &bmp) != GP_OK || !bmp) {
        VirtualFree(pixels, 0, MEM_RELEASE);
        goto done;
    }

    g_img_pixels = pixels;
    g_img_bytes = total_bytes;
    g_img_bmp = bmp;
    g_dec_w = (int)dw;
    g_dec_h = (int)dh;
    g_orig_w = (int)ow;
    g_orig_h = (int)oh;
    g_was_downscaled = scaled;
    ok = true;

done:
    if (conv)   IWICFormatConverter_Release(conv);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (frame)  IWICBitmapFrameDecode_Release(frame);
    if (dec)    IWICBitmapDecoder_Release(dec);
    if (fac)    IWICImagingFactory_Release(fac);
    return ok;
}

static float viewer_compute_fit_zoom(void) {
    if (g_dec_w <= 0 || g_dec_h <= 0) return 1.0f;
    int vw = (g_rc_view.right - g_rc_view.left) - 32;
    int vh = (g_rc_view.bottom - g_rc_view.top) - 32;
    if (vw < 40) vw = 40;
    if (vh < 40) vh = 40;
    float sx = (float)vw / (float)g_dec_w;
    float sy = (float)vh / (float)g_dec_h;
    float s = (sx < sy) ? sx : sy;
    /* Keep small images crisp at 1:1 when fitting so icons don't look stretched */
    float one_to_one = (g_orig_w > 0 && g_dec_w > 0) ? ((float)g_orig_w / (float)g_dec_w) : 1.0f;
    if (s > one_to_one) s = one_to_one;
    if (s < 0.05f) s = 0.05f;
    return s;
}

static float viewer_effective_zoom(void) {
    return g_fit_mode ? viewer_compute_fit_zoom() : g_zoom;
}

static void viewer_clamp_pan(void) {
    if (g_dec_w <= 0 || g_dec_h <= 0) {
        g_pan_x = 0.0f;
        g_pan_y = 0.0f;
        return;
    }
    float z = viewer_effective_zoom();
    float dw = (float)g_dec_w * z;
    float dh = (float)g_dec_h * z;
    float vw = (float)(g_rc_view.right - g_rc_view.left);
    float vh = (float)(g_rc_view.bottom - g_rc_view.top);

    float max_x = (dw > vw) ? ((dw - vw) * 0.5f + 48.0f) : 0.0f;
    float max_y = (dh > vh) ? ((dh - vh) * 0.5f + 48.0f) : 0.0f;
    if (g_pan_x >  max_x) g_pan_x =  max_x;
    if (g_pan_x < -max_x) g_pan_x = -max_x;
    if (g_pan_y >  max_y) g_pan_y =  max_y;
    if (g_pan_y < -max_y) g_pan_y = -max_y;
}

static void viewer_load_path(const wchar_t *path, bool rescan_dir) {
    if (!path || !path[0]) return;

    viewer_free_image();
    wcsncpy_s(g_cur_path, MAX_PATH, path, _TRUNCATE);

    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, MAX_PATH, path, _TRUNCATE);
    PathRemoveFileSpecW(dir);
    const wchar_t *fname = PathFindFileNameW(path);
    wcsncpy_s(g_cur_name, MAX_PATH, (fname && fname[0]) ? fname : path, _TRUNCATE);

    bool dir_changed = (_wcsicmp(dir, g_cur_dir) != 0);
    wcsncpy_s(g_cur_dir, MAX_PATH, dir, _TRUNCATE);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER u;
        u.LowPart = fad.nFileSizeLow;
        u.HighPart = fad.nFileSizeHigh;
        g_cur_file_size = u.QuadPart;
    } else {
        g_cur_file_size = 0;
    }

    if (rescan_dir || dir_changed || !g_folder_items) {
        viewer_scan_folder(g_cur_dir, g_cur_name);
    } else {
        g_folder_idx = -1;
        for (int i = 0; i < g_folder_count; i++) {
            if (_wcsicmp(g_folder_items[i].name, g_cur_name) == 0) {
                g_folder_idx = i;
                break;
            }
        }
    }

    if (!viewer_decode_wic(path)) {
        g_load_failed = true;
    }

    g_fit_mode = true;
    g_zoom = viewer_compute_fit_zoom();
    g_pan_x = 0.0f;
    g_pan_y = 0.0f;

    if (g_hViewer) {
        wchar_t title[MAX_PATH + 32];
        _snwprintf_s(title, MAX_PATH + 32, _TRUNCATE, L"%ls - Image Viewer", g_cur_name);
        SetWindowTextW(g_hViewer, title);
        InvalidateRect(g_hViewer, NULL, FALSE);
    }
}

static void viewer_step(int delta) {
    if (g_folder_count <= 1 || !g_folder_items) return;
    int next = (g_folder_idx < 0) ? 0 : (g_folder_idx + delta);
    if (next < 0) next = g_folder_count - 1;
    if (next >= g_folder_count) next = 0;
    g_folder_idx = next;

    wchar_t full[MAX_PATH];
    size_t dlen = wcslen(g_cur_dir);
    if (dlen > 0 && g_cur_dir[dlen - 1] == L'\\') {
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%ls%ls", g_cur_dir, g_folder_items[next].name);
    } else {
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%ls\\%ls", g_cur_dir, g_folder_items[next].name);
    }
    viewer_load_path(full, false);
}

static void viewer_zoom_at(float factor, int anchor_x, int anchor_y) {
    if (g_dec_w <= 0 || g_dec_h <= 0) return;
    float old_z = viewer_effective_zoom();
    float new_z = old_z * factor;
    float min_z = 0.05f;
    float max_z = max(16.0f, ((float)g_orig_w / (float)max(1, g_dec_w)) * 12.0f);
    if (new_z < min_z) new_z = min_z;
    if (new_z > max_z) new_z = max_z;

    float vcx = (float)(g_rc_view.left + g_rc_view.right) * 0.5f;
    float vcy = (float)(g_rc_view.top + g_rc_view.bottom) * 0.5f;
    float dx = (float)anchor_x - (vcx + g_pan_x);
    float dy = (float)anchor_y - (vcy + g_pan_y);
    float ratio = new_z / old_z;
    g_pan_x -= dx * (ratio - 1.0f);
    g_pan_y -= dy * (ratio - 1.0f);

    g_fit_mode = false;
    g_zoom = new_z;
    viewer_clamp_pan();
    if (g_hViewer) InvalidateRect(g_hViewer, NULL, FALSE);
}

static void viewer_set_fit(void) {
    g_fit_mode = true;
    g_zoom = viewer_compute_fit_zoom();
    g_pan_x = 0.0f;
    g_pan_y = 0.0f;
    if (g_hViewer) InvalidateRect(g_hViewer, NULL, FALSE);
}

static void viewer_set_actual_size(void) {
    if (g_dec_w <= 0) return;
    g_fit_mode = false;
    g_zoom = (g_orig_w > 0) ? ((float)g_orig_w / (float)g_dec_w) : 1.0f;
    g_pan_x = 0.0f;
    g_pan_y = 0.0f;
    viewer_clamp_pan();
    if (g_hViewer) InvalidateRect(g_hViewer, NULL, FALSE);
}

static void viewer_rotate_cw(void) {
    if (!g_img_pixels || g_dec_w <= 0 || g_dec_h <= 0) return;
    int nw = g_dec_h;
    int nh = g_dec_w;
    SIZE_T nbytes = (SIZE_T)nw * (SIZE_T)nh * 4;
    BYTE *npix = (BYTE *)VirtualAlloc(NULL, nbytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!npix) return;

    const uint32_t *src = (const uint32_t *)g_img_pixels;
    uint32_t *dst = (uint32_t *)npix;
    for (int y = 0; y < g_dec_h; y++) {
        int dx = g_dec_h - 1 - y;
        const uint32_t *row = src + (size_t)y * (size_t)g_dec_w;
        for (int x = 0; x < g_dec_w; x++) {
            dst[(size_t)x * (size_t)nw + (size_t)dx] = row[x];
        }
    }

    GpBitmap *nbmp = NULL;
    if (GdipCreateBitmapFromScan0(nw, nh, nw * 4, GP_PIXELFORMAT_32BPP_PARGB, npix, &nbmp) != GP_OK || !nbmp) {
        VirtualFree(npix, 0, MEM_RELEASE);
        return;
    }

    if (g_img_bmp) GdipDisposeImage((GpImage *)g_img_bmp);
    VirtualFree(g_img_pixels, 0, MEM_RELEASE);

    g_img_pixels = npix;
    g_img_bytes = nbytes;
    g_img_bmp = nbmp;
    g_dec_w = nw;
    g_dec_h = nh;
    int tmp = g_orig_w;
    g_orig_w = g_orig_h;
    g_orig_h = tmp;

    if (g_fit_mode) {
        g_zoom = viewer_compute_fit_zoom();
        g_pan_x = 0.0f;
        g_pan_y = 0.0f;
    } else {
        viewer_clamp_pan();
    }
    if (g_hViewer) InvalidateRect(g_hViewer, NULL, FALSE);
}

static void viewer_copy_to_clipboard(void) {
    if (!g_img_pixels || g_dec_w <= 0 || g_dec_h <= 0 || !g_hViewer) return;

    SIZE_T img_sz = (SIZE_T)g_dec_w * (SIZE_T)g_dec_h * 4;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + img_sz);
    if (!hMem) return;

    BYTE *dst = (BYTE *)GlobalLock(hMem);
    if (!dst) { GlobalFree(hMem); return; }

    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)dst;
    ZeroMemory(bih, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = g_dec_w;
    bih->biHeight = g_dec_h; /* bottom-up standard CF_DIB for maximum app compatibility */
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)img_sz;

    BYTE *bits = dst + sizeof(BITMAPINFOHEADER);
    size_t row_bytes = (size_t)g_dec_w * 4;
    for (int y = 0; y < g_dec_h; y++) {
        const BYTE *srow = g_img_pixels + (size_t)(g_dec_h - 1 - y) * row_bytes;
        BYTE *drow = bits + (size_t)y * row_bytes;
        /* Un-premultiply alpha over white if semi-transparent, or copy clean BGRA */
        memcpy(drow, srow, row_bytes);
    }
    GlobalUnlock(hMem);

    if (OpenClipboard(g_hViewer)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, hMem)) {
            GlobalFree(hMem);
        }
        CloseClipboard();
        wcsncpy_s(g_status_flash, 96, L"Copied image to clipboard", _TRUNCATE);
        g_status_flash_until = GetTickCount() + 2000;
        InvalidateRect(g_hViewer, NULL, FALSE);
    } else {
        GlobalFree(hMem);
    }
}

static void viewer_update_layout(int w, int h) {
    int title_h = 38;
    int tb_h = 42;
    int stat_h = 26;

    int cap_w = 46;
    for (int i = 0; i < 3; i++) {
        int cx = w - (3 - i) * cap_w;
        g_rc_cap[i].left   = cx;
        g_rc_cap[i].top    = 0;
        g_rc_cap[i].right  = cx + cap_w;
        g_rc_cap[i].bottom = title_h;
    }

    /* Toolbar buttons:
     * 0:Prev, 1:Next | 2:ZoomOut, 3:ZoomIn, 4:Fit, 5:1:1 | 6:Rotate, 7:Copy | Right: 8:External */
    int ty = title_h + 6;
    int bx = 12;
    for (int b = 0; b < 8; b++) {
        if (b == 2 || b == 6) bx += 10; /* subtle visual group gap */
        int bw = (b == 5) ? 36 : 30;
        g_rc_tb[b].left   = bx;
        g_rc_tb[b].top    = ty;
        g_rc_tb[b].right  = bx + bw;
        g_rc_tb[b].bottom = ty + 30;
        bx += bw + 4;
    }

    g_rc_tb[8].left   = w - 42;
    g_rc_tb[8].top    = ty;
    g_rc_tb[8].right  = w - 12;
    g_rc_tb[8].bottom = ty + 30;

    g_rc_view.left   = 0;
    g_rc_view.top    = title_h + tb_h;
    g_rc_view.right  = w;
    g_rc_view.bottom = max(g_rc_view.top + 40, h - stat_h);

    int mid_y = (g_rc_view.top + g_rc_view.bottom) / 2;
    g_rc_nav_l.left   = 14;
    g_rc_nav_l.top    = mid_y - 26;
    g_rc_nav_l.right  = 50;
    g_rc_nav_l.bottom = mid_y + 26;

    g_rc_nav_r.left   = w - 50;
    g_rc_nav_r.top    = mid_y - 26;
    g_rc_nav_r.right  = w - 14;
    g_rc_nav_r.bottom = mid_y + 26;
}

static void viewer_draw(LayerSurface *fb, int w, int h) {
    HWND hwnd = g_hViewer;
    (void)hwnd;
    viewer_update_layout(w, h);
    {
        GpGraphics *g = fb->g;
        GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
        GdipSetTextRenderingHint(g, GP_TEXT_ANTIALIAS);

        ARGB c_sb     = v_argb(255, g_theme.files_sidebar);
        ARGB c_tb     = v_argb(255, g_theme.files_toolbar);
        ARGB c_bdr    = v_argb(255, g_theme.files_border);
        ARGB c_txt    = v_argb(255, g_theme.files_text);
        ARGB c_muted  = v_argb(255, g_theme.files_text_muted);
        ARGB c_hov    = v_argb(255, g_theme.files_hover_bg);
        ARGB c_sel    = v_argb(255, g_theme.files_sel_bg);
        ARGB c_canvas = g_theme.is_dark ? MAKE_ARGB(255, 20, 20, 24) : MAKE_ARGB(255, 238, 240, 244);

        /* 1. Seamless Titlebar (38px) + Toolbar (42px) */
        draw_round_rect_fill(g, 0.0f, 0.0f, (float)w, 38.0f, 0.0f, c_sb);
        draw_round_rect_fill(g, 0.0f, 38.0f, (float)w, 42.0f, 0.0f, c_tb);
        draw_line(g, 0.0f, 38.0f, (float)w, 38.0f, c_bdr, 1.0f);
        draw_line(g, 0.0f, 79.5f, (float)w, 79.5f, c_bdr, 1.0f);

        draw_icon_glyph(g, 0xEB9F, 14.5f, g_theme.accent, 14.0f, 0.0f, 22.0f, 38.0f);
        wchar_t hdr[MAX_PATH + 32];
        if (g_folder_count > 0 && g_folder_idx >= 0) {
            _snwprintf_s(hdr, MAX_PATH + 32, _TRUNCATE, L"%ls   (%d / %d)",
                         g_cur_name, g_folder_idx + 1, g_folder_count);
        } else {
            wcsncpy_s(hdr, MAX_PATH + 32, g_cur_name[0] ? g_cur_name : L"Image Viewer", _TRUNCATE);
        }
        draw_text(g, hdr, L"Segoe UI", 12.5f, GP_FONT_BOLD, c_txt,
                  40.0f, 0.0f, (float)(w - 196), 38.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);

        /* Windows 11 Caption Controls */
        {
            bool zoomed = IsZoomed(hwnd) != 0;
            wchar_t cglyphs[3] = { 0xE921, zoomed ? 0xE923 : 0xE922, 0xE8BB };
            for (int c = 0; c < 3; c++) {
                const RECT *rc = &g_rc_cap[c];
                bool hov = (g_hover_cap == c + 1);
                if (hov) {
                    ARGB bg = (c == 2) ? MAKE_ARGB(255, 232, 17, 35) : c_hov;
                    draw_round_rect_fill(g, (float)rc->left, (float)rc->top,
                                         (float)(rc->right - rc->left), (float)(rc->bottom - rc->top), 0.0f, bg);
                }
                ARGB fg = (hov && c == 2) ? MAKE_ARGB(255, 255, 255, 255) : c_txt;
                draw_icon_glyph(g, cglyphs[c], 10.0f, fg, (float)rc->left, (float)rc->top,
                                (float)(rc->right - rc->left), (float)(rc->bottom - rc->top));
            }
        }

        /* 2. Toolbar Controls */
        {
            static const wchar_t tb_glyphs[9] = {
                0xE76B, 0xE76C, 0xE738, 0xE710, 0xE9A6, 0, 0xE7AD, 0xE8C8, 0xE8A7
            };
            for (int b = 0; b < 9; b++) {
                const RECT *r = &g_rc_tb[b];
                bool disabled = ((b == 0 || b == 1) && g_folder_count <= 1) ||
                                ((b >= 2 && b <= 7) && !g_img_bmp);
                bool active = (b == 4 && g_fit_mode && g_img_bmp);
                if (active) {
                    draw_round_rect_fill(g, (float)r->left, (float)r->top,
                                         (float)(r->right - r->left), (float)(r->bottom - r->top), 6.0f, c_sel);
                } else if (g_hover_tb == b && !disabled) {
                    draw_round_rect_fill(g, (float)r->left, (float)r->top,
                                         (float)(r->right - r->left), (float)(r->bottom - r->top), 6.0f, c_hov);
                }
                ARGB fg = disabled ? c_muted : (active ? g_theme.accent : c_txt);
                if (b == 5) {
                    draw_text(g, L"1:1", L"Segoe UI", 11.5f, GP_FONT_BOLD, fg,
                              (float)r->left, (float)r->top, (float)(r->right - r->left), (float)(r->bottom - r->top),
                              GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
                } else {
                    draw_icon_glyph(g, tb_glyphs[b], 13.5f, fg, (float)r->left, (float)r->top,
                                    (float)(r->right - r->left), (float)(r->bottom - r->top));
                }
            }

            /* Toolbar dividers */
            draw_line(g, (float)(g_rc_tb[1].right + 7), 47.0f, (float)(g_rc_tb[1].right + 7), 71.0f, c_bdr, 1.0f);
            draw_line(g, (float)(g_rc_tb[5].right + 7), 47.0f, (float)(g_rc_tb[5].right + 7), 71.0f, c_bdr, 1.0f);

            /* Zoom readout badge */
            if (g_img_bmp && g_orig_w > 0) {
                float z = viewer_effective_zoom();
                int pct = (int)((((float)g_dec_w * z) / (float)g_orig_w) * 100.0f + 0.5f);
                wchar_t ztxt[48];
                _snwprintf_s(ztxt, 48, _TRUNCATE, g_fit_mode ? L"Fit  %d%%" : L"%d%%", pct);
                float zx = (float)(w - 134);
                draw_round_rect_fill(g, zx, 46.0f, 84.0f, 26.0f, 6.0f, v_argb(255, g_theme.files_addrbar));
                draw_round_rect_stroke(g, zx + 0.5f, 46.5f, 83.0f, 25.0f, 6.0f, c_bdr, 1.0f);
                draw_text(g, ztxt, L"Segoe UI", 11.5f, GP_FONT_BOLD, c_txt,
                          zx, 46.0f, 84.0f, 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
            }
        }

        /* 3. Image Viewport */
        draw_round_rect_fill(g, (float)g_rc_view.left, (float)g_rc_view.top,
                             (float)(g_rc_view.right - g_rc_view.left),
                             (float)(g_rc_view.bottom - g_rc_view.top), 0.0f, c_canvas);

        GdipSetClipRect(g, (float)g_rc_view.left, (float)g_rc_view.top,
                        (float)(g_rc_view.right - g_rc_view.left),
                        (float)(g_rc_view.bottom - g_rc_view.top), GP_COMBINE_REPLACE);

        if (g_img_bmp && g_dec_w > 0 && g_dec_h > 0) {
            float z = viewer_effective_zoom();
            float dw = (float)g_dec_w * z;
            float dh = (float)g_dec_h * z;
            float vcx = (float)(g_rc_view.left + g_rc_view.right) * 0.5f;
            float vcy = (float)(g_rc_view.top + g_rc_view.bottom) * 0.5f;
            float ix = vcx + g_pan_x - dw * 0.5f;
            float iy = vcy + g_pan_y - dh * 0.5f;

            /* Subtle neutral backing behind transparent PNG/ICO/WEBP so alpha edges stay crisp */
            ARGB img_bg = g_theme.is_dark ? MAKE_ARGB(255, 34, 34, 40) : MAKE_ARGB(255, 224, 227, 234);
            draw_round_rect_fill(g, ix, iy, dw, dh, 0.0f, img_bg);

            /* Crisp pixel rendering when zoomed past 2.5x, high-quality bicubic otherwise */
            GdipSetInterpolationMode(g, (z >= 2.5f) ? GP_INTERP_NEAREST : GP_INTERP_HIGHQUALITY_BICUBIC);
            GdipDrawImageRect(g, (GpImage *)g_img_bmp, ix, iy, dw, dh);
            draw_round_rect_stroke(g, ix - 0.5f, iy - 0.5f, dw + 1.0f, dh + 1.0f, 0.0f, c_bdr, 1.0f);
        } else if (g_load_failed) {
            draw_icon_glyph(g, 0xE783, 30.0f, c_muted, (float)g_rc_view.left,
                            (float)(g_rc_view.top + (g_rc_view.bottom - g_rc_view.top) / 2 - 42),
                            (float)(g_rc_view.right - g_rc_view.left), 38.0f);
            draw_text(g, L"Unable to decode this image file", L"Segoe UI", 13.5f, GP_FONT_REGULAR, c_muted,
                      (float)g_rc_view.left, (float)(g_rc_view.top + (g_rc_view.bottom - g_rc_view.top) / 2 + 4),
                      (float)(g_rc_view.right - g_rc_view.left), 26.0f, GP_ALIGN_CENTER, GP_ALIGN_CENTER, false);
        }

        /* Subtle Left/Right edge browse pills when folder has multiple images */
        if (g_folder_count > 1) {
            ARGB pill_bg_l = (g_hover_nav == -1) ? MAKE_ARGB(215, 28, 28, 34) : MAKE_ARGB(120, 28, 28, 34);
            ARGB pill_bg_r = (g_hover_nav ==  1) ? MAKE_ARGB(215, 28, 28, 34) : MAKE_ARGB(120, 28, 28, 34);
            ARGB pill_fg   = MAKE_ARGB(245, 255, 255, 255);
            draw_round_rect_fill(g, (float)g_rc_nav_l.left, (float)g_rc_nav_l.top,
                                 (float)(g_rc_nav_l.right - g_rc_nav_l.left),
                                 (float)(g_rc_nav_l.bottom - g_rc_nav_l.top), 18.0f, pill_bg_l);
            draw_icon_glyph(g, 0xE76B, 14.0f, pill_fg, (float)g_rc_nav_l.left, (float)g_rc_nav_l.top,
                            (float)(g_rc_nav_l.right - g_rc_nav_l.left),
                            (float)(g_rc_nav_l.bottom - g_rc_nav_l.top));

            draw_round_rect_fill(g, (float)g_rc_nav_r.left, (float)g_rc_nav_r.top,
                                 (float)(g_rc_nav_r.right - g_rc_nav_r.left),
                                 (float)(g_rc_nav_r.bottom - g_rc_nav_r.top), 18.0f, pill_bg_r);
            draw_icon_glyph(g, 0xE76C, 14.0f, pill_fg, (float)g_rc_nav_r.left, (float)g_rc_nav_r.top,
                            (float)(g_rc_nav_r.right - g_rc_nav_r.left),
                            (float)(g_rc_nav_r.bottom - g_rc_nav_r.top));
        }

        GdipResetClip(g);

        /* 4. Status Bar */
        int sty = h - 26;
        draw_round_rect_fill(g, 0.0f, (float)sty, (float)w, 26.0f, 0.0f, c_tb);
        draw_line(g, 0.0f, (float)sty + 0.5f, (float)w, (float)sty + 0.5f, c_bdr, 1.0f);

        static const wchar_t *tb_tips[9] = {
            L"Previous Image (Left Arrow)", L"Next Image (Right Arrow)",
            L"Zoom Out (-)", L"Zoom In (+)",
            L"Fit to Window (0 or F)", L"Actual Size 100% (1)",
            L"Rotate 90 Degrees (R)", L"Copy Image to Clipboard (Ctrl+C)",
            L"Open with External App"
        };

        wchar_t left_st[192] = {0};
        if (g_status_flash[0] && GetTickCount() < g_status_flash_until) {
            wcsncpy_s(left_st, 192, g_status_flash, _TRUNCATE);
        } else if (g_hover_tb >= 0 && g_hover_tb < 9) {
            wcsncpy_s(left_st, 192, tb_tips[g_hover_tb], _TRUNCATE);
        } else if (g_img_bmp) {
            wchar_t sz_s[32];
            viewer_format_bytes(g_cur_file_size, sz_s, 32);
            if (g_was_downscaled) {
                _snwprintf_s(left_st, 192, _TRUNCATE, L"%d x %d px  |  %ls  |  Low-RAM preview (%d x %d)",
                             g_orig_w, g_orig_h, sz_s, g_dec_w, g_dec_h);
            } else {
                _snwprintf_s(left_st, 192, _TRUNCATE, L"%d x %d px  |  %ls", g_orig_w, g_orig_h, sz_s);
            }
        } else {
            wcsncpy_s(left_st, 192, g_cur_path, _TRUNCATE);
        }

        draw_text(g, left_st, L"Segoe UI", 11.5f, GP_FONT_REGULAR, c_muted,
                  14.0f, (float)sty, (float)(w / 2 + 100), 26.0f, GP_ALIGN_NEAR, GP_ALIGN_CENTER, true);
        draw_text(g, L"Scroll: Zoom  |  Drag: Pan  |  Arrows: Browse  |  Esc: Close",
                  L"Segoe UI", 11.0f, GP_FONT_REGULAR, c_muted,
                  (float)(w / 2), (float)sty, (float)(w / 2 - 14), 26.0f, GP_ALIGN_FAR, GP_ALIGN_CENTER, true);
    }
}

static void viewer_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        viewer_draw(&fb, cr.right, cr.bottom);
        opaque_frame_end(hdc, &fb);
    }
    EndPaint(hwnd, &ps);
}

/* The viewer drawn into a picture at its window's size, for checks. */
int viewer_snapshot(const wchar_t *png_path) {
    if (!g_hViewer) return 0;
    RECT cr;
    GetClientRect(g_hViewer, &cr);
    LayerSurface fb;
    ZeroMemory(&fb, sizeof(fb));
    int ok = 0;
    if (opaque_frame_begin(&fb, cr.right, cr.bottom)) {
        viewer_draw(&fb, cr.right, cr.bottom);
        ok = surface_save_png(&fb, png_path);
        surface_destroy(&fb);
    }
    return ok;
}

static LRESULT CALLBACK viewer_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
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
            return 0;
        }
        return 0;

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
                if (PtInRect(&g_rc_cap[c], cl)) return HTCLIENT;
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_SIZE:
        if (g_fit_mode) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            viewer_update_layout(cr.right, cr.bottom);
            g_zoom = viewer_compute_fit_zoom();
            g_pan_x = 0.0f;
            g_pan_y = 0.0f;
        } else {
            viewer_clamp_pan();
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        viewer_paint(hwnd);
        return 0;

    case WM_MOUSEMOVE: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        if (!g_tracking_mouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking_mouse = true;
        }
        if (g_dragging) {
            g_pan_x = g_drag_pan_x + (float)(pt.x - g_drag_start.x);
            g_pan_y = g_drag_pan_y + (float)(pt.y - g_drag_start.y);
            viewer_clamp_pan();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        int ncap = 0, ntb = -1, nnav = 0;
        for (int c = 0; c < 3; c++) {
            if (PtInRect(&g_rc_cap[c], pt)) { ncap = c + 1; break; }
        }
        for (int b = 0; b < 9; b++) {
            if (PtInRect(&g_rc_tb[b], pt)) { ntb = b; break; }
        }
        if (g_folder_count > 1) {
            if (PtInRect(&g_rc_nav_l, pt)) nnav = -1;
            else if (PtInRect(&g_rc_nav_r, pt)) nnav = 1;
        }
        if (ncap != g_hover_cap || ntb != g_hover_tb || nnav != g_hover_nav) {
            g_hover_cap = ncap;
            g_hover_tb = ntb;
            g_hover_nav = nnav;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking_mouse = false;
        g_hover_cap = 0;
        g_hover_tb = -1;
        g_hover_nav = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_dragging) {
            SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        SetFocus(hwnd);

        for (int c = 0; c < 3; c++) {
            if (PtInRect(&g_rc_cap[c], pt)) {
                if (c == 0) ShowWindow(hwnd, SW_MINIMIZE);
                else if (c == 1) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                else PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
        }

        for (int b = 0; b < 9; b++) {
            if (PtInRect(&g_rc_tb[b], pt)) {
                int vcx = (g_rc_view.left + g_rc_view.right) / 2;
                int vcy = (g_rc_view.top + g_rc_view.bottom) / 2;
                if (b == 0)      viewer_step(-1);
                else if (b == 1) viewer_step(1);
                else if (b == 2) viewer_zoom_at(1.0f / 1.25f, vcx, vcy);
                else if (b == 3) viewer_zoom_at(1.25f, vcx, vcy);
                else if (b == 4) viewer_set_fit();
                else if (b == 5) viewer_set_actual_size();
                else if (b == 6) viewer_rotate_cw();
                else if (b == 7) viewer_copy_to_clipboard();
                else if (b == 8 && g_cur_path[0]) {
                    ShellExecuteW(hwnd, L"open", g_cur_path, NULL, g_cur_dir, SW_SHOWNORMAL);
                }
                return 0;
            }
        }

        if (g_folder_count > 1) {
            if (PtInRect(&g_rc_nav_l, pt)) { viewer_step(-1); return 0; }
            if (PtInRect(&g_rc_nav_r, pt)) { viewer_step(1);  return 0; }
        }

        if (PtInRect(&g_rc_view, pt) && g_img_bmp) {
            g_dragging = true;
            g_drag_start = pt;
            g_drag_pan_x = g_pan_x;
            g_drag_pan_y = g_pan_y;
            SetCapture(hwnd);
            SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        if (PtInRect(&g_rc_view, pt) && g_img_bmp) {
            if (g_fit_mode) viewer_set_actual_size();
            else viewer_set_fit();
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float factor = (delta > 0) ? 1.18f : (1.0f / 1.18f);
        viewer_zoom_at(factor, pt.x, pt.y);
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        int vcx = (g_rc_view.left + g_rc_view.right) / 2;
        int vcy = (g_rc_view.top + g_rc_view.bottom) / 2;
        if (wParam == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_PRIOR)  { viewer_step(-1); return 0; }
        if (wParam == VK_RIGHT || wParam == VK_NEXT)  { viewer_step(1);  return 0; }
        if (wParam == VK_OEM_PLUS || wParam == VK_ADD)      { viewer_zoom_at(1.22f, vcx, vcy); return 0; }
        if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT){ viewer_zoom_at(1.0f / 1.22f, vcx, vcy); return 0; }
        if (wParam == '0' || wParam == VK_NUMPAD0 || wParam == 'F') { viewer_set_fit(); return 0; }
        if (wParam == '1' || wParam == VK_NUMPAD1) { viewer_set_actual_size(); return 0; }
        if (wParam == 'R') { viewer_rotate_cw(); return 0; }
        if (ctrl && wParam == 'C') { viewer_copy_to_clipboard(); return 0; }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
        }
        viewer_free_image();
        viewer_free_folder_list();
        g_cur_path[0] = L'\0';
        g_cur_dir[0] = L'\0';
        /* The program ends with its last window. */
        if (g_hHostWnd) PostMessageW(g_hHostWnd, WM_LARES_WINDOWS_CHANGED, 0, 0);
        g_cur_name[0] = L'\0';
        g_status_flash[0] = L'\0';
        g_hViewer = NULL;
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void viewer_init(HINSTANCE hInst) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = viewer_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = VIEWER_CLASS;
    RegisterClassExW(&wc);
}

void viewer_open(const wchar_t *file_path) {
    if (!file_path || !file_path[0]) return;

    if (!g_hViewer || !IsWindow(g_hViewer)) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int w = min(1060, (wa.right - wa.left) - 100);
        int h = min(720, (wa.bottom - wa.top) - 90);
        if (w < 680) w = 680;
        if (h < 480) h = 480;
        int x = wa.left + ((wa.right - wa.left) - w) / 2;
        int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

        /* A test copy's window sits far off the screen and out of Alt+Tab. */
        if (g_cfg.isolated) x = -30000;
        g_hViewer = CreateWindowExW(
            g_cfg.isolated ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW,
            VIEWER_CLASS,
            L"Image Viewer",
            WS_OVERLAPPEDWINDOW,
            x, y, w, h,
            NULL, NULL, g_hInst, NULL
        );
        if (!g_hViewer) return;

        BOOL dark = g_theme.is_dark ? TRUE : FALSE;
        DwmSetWindowAttribute(g_hViewer, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        DWORD corner = 2 /* DWMWCP_ROUND */;
        DwmSetWindowAttribute(g_hViewer, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
        SetWindowPos(g_hViewer, NULL, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        RECT cr;
        GetClientRect(g_hViewer, &cr);
        viewer_update_layout(cr.right, cr.bottom);
    }

    viewer_load_path(file_path, true);
    if (g_cfg.isolated) {
        ShowWindow(g_hViewer, SW_SHOWNOACTIVATE);
        return;
    }
    ShowWindow(g_hViewer, IsIconic(g_hViewer) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hViewer);
}

HWND viewer_window(void) {
    return g_hViewer;
}

void viewer_on_theme_changed(void) {
    if (g_hViewer && IsWindow(g_hViewer)) {
        BOOL dark = g_theme.is_dark ? TRUE : FALSE;
        DwmSetWindowAttribute(g_hViewer, 20, &dark, sizeof(dark));
        InvalidateRect(g_hViewer, NULL, FALSE);
    }
}

bool viewer_is_open(void) {
    return (g_hViewer != NULL && IsWindow(g_hViewer));
}

void viewer_shutdown(void) {
    if (g_hViewer && IsWindow(g_hViewer)) {
        DestroyWindow(g_hViewer);
    }
    viewer_free_image();
    viewer_free_folder_list();
    g_hViewer = NULL;
}
