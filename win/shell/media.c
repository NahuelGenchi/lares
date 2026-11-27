// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell.h"
#include <roapi.h>
#include <winstring.h>
#include <windows.media.control.h>
#include <windows.storage.streams.h>
#include <shcore.h>

/*
 * Pure C WinRT GlobalSystemMediaTransportControls (GSMTC) client.
 * Runs cleanly on a lightweight background MTA thread when polled so UI never blocks,
 * reading track title, artist, play/pause state, and album art thumbnail with near-zero RAM.
 */

static const GUID LARES_IID_GSMTCManagerStatics =
    { 0x2050C4EE, 0x11A0, 0x57DE, { 0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45 } };
static const GUID LARES_IID_IAsyncInfo =
    { 0x00000036, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

static CRITICAL_SECTION g_media_cs;
static MediaState g_media;
static HWND g_media_notify = NULL;
static HANDLE g_media_thread = NULL;
static HANDLE g_media_evt = NULL;
static volatile LONG g_media_stop = 0;
static volatile LONG g_media_cmd = 0; /* 1=prev, 2=playpause, 3=next */
static wchar_t g_last_thumb_key[360] = {0};

static bool wait_async_op(IInspectable *op, DWORD timeout_ms) {
    if (!op) return false;
    IAsyncInfo *info = NULL;
    HRESULT hr = op->lpVtbl->QueryInterface(op, &LARES_IID_IAsyncInfo, (void **)&info);
    if (FAILED(hr) || !info) return false;

    DWORD start = GetTickCount();
    AsyncStatus st = Started;
    while (GetTickCount() - start < timeout_ms && !InterlockedCompareExchange(&g_media_stop, 0, 0)) {
        hr = info->lpVtbl->get_Status(info, &st);
        if (FAILED(hr) || st != Started) break;
        Sleep(10);
    }
    info->lpVtbl->Release(info);
    return (st == Completed);
}

static GpBitmap *load_winrt_thumbnail(__x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStreamReference *ref) {
    if (!ref) return NULL;
    __FIAsyncOperation_1_Windows__CStorage__CStreams__CIRandomAccessStreamWithContentType *op = NULL;
    HRESULT hr = ref->lpVtbl->OpenReadAsync(ref, &op);
    if (FAILED(hr) || !op) return NULL;

    GpBitmap *bmp = NULL;
    if (wait_async_op((IInspectable *)op, 1200)) {
        __x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStreamWithContentType *stream = NULL;
        if (SUCCEEDED(op->lpVtbl->GetResults(op, &stream)) && stream) {
            IStream *com_stream = NULL;
            /* CreateStreamOverRandomAccessStream requires shcore.dll */
            typedef HRESULT (WINAPI *PFN_CreateStreamOverRAS)(IUnknown *, REFIID, void **);
            HMODULE hShcore = GetModuleHandleW(L"shcore.dll");
            if (!hShcore) hShcore = LoadLibraryW(L"shcore.dll");
            if (hShcore) {
                PFN_CreateStreamOverRAS fn = (PFN_CreateStreamOverRAS)GetProcAddress(hShcore, "CreateStreamOverRandomAccessStream");
                if (fn && SUCCEEDED(fn((IUnknown *)stream, &IID_IStream, (void **)&com_stream)) && com_stream) {
                    GdipCreateBitmapFromStream(com_stream, &bmp);
                    IStream_Release(com_stream);
                }
            }
            stream->lpVtbl->Release(stream);
        }
    }
    op->lpVtbl->Release(op);
    return bmp;
}

static void hstring_to_buf(HSTRING hs, wchar_t *dst, size_t dst_len) {
    dst[0] = L'\0';
    if (!hs) return;
    UINT32 len = 0;
    const wchar_t *raw = WindowsGetStringRawBuffer(hs, &len);
    if (raw && len > 0) {
        wcsncpy_s(dst, dst_len, raw, _TRUNCATE);
    }
}

static DWORD WINAPI media_worker_thread(LPVOID lp) {
    (void)lp;
    RoInitialize(RO_INIT_MULTITHREADED);

    __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSessionManager *mgr = NULL;
    HSTRING class_name = NULL;
    HSTRING_HEADER hdr;
    const wchar_t *cls = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    if (SUCCEEDED(WindowsCreateStringReference(cls, (UINT32)wcslen(cls), &hdr, &class_name))) {
        __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSessionManagerStatics *statics = NULL;
        if (SUCCEEDED(RoGetActivationFactory(class_name, &LARES_IID_GSMTCManagerStatics, (void **)&statics)) && statics) {
            __FIAsyncOperation_1_Windows__CMedia__CControl__CGlobalSystemMediaTransportControlsSessionManager *op = NULL;
            if (SUCCEEDED(statics->lpVtbl->RequestAsync(statics, &op)) && op) {
                if (wait_async_op((IInspectable *)op, 2500)) {
                    op->lpVtbl->GetResults(op, &mgr);
                }
                op->lpVtbl->Release(op);
            }
            statics->lpVtbl->Release(statics);
        }
    }

    while (!InterlockedCompareExchange(&g_media_stop, 0, 0)) {
        LONG cmd = InterlockedExchange(&g_media_cmd, 0);

        __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSession *sess = NULL;
        if (mgr) {
            mgr->lpVtbl->GetCurrentSession(mgr, &sess);
        }

        if (cmd != 0) {
            bool handled = false;
            if (sess) {
                __FIAsyncOperation_1_boolean *bop = NULL;
                HRESULT hr = E_FAIL;
                if (cmd == 1) hr = sess->lpVtbl->TrySkipPreviousAsync(sess, &bop);
                else if (cmd == 2) hr = sess->lpVtbl->TryTogglePlayPauseAsync(sess, &bop);
                else if (cmd == 3) hr = sess->lpVtbl->TrySkipNextAsync(sess, &bop);
                if (SUCCEEDED(hr) && bop) {
                    wait_async_op((IInspectable *)bop, 800);
                    bop->lpVtbl->Release(bop);
                    handled = true;
                }
            }
            if (!handled) {
                BYTE vk = (cmd == 1) ? VK_MEDIA_PREV_TRACK
                        : (cmd == 2) ? VK_MEDIA_PLAY_PAUSE : VK_MEDIA_NEXT_TRACK;
                keybd_event(vk, 0, 0, 0);
                keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
            }
            Sleep(120);
        }

        bool has_sess = false;
        bool playing = false;
        wchar_t title[160] = {0};
        wchar_t artist[160] = {0};
        wchar_t umid[256] = {0};
        GpBitmap *new_thumb = NULL;
        bool thumb_changed = false;

        if (sess) {
            has_sess = true;
            HSTRING hUmid = NULL;
            if (SUCCEEDED(sess->lpVtbl->get_SourceAppUserModelId(sess, &hUmid)) && hUmid) {
                hstring_to_buf(hUmid, umid, 256);
                WindowsDeleteString(hUmid);
            }

            __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSessionPlaybackInfo *pinfo = NULL;
            if (SUCCEEDED(sess->lpVtbl->GetPlaybackInfo(sess, &pinfo)) && pinfo) {
                __x_ABI_CWindows_CMedia_CControl_CGlobalSystemMediaTransportControlsSessionPlaybackStatus st;
                if (SUCCEEDED(pinfo->lpVtbl->get_PlaybackStatus(pinfo, &st))) {
                    playing = (st == GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing);
                }
                pinfo->lpVtbl->Release(pinfo);
            }

            __FIAsyncOperation_1_Windows__CMedia__CControl__CGlobalSystemMediaTransportControlsSessionMediaProperties *pop = NULL;
            if (SUCCEEDED(sess->lpVtbl->TryGetMediaPropertiesAsync(sess, &pop)) && pop) {
                if (wait_async_op((IInspectable *)pop, 1200)) {
                    __x_ABI_CWindows_CMedia_CControl_CIGlobalSystemMediaTransportControlsSessionMediaProperties *props = NULL;
                    if (SUCCEEDED(pop->lpVtbl->GetResults(pop, &props)) && props) {
                        HSTRING hTitle = NULL, hArtist = NULL;
                        if (SUCCEEDED(props->lpVtbl->get_Title(props, &hTitle)) && hTitle) {
                            hstring_to_buf(hTitle, title, 160);
                            WindowsDeleteString(hTitle);
                        }
                        if (SUCCEEDED(props->lpVtbl->get_Artist(props, &hArtist)) && hArtist) {
                            hstring_to_buf(hArtist, artist, 160);
                            WindowsDeleteString(hArtist);
                        }
                        wchar_t tkey[360];
                        _snwprintf_s(tkey, 360, _TRUNCATE, L"%ls|%ls|%ls", umid, title, artist);
                        if (wcscmp(tkey, g_last_thumb_key) != 0) {
                            wcsncpy_s(g_last_thumb_key, 360, tkey, _TRUNCATE);
                            thumb_changed = true;
                            __x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStreamReference *tref = NULL;
                            if (SUCCEEDED(props->lpVtbl->get_Thumbnail(props, &tref)) && tref) {
                                new_thumb = load_winrt_thumbnail(tref);
                                tref->lpVtbl->Release(tref);
                            }
                        }
                        props->lpVtbl->Release(props);
                    }
                }
                pop->lpVtbl->Release(pop);
            }
            sess->lpVtbl->Release(sess);
        } else if (g_last_thumb_key[0]) {
            g_last_thumb_key[0] = L'\0';
            thumb_changed = true;
        }

        bool changed = false;
        EnterCriticalSection(&g_media_cs);
        if (g_media.has_session != has_sess || g_media.is_playing != playing ||
            wcscmp(g_media.title, title) != 0 || wcscmp(g_media.artist, artist) != 0 ||
            thumb_changed) {
            changed = true;
            g_media.has_session = has_sess;
            g_media.is_playing = playing;
            wcsncpy_s(g_media.title, 160, title, _TRUNCATE);
            wcsncpy_s(g_media.artist, 160, artist, _TRUNCATE);
            wcsncpy_s(g_media.app_umid, 256, umid, _TRUNCATE);
            if (thumb_changed) {
                if (g_media.thumbnail) {
                    GdipDisposeImage((GpImage *)g_media.thumbnail);
                    g_media.thumbnail = NULL;
                }
                g_media.thumbnail = new_thumb;
                new_thumb = NULL;
                g_media.thumb_version++;
            }
        }
        LeaveCriticalSection(&g_media_cs);

        if (new_thumb) GdipDisposeImage((GpImage *)new_thumb);
        if (changed && g_media_notify) {
            PostMessageW(g_media_notify, WM_LARES_MEDIA_UPDATED, 0, 0);
        }

        WaitForSingleObject(g_media_evt, 2500);
    }

    if (mgr) mgr->lpVtbl->Release(mgr);
    RoUninitialize();
    return 0;
}

/* Only while the dock shows the media controls; until then every reading is
 * empty. Read and set on the shell's own thread only. */
static bool g_media_ready = false;

void media_init(HWND notify_hwnd) {
    if (g_media_ready) return;
    InitializeCriticalSection(&g_media_cs);
    g_media_ready = true;
    ZeroMemory(&g_media, sizeof(g_media));
    g_media_notify = notify_hwnd;
    g_media_stop = 0;
    g_media_evt = CreateEventW(NULL, FALSE, TRUE, NULL);
    g_media_thread = CreateThread(NULL, 0, media_worker_thread, NULL, 0, NULL);
}

void media_poll(void) {
    if (g_media_evt) SetEvent(g_media_evt);
}

void media_get_state(MediaState *out) {
    if (!g_media_ready) {
        ZeroMemory(out, sizeof(*out));
        return;
    }
    EnterCriticalSection(&g_media_cs);
    *out = g_media;
    out->thumbnail = NULL; /* callers must use media_lock_thumbnail() */
    LeaveCriticalSection(&g_media_cs);
}

GpBitmap *media_lock_thumbnail(void) {
    if (!g_media_ready) return NULL;
    EnterCriticalSection(&g_media_cs);
    return g_media.thumbnail;
}

void media_unlock_thumbnail(void) {
    if (g_media_ready) LeaveCriticalSection(&g_media_cs);
}

void media_prev(void) {
    InterlockedExchange(&g_media_cmd, 1);
    if (g_media_evt) SetEvent(g_media_evt);
}

void media_play_pause(void) {
    InterlockedExchange(&g_media_cmd, 2);
    if (g_media_evt) SetEvent(g_media_evt);
}

void media_next(void) {
    InterlockedExchange(&g_media_cmd, 3);
    if (g_media_evt) SetEvent(g_media_evt);
}

void media_shutdown(void) {
    if (!g_media_ready) return;
    InterlockedExchange(&g_media_stop, 1);
    if (g_media_evt) SetEvent(g_media_evt);
    bool thread_exited = true;
    if (g_media_thread) {
        thread_exited = (WaitForSingleObject(g_media_thread, 1500) == WAIT_OBJECT_0);
        CloseHandle(g_media_thread);
        g_media_thread = NULL;
    }
    if (g_media_evt) {
        CloseHandle(g_media_evt);
        g_media_evt = NULL;
    }
    if (thread_exited) {
        EnterCriticalSection(&g_media_cs);
        if (g_media.thumbnail) {
            GdipDisposeImage((GpImage *)g_media.thumbnail);
            g_media.thumbnail = NULL;
        }
        LeaveCriticalSection(&g_media_cs);
        DeleteCriticalSection(&g_media_cs);
        g_media_ready = false;
    }
}
