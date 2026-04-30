#include "wmf.h"

/* MS-WMF record types we recognize. */
#define META_EOF                  0x0000
#define META_SAVEDC               0x001E
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

#define WMF_STACK_MAX 32
#define WMF_OBJ_MAX   256

/* ---------- object table -------------------------------------------- */

typedef enum { OBJ_NONE = 0, OBJ_PEN, OBJ_BRUSH, OBJ_FONT } ObjKind;

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

static void emit_text_color_if_changed(Buf* b, State* st, Emitted* em) {
    if (!st->has_text_color) return;
    if (em->text_color_valid && em->text_color == st->text_color) return;
    buf_u8(b, WMF_OP_SET_TEXT_COLOR);
    buf_u32(b, st->text_color);
    em->text_color_valid = 1;
    em->text_color = st->text_color;
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

    /* BOUNDS placeholder — payload patched as SetWindowOrg/Ext are seen. */
    buf_u8(&b, WMF_OP_BOUNDS);
    size_t bounds_off = b.len;
    buf_i16(&b, 0); buf_i16(&b, 0); buf_i16(&b, 0); buf_i16(&b, 0);
    int org_locked = 0, ext_locked = 0;

    WmfObj objects[WMF_OBJ_MAX];
    for (int i = 0; i < WMF_OBJ_MAX; i++) objects[i].kind = OBJ_NONE;
    int objects_count = num_objects;

    State state; memset(&state, 0, sizeof(state));
    State stack[WMF_STACK_MAX];
    int stack_top = -1;
    Emitted em; memset(&em, 0, sizeof(em));

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
        case META_SETWINDOWORG: {
            if (parm_len < 4) break;
            int16_t y = get_i16(parm, 0);
            int16_t x = get_i16(parm, 2);
            state.win_org_x = x; state.win_org_y = y;
            if (!org_locked) {
                buf_patch_i16(&b, bounds_off + 0, x);
                buf_patch_i16(&b, bounds_off + 2, y);
                org_locked = 1;
            }
            break;
        }
        case META_SETWINDOWEXT: {
            if (parm_len < 4) break;
            int16_t y = get_i16(parm, 0);
            int16_t x = get_i16(parm, 2);
            state.win_ext_x = x; state.win_ext_y = y;
            if (!ext_locked) {
                buf_patch_i16(&b, bounds_off + 4, x);
                buf_patch_i16(&b, bounds_off + 6, y);
                ext_locked = 1;
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
            obj_add(objects, &objects_count, WMF_OBJ_MAX, &o);
            break;
        }
        case META_CREATEBRUSHINDIRECT: {
            if (parm_len < 8) break;
            WmfObj o = {0};
            o.kind = OBJ_BRUSH;
            o.u.brush.style = get_u16(parm, 0);
            o.u.brush.color = get_u32(parm, 2);
            o.u.brush.hatch = get_u16(parm, 6);
            obj_add(objects, &objects_count, WMF_OBJ_MAX, &o);
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
            obj_add(objects, &objects_count, WMF_OBJ_MAX, &o);
            break;
        }
        case META_DELETEOBJECT: {
            if (parm_len < 2) break;
            uint16_t idx = get_u16(parm, 0);
            if (idx < (uint16_t)objects_count) objects[idx].kind = OBJ_NONE;
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
            break;
        case META_RESTOREDC: {
            if (parm_len < 2) break;
            int16_t n = get_i16(parm, 0);
            int idx = (n >= 0) ? n : (stack_top + n + 1);
            if (idx >= 0 && idx <= stack_top) {
                state = stack[idx];
                stack_top = idx - 1;
            }
            break;
        }

        case META_POLYLINE: {
            if (parm_len < 2) break;
            uint16_t n = get_u16(parm, 0);
            if (parm_len < 2u + (size_t)n * 4u) break;
            emit_pen_if_changed(&b, &state, &em);
            buf_u8(&b, WMF_OP_POLYLINE);
            buf_u16(&b, n);
            for (uint16_t i = 0; i < n; i++) {
                buf_i16(&b, get_i16(parm, 2 + (size_t)i * 4));
                buf_i16(&b, get_i16(parm, 2 + (size_t)i * 4 + 2));
            }
            break;
        }
        case META_POLYGON: {
            if (parm_len < 2) break;
            uint16_t n = get_u16(parm, 0);
            if (parm_len < 2u + (size_t)n * 4u) break;
            emit_pen_if_changed(&b, &state, &em);
            emit_brush_if_changed(&b, &state, &em);
            emit_pfm_if_changed(&b, &state, &em);
            buf_u8(&b, WMF_OP_POLYGON);
            buf_u16(&b, n);
            for (uint16_t i = 0; i < n; i++) {
                buf_i16(&b, get_i16(parm, 2 + (size_t)i * 4));
                buf_i16(&b, get_i16(parm, 2 + (size_t)i * 4 + 2));
            }
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

            emit_pen_if_changed(&b, &state, &em);
            emit_brush_if_changed(&b, &state, &em);
            emit_pfm_if_changed(&b, &state, &em);
            buf_u8(&b, WMF_OP_POLYPOLYGON);
            buf_u16(&b, npoly);
            for (uint16_t i = 0; i < npoly; i++) buf_u16(&b, get_u16(sizes, (size_t)i * 2));
            for (uint32_t i = 0; i < total_pts; i++) {
                buf_i16(&b, get_i16(pts, (size_t)i * 4));
                buf_i16(&b, get_i16(pts, (size_t)i * 4 + 2));
            }
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

            emit_font_if_changed(&b, &state, &em);
            emit_text_color_if_changed(&b, &state, &em);

            buf_u8(&b, WMF_OP_TEXT);
            buf_i16(&b, x);
            buf_i16(&b, y);
            buf_u16(&b, slen);
            buf_bytes(&b, parm + soff, slen);
            break;
        }

        /* Records ignored in phase 3; later phases will plug in. */
        case META_SETBKMODE:
        case META_SETMAPMODE:
        case META_SETSTRETCHBLTMODE:
        case META_SETTEXTALIGN:
        case META_INTERSECTCLIPRECT:
        case META_SELECTCLIPREGION:
        case META_ESCAPE:
        case META_DIBBITBLT:
        case META_DIBSTRETCHBLT:
        default:
            break;
        }

        off += rsize;
    }

    buf_u8(&b, WMF_OP_END);
    if (b.oom) { free(b.data); hlp_set_error("WMF: oom"); return -1; }

    *out_ops = b.data;
    *out_len = b.len;
    return 0;
}
