/* GDI-internal declarations (objects, surfaces, DIBs, fonts). */
#ifndef GDI_H
#define GDI_H

#include "win.h"

enum { BS_SOLID = 0, BS_NULL = 1, BS_HATCHED = 2, BS_PATTERN = 3, BS_DIBPATTERN = 5 };

typedef struct Pen {
    int style, width;
    uint32_t color;
} Pen;

typedef struct Brush {
    int style;
    uint32_t color;
    int hatch;
    Surface pat;                 /* 8x8 pattern for BS_PATTERN / BS_DIBPATTERN */
} Brush;

#define CS_OWNDC 0x0020

int surface_init(Surface *s, int w, int h, int mono);
/* DIB DCs (gdi_pal.c) */
int dib_nearest(DC *dc, int r, int g, int b);
void dib_refresh_maps(DC *dc);
uint16_t gdi_new_bitmap(int w, int h, int mono);

/* Decode rows [start, start + nlines) (counted from the bottom, as DIBs do; nlines < 0 = all)
 * of the DIB whose BITMAPINFO is at `info` and pixels at `bits` (0: right after the colour
 * table, a packed DIB) into a new surface of system indices for `dc`'s palette (NULL: the
 * default palette), the full size of the DIB, top row first. mono_dst gives 0x00/0xFF
 * pixels. Returns 0 if the format isn't understood. */
int dib_decode(DC *dc, uint32_t info, uint32_t bits, int usage, int start, int nlines, Surface *out);
int dib_decode_mono(DC *dc, uint32_t info, uint32_t bits, int usage, int start, int nlines, int mono_dst,
                    Surface *out);
/* Packed DIB in host memory (resources). */
int dib_decode_host(DC *dc, const uint8_t *info, const uint8_t *bits, int usage, int mono_dst, Surface *out);
/* Header facts of a BITMAPINFO in host memory. */
int dib_info(const uint8_t *info, int *w, int *h, int *bpp, int *ncolors, int *hdr_size, int *entry_size);

/* Blits used by text and USER (surface coordinates, clipped to t->clip). */
void blit_fill(const Target *t, IRect r, uint8_t pixel);
void blit_mono_mask(const Target *t, int x, int y, const uint8_t *mask, int mw, int mh, int stride, uint8_t fg);

/* fonts (gdi_text.c) */
uint16_t gdi_stock_font(int index);
void gdi_free_font(uint16_t h);
int gdi_font_logfont(uint16_t h, uint8_t *out50);
void gdi_free_window_dc(uint16_t hdc);

#endif
