#ifndef HLP_H
#define HLP_H

#include <stdint.h>
#include <stddef.h>

#define EXPORT(name) __attribute__((export_name(#name)))

/* Freestanding libc */
void*  malloc(size_t size);
void   free(void* ptr);
void*  realloc(void* ptr, size_t size);
void*  calloc(size_t count, size_t size);
void*  memcpy(void* dst, const void* src, size_t n);
void*  memset(void* dst, int c, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
void*  memchr(const void* s, int c, size_t n);

#define STB_SPRINTF_NOFLOAT
#include "stb_sprintf.h"
#define sprintf  stbsp_sprintf
#define snprintf stbsp_snprintf

/* Little-endian reads from buffer */

static inline uint16_t get_u16(const uint8_t* buf, size_t off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off+1] << 8);
}

static inline int16_t get_i16(const uint8_t* buf, size_t off) {
    return (int16_t)get_u16(buf, off);
}

static inline uint32_t get_u32(const uint8_t* buf, size_t off) {
    return (uint32_t)get_u16(buf, off) | ((uint32_t)get_u16(buf, off+2) << 16);
}

static inline int32_t get_i32(const uint8_t* buf, size_t off) {
    return (int32_t)get_u32(buf, off);
}

/* HLP file handle */

#define HLP_MAGIC   0x00035F3F
#define BTREE_MAGIC 0x293B

#pragma pack(push, 1)
typedef struct {
    char     face_name[32];
    uint16_t half_points;
    uint16_t weight;
    uint8_t  italic;
    uint8_t  underline;
    uint8_t  strikeout;
    uint8_t  small_caps;
    uint8_t  family;
    uint8_t  fg_rgb[3];
    uint8_t  bg_rgb[3];
} HlpFont; /* 47 bytes */

typedef struct {
    char     title[128];
    char     copyright[128];
    uint32_t contents_hash;
    uint16_t version; /* minor version (major is always 1) */
    uint16_t charset;
    uint16_t num_windows;
    uint16_t scale;   /* 10 for old fonts (half-points), 1 for new fonts (twips) */
    uint16_t lcid;    /* language ID from |SYSTEM record 9 (0 if absent) */
    uint8_t  sr_color[3]; /* scrollable region background RGB */
    uint8_t  has_sr_color;
    uint8_t  nsr_color[3]; /* non-scrollable region background RGB */
    uint8_t  has_nsr_color;
} HlpInfo;

#pragma pack(pop)

typedef struct {
    uint8_t* file_buffer;
    size_t   file_buffer_size;
    uint8_t* directory;

    /* From |SYSTEM */
    char     title[128];
    char     copyright[128];
    uint16_t version;
    uint16_t flags;
    uint16_t charset;
    uint16_t lcid;
    uint8_t  sr_color[3];
    uint8_t  has_sr_color;
    uint8_t  nsr_color[3];
    uint8_t  has_nsr_color;
    uint16_t tbsize;    /* topic block size */
    uint16_t dsize;     /* decompressed size */
    uint8_t  compressed;
    uint32_t contents_start;

    /* From |FONT */
    uint16_t  num_fonts;
    HlpFont*  fonts;
    uint16_t  scale;     /* 10 for old fonts, 1 for new fonts */
    uint16_t  rounderr;  /* 5 for old fonts, 0 for new fonts */

    /* Phrase tables */
    uint8_t   has_phrases;
    uint8_t   has_phrases40;
    unsigned  num_phrases;
    unsigned* phrases_offsets;
    char*     phrases_buffer;

    /* Index tables */
    uint8_t*  context;    /* raw copy of |CONTEXT */
    uint8_t*  kwbtree;    /* raw copy of |KWBTREE */
    uint8_t*  kwdata;     /* raw copy of |KWDATA */
    uint8_t*  awbtree;    /* raw copy of |AWBTREE */
    uint8_t*  awdata;     /* raw copy of |AWDATA */
    uint32_t* tomap;      /* |TOMAP for v3.0 */
    unsigned  tomap_len;

    /* Startup macros from |SYSTEM type 4 */
    char*    startup_macros;   /* null-separated list, double-null terminated */
    size_t   startup_macros_len;

    /* Decompressed |TOPIC */
    uint8_t** topic_map;
    uint8_t*  topic_end;
    unsigned  topic_maplen;
} HlpFile;

/* hlp_parse.c */
int  hlp_parse_header(HlpFile* hlp);
int  hlp_find_subfile(HlpFile* hlp, const char* name,
                      uint8_t** buf, uint8_t** end);
void hlp_enum_subfiles(HlpFile* hlp,
                       void (*cb)(const char* name, uint32_t offset, void* ctx),
                       void* ctx);

/* hlp_system.c */
int hlp_parse_system(HlpFile* hlp);

/* hlp_font.c */
int hlp_parse_fonts(HlpFile* hlp);

/* hlp_decomp.c */
int  hlp_load_phrases(HlpFile* hlp);
int  hlp_load_phrases40(HlpFile* hlp);
int  hlp_decompress_topic(HlpFile* hlp);
void hlp_phrase_decompress2(HlpFile* hlp, const uint8_t* src, const uint8_t* src_end,
                            uint8_t* dst, const uint8_t* dst_end);
void hlp_phrase_decompress3(HlpFile* hlp, const uint8_t* src, const uint8_t* src_end,
                            uint8_t* dst, const uint8_t* dst_end);
void hlp_rle_decompress(const uint8_t* src, const uint8_t* end,
                        uint8_t* dst, size_t dstsz);

/* hlp_image.c */
int hlp_decode_bitmap(HlpFile* hlp, uint32_t bm_index,
                      const uint8_t** out_ptr, size_t* out_len);
void hlp_get_last_image_size(uint16_t* out_w, uint16_t* out_h, uint8_t* out_type);

/* Convert a paletted/RGB/RGBA DIB pixel buffer to RGBA. Caller frees
   the returned buffer with free(). Supports 1/4/8/24/32 bpp; row-padded
   to 32-bit boundaries (DIB convention) and bottom-up (also DIB
   convention — output is top-down). */
uint8_t* dib_to_rgba(uint32_t width, uint32_t height, uint16_t bpp,
                     const uint8_t* palette, uint32_t ncolors,
                     const uint8_t* pixels, uint32_t* out_size);

/* Peek at a |bm{index} subfile and return its picture type byte
   (5/6 = bitmap, 8 = metafile). Returns -1 if not found. */
int hlp_peek_image_type(HlpFile* hlp, uint32_t bm_index);

/* Decompress and return the raw WMF bytes for a |bm{index} subfile of
   image type=8. *out_alloc is the malloc'd buffer that owns the bytes
   (caller frees with free()); *out_data points into it (may equal
   *out_alloc, or may be NULL when the WMF data was uncompressed and
   *out_alloc is also NULL — in that case do not free). *out_len is the
   decompressed size. Also updates the last-image-size globals. */
int hlp_get_wmf_bytes(HlpFile* hlp, uint32_t bm_index,
                      uint8_t** out_alloc, const uint8_t** out_data,
                      size_t* out_len);

/* Render a |bm{index} type=8 metafile to a WMF opcode stream consumed
   by JS. On success, *out_ops is malloc'd (caller frees). */
int hlp_decode_image_wmf(HlpFile* hlp, uint32_t bm_index,
                         const uint8_t** out_ops, size_t* out_len);

typedef struct {
    uint8_t kind;
    uint32_t hash;
    uint16_t x, y, w, h;
    const char* macro;
} HlpHotspot;

int hlp_get_bitmap_hotspots(HlpFile* hlp, uint32_t bm_index,
                            HlpHotspot* out, int max_hotspots);

/* hlp_index.c */
int     hlp_load_context(HlpFile* hlp);
int32_t hlp_hash_to_offset(HlpFile* hlp, int32_t hash);
int     hlp_load_tomap(HlpFile* hlp);
int     hlp_load_keywords(HlpFile* hlp);
int     hlp_load_alinks(HlpFile* hlp);
int     hlp_search_keyword(HlpFile* hlp, int use_alink, const char* keyword,
                           uint32_t* out_offsets, int out_max);
int32_t hlp_get_keyword_count(HlpFile* hlp);
int     hlp_enum_keywords(HlpFile* hlp, uint32_t* out_ptrs, int max);

/* hlp_topic.c */
int hlp_build_page_index(HlpFile* hlp);
int hlp_render_page_by_topic_offset(HlpFile* hlp, uint32_t topic_offset,
                                    const uint8_t** out_ptr, size_t* out_len);
int hlp_render_page_by_hash(HlpFile* hlp, int32_t hash,
                            const uint8_t** out_ptr, size_t* out_len);
int hlp_render_first_page(HlpFile* hlp,
                          const uint8_t** out_ptr, size_t* out_len);
unsigned hlp_get_page_count(void);
uint32_t hlp_get_page_topic_offset(unsigned index);
const char* hlp_get_page_title(HlpFile* hlp, unsigned index);
int hlp_find_page_by_topic_offset(uint32_t topic_offset);

void hlp_reset_topic_state(void);

/* Error handling */
void        hlp_set_error(const char* msg);
const char* hlp_get_error_str(void);

#endif
