// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef LARES_SHELL_DWRITE_C_H
#define LARES_SHELL_DWRITE_C_H

/* The part of DirectWrite (dwrite.dll) the shell uses, declared for C: the
 * Windows SDK's DirectWrite headers are C++ only. Slots the shell never
 * calls are padding, so the order of every interface must match dwrite.h.
 * lares-shell-draw-test draws and measures through them on a real factory. */

#include <windows.h>
#include <unknwn.h>
#include <stdint.h>

typedef struct IDWriteFactory          IDWriteFactory;
typedef struct IDWriteFontCollection   IDWriteFontCollection;
typedef struct IDWriteTextFormat       IDWriteTextFormat;
typedef struct IDWriteTextLayout       IDWriteTextLayout;
typedef struct IDWriteInlineObject     IDWriteInlineObject;
typedef struct IDWriteTextRenderer     IDWriteTextRenderer;
typedef struct IDWriteGlyphRunAnalysis IDWriteGlyphRunAnalysis;

typedef int DW_FACTORY_TYPE;
#define DW_FACTORY_TYPE_SHARED 0

typedef int DW_FONT_WEIGHT;
#define DW_WEIGHT_REGULAR   400
#define DW_WEIGHT_MEDIUM    500
#define DW_WEIGHT_SEMIBOLD  600
#define DW_WEIGHT_BOLD      700

typedef int DW_FONT_STYLE;
#define DW_STYLE_NORMAL 0
#define DW_STYLE_ITALIC 2

typedef int DW_FONT_STRETCH;
#define DW_STRETCH_NORMAL 5

typedef int DW_TEXT_ALIGNMENT;
#define DW_ALIGN_LEADING  0
#define DW_ALIGN_TRAILING 1
#define DW_ALIGN_CENTER   2

typedef int DW_PARAGRAPH_ALIGNMENT;
#define DW_PALIGN_NEAR   0
#define DW_PALIGN_FAR    1
#define DW_PALIGN_CENTER 2

typedef int DW_WORD_WRAPPING;
#define DW_WRAP_WRAP 0
#define DW_WRAP_NONE 1

typedef int DW_TRIMMING_GRANULARITY;
#define DW_TRIM_NONE      0
#define DW_TRIM_CHARACTER 1

typedef int DW_MEASURING_MODE;
#define DW_MEASURE_NATURAL 0

typedef int DW_RENDERING_MODE;
#define DW_RENDER_NATURAL           4
#define DW_RENDER_NATURAL_SYMMETRIC 5

typedef int DW_TEXTURE_TYPE;
#define DW_TEX_ALIASED_1x1   0
#define DW_TEX_CLEARTYPE_3x1 1

typedef struct {
    FLOAT m11, m12, m21, m22, dx, dy;
} DW_MATRIX;

typedef struct {
    DW_TRIMMING_GRANULARITY granularity;
    UINT32 delimiter;
    UINT32 delimiterCount;
} DW_TRIMMING;

typedef struct {
    FLOAT left;
    FLOAT top;
    FLOAT width;
    FLOAT widthIncludingTrailingWhitespace;
    FLOAT height;
    FLOAT layoutWidth;
    FLOAT layoutHeight;
    UINT32 maxBidiReorderingDepth;
    UINT32 lineCount;
} DW_TEXT_METRICS;

/* IDWriteFontCollection */
typedef struct IDWriteFontCollectionVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDWriteFontCollection *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDWriteFontCollection *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDWriteFontCollection *This);
    UINT32  (STDMETHODCALLTYPE *GetFontFamilyCount)(IDWriteFontCollection *This);
    HRESULT (STDMETHODCALLTYPE *GetFontFamily)(IDWriteFontCollection *This, UINT32 index, void **family);
    HRESULT (STDMETHODCALLTYPE *FindFamilyName)(IDWriteFontCollection *This, const WCHAR *familyName, UINT32 *index, BOOL *exists);
    HRESULT (STDMETHODCALLTYPE *GetFontFromFontFace)(IDWriteFontCollection *This, void *fontFace, void **font);
} IDWriteFontCollectionVtbl;
struct IDWriteFontCollection { const IDWriteFontCollectionVtbl *lpVtbl; };

/* IDWriteInlineObject */
typedef struct IDWriteInlineObjectVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDWriteInlineObject *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDWriteInlineObject *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDWriteInlineObject *This);
    HRESULT (STDMETHODCALLTYPE *Draw)(IDWriteInlineObject *This, void *ctx, IDWriteTextRenderer *renderer,
                                      FLOAT originX, FLOAT originY, BOOL isSideways, BOOL isRightToLeft, IUnknown *effect);
    HRESULT (STDMETHODCALLTYPE *GetMetrics)(IDWriteInlineObject *This, void *metrics);
    HRESULT (STDMETHODCALLTYPE *GetOverhangMetrics)(IDWriteInlineObject *This, void *overhangs);
    HRESULT (STDMETHODCALLTYPE *GetBreakConditions)(IDWriteInlineObject *This, void *before, void *after);
} IDWriteInlineObjectVtbl;
struct IDWriteInlineObject { const IDWriteInlineObjectVtbl *lpVtbl; };

/* IDWriteTextFormat (slots 0..27) */
typedef struct IDWriteTextFormatVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDWriteTextFormat *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDWriteTextFormat *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDWriteTextFormat *This);
    HRESULT (STDMETHODCALLTYPE *SetTextAlignment)(IDWriteTextFormat *This, DW_TEXT_ALIGNMENT align);
    HRESULT (STDMETHODCALLTYPE *SetParagraphAlignment)(IDWriteTextFormat *This, DW_PARAGRAPH_ALIGNMENT align);
    HRESULT (STDMETHODCALLTYPE *SetWordWrapping)(IDWriteTextFormat *This, DW_WORD_WRAPPING wrap);
    HRESULT (STDMETHODCALLTYPE *SetReadingDirection)(IDWriteTextFormat *This, int dir);
    HRESULT (STDMETHODCALLTYPE *SetFlowDirection)(IDWriteTextFormat *This, int dir);
    HRESULT (STDMETHODCALLTYPE *SetIncrementalTabStop)(IDWriteTextFormat *This, FLOAT tab);
    HRESULT (STDMETHODCALLTYPE *SetTrimming)(IDWriteTextFormat *This, const DW_TRIMMING *trim, IDWriteInlineObject *sign);
    HRESULT (STDMETHODCALLTYPE *SetLineSpacing)(IDWriteTextFormat *This, int method, FLOAT spacing, FLOAT baseline);
    void *getters[17];
} IDWriteTextFormatVtbl;
struct IDWriteTextFormat { const IDWriteTextFormatVtbl *lpVtbl; };

/* IDWriteTextRenderer (slots 0..9) */
typedef struct IDWriteTextRendererVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDWriteTextRenderer *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDWriteTextRenderer *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDWriteTextRenderer *This);
    HRESULT (STDMETHODCALLTYPE *IsPixelSnappingDisabled)(IDWriteTextRenderer *This, void *ctx, BOOL *isDisabled);
    HRESULT (STDMETHODCALLTYPE *GetCurrentTransform)(IDWriteTextRenderer *This, void *ctx, DW_MATRIX *transform);
    HRESULT (STDMETHODCALLTYPE *GetPixelsPerDip)(IDWriteTextRenderer *This, void *ctx, FLOAT *pixelsPerDip);
    HRESULT (STDMETHODCALLTYPE *DrawGlyphRun)(IDWriteTextRenderer *This, void *ctx,
                                              FLOAT baselineX, FLOAT baselineY, DW_MEASURING_MODE mode,
                                              const void *glyphRun, const void *glyphRunDesc, IUnknown *effect);
    HRESULT (STDMETHODCALLTYPE *DrawUnderline)(IDWriteTextRenderer *This, void *ctx,
                                               FLOAT baselineX, FLOAT baselineY, const void *underline, IUnknown *effect);
    HRESULT (STDMETHODCALLTYPE *DrawStrikethrough)(IDWriteTextRenderer *This, void *ctx,
                                                   FLOAT baselineX, FLOAT baselineY, const void *strike, IUnknown *effect);
    HRESULT (STDMETHODCALLTYPE *DrawInlineObject)(IDWriteTextRenderer *This, void *ctx,
                                                  FLOAT originX, FLOAT originY, IDWriteInlineObject *inlineObj,
                                                  BOOL isSideways, BOOL isRightToLeft, IUnknown *effect);
} IDWriteTextRendererVtbl;
struct IDWriteTextRenderer { const IDWriteTextRendererVtbl *lpVtbl; };

/* IDWriteTextLayout (inherits IDWriteTextFormat [0..27], own methods start at 28) */
typedef struct IDWriteTextLayoutVtbl {
    IDWriteTextFormatVtbl base; /* 0..27 */
    HRESULT (STDMETHODCALLTYPE *SetMaxWidth)(IDWriteTextLayout *This, FLOAT maxWidth);   /* 28 */
    HRESULT (STDMETHODCALLTYPE *SetMaxHeight)(IDWriteTextLayout *This, FLOAT maxHeight); /* 29 */
    void *pad_30_57[28]; /* slots 30..57 */
    HRESULT (STDMETHODCALLTYPE *Draw)(IDWriteTextLayout *This, void *ctx, IDWriteTextRenderer *renderer,
                                      FLOAT originX, FLOAT originY); /* 58 */
    HRESULT (STDMETHODCALLTYPE *GetLineMetrics)(IDWriteTextLayout *This, void *metrics, UINT32 maxC, UINT32 *actC); /* 59 */
    HRESULT (STDMETHODCALLTYPE *GetMetrics)(IDWriteTextLayout *This, DW_TEXT_METRICS *metrics); /* 60 */
} IDWriteTextLayoutVtbl;
struct IDWriteTextLayout { const IDWriteTextLayoutVtbl *lpVtbl; };

/* IDWriteGlyphRunAnalysis (slots 0..5) */
typedef struct IDWriteGlyphRunAnalysisVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDWriteGlyphRunAnalysis *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDWriteGlyphRunAnalysis *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IDWriteGlyphRunAnalysis *This);
    HRESULT (STDMETHODCALLTYPE *GetAlphaTextureBounds)(IDWriteGlyphRunAnalysis *This, DW_TEXTURE_TYPE type, RECT *bounds);
    HRESULT (STDMETHODCALLTYPE *CreateAlphaTexture)(IDWriteGlyphRunAnalysis *This, DW_TEXTURE_TYPE type,
                                                    const RECT *bounds, BYTE *alphaValues, UINT32 bufferSize);
    HRESULT (STDMETHODCALLTYPE *GetAlphaBlendParams)(IDWriteGlyphRunAnalysis *This, void *params,
                                                     FLOAT *gamma, FLOAT *contrast, FLOAT *ctLevel);
} IDWriteGlyphRunAnalysisVtbl;
struct IDWriteGlyphRunAnalysis { const IDWriteGlyphRunAnalysisVtbl *lpVtbl; };

/* IDWriteFactory (slots 0..23) */
typedef struct IDWriteFactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDWriteFactory *This, REFIID riid, void **ppv); /* 0 */
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDWriteFactory *This);                                   /* 1 */
    ULONG   (STDMETHODCALLTYPE *Release)(IDWriteFactory *This);                                  /* 2 */
    HRESULT (STDMETHODCALLTYPE *GetSystemFontCollection)(IDWriteFactory *This, IDWriteFontCollection **col, BOOL checkUpdates); /* 3 */
    void *pad_4_14[11]; /* slots 4..14 */
    HRESULT (STDMETHODCALLTYPE *CreateTextFormat)(IDWriteFactory *This, const WCHAR *family,
                                                  IDWriteFontCollection *col, DW_FONT_WEIGHT weight,
                                                  DW_FONT_STYLE style, DW_FONT_STRETCH stretch,
                                                  FLOAT size, const WCHAR *locale,
                                                  IDWriteTextFormat **outFmt); /* 15 */
    void *pad_16_17[2]; /* slots 16..17 */
    HRESULT (STDMETHODCALLTYPE *CreateTextLayout)(IDWriteFactory *This, const WCHAR *str, UINT32 len,
                                                  IDWriteTextFormat *fmt, FLOAT maxW, FLOAT maxH,
                                                  IDWriteTextLayout **outLayout); /* 18 */
    void *pad_19; /* slot 19 */
    HRESULT (STDMETHODCALLTYPE *CreateEllipsisTrimmingSign)(IDWriteFactory *This, IDWriteTextFormat *fmt,
                                                            IDWriteInlineObject **outSign); /* 20 */
    void *pad_21_22[2]; /* slots 21..22 */
    HRESULT (STDMETHODCALLTYPE *CreateGlyphRunAnalysis)(IDWriteFactory *This, const void *glyphRun,
                                                        FLOAT pixelsPerDip, const DW_MATRIX *transform,
                                                        DW_RENDERING_MODE renderMode, DW_MEASURING_MODE measureMode,
                                                        FLOAT baselineX, FLOAT baselineY,
                                                        IDWriteGlyphRunAnalysis **outAnalysis); /* 23 */
} IDWriteFactoryVtbl;
struct IDWriteFactory { const IDWriteFactoryVtbl *lpVtbl; };

HRESULT WINAPI DWriteCreateFactory(DW_FACTORY_TYPE factoryType, REFIID iid, IUnknown **factory);

#endif /* LARES_SHELL_DWRITE_C_H */
