#ifndef WMF_H
#define WMF_H

#include "hlp.h"

/* Binary opcode stream emitted by wmf_parse(), consumed by JS.
   Little-endian throughout. Coordinates are i16 (WMF native);
   colors are u32 in 0x00BBGGRR (WMF native, GDI COLORREF).
   Text strings (face names and ExtTextOut payloads) are raw bytes in
   the current font's charset — JS decodes via TextDecoder, which
   natively supports the relevant single- and double-byte codepages. */

#define WMF_OP_END               0x00
#define WMF_OP_BOUNDS            0x01  /* i16 orgX, orgY, extX, extY */
#define WMF_OP_SET_PEN           0x02  /* u16 style, u16 width, u32 color */
#define WMF_OP_SET_BRUSH         0x03  /* u16 style, u32 color, u16 hatch */
#define WMF_OP_SET_FONT          0x04  /* i16 height, u16 weight, u8 italic,
                                          i16 angle_deci_deg, u8 charset,
                                          u16 nlen, name (raw bytes) */
#define WMF_OP_SET_TEXT_COLOR    0x05  /* u32 color */
#define WMF_OP_SET_POLY_FILL_MODE 0x06 /* u8 mode (1=alt, 2=winding) */
#define WMF_OP_POLYLINE          0x07  /* u16 n, [i16 x, i16 y]*n */
#define WMF_OP_POLYGON           0x08  /* u16 n, [i16 x, i16 y]*n */
#define WMF_OP_POLYPOLYGON       0x09  /* u16 nPolys, [u16 size]*n,
                                          [i16 x, i16 y]*sum */
#define WMF_OP_TEXT              0x0A  /* i16 x, i16 y, u16 nbytes,
                                          raw bytes in current font charset */
#define WMF_OP_CLIP_SAVE         0x0B
#define WMF_OP_CLIP_RESTORE      0x0C
#define WMF_OP_CLIP_INTERSECT    0x0D  /* same payload as POLYPOLYGON */
#define WMF_OP_DIB_BLIT          0x0E  /* i16 dx,dy,dw,dh; u16 w,h; rgba w*h*4 */
#define WMF_OP_BIT_COPY          0x0F  /* i16 dx,dy,sx,sy,w,h */
#define WMF_OP_SET_WINDOW        0x10  /* i16 orgX, orgY, extX, extY —
                                          mid-stream SetWindowOrg/Ext
                                          after BOUNDS has been locked. */
#define WMF_OP_ELLIPSE           0x11  /* i16 left, top, right, bottom —
                                          ellipse inscribed in rect,
                                          stroke + fill from current
                                          pen/brush. RECTANGLE has no
                                          opcode; the parser emits a
                                          4-point POLYGON instead. */
#define WMF_OP_PIXEL             0x12  /* i16 x, i16 y, u32 color —
                                          paints a single device pixel
                                          (after the current transform)
                                          for META_SETPIXEL. */
#define WMF_OP_ARC               0x13  /* u8 kind (0=arc, 1=pie,
                                          2=chord), i16 left, top,
                                          right, bottom, i16 xs, ys,
                                          xe, ye. The bounding rect
                                          defines the ellipse; (xs,ys)
                                          and (xe,ye) are radial-line
                                          endpoints (where each radial
                                          ray, drawn from the center,
                                          intersects the ellipse).
                                          Drawn counter-clockwise per
                                          GDI default. */
#define WMF_OP_ROUNDRECT         0x14  /* i16 left, top, right, bottom,
                                          cornerW, cornerH — rounded
                                          rectangle with elliptical
                                          corners of size (cw,ch). */

/* Parse a decompressed WMF buffer and emit an opcode stream.
   On success, *out_ops points to a malloc'd buffer (caller frees) of
   *out_len bytes. Returns 0 on success, -1 on error (sets hlp_set_error). */
int wmf_parse(const uint8_t* data, size_t len,
              uint8_t** out_ops, size_t* out_len);

#endif
