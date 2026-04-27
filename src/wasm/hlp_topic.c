#include "hlp.h"

/* --- Opcode emitter --- */

#define OP_PAGE_START      0x01
#define OP_PARAGRAPH       0x02
#define OP_TEXT            0x03
#define OP_FONT_CHANGE     0x04
#define OP_LINE_BREAK      0x05
#define OP_TAB             0x06
#define OP_LINK_START      0x07
#define OP_LINK_END        0x08
#define OP_IMAGE           0x09
#define OP_HOTSPOT_LINK    0x0A
#define OP_TABLE_START     0x0B
#define OP_TABLE_CELL      0x0C
#define OP_TABLE_ROW_END   0x0D
#define OP_TABLE_END       0x0E
#define OP_MACRO           0x0F
#define OP_NON_SCROLL_START 0x10
#define OP_NON_SCROLL_END  0x11
#define OP_NBSP            0x12
#define OP_NBHYPHEN        0x13
#define OP_ANCHOR          0x14
#define OP_PAGE_END        0xFF

typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
} EmitBuf;

static EmitBuf g_emit = {0};

static void emit_ensure(size_t needed) {
    if (g_emit.len + needed > g_emit.cap) {
        size_t newcap = g_emit.cap ? g_emit.cap * 2 : 4096;
        while (newcap < g_emit.len + needed) newcap *= 2;
        g_emit.data = realloc(g_emit.data, newcap);
        g_emit.cap = newcap;
    }
}

static void emit_u8(uint8_t v) {
    emit_ensure(1);
    g_emit.data[g_emit.len++] = v;
}

static void emit_u16(uint16_t v) {
    emit_ensure(2);
    g_emit.data[g_emit.len++] = v & 0xFF;
    g_emit.data[g_emit.len++] = v >> 8;
}

static void emit_i16(int16_t v) { emit_u16((uint16_t)v); }

static void emit_u32(uint32_t v) {
    emit_ensure(4);
    g_emit.data[g_emit.len++] = v & 0xFF;
    g_emit.data[g_emit.len++] = (v >> 8) & 0xFF;
    g_emit.data[g_emit.len++] = (v >> 16) & 0xFF;
    g_emit.data[g_emit.len++] = (v >> 24) & 0xFF;
}

static void emit_bytes(const uint8_t* p, size_t n) {
    emit_ensure(n);
    memcpy(g_emit.data + g_emit.len, p, n);
    g_emit.len += n;
}

static void emit_text(const char* text, size_t len) {
    emit_u8(OP_TEXT);
    emit_u16((uint16_t)len);
    emit_bytes((const uint8_t*)text, len);
}

/* Compressed integer readers — bit 0 of first byte selects width */

static int32_t fetch_long(const uint8_t** p) {
    if (**p & 1) {
        int32_t ret = (int32_t)(get_u32(*p, 0) - 0x80000000) / 2;
        *p += 4;
        return ret;
    } else {
        int32_t ret = (int32_t)(get_u16(*p, 0) - 0x8000) / 2;
        *p += 2;
        return ret;
    }
}

static uint16_t fetch_ushort(const uint8_t** p) {
    if (**p & 1) {
        uint16_t ret = get_u16(*p, 0) / 2;
        *p += 2;
        return ret;
    } else {
        uint16_t ret = **p / 2;
        *p += 1;
        return ret;
    }
}

static int16_t fetch_short(const uint8_t** p) {
    if (**p & 1) {
        int16_t ret = (int16_t)(get_u16(*p, 0) - 0x8000) / 2;
        *p += 2;
        return ret;
    } else {
        int16_t ret = (int16_t)(**p - 0x80) / 2;
        *p += 1;
        return ret;
    }
}

/* --- Page index --- */

typedef struct {
    uint32_t ref;           /* topic link reference (for rendering) */
    uint32_t topic_offset;  /* topic offset (for lookups from |CONTEXT) */
    uint32_t browse_bwd;
    uint32_t browse_fwd;
    uint32_t scroll_ref;    /* first scrolling display record ref, 0xFFFFFFFF = all scrolling */
} HlpPage;

static HlpPage* g_pages = 0;
static unsigned  g_num_pages = 0;
static unsigned  g_pages_cap = 0;

/* Precomputed ref → TOPICOFFSET mapping (built during page indexing) */
static uint32_t* g_toff_refs = 0;
static uint32_t* g_toff_vals = 0;
static unsigned  g_toff_count = 0;
static unsigned  g_toff_cap = 0;

static void resolve_ref(HlpFile* hlp, uint32_t ref,
                        unsigned* out_index, unsigned* out_offset) {
    if (hlp->version <= 16) {
        *out_index  = (ref - 0x0C) / hlp->dsize;
        *out_offset = (ref - 0x0C) % hlp->dsize;
    } else {
        *out_index  = (ref - 0x0C) >> 14;
        *out_offset = (ref - 0x0C) & 0x3FFF;
    }
}

/* Binary search for ref in the precomputed TOPICOFFSET table.
   Refs are stored in linked-list order (monotonically increasing). */
static uint32_t lookup_toff(uint32_t ref) {
    unsigned lo = 0, hi = g_toff_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_toff_refs[mid] < ref) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_toff_count && g_toff_refs[lo] == ref)
        return g_toff_vals[lo];
    return 0;
}

static uint8_t* get_topic_block(HlpFile* hlp, uint32_t ref, uint8_t** out_end) {
    unsigned index, offset;
    resolve_ref(hlp, ref, &index, &offset);
    if (index >= hlp->topic_maplen) return 0;

    uint8_t* block = hlp->topic_map[index] + offset;
    if (block + 0x15 >= hlp->topic_end) return 0;

    uint32_t blocksize = get_u32(block, 0);
    *out_end = block + blocksize;
    if (*out_end > hlp->topic_end) *out_end = hlp->topic_end;
    return block;
}

void hlp_reset_topic_state(void) {
    g_pages = 0;
    g_num_pages = 0;
    g_pages_cap = 0;
    g_toff_refs = 0;
    g_toff_vals = 0;
    g_toff_count = 0;
    g_toff_cap = 0;
    g_emit.data = 0;
    g_emit.len = 0;
    g_emit.cap = 0;
}

int hlp_build_page_index(HlpFile* hlp) {
    g_pages = 0;
    g_num_pages = 0;
    g_pages_cap = 0;
    g_toff_refs = 0;
    g_toff_vals = 0;
    g_toff_count = 0;
    g_toff_cap = 0;
    g_emit.data = 0;
    g_emit.len = 0;
    g_emit.cap = 0;

    uint32_t ref = 0x0C;
    unsigned old_index = (unsigned)-1;
    unsigned offs = 0;

    while (ref != 0xFFFFFFFF) {
        unsigned index, offset;
        resolve_ref(hlp, ref, &index, &offset);

        if (hlp->version <= 16 && index != old_index && old_index != (unsigned)-1) {
            ref -= 12;
            resolve_ref(hlp, ref, &index, &offset);
        }

        if (index >= hlp->topic_maplen) break;
        uint8_t* buf = hlp->topic_map[index] + offset;
        if (buf + 0x15 >= hlp->topic_end) break;

        if (index != old_index) { offs = 0; old_index = index; }
        uint8_t rec_type = buf[0x14];

        /* Store ref → TOPICOFFSET mapping for all records */
        {
            uint32_t toff = (hlp->version <= 16)
                ? ref + index * 12
                : index * 0x8000 + offs;
            if (g_toff_count >= g_toff_cap) {
                unsigned newcap = g_toff_cap ? g_toff_cap * 2 : 256;
                uint32_t* nr = realloc(g_toff_refs, newcap * sizeof(uint32_t));
                uint32_t* nv = realloc(g_toff_vals, newcap * sizeof(uint32_t));
                if (!nr || !nv) break;
                g_toff_refs = nr;
                g_toff_vals = nv;
                g_toff_cap = newcap;
            }
            g_toff_refs[g_toff_count] = ref;
            g_toff_vals[g_toff_count] = toff;
            g_toff_count++;
        }

        if (rec_type == 0x02) {
            if (g_num_pages >= g_pages_cap) {
                g_pages_cap = g_pages_cap ? g_pages_cap * 2 : 256;
                g_pages = realloc(g_pages, g_pages_cap * sizeof(HlpPage));
            }
            HlpPage* page = &g_pages[g_num_pages++];
            page->ref = ref;
            page->browse_bwd = get_u32(buf, 0x19);
            page->browse_fwd = get_u32(buf, 0x1D);
            uint32_t datalen_hdr = get_u32(buf, 0x10);
            page->scroll_ref = (hlp->version > 16 && datalen_hdr >= 0x2D)
                ? get_u32(buf, 0x29) : 0xFFFFFFFF;

            if (hlp->version <= 16)
                page->topic_offset = ref + index * 12;
            else
                page->topic_offset = index * 0x8000 + offs;
        }

        /* Accumulate TopicLength (only 0x20 and 0x23 contribute per spec) */
        if (rec_type == 0x20 || rec_type == 0x23) {
            const uint8_t* fmt = buf + 0x15;
            fetch_long(&fmt);
            offs += fetch_ushort(&fmt);
        }

        uint32_t next = get_u32(buf, 0x0C);
        if (hlp->version <= 16) {
            if (next == 0) break;
            ref += next;
        } else {
            ref = next;
        }
    }

    return 0;
}

/* Find page by topic offset — pages are sorted, find the last page
   whose topic_offset <= target */
int hlp_find_page_by_topic_offset(uint32_t topic_offset) {
    int found = -1;
    for (unsigned i = 0; i < g_num_pages; i++) {
        if (g_pages[i].topic_offset <= topic_offset)
            found = (int)i;
        else
            break;
    }
    return found;
}

/* Find page by ref (exact match) */
static int find_page_by_ref(uint32_t ref) {
    for (unsigned i = 0; i < g_num_pages; i++) {
        if (g_pages[i].ref == ref) return (int)i;
    }
    return -1;
}

/* Resolve a hash from format codes to a topic offset for navigation.
   All link format codes (0xE0-0xEF) store hash values that need
   |CONTEXT lookup to get the actual topic offset. */
static uint32_t resolve_link_hash(HlpFile* hlp, uint32_t hash) {
    int32_t off = hlp_hash_to_offset(hlp, (int32_t)hash);
    return (off >= 0) ? (uint32_t)off : hash;
}

/* --- Paragraph renderer --- */

static void render_paragraph(HlpFile* hlp, uint8_t* buf, uint8_t* end, uint32_t topic_offset) {
    uint32_t blocksize = get_u32(buf, 0);
    uint32_t textsize = get_u32(buf, 4);
    uint32_t datalen = get_u32(buf, 0x10);
    uint8_t rec_type = buf[0x14];

    /* Decompress text */
    char* text_base = malloc(textsize + 1);
    if (!text_base) return;

    uint8_t* raw_text = buf + datalen;
    if (textsize > blocksize - datalen) {
        if (hlp->has_phrases)
            hlp_phrase_decompress2(hlp, raw_text, buf + blocksize,
                                  (uint8_t*)text_base, (uint8_t*)text_base + textsize);
        else if (hlp->has_phrases40)
            hlp_phrase_decompress3(hlp, raw_text, buf + blocksize,
                                  (uint8_t*)text_base, (uint8_t*)text_base + textsize);
        else {
            textsize = blocksize - datalen;
            memcpy(text_base, raw_text, textsize);
        }
    } else {
        memcpy(text_base, raw_text, textsize);
    }
    text_base[textsize] = '\0';

    char* text = text_base;
    char* text_end = text_base + textsize;

    const uint8_t* format = buf + 0x15;
    const uint8_t* format_end = buf + datalen;

    /* Parse paragraph header for v3.1+ */
    unsigned ncol = 1;
    if (rec_type == 0x20 || rec_type == 0x23) {
        fetch_long(&format);
        fetch_ushort(&format);
    }

    if (rec_type == 0x23) {
        /* Table header: ncol columns, each with gap(2) + width(2) */
        ncol = *format++;
        uint8_t ttype = *format++;
        int16_t table_width = 32767;
        if (ttype == 0 || ttype == 2) {
            table_width = get_i16(format, 0);
            format += 2;
        }
        emit_u8(OP_TABLE_START);
        emit_u8((uint8_t)ncol);
        emit_u8(ttype);
        /* Min table width (only meaningful for variable-width types 0,2) */
        emit_i16((ttype == 0 || ttype == 2) ? table_width : 0);
        /* Emit trleft and trgaph (half-gap), then cumulative cellx positions */
        /* trleft and trgaph. For variable-width tables (type 0,2),
           values are proportional to 32767 and need table_width scaling.
           For fixed tables (type 1,3), they're in native units like paragraphs. */
        int16_t trleft, trgaph;
        if (ncol > 1) {
            int16_t gap = get_i16(format, 6);
            int16_t w0 = get_i16(format, 2);
            if (ttype == 0 || ttype == 2) {
                /* Variable: scale by table_width/32767, result in native units */
                trgaph = (int16_t)((int32_t)gap * table_width / 32767);
                trleft = (int16_t)((int32_t)(w0 - gap) * table_width / 32767);
            } else {
                trgaph = gap;
                trleft = w0 - gap;
            }
        } else {
            trgaph = 0;
            if (ttype == 0 || ttype == 2) {
                trleft = (int16_t)((int32_t)get_i16(format, 2) * table_width / 32767);
            } else {
                trleft = get_i16(format, 2);
            }
        }
        emit_i16(trleft);
        emit_i16(trgaph);
        int16_t pos = (ncol > 1) ? get_i16(format, 6) / 2 : 0;
        for (unsigned c = 0; c < ncol; c++) {
            int16_t gap = get_i16(format, c * 4);
            int16_t wid = get_i16(format, c * 4 + 2);
            pos += gap + wid;
            int16_t cellx;
            if (ttype == 0 || ttype == 2) {
                cellx = (int16_t)((int32_t)pos * table_width / 32767);
            } else {
                cellx = pos;
            }
            emit_i16(gap);
            emit_i16(wid);
            emit_i16(cellx);
        }
        format += ncol * 4;
    }

    int16_t lastcol = -1;
    for (;;) {
        /* For tables, read column index from format stream */
        if (rec_type == 0x23) {
            int16_t col = get_i16(format, 0);
            if (col == -1) {
                /* End of row */
                emit_u8(OP_TABLE_ROW_END);
                break;
            }
            lastcol = col;
            format += 5; /* column(2) + unknown(2) + always0(1) */
            if (col > 0) emit_u8(OP_TABLE_CELL);
        }

        /* Skip per-paragraph header */
        if (rec_type == 0x01)
            format += 6;
        else
            format += 4;

        /* Paragraph formatting bits (plain u16) */
        uint16_t bits = get_u16(format, 0);
        format += 2;

        uint16_t pflags = bits;
        int16_t space_before = 0, space_after = 0, line_space = 0;
        int16_t indent_left = 0, indent_right = 0, indent_first = 0;
        uint8_t alignment = 0;
        uint8_t border_flags = 0;
        int16_t border_width = 0;

        if (bits & 0x0001) fetch_long(&format);
        if (bits & 0x0002) space_before = fetch_short(&format);
        if (bits & 0x0004) space_after = fetch_short(&format);
        if (bits & 0x0008) line_space = fetch_short(&format);
        if (bits & 0x0010) indent_left = fetch_short(&format);
        if (bits & 0x0020) indent_right = fetch_short(&format);
        if (bits & 0x0040) indent_first = fetch_short(&format);
        if (bits & 0x0100) {
            border_flags = *format++;
            border_width = get_i16(format, 0);
            format += 2;
        }
        uint16_t tab_stops[32];
        uint8_t num_tabs = 0;
        if (bits & 0x0200) {
            int16_t ntab = fetch_short(&format);
            for (int i = 0; i < ntab && i < 32; i++) {
                uint16_t tab = fetch_ushort(&format);
                tab_stops[num_tabs++] = tab & 0x3FFF; /* position in half-points */
                if (tab & 0x4000) fetch_ushort(&format); /* tab type (right/center) */
            }
        }
        switch (bits & 0x0C00) {
        case 0x000: alignment = 0; break;
        case 0x400: alignment = 1; break;
        case 0x800: alignment = 2; break;
        }

        /* Emit paragraph formatting — \pard in RTF.
           This sets formatting state but does NOT create a new block.
           Only 0x82 (\par) creates an actual paragraph break.
           Values are in the file's native units (half-points for old fonts,
           twips for new fonts). JS applies the scale factor from HlpInfo. */
        emit_u8(OP_PARAGRAPH);
        emit_u32(topic_offset + (unsigned)(text - text_base));
        emit_u16(pflags);
        emit_i16(space_before);
        emit_i16(space_after);
        emit_i16(line_space);
        emit_i16(indent_left);
        emit_i16(indent_right);
        emit_i16(indent_first);
        emit_u8(alignment);
        emit_u8(border_flags);
        emit_i16(border_width);
        emit_u8(num_tabs);
        for (int i = 0; i < num_tabs; i++)
            emit_u16(tab_stops[i]);

        /* Walk text + format codes */
        while (text < text_end && format < format_end) {
            size_t tlen = strlen(text);
            if (tlen > 0)
                emit_text(text, tlen);
            text += tlen + 1;

            if (*format == 0xFF) {
                format++;
                break;
            }

            switch (*format) {
            case 0x20:
                format += 5;
                break;
            case 0x21:
                format += 3;
                break;
            case 0x80: {
                uint16_t font = get_u16(format, 1);
                emit_u8(OP_FONT_CHANGE);
                emit_u16(font);
                format += 3;
                break;
            }
            case 0x81:
                emit_u8(OP_LINE_BREAK);
                format += 1;
                break;
            case 0x82:
                /* \par — paragraph break. Signals a new block.
                   Emit updated TOPICOFFSET so each div gets a unique id. */
                emit_u8(0x82);
                emit_u32(topic_offset + (unsigned)(text - text_base));
                format += 1;
                break;
            case 0x83:
                emit_u8(OP_TAB);
                format += 1;
                break;
            case 0x86:
            case 0x87:
            case 0x88: {
                uint8_t pos = *format - 0x86;
                uint8_t itype = format[1];
                format += 2;
                int32_t size = fetch_long(&format);
                if (itype == 0x22) fetch_ushort(&format);
                if (itype == 0x03 || itype == 0x22) {
                    uint16_t by_ref = get_u16(format, 0);
                    if (by_ref == 0) {
                        uint16_t bm_idx = get_u16(format, 2);
                        emit_u8(OP_IMAGE);
                        emit_u8(pos);
                        emit_u32(bm_idx);
                        /* Emit hotspot links for this image */
                        HlpHotspot spots[32];
                        int nspots = hlp_get_bitmap_hotspots(hlp, bm_idx, spots, 32);
                        for (int s = 0; s < nspots; s++) {
                            if (spots[s].kind == 2 && spots[s].macro) {
                                /* Macro hotspot — emit as HOTSPOT_LINK + MACRO */
                                emit_u8(OP_HOTSPOT_LINK);
                                emit_u8(2);
                                emit_u32(0);
                                emit_u16(spots[s].x);
                                emit_u16(spots[s].y);
                                emit_u16(spots[s].w);
                                emit_u16(spots[s].h);
                                uint16_t mlen = (uint16_t)strlen(spots[s].macro);
                                emit_u8(OP_MACRO);
                                emit_u16(mlen);
                                emit_bytes((const uint8_t*)spots[s].macro, mlen);
                            } else {
                                uint32_t target = spots[s].hash >= 0 ? spots[s].hash : 0;
                                emit_u8(OP_HOTSPOT_LINK);
                                emit_u8(spots[s].kind);
                                emit_u32(target);
                                emit_u16(spots[s].x);
                                emit_u16(spots[s].y);
                                emit_u16(spots[s].w);
                                emit_u16(spots[s].h);
                            }
                        }
                    }
                }
                format += size;
                break;
            }
            case 0x89:
                emit_u8(OP_LINK_END);
                format += 1;
                break;
            case 0x8B:
                emit_u8(OP_NBSP);
                format += 1;
                break;
            case 0x8C:
                emit_u8(OP_NBHYPHEN);
                format += 1;
                break;
            case 0xC8:
            case 0xCC: {
                uint16_t mlen = get_u16(format, 1);
                const char* macro = (const char*)format + 3;
                uint16_t slen = (uint16_t)strlen(macro);
                emit_u8(OP_LINK_START);
                emit_u8(2); /* macro */
                emit_u32(0);
                emit_u16(0); /* no file */
                emit_u16(0xFFFF); /* window */
                emit_u8(OP_MACRO);
                emit_u16(slen);
                emit_bytes((const uint8_t*)macro, slen);
                format += 3 + mlen;
                break;
            }
            case 0xE0:
            case 0xE1: {
                uint32_t target = resolve_link_hash(hlp, get_u32(format, 1));
                emit_u8(OP_LINK_START);
                emit_u8((*format & 1) ? 0 : 1);
                emit_u32(target);
                emit_u16(0);
                emit_u16(0xFFFF);
                format += 5;
                break;
            }
            case 0xE2:
            case 0xE3:
            case 0xE6:
            case 0xE7: {
                uint32_t target = resolve_link_hash(hlp, get_u32(format, 1));
                emit_u8(OP_LINK_START);
                emit_u8((*format & 1) ? 0 : 1);
                emit_u32(target);
                emit_u16(0);
                emit_u16(0xFFFF);
                format += 5;
                break;
            }
            case 0xEA:
            case 0xEB:
            case 0xEE:
            case 0xEF: {
                uint16_t link_size = get_u16(format, 1);
                uint32_t target = resolve_link_hash(hlp, get_u32(format, 4));
                uint8_t etype = format[3];
                const char* fname = (const char*)format + 8;

                emit_u8(OP_LINK_START);
                emit_u8((*format & 1) ? 0 : 1);
                emit_u32(target);

                if (etype == 4 || etype == 6) {
                    uint16_t flen = (uint16_t)strlen(fname);
                    emit_u16(flen);
                    emit_bytes((const uint8_t*)fname, flen);
                } else {
                    emit_u16(0);
                }
                emit_u16(0xFFFF);
                format += 3 + link_size;
                break;
            }
            default:
                format++;
                break;
            }
        }

        /* For non-table records, only one paragraph per call */
        if (rec_type != 0x23) break;
    }

    if (rec_type == 0x23)
        emit_u8(OP_TABLE_END);

    free(text_base);
}

/* --- Render a page by its index --- */

static int render_page_by_index(HlpFile* hlp, unsigned page_idx) {
    if (page_idx >= g_num_pages) {
        hlp_set_error("page index out of range");
        return -1;
    }

    g_emit.len = 0;
    HlpPage* page = &g_pages[page_idx];

    /* Read title from topic header */
    uint8_t* end;
    uint8_t* buf = get_topic_block(hlp, page->ref, &end);
    if (!buf) { hlp_set_error("bad topic ref"); return -1; }

    uint32_t blocksize = get_u32(buf, 0);
    uint32_t titlesize = get_u32(buf, 4);
    uint32_t datalen = get_u32(buf, 0x10);

    char title[256] = {0};
    if (titlesize > 0 && titlesize < sizeof(title)) {
        uint8_t* title_src = buf + datalen;
        if (titlesize > blocksize - datalen) {
            if (hlp->has_phrases)
                hlp_phrase_decompress2(hlp, title_src, buf + blocksize,
                                      (uint8_t*)title, (uint8_t*)title + titlesize);
            else if (hlp->has_phrases40)
                hlp_phrase_decompress3(hlp, title_src, buf + blocksize,
                                      (uint8_t*)title, (uint8_t*)title + titlesize);
        } else {
            memcpy(title, title_src, titlesize);
        }
        title[titlesize] = '\0';
    }

    /* PAGE_START */
    size_t title_len = strlen(title);
    emit_u8(OP_PAGE_START);
    emit_u32((uint32_t)title_len);
    emit_bytes((const uint8_t*)title, title_len);
    emit_u32(page->browse_bwd);
    emit_u32(page->browse_fwd);

    /* Emit auto-execute macros from title field (after title + null).
       Only emit strings that look like macro calls (contain '('). */
    if (titlesize > title_len + 1) {
        char* macro_ptr = title + title_len + 1;
        while (macro_ptr < title + titlesize) {
            size_t mlen = strlen(macro_ptr);
            if (mlen > 0 && memchr(macro_ptr, '(', mlen)) {
                emit_u8(OP_MACRO);
                emit_u16((uint16_t)mlen);
                emit_bytes((const uint8_t*)macro_ptr, mlen);
            }
            macro_ptr += mlen + 1;
        }
    }

    /* Walk display records following this topic header */
    uint32_t ref = page->ref;
    unsigned old_index = (unsigned)-1;
    int seen_header = 0;
    int in_nsr = 0;
    uint32_t scroll_ref = page->scroll_ref;
    while (ref != 0xFFFFFFFF) {
        unsigned index, offset;
        resolve_ref(hlp, ref, &index, &offset);

        if (hlp->version <= 16 && index != old_index && old_index != (unsigned)-1) {
            ref -= 12;
            resolve_ref(hlp, ref, &index, &offset);
        }

        if (index >= hlp->topic_maplen) break;
        buf = hlp->topic_map[index] + offset;
        if (buf + 0x15 >= hlp->topic_end) break;
        end = buf + get_u32(buf, 0);
        if (end > hlp->topic_end) end = hlp->topic_end;
        old_index = index;

        uint8_t rec_type = buf[0x14];
        uint32_t toff = lookup_toff(ref);

        if (rec_type == 0x02) {
            if (seen_header) break; /* next page's header — stop */
            seen_header = 1;
            emit_u8(OP_ANCHOR);
            emit_u32(toff);
        } else if (rec_type == 0x20 || rec_type == 0x23 || rec_type == 0x01) {
            /* Non-scrolling region boundary: records before scroll_ref are NSR */
            if (scroll_ref != 0xFFFFFFFF && !in_nsr && ref < scroll_ref) {
                emit_u8(OP_NON_SCROLL_START);
                in_nsr = 1;
            }
            if (in_nsr && ref >= scroll_ref) {
                emit_u8(OP_NON_SCROLL_END);
                in_nsr = 0;
            }

            render_paragraph(hlp, buf, end, toff);
        }

        uint32_t next = get_u32(buf, 0x0C);
        if (hlp->version <= 16) {
            if (next == 0) break;
            ref += next;
        } else {
            ref = next;
        }
    }

    if (in_nsr) emit_u8(OP_NON_SCROLL_END);
    emit_u8(OP_PAGE_END);
    return 0;
}

/* --- Public API --- */

/* Render page by topic offset (as stored in link targets and |CONTEXT) */
int hlp_render_page_by_topic_offset(HlpFile* hlp, uint32_t topic_offset,
                                    const uint8_t** out_ptr, size_t* out_len) {
    int idx = hlp_find_page_by_topic_offset(topic_offset);
    if (idx < 0) {
        if (g_num_pages > 0) idx = 0;
        else { hlp_set_error("no pages"); return -1; }
    }
    int rc = render_page_by_index(hlp, (unsigned)idx);
    if (rc != 0) return rc;
    *out_ptr = g_emit.data;
    *out_len = g_emit.len;
    return 0;
}

/* Render page by hash (resolve via |CONTEXT B+ tree) */
int hlp_render_page_by_hash(HlpFile* hlp, int32_t hash,
                            const uint8_t** out_ptr, size_t* out_len) {
    int32_t topic_offset = hlp_hash_to_offset(hlp, hash);
    if (topic_offset < 0) {
        hlp_set_error("hash not found in |CONTEXT");
        return -1;
    }
    return hlp_render_page_by_topic_offset(hlp, (uint32_t)topic_offset, out_ptr, out_len);
}

int hlp_render_first_page(HlpFile* hlp,
                          const uint8_t** out_ptr, size_t* out_len) {
    if (g_num_pages == 0) { hlp_set_error("no pages"); return -1; }
    int rc = render_page_by_index(hlp, 0);
    if (rc != 0) return rc;
    *out_ptr = g_emit.data;
    *out_len = g_emit.len;
    return 0;
}

unsigned hlp_get_page_count(void) {
    return g_num_pages;
}

/* Get topic offset for page at given index */
uint32_t hlp_get_page_topic_offset(unsigned index) {
    if (index >= g_num_pages) return 0;
    return g_pages[index].topic_offset;
}

/* Get title for page at given index. Returns pointer to static buffer. */
static char g_title_buf[256];

const char* hlp_get_page_title(HlpFile* hlp, unsigned index) {
    if (index >= g_num_pages) return "";
    uint8_t* end;
    uint8_t* buf = get_topic_block(hlp, g_pages[index].ref, &end);
    if (!buf) return "";

    uint32_t blocksize = get_u32(buf, 0);
    uint32_t titlesize = get_u32(buf, 4);
    uint32_t datalen = get_u32(buf, 0x10);

    if (titlesize == 0 || titlesize >= sizeof(g_title_buf)) return "";

    uint8_t* title_src = buf + datalen;
    if (titlesize > blocksize - datalen) {
        if (hlp->has_phrases)
            hlp_phrase_decompress2(hlp, title_src, buf + blocksize,
                                  (uint8_t*)g_title_buf, (uint8_t*)g_title_buf + titlesize);
        else if (hlp->has_phrases40)
            hlp_phrase_decompress3(hlp, title_src, buf + blocksize,
                                  (uint8_t*)g_title_buf, (uint8_t*)g_title_buf + titlesize);
        else return "";
    } else {
        memcpy(g_title_buf, title_src, titlesize);
    }
    g_title_buf[titlesize] = '\0';
    return g_title_buf;
}

