#include "hlp.h"
#include "wmf/wmf.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#define STBIW_MALLOC(sz)       malloc(sz)
#define STBIW_REALLOC(p,newsz) realloc(p,newsz)
#define STBIW_FREE(p)          free(p)
#include "stb_image_write.h"

/* Compressed unsigned long reader (same format as fetch_ushort but 4-byte) */
static uint32_t fetch_ulong(const uint8_t** p) {
    if (**p & 1) {
        uint32_t ret = get_u32(*p, 0) / 2;
        *p += 4;
        return ret;
    } else {
        uint32_t ret = get_u16(*p, 0) / 2;
        *p += 2;
        return ret;
    }
}

static uint16_t img_fetch_ushort(const uint8_t** p) {
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

/* LZ77 decompression (duplicated here to keep hlp_image self-contained) */
static size_t img_lz77_size(const uint8_t* ptr, const uint8_t* end) {
    size_t newsize = 0;
    while (ptr < end) {
        int mask = *ptr++;
        for (int i = 0; i < 8 && ptr < end; i++, mask >>= 1) {
            if (mask & 1) { newsize += 3 + (get_u16(ptr, 0) >> 12); ptr += 2; }
            else { newsize++; ptr++; }
        }
    }
    return newsize;
}

static uint8_t* img_lz77_decompress(const uint8_t* ptr, const uint8_t* end, uint8_t* dst) {
    while (ptr < end) {
        int mask = *ptr++;
        for (int i = 0; i < 8 && ptr < end; i++, mask >>= 1) {
            if (mask & 1) {
                uint16_t code = get_u16(ptr, 0);
                int len = 3 + (code >> 12);
                int offset = code & 0xFFF;
                for (; len > 0; len--, dst++) *dst = *(dst - offset - 1);
                ptr += 2;
            } else {
                *dst++ = *ptr++;
            }
        }
    }
    return dst;
}

/* Decompress graphics data based on packing type */
static uint8_t* decompress_gfx(const uint8_t* src, uint32_t csz, uint32_t sz,
                                uint8_t packing, uint8_t** alloc) {
    switch (packing) {
    case 0: /* uncompressed */
        *alloc = 0;
        return (uint8_t*)src;
    case 1: { /* RLE */
        *alloc = malloc(sz);
        if (!*alloc) return 0;
        hlp_rle_decompress(src, src + csz, *alloc, sz);
        return *alloc;
    }
    case 2: { /* LZ77 */
        size_t sz77 = img_lz77_size(src, src + csz);
        *alloc = malloc(sz77);
        if (!*alloc) return 0;
        img_lz77_decompress(src, src + csz, *alloc);
        return *alloc;
    }
    case 3: { /* LZ77 then RLE */
        size_t sz77 = img_lz77_size(src, src + csz);
        uint8_t* tmp = malloc(sz77);
        if (!tmp) return 0;
        img_lz77_decompress(src, src + csz, tmp);
        *alloc = malloc(sz);
        if (!*alloc) { free(tmp); return 0; }
        hlp_rle_decompress(tmp, tmp + sz77, *alloc, sz);
        free(tmp);
        return *alloc;
    }
    default:
        *alloc = 0;
        return 0;
    }
}

/* Convert a paletted or RGB DIB to RGBA pixels */
uint8_t* dib_to_rgba(uint32_t width, uint32_t height, uint16_t bpp,
                     const uint8_t* palette, uint32_t ncolors,
                     const uint8_t* pixels, uint32_t* out_size) {
    uint32_t stride = ((width * bpp + 31) & ~31) / 8;
    uint32_t rgba_size = width * height * 4;
    uint8_t* rgba = malloc(rgba_size);
    if (!rgba) return 0;
    *out_size = rgba_size;

    for (uint32_t y = 0; y < height; y++) {
        /* DIBs are bottom-up */
        const uint8_t* row = pixels + (height - 1 - y) * stride;
        uint8_t* dst = rgba + y * width * 4;

        for (uint32_t x = 0; x < width; x++) {
            uint8_t r, g, b, a = 255;
            if (bpp <= 8) {
                uint32_t idx;
                if (bpp == 1)
                    idx = (row[x / 8] >> (7 - (x & 7))) & 1;
                else if (bpp == 4)
                    idx = (x & 1) ? (row[x / 2] & 0x0F) : (row[x / 2] >> 4);
                else
                    idx = row[x];
                if (idx >= ncolors) idx = 0;
                b = palette[idx * 4 + 0];
                g = palette[idx * 4 + 1];
                r = palette[idx * 4 + 2];
            } else if (bpp == 24) {
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
            } else if (bpp == 32) {
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
            } else {
                r = g = b = 128;
            }
            dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
            dst += 4;
        }
    }
    return rgba;
}

static uint16_t g_last_img_w = 0, g_last_img_h = 0;
static uint8_t g_last_img_type = 0; /* 5/6=bitmap(pixels), 8=metafile(twips) */

/* Decode a bitmap from |bm{index} and return PNG data */
int hlp_decode_bitmap(HlpFile* hlp, uint32_t bm_index,
                      const uint8_t** out_ptr, size_t* out_len) {
    char name[16];
    snprintf(name, sizeof(name), "|bm%u", bm_index);

    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, name, &buf, &end) != 0) {
        hlp_set_error("bitmap subfile not found");
        return -1;
    }

    uint8_t* ref = buf + 9;
    uint16_t numpict = get_u16(ref, 2);
    if (numpict == 0) { hlp_set_error("no pictures"); return -1; }

    /* Use first picture format */
    const uint8_t* beg = ref + get_u32(ref, 4);
    uint8_t type = beg[0];
    uint8_t pack = beg[1];
    const uint8_t* ptr = beg + 2;

    if (type == 5 || type == 6) {
        /* Device independent or dependent bitmap */
        uint32_t xpels  = fetch_ulong(&ptr);
        uint32_t ypels  = fetch_ulong(&ptr);
        uint16_t planes = img_fetch_ushort(&ptr);
        uint16_t bpp    = img_fetch_ushort(&ptr);
        uint32_t width  = fetch_ulong(&ptr);
        uint32_t height = fetch_ulong(&ptr);
        uint32_t clrused = fetch_ulong(&ptr);
        uint32_t clrimp  = fetch_ulong(&ptr);
        (void)xpels; (void)ypels; (void)planes; (void)clrimp;
        g_last_img_w = (uint16_t)width;
        g_last_img_h = (uint16_t)height;
        g_last_img_type = type;

        uint32_t image_size = (((width * bpp + 31) & ~31) / 8) * height;
        uint32_t csz = fetch_ulong(&ptr);
        fetch_ulong(&ptr); /* hotspot size */

        uint32_t data_off = get_u32(ptr, 0); ptr += 4;
        ptr += 4; /* hotspot offset */

        /* Read palette for type 6 (DIB) */
        uint32_t ncolors = 0;
        const uint8_t* palette = 0;
        if (type == 6) {
            ncolors = clrused;
            if (!ncolors && bpp <= 8) ncolors = 1u << bpp;
            palette = ptr;
        }

        /* Decompress pixel data */
        uint8_t* alloc = 0;
        uint8_t* pixels = decompress_gfx(beg + data_off, csz, image_size, pack, &alloc);
        if (!pixels) { hlp_set_error("gfx decompress failed"); return -1; }

        /* Convert to RGBA */
        uint32_t rgba_size;
        uint8_t* rgba = dib_to_rgba(width, height, bpp, palette, ncolors, pixels, &rgba_size);
        free(alloc);
        if (!rgba) { hlp_set_error("RGBA conversion failed"); return -1; }

        /* Encode as PNG */
        if (width == 0 || height == 0) {
            hlp_set_error("zero dimensions");
            free(rgba);
            return -1;
        }

        int png_len = 0;
        uint8_t* png = stbi_write_png_to_mem(rgba, width * 4, width, height, 4, &png_len);
        free(rgba);
        if (!png) {
            hlp_set_error("PNG encoding failed");
            return -1;
        }

        *out_ptr = png;
        *out_len = (size_t)png_len;
        return 0;
    }

    if (type == 8) {
        /* Windows Metafile — parse header then extract embedded DIB from
           STRETCHDIB / DIBBITBLT / DIBSTRETCHBLT records */
        uint16_t mm = img_fetch_ushort(&ptr); /* mapping mode */
        uint16_t mf_width = get_u16(ptr, 0);
        uint16_t mf_height = get_u16(ptr, 2);
        ptr += 4;
        (void)mm;
        g_last_img_w = mf_width;
        g_last_img_h = mf_height;
        g_last_img_type = 8;

        uint32_t dsize = fetch_ulong(&ptr);
        uint32_t csize = fetch_ulong(&ptr);
        fetch_ulong(&ptr); /* hotspot size */
        uint32_t data_off = get_u32(ptr, 0); ptr += 4;
        ptr += 4; /* hotspot offset */

        /* Decompress WMF data */
        uint8_t* alloc = 0;
        uint8_t* wmf = decompress_gfx(beg + data_off, csize, dsize, pack, &alloc);
        if (!wmf) { hlp_set_error("WMF decompress failed"); return -1; }

        /* Parse METAHEADER */
        uint16_t mt_hdr_size = get_u16(wmf, 2); /* in 16-bit words */
        uint32_t mt_total = get_u32(wmf, 6);    /* total size in words */
        (void)mt_total;

        /* Scan records for STRETCHDIB (0x0F43), DIBBITBLT (0x0940),
           or DIBSTRETCHBLT (0x0B41) */
        uint32_t rec_off = mt_hdr_size * 2;
        while (rec_off + 6 <= dsize) {
            uint32_t rd_size = get_u32(wmf, rec_off);
            uint16_t rd_func = get_u16(wmf, rec_off + 4);
            if (rd_size == 0 || rd_func == 0) break;

            if (rd_func == 0x0F43) {
                /* META_STRETCHDIB: rdParm[11] is the BITMAPINFO
                   (11 WORDs = 22 bytes of params before DIB) */
                const uint8_t* dib = wmf + rec_off + 6 + 2*11;
                uint32_t dib_size = rd_size * 2 - 6 - 22;

                /* Read BITMAPINFOHEADER */
                uint32_t bi_size = get_u32(dib, 0);
                if (bi_size < 40) break;
                int32_t width = get_i32(dib, 4);
                int32_t height = get_i32(dib, 8);
                uint16_t bpp = get_u16(dib, 14);
                uint32_t clrused = get_u32(dib, 32);

                if (width <= 0 || height <= 0) break;

                uint32_t ncolors = clrused;
                if (!ncolors && bpp <= 8) ncolors = 1u << bpp;
                const uint8_t* palette = dib + bi_size;
                const uint8_t* pixels = palette + ncolors * 4;
                uint32_t image_size = (((width * bpp + 31) & ~31) / 8) * height;
                (void)dib_size; (void)image_size;

                uint32_t rgba_size;
                uint8_t* rgba = dib_to_rgba(width, height, bpp, palette, ncolors, pixels, &rgba_size);
                free(alloc);
                if (!rgba) { hlp_set_error("WMF DIB conversion failed"); return -1; }

                int png_len = 0;
                uint8_t* png = stbi_write_png_to_mem(rgba, width * 4, width, height, 4, &png_len);
                free(rgba);
                if (!png) { hlp_set_error("WMF PNG failed"); return -1; }

                *out_ptr = png;
                *out_len = (size_t)png_len;
                return 0;
            }

            if (rd_func == 0x0940 || rd_func == 0x0B41) {
                /* DIBBITBLT: rdParm[8], DIBSTRETCHBLT: rdParm[10] */
                uint32_t param_words = (rd_func == 0x0940) ? 8 : 10;
                if (rd_func == 0x0940 && rd_size <= 12) { rec_off += rd_size * 2; continue; }
                const uint8_t* dib = wmf + rec_off + 6 + param_words * 2;

                uint32_t bi_size = get_u32(dib, 0);
                if (bi_size < 40) break;
                int32_t width = get_i32(dib, 4);
                int32_t height = get_i32(dib, 8);
                uint16_t bpp = get_u16(dib, 14);
                uint32_t clrused = get_u32(dib, 32);

                if (width <= 0 || height <= 0) break;

                uint32_t ncolors = clrused;
                if (!ncolors && bpp <= 8) ncolors = 1u << bpp;
                const uint8_t* palette = dib + bi_size;
                const uint8_t* pixels = palette + ncolors * 4;

                uint32_t rgba_size;
                uint8_t* rgba = dib_to_rgba(width, height, bpp, palette, ncolors, pixels, &rgba_size);
                free(alloc);
                if (!rgba) { hlp_set_error("WMF DIB conversion failed"); return -1; }

                int png_len = 0;
                uint8_t* png = stbi_write_png_to_mem(rgba, width * 4, width, height, 4, &png_len);
                free(rgba);
                if (!png) { hlp_set_error("WMF PNG failed"); return -1; }

                *out_ptr = png;
                *out_len = (size_t)png_len;
                return 0;
            }

            rec_off += rd_size * 2;
        }

        free(alloc);
        hlp_set_error("WMF: no embedded bitmap found");
        return -1;
    }

    {
        char err[64];
        snprintf(err, sizeof(err), "unsupported image type %d", type);
        hlp_set_error(err);
    }
    return -1;
}

/* Decompress and hand back the raw WMF byte stream for a type=8 |bm{n}.
   *out_alloc owns memory (free with free()) unless the WMF data was
   stored uncompressed, in which case *out_alloc is NULL. */
int hlp_get_wmf_bytes(HlpFile* hlp, uint32_t bm_index,
                      uint8_t** out_alloc, const uint8_t** out_data,
                      size_t* out_len) {
    char name[16];
    snprintf(name, sizeof(name), "|bm%u", bm_index);

    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, name, &buf, &end) != 0) {
        hlp_set_error("bitmap subfile not found");
        return -1;
    }

    uint8_t* ref = buf + 9;
    uint16_t numpict = get_u16(ref, 2);
    if (numpict == 0) { hlp_set_error("no pictures"); return -1; }

    const uint8_t* beg = ref + get_u32(ref, 4);
    uint8_t type = beg[0];
    uint8_t pack = beg[1];
    const uint8_t* ptr = beg + 2;

    if (type != 8) { hlp_set_error("not a metafile"); return -1; }

    uint16_t mm = img_fetch_ushort(&ptr); /* mapping mode */
    uint16_t mf_w = get_u16(ptr, 0);
    uint16_t mf_h = get_u16(ptr, 2);
    ptr += 4;
    (void)mm;
    g_last_img_w = mf_w;
    g_last_img_h = mf_h;
    g_last_img_type = 8;

    uint32_t dsize = fetch_ulong(&ptr);
    uint32_t csize = fetch_ulong(&ptr);
    fetch_ulong(&ptr); /* hotspot size */
    uint32_t data_off = get_u32(ptr, 0); ptr += 4;
    ptr += 4; /* hotspot offset */

    uint8_t* alloc = 0;
    uint8_t* wmf = decompress_gfx(beg + data_off, csize, dsize, pack, &alloc);
    if (!wmf) { hlp_set_error("WMF decompress failed"); return -1; }

    *out_alloc = alloc;
    *out_data = wmf;
    *out_len = dsize;
    return 0;
}

int hlp_peek_image_type(HlpFile* hlp, uint32_t bm_index) {
    char name[16];
    snprintf(name, sizeof(name), "|bm%u", bm_index);
    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, name, &buf, &end) != 0) return -1;
    uint8_t* ref = buf + 9;
    if (get_u16(ref, 2) == 0) return -1;
    const uint8_t* beg = ref + get_u32(ref, 4);
    return (int)beg[0];
}

int hlp_decode_image_wmf(HlpFile* hlp, uint32_t bm_index,
                         const uint8_t** out_ops, size_t* out_len) {
    uint8_t* alloc = 0;
    const uint8_t* data = 0;
    size_t data_len = 0;
    if (hlp_get_wmf_bytes(hlp, bm_index, &alloc, &data, &data_len) != 0)
        return -1;

    uint8_t* ops = 0;
    size_t ops_len = 0;
    int rc = wmf_parse(data, data_len, &ops, &ops_len);
    free(alloc);
    if (rc != 0) return -1;

    *out_ops = ops;
    *out_len = ops_len;
    return 0;
}

/*
 * Parse hotspot data from a bitmap and emit HOTSPOT_LINK opcodes.
 * Called from hlp_topic.c after emitting an IMAGE opcode.
 * emit_fn is a callback to write opcodes.
 */

/* WinHelp hash function */
static int32_t hlp_hash_string(const char* str) {
    int32_t hash = 0;
    char c;
    while ((c = *str++)) {
        int32_t x = 0;
        if (c >= 'A' && c <= 'Z') x = c - 'A' + 17;
        if (c >= 'a' && c <= 'z') x = c - 'a' + 17;
        if (c >= '1' && c <= '9') x = c - '0';
        if (c == '0') x = 10;
        if (c == '.') x = 12;
        if (c == '_') x = 13;
        if (x) hash = hash * 43 + x;
    }
    return hash;
}

/* Returns dimensions and type of the last decoded image */
void hlp_get_last_image_size(uint16_t* out_w, uint16_t* out_h, uint8_t* out_type) {
    *out_w = g_last_img_w;
    *out_h = g_last_img_h;
    *out_type = g_last_img_type;
}

int hlp_get_bitmap_hotspots(HlpFile* hlp, uint32_t bm_index,
                            HlpHotspot* out, int max_hotspots) {
    char name[16];
    snprintf(name, sizeof(name), "|bm%u", bm_index);

    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, name, &buf, &end) != 0) return 0;

    uint8_t* ref = buf + 9;
    uint16_t numpict = get_u16(ref, 2);
    if (numpict == 0) return 0;

    const uint8_t* beg = ref + get_u32(ref, 4);
    uint8_t type = beg[0];
    const uint8_t* ptr = beg + 2;

    uint32_t hs_size = 0, hs_offset = 0;

    if (type == 5 || type == 6) {
        fetch_ulong(&ptr); fetch_ulong(&ptr); /* xpels, ypels */
        img_fetch_ushort(&ptr); img_fetch_ushort(&ptr); /* planes, bpp */
        fetch_ulong(&ptr); fetch_ulong(&ptr); /* width, height */
        fetch_ulong(&ptr); fetch_ulong(&ptr); /* clrused, clrimp */
        fetch_ulong(&ptr); /* csz */
        hs_size = fetch_ulong(&ptr);
        ptr += 4; /* data_off */
        hs_offset = get_u32(ptr, 0);
    } else if (type == 8) {
        img_fetch_ushort(&ptr); ptr += 4; /* mm, w, h */
        fetch_ulong(&ptr); fetch_ulong(&ptr); /* dsize, csize */
        hs_size = fetch_ulong(&ptr);
        ptr += 4; /* data_off */
        hs_offset = get_u32(ptr, 0);
    } else {
        return 0;
    }

    if (hs_size == 0 || hs_offset == 0) return 0;

    const uint8_t* hs = beg + hs_offset;
    uint16_t hs_num = get_u16(hs, 1);
    uint32_t hs_macro_off = get_u32(hs, 3);

    const char* str = (const char*)hs + 7 + 15 * hs_num + hs_macro_off;
    int count = 0;

    for (unsigned i = 0; i < hs_num && count < max_hotspots; i++) {
        uint8_t hs_type = hs[7 + 15 * i + 0];
        uint16_t x = get_u16(hs, 7 + 15 * i + 3);
        uint16_t y = get_u16(hs, 7 + 15 * i + 5);
        uint16_t w = get_u16(hs, 7 + 15 * i + 7);
        uint16_t h = get_u16(hs, 7 + 15 * i + 9);

        str += strlen(str) + 1; /* skip hotspot name */

        HlpHotspot* spot = &out[count++];
        spot->x = x;
        spot->y = y;
        spot->w = w;
        spot->h = h;
        spot->macro = 0;

        switch (hs_type) {
        case 0xC8:
            spot->kind = 2;
            spot->hash = 0;
            spot->macro = str;
            break;
        case 0xE6:
        case 0xE7:
            spot->kind = (hs_type & 1) ? 0 : 1;
            spot->hash = hlp_hash_to_offset(hlp, hlp_hash_string(str));
            break;
        case 0xEE:
        case 0xEF:
            spot->kind = (hs_type & 1) ? 0 : 1;
            spot->hash = hlp_hash_to_offset(hlp, hlp_hash_string(str));
            break;
        default:
            count--;
            break;
        }
        str += strlen(str) + 1;
    }

    return count;
}

/* Debug: log bitmap header info */
__attribute__((import_module("env"), import_name("js_log")))
void js_log(const char* ptr, uint32_t len);

EXPORT(hlp_debug_bitmap)
void hlp_debug_bitmap(uint32_t handle, uint32_t bm_index) {
    HlpFile* hlp = (HlpFile*)(uintptr_t)handle;
    char name[16];
    snprintf(name, sizeof(name), "|bm%u", bm_index);

    uint8_t *buf, *end;
    /* We need the HlpFile pointer, not the handle — use global */
    /* handle param is unused — we receive the hlp pointer from the API layer */
    (void)handle;
    /* For debug, call via hlp_api.c which passes g_hlp */
    /* Actually, let's just remove this and add the debug to hlp_decode_bitmap */
    return;
    if (0 && hlp_find_subfile(0, name, &buf, &end) != 0) {
        js_log("bitmap not found", 16);
        return;
    }

    uint8_t* ref = buf + 9;
    uint16_t magic = get_u16(ref, 0);
    uint16_t numpict = get_u16(ref, 2);
    char msg[256];
    int len = snprintf(msg, sizeof(msg), "bm%u: magic=0x%x numpict=%u", bm_index, magic, numpict);
    js_log(msg, len);

    if (numpict == 0) return;
    const uint8_t* beg = ref + get_u32(ref, 4);
    uint8_t type = beg[0];
    uint8_t pack = beg[1];
    const uint8_t* ptr = beg + 2;

    uint32_t xpels  = fetch_ulong(&ptr);
    uint32_t ypels  = fetch_ulong(&ptr);
    uint16_t planes = img_fetch_ushort(&ptr);
    uint16_t bpp    = img_fetch_ushort(&ptr);
    uint32_t width  = fetch_ulong(&ptr);
    uint32_t height = fetch_ulong(&ptr);
    uint32_t clrused = fetch_ulong(&ptr);
    uint32_t clrimp  = fetch_ulong(&ptr);
    uint32_t csz     = fetch_ulong(&ptr);

    len = snprintf(msg, sizeof(msg),
        "  type=%u pack=%u %ux%u bpp=%u planes=%u clr=%u csz=%u xpels=%u ypels=%u clrimp=%u",
        type, pack, width, height, bpp, planes, clrused, csz, xpels, ypels, clrimp);
    js_log(msg, len);
}
