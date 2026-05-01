#include "wmf.h"

/* MS-WMF record types we recognize. */
#define META_EOF                  0x0000
#define META_SAVEDC               0x001E
#define META_CREATEPALETTE        0x00F7
#define META_DIBCREATEPATTERNBRUSH 0x0142
#define META_CREATEPATTERNBRUSH   0x01F9
#define META_CREATEREGION         0x06FF
#define META_SETBKMODE            0x0102
#define META_SETMAPMODE           0x0103
#define META_SETPOLYFILLMODE      0x0106
#define META_SETSTRETCHBLTMODE    0x0107
#define META_RESTOREDC            0x0127
#define META_SELECTCLIPREGION     0x012C
#define META_SELECTOBJECT         0x012D
#define META_SETTEXTALIGN         0x012E
#define META_SETTEXTCOLOR         0x0209
#define META_SETWINDOWORG         0x020B
#define META_SETWINDOWEXT         0x020C
#define META_POLYGON              0x0324
#define META_POLYLINE             0x0325
#define META_ELLIPSE              0x0418
#define META_RECTANGLE            0x041B
#define META_ESCAPE               0x0626
#define META_INTERSECTCLIPRECT    0x0416
#define META_DELETEOBJECT         0x01F0
#define META_CREATEPENINDIRECT    0x02FA
#define META_CREATEFONTINDIRECT   0x02FB
#define META_CREATEBRUSHINDIRECT  0x02FC
#define META_POLYPOLYGON          0x0538
#define META_DIBBITBLT            0x0940
#define META_EXTTEXTOUT           0x0A32
#define META_DIBSTRETCHBLT        0x0B41
#define META_STRETCHDIB           0x0F43

/* WMF Escape function codes we recognize. */
#define ESC_ENHANCED_METAFILE  0x000F  /* also Designer ASCII comments */
#define ESC_BEGIN_PATH         0x1000
#define ESC_CLIP_TO_PATH       0x1001
#define ESC_END_PATH           0x1002

/* Modes for ESC_CLIP_TO_PATH (low 16 bits of the DWORD parameter). */
#define CLIP_MODE_SAVE      0
#define CLIP_MODE_RESTORE   1
#define CLIP_MODE_INTERSECT 2
#define CLIP_MODE_EXCLUDE   3

#define WMF_STACK_MAX 32
/* MS-WMF NumberOfObjects is u16, so 65535 is the spec ceiling. We cap
   our allocation here as a sanity bound for malformed inputs; real
   files top out in the low thousands. */
#define WMF_OBJ_MAX   65535

/* ---------- object table -------------------------------------------- */

/* OBJ_OTHER is for records like META_CREATEPALETTE / CREATEREGION /
   CREATEPATTERNBRUSH / DIBCREATEPATTERNBRUSH that consume an object
   table slot but don't carry pen/brush/font state we model. We must
   reserve their slots so subsequent SELECTOBJECT indices line up;
   js-wmf misses this and mis-routes selections (visible e.g. in
   STAR13.WMF, where the palette at index 0 shifts every later object,
   producing a black square instead of a blue star). */
typedef enum { OBJ_NONE = 0, OBJ_PEN, OBJ_BRUSH, OBJ_FONT, OBJ_OTHER } ObjKind;

typedef struct {
    int16_t  height;
    uint16_t weight;
    uint8_t  italic;
    int16_t  angle_decideg; /* WMF Escapement, tenths of a degree */
    uint8_t  charset;
    char     name[33];      /* NUL-terminated facename (≤32 bytes) */
} WmfFont;

typedef struct {
    ObjKind kind;
    union {
        struct { uint16_t style, width; uint32_t color; } pen;
        struct { uint16_t style; uint32_t color; uint16_t hatch; } brush;
        WmfFont font;
    } u;
} WmfObj;

static void obj_add(WmfObj* objects, int* count, int cap, const WmfObj* obj) {
    for (int i = 0; i < *count; i++) {
        if (objects[i].kind == OBJ_NONE) { objects[i] = *obj; return; }
    }
    if (*count < cap) { objects[(*count)++] = *obj; }
}

/* ---------- DC state ------------------------------------------------- */

typedef struct {
    /* selected pen */
    uint8_t  has_pen;
    uint16_t pen_style, pen_width;
    uint32_t pen_color;
    /* selected brush */
    uint8_t  has_brush;
    uint16_t brush_style;
    uint32_t brush_color;
    uint16_t brush_hatch;
    /* selected font */
    uint8_t  has_font;
    WmfFont  font;
    /* text color */
    uint8_t  has_text_color;
    uint32_t text_color;
    /* polygon fill mode (1=ALTERNATE, 2=WINDING; 0 means unset) */
    uint8_t  poly_fill_mode;
    /* viewport */
    int16_t  win_org_x, win_org_y;
    int16_t  win_ext_x, win_ext_y;
} State;

/* ---------- growable opcode buffer ---------------------------------- */

typedef struct {
    uint8_t* data;
    size_t   len, cap;
    int      oom;
} Buf;

static void buf_init(Buf* b) {
    b->cap = 4096;
    b->data = (uint8_t*)malloc(b->cap);
    b->len = 0;
    b->oom = b->data ? 0 : 1;
}

static void buf_reserve(Buf* b, size_t extra) {
    if (b->oom) return;
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra) cap *= 2;
    uint8_t* p = (uint8_t*)realloc(b->data, cap);
    if (!p) { b->oom = 1; return; }
    b->data = p;
    b->cap = cap;
}

static void buf_u8(Buf* b, uint8_t v) {
    buf_reserve(b, 1); if (b->oom) return;
    b->data[b->len++] = v;
}
static void buf_u16(Buf* b, uint16_t v) {
    buf_reserve(b, 2); if (b->oom) return;
    b->data[b->len++] = (uint8_t)v;
    b->data[b->len++] = (uint8_t)(v >> 8);
}
static void buf_i16(Buf* b, int16_t v) { buf_u16(b, (uint16_t)v); }
static void buf_u32(Buf* b, uint32_t v) {
    buf_reserve(b, 4); if (b->oom) return;
    b->data[b->len++] = (uint8_t)v;
    b->data[b->len++] = (uint8_t)(v >> 8);
    b->data[b->len++] = (uint8_t)(v >> 16);
    b->data[b->len++] = (uint8_t)(v >> 24);
}

static void buf_patch_i16(Buf* b, size_t off, int16_t v) {
    if (off + 2 > b->len) return;
    b->data[off]     = (uint8_t)v;
    b->data[off + 1] = (uint8_t)((uint16_t)v >> 8);
}

static void buf_patch_u16(Buf* b, size_t off, uint16_t v) {
    if (off + 2 > b->len) return;
    b->data[off]     = (uint8_t)v;
    b->data[off + 1] = (uint8_t)(v >> 8);
}

static void buf_bytes(Buf* b, const uint8_t* src, size_t n) {
    buf_reserve(b, n); if (b->oom) return;
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

/* ---------- coalescing state-change emission ------------------------ */

typedef struct {
    int      pen_valid;
    uint16_t pen_style, pen_width;
    uint32_t pen_color;

    int      brush_valid;
    uint16_t brush_style;
    uint32_t brush_color;
    uint16_t brush_hatch;

    int      font_valid;
    WmfFont  font;

    int      text_color_valid;
    uint32_t text_color;

    uint8_t  pfm; /* last emitted poly_fill_mode (0 = never emitted) */
} Emitted;

static void emit_pen_if_changed(Buf* b, State* st, Emitted* em) {
    if (!st->has_pen) return;
    if (em->pen_valid &&
        em->pen_style == st->pen_style &&
        em->pen_width == st->pen_width &&
        em->pen_color == st->pen_color) return;
    buf_u8(b, WMF_OP_SET_PEN);
    buf_u16(b, st->pen_style);
    buf_u16(b, st->pen_width);
    buf_u32(b, st->pen_color);
    em->pen_valid = 1;
    em->pen_style = st->pen_style;
    em->pen_width = st->pen_width;
    em->pen_color = st->pen_color;
}

static void emit_brush_if_changed(Buf* b, State* st, Emitted* em) {
    if (!st->has_brush) return;
    if (em->brush_valid &&
        em->brush_style == st->brush_style &&
        em->brush_color == st->brush_color &&
        em->brush_hatch == st->brush_hatch) return;
    buf_u8(b, WMF_OP_SET_BRUSH);
    buf_u16(b, st->brush_style);
    buf_u32(b, st->brush_color);
    buf_u16(b, st->brush_hatch);
    em->brush_valid = 1;
    em->brush_style = st->brush_style;
    em->brush_color = st->brush_color;
    em->brush_hatch = st->brush_hatch;
}

static void emit_pfm_if_changed(Buf* b, State* st, Emitted* em) {
    if (!st->poly_fill_mode) return;
    if (em->pfm == st->poly_fill_mode) return;
    buf_u8(b, WMF_OP_SET_POLY_FILL_MODE);
    buf_u8(b, st->poly_fill_mode);
    em->pfm = st->poly_fill_mode;
}

static int font_eq(const WmfFont* a, const WmfFont* b) {
    if (a->height != b->height) return 0;
    if (a->weight != b->weight) return 0;
    if (a->italic != b->italic) return 0;
    if (a->angle_decideg != b->angle_decideg) return 0;
    if (a->charset != b->charset) return 0;
    return strcmp(a->name, b->name) == 0;
}

static void emit_font_if_changed(Buf* b, State* st, Emitted* em) {
    if (!st->has_font) return;
    if (em->font_valid && font_eq(&em->font, &st->font)) return;
    buf_u8(b, WMF_OP_SET_FONT);
    buf_i16(b, st->font.height);
    buf_u16(b, st->font.weight);
    buf_u8(b, st->font.italic);
    buf_i16(b, st->font.angle_decideg);
    buf_u8(b, st->font.charset);
    size_t nlen = strlen(st->font.name);
    if (nlen > 32) nlen = 32;
    buf_u16(b, (uint16_t)nlen);
    buf_bytes(b, (const uint8_t*)st->font.name, nlen);
    em->font_valid = 1;
    em->font = st->font;
}

/* Lock the BOUNDS payload on the first drawing opcode whose state has
   both SetWindowOrg and SetWindowExt seen. Mirrors render.js, which
   iterates emitted actions and breaks on the first one whose snapshot
   has both `Extent` and `Origin` — picking up later values when the
   WMF re-set the window before drawing. Locking on the *first record*
   instead caused us to capture stub (1,1) extents for clip-art that
   sets up a tiny initial window before declaring its real one. */
static void try_lock_bounds(Buf* b, size_t bounds_off, const State* st,
                            int ext_seen, int org_seen, int* locked,
                            int16_t* last_org_x, int16_t* last_org_y,
                            int16_t* last_ext_x, int16_t* last_ext_y) {
    if (*locked) return;
    if (!ext_seen || !org_seen) return;
    buf_patch_i16(b, bounds_off + 0, st->win_org_x);
    buf_patch_i16(b, bounds_off + 2, st->win_org_y);
    buf_patch_i16(b, bounds_off + 4, st->win_ext_x);
    buf_patch_i16(b, bounds_off + 6, st->win_ext_y);
    *locked = 1;
    *last_org_x = st->win_org_x;
    *last_org_y = st->win_org_y;
    *last_ext_x = st->win_ext_x;
    *last_ext_y = st->win_ext_y;
}

/* ctx.save / ctx.restore reverts pen / brush / font / text-color on the
   dispatcher side, so our coalescer's "last emitted" view becomes
   stale after a CLIP_RESTORE. Without this invalidation, a draw op
   following the restore can match em exactly, skip its SET_*, and
   then use the post-restore canvas state (which is the *outside*
   bracket value) instead of our intended inside-bracket value.
   PolyFillMode is an argument to ctx.fill(), not canvas state, so
   it's not affected by save/restore — we leave em.pfm alone. */
static void em_invalidate_canvas_state(Emitted* em) {
    em->pen_valid = 0;
    em->brush_valid = 0;
    em->font_valid = 0;
    em->text_color_valid = 0;
}

static void emit_text_color_if_changed(Buf* b, State* st, Emitted* em) {
    if (!st->has_text_color) return;
    if (em->text_color_valid && em->text_color == st->text_color) return;
    buf_u8(b, WMF_OP_SET_TEXT_COLOR);
    buf_u32(b, st->text_color);
    em->text_color_valid = 1;
    em->text_color = st->text_color;
}

/* Decode a DIB starting with BITMAPINFOHEADER and emit a DIB_BLIT
   opcode. dst_x/dst_y/dst_w/dst_h are in WMF logical coords; the
   image is decoded to top-down RGBA at its natural pixel size. The
   12-byte BITMAPCOREHEADER form is ignored (rare in WinHelp WMFs). */
static void emit_dib_blit(Buf* b, const uint8_t* dib, size_t dib_len,
                          int16_t dst_x, int16_t dst_y,
                          int16_t dst_w, int16_t dst_h) {
    if (dib_len < 40) return;
    uint32_t bi_size = get_u32(dib, 0);
    if (bi_size < 40 || bi_size > dib_len) return;
    int32_t  width  = get_i32(dib, 4);
    int32_t  height = get_i32(dib, 8);
    uint16_t bpp    = get_u16(dib, 14);
    uint32_t clrused = get_u32(dib, 32);
    if (width <= 0 || height <= 0) return;
    if (width > 65535 || height > 65535) return;

    uint32_t ncolors = clrused;
    if (!ncolors && bpp <= 8) ncolors = 1u << bpp;
    /* Each palette entry in BITMAPINFOHEADER DIBs is RGBQUAD (4 bytes). */
    size_t palette_bytes = (size_t)ncolors * 4;
    if (bi_size + palette_bytes > dib_len) return;
    const uint8_t* palette = dib + bi_size;
    const uint8_t* pixels  = palette + palette_bytes;

    uint32_t rgba_size = 0;
    uint8_t* rgba = dib_to_rgba((uint32_t)width, (uint32_t)height, bpp,
                                palette, ncolors, pixels, &rgba_size);
    if (!rgba) return;

    buf_u8(b, WMF_OP_DIB_BLIT);
    buf_i16(b, dst_x);
    buf_i16(b, dst_y);
    buf_i16(b, dst_w);
    buf_i16(b, dst_h);
    buf_u16(b, (uint16_t)width);
    buf_u16(b, (uint16_t)height);
    buf_bytes(b, rgba, rgba_size);
    free(rgba);
}

/* ---------- public API ---------------------------------------------- */

int wmf_parse(const uint8_t* data, size_t len,
              uint8_t** out_ops, size_t* out_len) {
    if (len < 18) { hlp_set_error("WMF: too short for header"); return -1; }
    uint16_t mtype = get_u16(data, 0);
    if (mtype != 1 && mtype != 2) { hlp_set_error("WMF: bad header Type"); return -1; }
    if (get_u16(data, 2) != 9) { hlp_set_error("WMF: bad header size"); return -1; }
    uint16_t version = get_u16(data, 4);
    if (version != 0x0100 && version != 0x0300) {
        hlp_set_error("WMF: bad header version");
        return -1;
    }
    uint16_t num_objects = get_u16(data, 10);
    if (num_objects > WMF_OBJ_MAX) num_objects = WMF_OBJ_MAX;

    Buf b; buf_init(&b);
    if (b.oom) { hlp_set_error("WMF: oom"); return -1; }

    /* BOUNDS placeholder — payload patched on the first drawing opcode
       once both SetWindowOrg and SetWindowExt have been observed.
       Falls back to "last ext seen" with origin (0,0) at end-of-stream
       if no drawing op ever locks them. */
    buf_u8(&b, WMF_OP_BOUNDS);
    size_t bounds_off = b.len;
    buf_i16(&b, 0); buf_i16(&b, 0); buf_i16(&b, 0); buf_i16(&b, 0);
    int bounds_locked = 0;
    int ext_seen = 0, org_seen = 0;
    /* Track the last (org, ext) we exposed to the dispatcher (initially
       through BOUNDS, later through SET_WINDOW) so we don't re-emit
       redundant transforms when a WMF re-states the same window. */
    int16_t last_emitted_org_x = 0, last_emitted_org_y = 0;
    int16_t last_emitted_ext_x = 0, last_emitted_ext_y = 0;

    /* Heap-allocate the object table sized to the header — the stack
       isn't viable here (Buttrfly6.wmf in the test corpus declares 462
       objects, and well-formed files can go up to 65535). */
    size_t obj_cap = num_objects > 0 ? num_objects : 1;
    WmfObj* objects = (WmfObj*)calloc(obj_cap, sizeof(WmfObj));
    if (!objects) { free(b.data); hlp_set_error("WMF: oom"); return -1; }
    int objects_count = (int)num_objects;

    State state; memset(&state, 0, sizeof(state));
    State stack[WMF_STACK_MAX];
    int stack_top = -1;
    Emitted em; memset(&em, 0, sizeof(em));

    /* Path-tracking state for the BEGIN_PATH/END_PATH/CLIP_TO_PATH escape
       sequence. Polylines/polygons/polypolygons issued while building a
       path are diverted into path_sizes (one u16 per sub-poly) and
       path_points (i16 x,y pairs) instead of being emitted; CLIP_TO_PATH
       then flushes them as a CLIP_INTERSECT opcode payload. */
    int building_path = 0;
    Buf path_sizes;  buf_init(&path_sizes);
    Buf path_points; buf_init(&path_points);
    uint16_t path_npoly = 0;

    /* Designer "Begin Skip" / "End Skip" comments wrap legacy fallback
       drawing that smart viewers ignore. We render the fallback (since
       we don't decode the GRADIENT comment) but drop the same primitives
       when they're being collected into a clip path — empty stubs there
       would corrupt the clip region. */
    int skip_mode = 0;

    size_t off = 18;
    while (off + 6 <= len) {
        uint32_t rsize_words = get_u32(data, off);
        if (rsize_words < 3) break;
        size_t rsize = (size_t)rsize_words * 2;
        if (rsize > len || off + rsize > len) break;
        uint16_t rfunc = get_u16(data, off + 4);
        if (rfunc == META_EOF) break;

        const uint8_t* parm = data + off + 6;
        size_t parm_len = rsize - 6;

        switch (rfunc) {
        case META_SETWINDOWORG:
        case META_SETWINDOWEXT: {
            if (parm_len < 4) break;
            if (rfunc == META_SETWINDOWORG) {
                state.win_org_y = get_i16(parm, 0);
                state.win_org_x = get_i16(parm, 2);
                org_seen = 1;
            } else {
                state.win_ext_y = get_i16(parm, 0);
                state.win_ext_x = get_i16(parm, 2);
                ext_seen = 1;
            }
            /* After BOUNDS is locked, mid-stream window changes update
               the dispatcher's transform via SET_WINDOW so files that
               draw inside a SaveDC + new window + RestoreDC bracket
               (e.g. STAR13.WMF) render correctly. Real GDI re-maps
               logical→device on every SetWindow; render.js doesn't, so
               we knowingly diverge from those baselines. To stay
               pixel-identical when the window is just being re-stated
               (very common — many WMFs repeat the initial values), we
               only emit when the values actually differ from the
               last-emitted set. */
            if (bounds_locked &&
                (state.win_org_x != last_emitted_org_x ||
                 state.win_org_y != last_emitted_org_y ||
                 state.win_ext_x != last_emitted_ext_x ||
                 state.win_ext_y != last_emitted_ext_y)) {
                buf_u8(&b, WMF_OP_SET_WINDOW);
                buf_i16(&b, state.win_org_x);
                buf_i16(&b, state.win_org_y);
                buf_i16(&b, state.win_ext_x);
                buf_i16(&b, state.win_ext_y);
                last_emitted_org_x = state.win_org_x;
                last_emitted_org_y = state.win_org_y;
                last_emitted_ext_x = state.win_ext_x;
                last_emitted_ext_y = state.win_ext_y;
            }
            break;
        }

        case META_SETPOLYFILLMODE:
            if (parm_len >= 2) state.poly_fill_mode = (uint8_t)get_u16(parm, 0);
            break;

        case META_CREATEPENINDIRECT: {
            if (parm_len < 10) break;
            WmfObj o = {0};
            o.kind = OBJ_PEN;
            o.u.pen.style = get_u16(parm, 0);
            /* Width is a PointS (2 i16), but only the low byte of x is used,
               matching js-wmf's `data.read_shift(4) & 0xFF`. */
            o.u.pen.width = (uint16_t)(get_u32(parm, 2) & 0xFF);
            o.u.pen.color = get_u32(parm, 6);
            obj_add(objects, &objects_count, (int)obj_cap, &o);
            break;
        }
        case META_CREATEBRUSHINDIRECT: {
            if (parm_len < 8) break;
            WmfObj o = {0};
            o.kind = OBJ_BRUSH;
            o.u.brush.style = get_u16(parm, 0);
            o.u.brush.color = get_u32(parm, 2);
            o.u.brush.hatch = get_u16(parm, 6);
            obj_add(objects, &objects_count, (int)obj_cap, &o);
            break;
        }
        case META_CREATEFONTINDIRECT: {
            /* MS-WMF 2.2.1.2 Font: 18 bytes of fixed fields + 32-byte facename. */
            if (parm_len < 18) break;
            WmfObj o = {0};
            o.kind = OBJ_FONT;
            o.u.font.height        = get_i16(parm, 0);
            /* offset 2: Width    (unused by GDI fillText) */
            o.u.font.angle_decideg = get_i16(parm, 4); /* Escapement */
            /* offset 6: Orientation (mostly tracks Escapement) */
            o.u.font.weight        = get_u16(parm, 8);
            o.u.font.italic        = parm[10];
            /* offset 11: Underline, 12: StrikeOut */
            o.u.font.charset       = parm[13];
            /* offset 14-17: OutPrecision/ClipPrecision/Quality/PitchAndFamily */
            size_t name_max = parm_len > 18 ? parm_len - 18 : 0;
            if (name_max > 32) name_max = 32;
            size_t i;
            for (i = 0; i < name_max && parm[18 + i]; i++) {
                o.u.font.name[i] = (char)parm[18 + i];
            }
            o.u.font.name[i] = 0;
            obj_add(objects, &objects_count, (int)obj_cap, &o);
            break;
        }
        case META_DELETEOBJECT: {
            if (parm_len < 2) break;
            uint16_t idx = get_u16(parm, 0);
            if (idx < (uint16_t)objects_count) objects[idx].kind = OBJ_NONE;
            break;
        }
        case META_CREATEPALETTE:
        case META_CREATEPATTERNBRUSH:
        case META_DIBCREATEPATTERNBRUSH:
        case META_CREATEREGION: {
            /* Consume an object slot so later SELECTOBJECT indices line
               up. We don't render through these objects, so the slot
               just holds an OBJ_OTHER placeholder. */
            WmfObj o = {0};
            o.kind = OBJ_OTHER;
            obj_add(objects, &objects_count, (int)obj_cap, &o);
            break;
        }
        case META_SELECTOBJECT: {
            if (parm_len < 2) break;
            uint16_t idx = get_u16(parm, 0);
            if (idx >= (uint16_t)objects_count) break;
            WmfObj* o = &objects[idx];
            if (o->kind == OBJ_PEN) {
                state.has_pen   = 1;
                state.pen_style = o->u.pen.style;
                state.pen_width = o->u.pen.width;
                state.pen_color = o->u.pen.color;
            } else if (o->kind == OBJ_BRUSH) {
                state.has_brush    = 1;
                state.brush_style  = o->u.brush.style;
                state.brush_color  = o->u.brush.color;
                state.brush_hatch  = o->u.brush.hatch;
            } else if (o->kind == OBJ_FONT) {
                state.has_font = 1;
                state.font = o->u.font;
            }
            break;
        }

        case META_SAVEDC:
            if (stack_top + 1 < WMF_STACK_MAX) stack[++stack_top] = state;
            /* Mirror canvas state via the dispatcher's save stack so
               ctx.save() preserves the transform across mid-stream
               SetWindow changes that show up between SAVEDC/RESTOREDC. */
            buf_u8(&b, WMF_OP_CLIP_SAVE);
            break;
        case META_RESTOREDC: {
            if (parm_len < 2) break;
            int16_t n = get_i16(parm, 0);
            int idx = (n >= 0) ? n : (stack_top + n + 1);
            if (idx >= 0 && idx <= stack_top) {
                state = stack[idx];
                stack_top = idx - 1;
            }
            buf_u8(&b, WMF_OP_CLIP_RESTORE);
            em_invalidate_canvas_state(&em);
            break;
        }

        case META_POLYLINE:
        case META_POLYGON: {
            if (parm_len < 2) break;
            uint16_t n = get_u16(parm, 0);
            if (parm_len < 2u + (size_t)n * 4u) break;
            const uint8_t* pts = parm + 2;
            if (building_path) {
                if (skip_mode) break; /* drop placeholder polys */
                buf_u16(&path_sizes, n);
                buf_bytes(&path_points, pts, (size_t)n * 4);
                path_npoly++;
                break;
            }
            try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
            emit_pen_if_changed(&b, &state, &em);
            if (rfunc == META_POLYGON) {
                emit_brush_if_changed(&b, &state, &em);
                emit_pfm_if_changed(&b, &state, &em);
            }
            buf_u8(&b, rfunc == META_POLYGON ? WMF_OP_POLYGON : WMF_OP_POLYLINE);
            buf_u16(&b, n);
            buf_bytes(&b, pts, (size_t)n * 4);
            break;
        }
        case META_POLYPOLYGON: {
            if (parm_len < 2) break;
            uint16_t npoly = get_u16(parm, 0);
            if (parm_len < 2u + (size_t)npoly * 2u) break;
            const uint8_t* sizes = parm + 2;
            uint32_t total_pts = 0;
            for (uint16_t i = 0; i < npoly; i++) total_pts += get_u16(sizes, (size_t)i * 2);
            if (parm_len < 2u + (size_t)npoly * 2u + (size_t)total_pts * 4u) break;
            const uint8_t* pts = sizes + (size_t)npoly * 2;

            if (building_path) {
                if (skip_mode) break;
                size_t pts_off = 0;
                for (uint16_t i = 0; i < npoly; i++) {
                    uint16_t sz = get_u16(sizes, (size_t)i * 2);
                    buf_u16(&path_sizes, sz);
                    buf_bytes(&path_points, pts + pts_off, (size_t)sz * 4);
                    pts_off += (size_t)sz * 4;
                    path_npoly++;
                }
                break;
            }
            try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
            emit_pen_if_changed(&b, &state, &em);
            emit_brush_if_changed(&b, &state, &em);
            emit_pfm_if_changed(&b, &state, &em);
            buf_u8(&b, WMF_OP_POLYPOLYGON);
            buf_u16(&b, npoly);
            buf_bytes(&b, sizes, (size_t)npoly * 2);
            buf_bytes(&b, pts, (size_t)total_pts * 4);
            break;
        }

        case META_SETTEXTCOLOR:
            if (parm_len >= 4) {
                state.text_color = get_u32(parm, 0);
                state.has_text_color = 1;
            }
            break;

        case META_EXTTEXTOUT: {
            /* MS-WMF 2.3.3.5: Y, X, StringLength, fwOpts, [Rectangle (8B)
               if fwOpts & 0x06], String, [Dx]. */
            if (parm_len < 8) break;
            int16_t y = get_i16(parm, 0);
            int16_t x = get_i16(parm, 2);
            uint16_t slen = get_u16(parm, 4);
            uint16_t opts = get_u16(parm, 6);
            size_t soff = 8;
            if (opts & 0x06) soff += 8;
            if (parm_len < soff + slen) break;

            try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
            emit_font_if_changed(&b, &state, &em);
            emit_text_color_if_changed(&b, &state, &em);

            buf_u8(&b, WMF_OP_TEXT);
            buf_i16(&b, x);
            buf_i16(&b, y);
            buf_u16(&b, slen);
            buf_bytes(&b, parm + soff, slen);
            break;
        }

        case META_ESCAPE: {
            if (parm_len < 4) break;
            uint16_t esc_func = get_u16(parm, 0);
            uint16_t byte_count = get_u16(parm, 2);
            const uint8_t* esc_parm = parm + 4;
            size_t esc_parm_len = parm_len > 4 ? parm_len - 4 : 0;
            if (esc_parm_len > byte_count) esc_parm_len = byte_count;

            switch (esc_func) {
            case ESC_BEGIN_PATH:
                building_path = 1;
                path_sizes.len = 0;
                path_points.len = 0;
                path_npoly = 0;
                break;
            case ESC_END_PATH:
                building_path = 0;
                break;
            case ESC_CLIP_TO_PATH: {
                /* Payload begins with the 16-bit ByteCount we already
                   read (it doubles as the inner record byte count); the
                   mode is in the next DWORD's low word. */
                if (esc_parm_len < 4) break;
                uint16_t mode = (uint16_t)(get_u32(esc_parm, 0) & 0xFFFF);
                if (mode == CLIP_MODE_SAVE) {
                    try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
                    buf_u8(&b, WMF_OP_CLIP_SAVE);
                } else if (mode == CLIP_MODE_RESTORE) {
                    buf_u8(&b, WMF_OP_CLIP_RESTORE);
                    em_invalidate_canvas_state(&em);
                } else if (mode == CLIP_MODE_INTERSECT && path_npoly > 0) {
                    try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
                    emit_pfm_if_changed(&b, &state, &em);
                    buf_u8(&b, WMF_OP_CLIP_INTERSECT);
                    buf_u16(&b, path_npoly);
                    buf_bytes(&b, path_sizes.data, path_sizes.len);
                    buf_bytes(&b, path_points.data, path_points.len);
                }
                /* CLIP_MODE_EXCLUDE (3) is a rare no-op approximation. */
                break;
            }
            case ESC_ENHANCED_METAFILE: {
                /* The body is either a Designer ASCII comment ("Begin
                   Skip" / "End Skip" / "Begin Layer" / etc.) or an EMF
                   chunk. We only act on the Skip pair; everything else
                   is ignored. */
                if (esc_parm_len < 1) break;
                uint8_t b0 = esc_parm[0];
                if (b0 != 0x42 && b0 != 0x45 && b0 != 0x47 && b0 != 0x4C) break;
                /* Body length is bc bytes; the string may or may not be
                   NUL-terminated within it (e.g. 1PROCOMP.WMF stores
                   "Begin Skip" with bc=10 and no trailing NUL). Stop at
                   the first NUL we see, otherwise treat the whole body
                   as the string — matches js-wmf's read-until-NUL-or-end. */
                size_t plen = esc_parm_len < 32 ? esc_parm_len : 32;
                size_t slen;
                for (slen = 0; slen < plen && esc_parm[slen]; slen++) {}
                if (slen == 10 && strncmp((const char*)esc_parm, "Begin Skip", 10) == 0) skip_mode = 1;
                else if (slen == 8 && strncmp((const char*)esc_parm, "End Skip",  8) == 0) skip_mode = 0;
                /* Other Designer comments (Layer/Group/Gradient) — ignore. */
                break;
            }
            default:
                break;
            }
            break;
        }

        case META_DIBBITBLT: {
            /* MS-WMF 2.3.1.2: 14B + DIB if has_bitmap, else 16B (extra
               reserved word) and no DIB. Detection is a structural
               size check, per the spec note. */
            if (parm_len < 14) break;
            int has_bitmap = (rsize_words != 12);
            int16_t y_src = get_i16(parm, 4);
            int16_t x_src = get_i16(parm, 6);
            size_t off2 = 8;
            if (!has_bitmap) off2 += 2; /* reserved word */
            if (parm_len < off2 + 8) break;
            int16_t height = get_i16(parm, off2);
            int16_t width  = get_i16(parm, off2 + 2);
            int16_t y_dst  = get_i16(parm, off2 + 4);
            int16_t x_dst  = get_i16(parm, off2 + 6);

            try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
            if (has_bitmap) {
                size_t dib_off = off2 + 8;
                if (parm_len <= dib_off) break;
                emit_dib_blit(&b, parm + dib_off, parm_len - dib_off,
                              x_dst, y_dst, width, height);
            } else {
                buf_u8(&b, WMF_OP_BIT_COPY);
                buf_i16(&b, x_dst); buf_i16(&b, y_dst);
                buf_i16(&b, x_src); buf_i16(&b, y_src);
                buf_i16(&b, width); buf_i16(&b, height);
            }
            break;
        }
        case META_DIBSTRETCHBLT: {
            /* MS-WMF 2.3.1.5: 18B + DIB if has_bitmap, else 20B + no DIB. */
            if (parm_len < 18) break;
            int has_bitmap = (rsize_words != 14);
            int16_t src_h = get_i16(parm, 4);
            int16_t src_w = get_i16(parm, 6);
            int16_t y_src = get_i16(parm, 8);
            int16_t x_src = get_i16(parm, 10);
            size_t off2 = 12;
            if (!has_bitmap) off2 += 2;
            if (parm_len < off2 + 8) break;
            int16_t dst_h = get_i16(parm, off2);
            int16_t dst_w = get_i16(parm, off2 + 2);
            int16_t y_dst = get_i16(parm, off2 + 4);
            int16_t x_dst = get_i16(parm, off2 + 6);

            try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
            if (has_bitmap) {
                size_t dib_off = off2 + 8;
                if (parm_len <= dib_off) break;
                emit_dib_blit(&b, parm + dib_off, parm_len - dib_off,
                              x_dst, y_dst, dst_w, dst_h);
            } else {
                buf_u8(&b, WMF_OP_BIT_COPY);
                buf_i16(&b, x_dst); buf_i16(&b, y_dst);
                buf_i16(&b, x_src); buf_i16(&b, y_src);
                buf_i16(&b, src_w); buf_i16(&b, src_h);
            }
            break;
        }
        case META_RECTANGLE: {
            /* MS-WMF 2.3.3.17: 4 i16 (Bottom, Right, Top, Left).
               GDI fills the inclusive rect (left..right, top..bottom)
               with the current brush and strokes the outline with the
               current pen — same as a 4-point POLYGON of the corners,
               so we emit one instead of inventing a new opcode. */
            if (parm_len < 8) break;
            int16_t bot   = get_i16(parm, 0);
            int16_t right = get_i16(parm, 2);
            int16_t top   = get_i16(parm, 4);
            int16_t left  = get_i16(parm, 6);
            try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
            emit_pen_if_changed(&b, &state, &em);
            emit_brush_if_changed(&b, &state, &em);
            emit_pfm_if_changed(&b, &state, &em);
            buf_u8(&b, WMF_OP_POLYGON);
            buf_u16(&b, 4);
            buf_i16(&b, left);  buf_i16(&b, top);
            buf_i16(&b, right); buf_i16(&b, top);
            buf_i16(&b, right); buf_i16(&b, bot);
            buf_i16(&b, left);  buf_i16(&b, bot);
            break;
        }
        case META_ELLIPSE: {
            /* MS-WMF 2.3.3.3: 4 i16 (Bottom, Right, Top, Left). The
               ellipse is inscribed in this bounding rect. We emit a
               native ELLIPSE opcode rather than approximating with a
               polygon — Canvas2D's ctx.ellipse renders cleaner. */
            if (parm_len < 8) break;
            int16_t bot   = get_i16(parm, 0);
            int16_t right = get_i16(parm, 2);
            int16_t top   = get_i16(parm, 4);
            int16_t left  = get_i16(parm, 6);
            try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
            emit_pen_if_changed(&b, &state, &em);
            emit_brush_if_changed(&b, &state, &em);
            buf_u8(&b, WMF_OP_ELLIPSE);
            buf_i16(&b, left); buf_i16(&b, top);
            buf_i16(&b, right); buf_i16(&b, bot);
            break;
        }
        case META_STRETCHDIB: {
            /* MS-WMF 2.3.1.3: 11 fixed words after rfunc, DIB always
               present. RasterOp(4)/ColorUsage(2)/SrcH(2)/SrcW(2)/
               YSrc(2)/XSrc(2)/DestH(2)/DestW(2)/YDest(2)/XDest(2)/DIB. */
            if (parm_len < 22) break;
            int16_t dst_h = get_i16(parm, 14);
            int16_t dst_w = get_i16(parm, 16);
            int16_t y_dst = get_i16(parm, 18);
            int16_t x_dst = get_i16(parm, 20);
            if (parm_len <= 22) break;
            try_lock_bounds(&b, bounds_off, &state, ext_seen, org_seen, &bounds_locked, &last_emitted_org_x, &last_emitted_org_y, &last_emitted_ext_x, &last_emitted_ext_y);
            emit_dib_blit(&b, parm + 22, parm_len - 22,
                          x_dst, y_dst, dst_w, dst_h);
            break;
        }

        /* Records still ignored: BkMode/MapMode/StretchBltMode set DC
           state we don't yet need; clip-rect/region are TODO. */
        case META_SETBKMODE:
        case META_SETMAPMODE:
        case META_SETSTRETCHBLTMODE:
        case META_SETTEXTALIGN:
        case META_INTERSECTCLIPRECT:
        case META_SELECTCLIPREGION:
        default:
            break;
        }

        off += rsize;
    }

    /* If no drawing opcode ever locked BOUNDS — e.g. an empty WMF or
       a state-only file — fall back to the last-seen extent (or 0)
       with origin (0,0). Matches render.js's image_size fallback. */
    if (!bounds_locked && ext_seen) {
        buf_patch_i16(&b, bounds_off + 0, 0);
        buf_patch_i16(&b, bounds_off + 2, 0);
        buf_patch_i16(&b, bounds_off + 4, state.win_ext_x);
        buf_patch_i16(&b, bounds_off + 6, state.win_ext_y);
    }

    buf_u8(&b, WMF_OP_END);

    free(objects);
    free(path_sizes.data);
    free(path_points.data);

    if (b.oom) { free(b.data); hlp_set_error("WMF: oom"); return -1; }

    *out_ops = b.data;
    *out_len = b.len;
    return 0;
}
