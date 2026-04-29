#include "wmf.h"

/* WMF record opcodes we care about (subset, from MS-WMF). */
#define META_EOF              0x0000
#define META_SETWINDOWORG     0x020B
#define META_SETWINDOWEXT     0x020C

/* ---------- growable opcode buffer ------------------------------------ */

typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
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
    size_t cap = b->cap;
    while (cap < b->len + extra) cap *= 2;
    uint8_t* p = (uint8_t*)realloc(b->data, cap);
    if (!p) { b->oom = 1; return; }
    b->data = p;
    b->cap = cap;
}

static void buf_u8(Buf* b, uint8_t v) {
    buf_reserve(b, 1);
    if (b->oom) return;
    b->data[b->len++] = v;
}

static void buf_u16(Buf* b, uint16_t v) {
    buf_reserve(b, 2);
    if (b->oom) return;
    b->data[b->len++] = (uint8_t)v;
    b->data[b->len++] = (uint8_t)(v >> 8);
}

static void buf_i16(Buf* b, int16_t v) { buf_u16(b, (uint16_t)v); }

#if 0  /* unused until later phases */
static void buf_u32(Buf* b, uint32_t v) {
    buf_reserve(b, 4);
    if (b->oom) return;
    b->data[b->len++] = (uint8_t)v;
    b->data[b->len++] = (uint8_t)(v >> 8);
    b->data[b->len++] = (uint8_t)(v >> 16);
    b->data[b->len++] = (uint8_t)(v >> 24);
}
#endif

/* ---------- public API ------------------------------------------------ */

int wmf_parse(const uint8_t* data, size_t len,
              uint8_t** out_ops, size_t* out_len) {
    if (len < 18) { hlp_set_error("WMF: too short for header"); return -1; }

    /* METAHEADER */
    uint16_t type = get_u16(data, 0);
    if (type != 1 && type != 2) {
        hlp_set_error("WMF: bad header Type");
        return -1;
    }
    uint16_t hdr_words = get_u16(data, 2);
    if (hdr_words != 9) {
        hlp_set_error("WMF: bad header size");
        return -1;
    }
    uint16_t version = get_u16(data, 4);
    if (version != 0x0100 && version != 0x0300) {
        hlp_set_error("WMF: bad header version");
        return -1;
    }

    Buf b; buf_init(&b);
    if (b.oom) { hlp_set_error("WMF: oom"); return -1; }

    /* Scan records. For phase 1 we only locate SetWindowOrg /
       SetWindowExt so we can emit BOUNDS and call it a day. */
    int16_t orgX = 0, orgY = 0;
    int16_t extX = 0, extY = 0;
    int have_org = 0, have_ext = 0;

    size_t off = (size_t)hdr_words * 2;
    while (off + 6 <= len) {
        uint32_t rsize_words = get_u32(data, off);
        if (rsize_words < 3) break;
        size_t rsize = (size_t)rsize_words * 2;
        if (off + rsize > len) break;
        uint16_t rfunc = get_u16(data, off + 4);
        if (rfunc == META_EOF) break;

        const uint8_t* parm = data + off + 6;

        switch (rfunc) {
        case META_SETWINDOWORG:
            /* y first, then x (per MS-WMF 2.3.5.31) */
            orgY = get_i16(parm, 0);
            orgX = get_i16(parm, 2);
            have_org = 1;
            break;
        case META_SETWINDOWEXT:
            extY = get_i16(parm, 0);
            extX = get_i16(parm, 2);
            have_ext = 1;
            break;
        default:
            break;
        }

        off += rsize;
    }

    if (!have_ext) {
        free(b.data);
        hlp_set_error("WMF: no SetWindowExt");
        return -1;
    }
    (void)have_org; /* origin defaults to 0 if absent */

    buf_u8(&b, WMF_OP_BOUNDS);
    buf_i16(&b, orgX);
    buf_i16(&b, orgY);
    buf_i16(&b, extX);
    buf_i16(&b, extY);
    buf_u8(&b, WMF_OP_END);

    if (b.oom) { free(b.data); hlp_set_error("WMF: oom"); return -1; }

    *out_ops = b.data;
    *out_len = b.len;
    return 0;
}
