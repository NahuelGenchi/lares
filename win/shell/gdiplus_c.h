// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef LARES_SHELL_GDIPLUS_C_H
#define LARES_SHELL_GDIPLUS_C_H

/* The part of GDI+'s flat API (gdiplus.dll) the shell uses, declared for C:
 * the Windows SDK's own GDI+ headers are C++ only. Shapes and pictures are
 * drawn with it; text is not (see draw.c). */

#include <windows.h>
#include <objidl.h>
#include <stdint.h>

typedef int GpStatus;
typedef uint32_t ARGB;
typedef float REAL;

typedef struct GpGraphics GpGraphics;
typedef struct GpBrush GpBrush;
typedef struct GpSolidFill GpSolidFill;
typedef struct GpLineGradient GpLineGradient;
typedef struct GpPen GpPen;
typedef struct GpPath GpPath;
typedef struct GpFontFamily GpFontFamily;
typedef struct GpFont GpFont;
typedef struct GpStringFormat GpStringFormat;
typedef struct GpImage GpImage;
typedef struct GpBitmap GpBitmap;
typedef struct GpImageAttributes GpImageAttributes;

typedef struct {
    REAL X;
    REAL Y;
} GpPointF;

typedef struct {
    REAL X;
    REAL Y;
    REAL Width;
    REAL Height;
} GpRectF;

typedef struct {
    INT X;
    INT Y;
    INT Width;
    INT Height;
} GpRect;

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

#define GP_OK 0

/* SmoothingMode */
#define GP_SMOOTH_DEFAULT     0
#define GP_SMOOTH_HIGHQUALITY 2
#define GP_SMOOTH_ANTIALIAS   4

/* TextRenderingHint */
#define GP_TEXT_SYSTEM_DEFAULT     0
#define GP_TEXT_ANTIALIAS_GRIDFIT  3
#define GP_TEXT_ANTIALIAS          4
#define GP_TEXT_CLEARTYPE_GRIDFIT  5

/* InterpolationMode */
#define GP_INTERP_NEAREST             5
#define GP_INTERP_HIGHQUALITY_BICUBIC 7

/* CompositingMode */
#define GP_COMP_SOURCE_OVER 0
#define GP_COMP_SOURCE_COPY 1

/* PixelOffsetMode */
#define GP_PIXEL_OFFSET_HALF 4

/* FillMode */
#define GP_FILL_ALTERNATE 0
#define GP_FILL_WINDING   1

/* Unit */
#define GP_UNIT_WORLD 0
#define GP_UNIT_PIXEL 2

/* FontStyle */
#define GP_FONT_REGULAR 0
#define GP_FONT_BOLD    1
#define GP_FONT_ITALIC  2

/* StringAlignment */
#define GP_ALIGN_NEAR   0
#define GP_ALIGN_CENTER 1
#define GP_ALIGN_FAR    2

/* StringTrimming */
#define GP_TRIM_NONE               0
#define GP_TRIM_ELLIPSIS_CHARACTER 3
#define GP_TRIM_ELLIPSIS_PATH      5

/* StringFormatFlags */
#define GP_STRFMT_NOWRAP      0x00001000
#define GP_STRFMT_NOFITBLACK  0x00000004
#define GP_STRFMT_MEASURE_TRAIL 0x00000800

/* PixelFormat */
#define GP_PIXELFORMAT_32BPP_ARGB  0x0026200A
#define GP_PIXELFORMAT_32BPP_PARGB 0x000E200B

/* CombineMode */
#define GP_COMBINE_REPLACE 0

/* FlushIntention */
#define GP_FLUSH_INTENTION_FLUSH 0
#define GP_FLUSH_INTENTION_SYNC  1

#define MAKE_ARGB(a, r, g, b) \
    ((((ARGB)(a) & 0xFFu) << 24) | (((ARGB)(r) & 0xFFu) << 16) | (((ARGB)(g) & 0xFFu) << 8) | ((ARGB)(b) & 0xFFu))

GpStatus WINAPI GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);

GpStatus WINAPI GdipCreateFromHDC(HDC hdc, GpGraphics **graphics);
GpStatus WINAPI GdipDeleteGraphics(GpGraphics *graphics);
GpStatus WINAPI GdipGetDC(GpGraphics *graphics, HDC *hdc);
GpStatus WINAPI GdipReleaseDC(GpGraphics *graphics, HDC hdc);
GpStatus WINAPI GdipFlush(GpGraphics *graphics, int intention);
GpStatus WINAPI GdipSetSmoothingMode(GpGraphics *graphics, int smoothingMode);
GpStatus WINAPI GdipSetTextRenderingHint(GpGraphics *graphics, int mode);
GpStatus WINAPI GdipGetTextRenderingHint(GpGraphics *graphics, int *mode);
GpStatus WINAPI GdipSetInterpolationMode(GpGraphics *graphics, int interpolationMode);
GpStatus WINAPI GdipSetCompositingMode(GpGraphics *graphics, int compositingMode);
GpStatus WINAPI GdipSetPixelOffsetMode(GpGraphics *graphics, int pixelOffsetMode);
GpStatus WINAPI GdipGraphicsClear(GpGraphics *graphics, ARGB color);
GpStatus WINAPI GdipSetClipRect(GpGraphics *graphics, REAL x, REAL y, REAL width, REAL height, int combineMode);
GpStatus WINAPI GdipSetClipPath(GpGraphics *graphics, GpPath *path, int combineMode);
GpStatus WINAPI GdipGetClipBoundsI(GpGraphics *graphics, GpRect *rect);
GpStatus WINAPI GdipResetClip(GpGraphics *graphics);

GpStatus WINAPI GdipCreateSolidFill(ARGB color, GpSolidFill **brush);
GpStatus WINAPI GdipSetSolidFillColor(GpSolidFill *brush, ARGB color);
GpStatus WINAPI GdipCreateLineBrush(const GpPointF *point1, const GpPointF *point2,
                                    ARGB color1, ARGB color2, int wrapMode, GpLineGradient **lineGradient);
GpStatus WINAPI GdipDeleteBrush(GpBrush *brush);

GpStatus WINAPI GdipCreatePen1(ARGB color, REAL width, int unit, GpPen **pen);
GpStatus WINAPI GdipDeletePen(GpPen *pen);

GpStatus WINAPI GdipCreatePath(int brushMode, GpPath **path);
GpStatus WINAPI GdipDeletePath(GpPath *path);
GpStatus WINAPI GdipAddPathArc(GpPath *path, REAL x, REAL y, REAL width, REAL height, REAL startAngle, REAL sweepAngle);
GpStatus WINAPI GdipAddPathLine(GpPath *path, REAL x1, REAL y1, REAL x2, REAL y2);
GpStatus WINAPI GdipAddPathBezier(GpPath *path, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4);
GpStatus WINAPI GdipClosePathFigure(GpPath *path);
GpStatus WINAPI GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path);
GpStatus WINAPI GdipDrawPath(GpGraphics *graphics, GpPen *pen, GpPath *path);

GpStatus WINAPI GdipFillRectangle(GpGraphics *graphics, GpBrush *brush, REAL x, REAL y, REAL width, REAL height);
GpStatus WINAPI GdipDrawRectangle(GpGraphics *graphics, GpPen *pen, REAL x, REAL y, REAL width, REAL height);
GpStatus WINAPI GdipFillEllipse(GpGraphics *graphics, GpBrush *brush, REAL x, REAL y, REAL width, REAL height);
GpStatus WINAPI GdipDrawEllipse(GpGraphics *graphics, GpPen *pen, REAL x, REAL y, REAL width, REAL height);
GpStatus WINAPI GdipDrawLine(GpGraphics *graphics, GpPen *pen, REAL x1, REAL y1, REAL x2, REAL y2);
GpStatus WINAPI GdipDrawArc(GpGraphics *graphics, GpPen *pen, REAL x, REAL y, REAL width, REAL height,
                            REAL startAngle, REAL sweepAngle);

GpStatus WINAPI GdipCreateFontFamilyFromName(const WCHAR *name, void *fontCollection, GpFontFamily **fontFamily);
GpStatus WINAPI GdipDeleteFontFamily(GpFontFamily *fontFamily);
GpStatus WINAPI GdipCreateFont(const GpFontFamily *fontFamily, REAL emSize, INT style, int unit, GpFont **font);
GpStatus WINAPI GdipDeleteFont(GpFont *font);

GpStatus WINAPI GdipCreateStringFormat(INT formatAttributes, LANGID language, GpStringFormat **format);
GpStatus WINAPI GdipDeleteStringFormat(GpStringFormat *format);
GpStatus WINAPI GdipSetStringFormatAlign(GpStringFormat *format, int align);
GpStatus WINAPI GdipSetStringFormatLineAlign(GpStringFormat *format, int align);
GpStatus WINAPI GdipSetStringFormatTrimming(GpStringFormat *format, int trimming);
GpStatus WINAPI GdipSetStringFormatFlags(GpStringFormat *format, INT flags);

GpStatus WINAPI GdipDrawString(GpGraphics *graphics, const WCHAR *string, INT length, const GpFont *font,
                               const GpRectF *layoutRect, const GpStringFormat *stringFormat, const GpBrush *brush);
GpStatus WINAPI GdipMeasureString(GpGraphics *graphics, const WCHAR *string, INT length, const GpFont *font,
                                  const GpRectF *layoutRect, const GpStringFormat *stringFormat,
                                  GpRectF *boundingBox, INT *codepointsFitted, INT *linesFilled);

GpStatus WINAPI GdipCreateBitmapFromFile(const WCHAR *filename, GpBitmap **bitmap);
GpStatus WINAPI GdipCreateBitmapFromHICON(HICON hicon, GpBitmap **bitmap);
GpStatus WINAPI GdipCreateBitmapFromHBITMAP(HBITMAP hbm, HPALETTE hpal, GpBitmap **bitmap);
GpStatus WINAPI GdipCreateBitmapFromScan0(INT width, INT height, INT stride, int format, BYTE *scan0, GpBitmap **bitmap);
GpStatus WINAPI GdipCreateBitmapFromStream(IStream *stream, GpBitmap **bitmap);
GpStatus WINAPI GdipGetImageWidth(GpImage *image, UINT *width);
GpStatus WINAPI GdipGetImageHeight(GpImage *image, UINT *height);
GpStatus WINAPI GdipDrawImageRect(GpGraphics *graphics, GpImage *image, REAL x, REAL y, REAL width, REAL height);
GpStatus WINAPI GdipDrawImageRectI(GpGraphics *graphics, GpImage *image, INT x, INT y, INT width, INT height);
GpStatus WINAPI GdipDrawImageRectRectI(GpGraphics *graphics, GpImage *image,
                                       INT dstx, INT dsty, INT dstwidth, INT dstheight,
                                       INT srcx, INT srcy, INT srcwidth, INT srcheight,
                                       int srcUnit, const GpImageAttributes *imageAttributes,
                                       void *callback, void *callbackData);
GpStatus WINAPI GdipGetImageGraphicsContext(GpImage *image, GpGraphics **graphics);
GpStatus WINAPI GdipSaveImageToFile(GpImage *image, const WCHAR *filename, const CLSID *clsidEncoder, const void *encoderParams);
GpStatus WINAPI GdipImageRotateFlip(GpImage *image, int rfType);
GpStatus WINAPI GdipDisposeImage(GpImage *image);

/* RotateFlipType */
#define GP_ROTATE_NONE_FLIP_NONE 0
#define GP_ROTATE_90_FLIP_NONE   1
#define GP_ROTATE_180_FLIP_NONE  2
#define GP_ROTATE_270_FLIP_NONE  3

#endif /* LARES_SHELL_GDIPLUS_C_H */
