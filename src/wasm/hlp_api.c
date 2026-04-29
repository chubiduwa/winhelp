#include "hlp.h"

static HlpFile g_hlp = {0};
static HlpInfo g_info = {0};

__attribute__((import_module("env"), import_name("js_log")))
void js_log(const char* ptr, uint32_t len);

EXPORT(hlp_version)
uint32_t hlp_version(void) {
    return 1;
}

EXPORT(hlp_open)
uint32_t hlp_open(const uint8_t* data, size_t len) {
    memset(&g_hlp, 0, sizeof(g_hlp));
    g_hlp.file_buffer = (uint8_t*)data;
    g_hlp.file_buffer_size = len;

    if (hlp_parse_header(&g_hlp) != 0)
        return 0;
    if (hlp_parse_system(&g_hlp) != 0)
        return 0;
    hlp_parse_fonts(&g_hlp);

    /* Load phrase tables */
    if (hlp_load_phrases(&g_hlp) != 0)
        hlp_load_phrases40(&g_hlp);

    /* Decompress topic blocks */
    if (hlp_decompress_topic(&g_hlp) != 0)
        return 0;

    /* Build page index and load navigation tables */
    hlp_build_page_index(&g_hlp);
    hlp_load_context(&g_hlp);
    hlp_load_keywords(&g_hlp);
    hlp_load_alinks(&g_hlp);
    if (g_hlp.version <= 16) hlp_load_tomap(&g_hlp);

    /* Fill the info struct for JS */
    memcpy(g_info.title, g_hlp.title, 128);
    memcpy(g_info.copyright, g_hlp.copyright, 128);
    g_info.contents_hash = g_hlp.contents_start;
    g_info.version = g_hlp.version;
    g_info.charset = g_hlp.charset;
    g_info.num_windows = 0;
    g_info.scale = g_hlp.scale;
    g_info.lcid = g_hlp.lcid;
    g_info.has_sr_color = g_hlp.has_sr_color;
    if (g_hlp.has_sr_color) {
        g_info.sr_color[0] = g_hlp.sr_color[0];
        g_info.sr_color[1] = g_hlp.sr_color[1];
        g_info.sr_color[2] = g_hlp.sr_color[2];
    }
    g_info.has_nsr_color = g_hlp.has_nsr_color;
    if (g_hlp.has_nsr_color) {
        g_info.nsr_color[0] = g_hlp.nsr_color[0];
        g_info.nsr_color[1] = g_hlp.nsr_color[1];
        g_info.nsr_color[2] = g_hlp.nsr_color[2];
    }

    return 1;
}

EXPORT(hlp_close)
void hlp_close(uint32_t handle) {
    free(g_hlp.fonts);
    free(g_hlp.phrases_offsets);
    free(g_hlp.phrases_buffer);
    free(g_hlp.topic_map);
    free(g_hlp.context);
    free(g_hlp.kwbtree);
    free(g_hlp.kwdata);
    free(g_hlp.awbtree);
    free(g_hlp.awdata);
    free(g_hlp.tomap);
    free(g_hlp.startup_macros);
    memset(&g_hlp, 0, sizeof(g_hlp));
    memset(&g_info, 0, sizeof(g_info));
}

EXPORT(hlp_get_error)
const char* hlp_get_error(void) {
    return hlp_get_error_str();
}

EXPORT(hlp_get_info)
const uint8_t* hlp_get_info(uint32_t handle) {
    return (const uint8_t*)&g_info;
}

EXPORT(hlp_get_num_fonts)
uint16_t hlp_get_num_fonts(uint32_t handle) {
    return g_hlp.num_fonts;
}

EXPORT(hlp_get_fonts)
const uint8_t* hlp_get_fonts(uint32_t handle) {
    return (const uint8_t*)g_hlp.fonts;
}

EXPORT(hlp_render_page)
int32_t hlp_render_page(uint32_t handle, uint32_t topic_offset,
                        const uint8_t** out_ptr, size_t* out_len) {
    if (topic_offset == 0)
        return hlp_render_first_page(&g_hlp, out_ptr, out_len);
    return hlp_render_page_by_topic_offset(&g_hlp, topic_offset, out_ptr, out_len);
}

EXPORT(hlp_get_page_count)
unsigned hlp_get_page_count_export(void) {
    return hlp_get_page_count();
}

EXPORT(hlp_get_page_topic_offset)
uint32_t hlp_get_page_topic_offset_export(unsigned index) {
    return hlp_get_page_topic_offset(index);
}

EXPORT(hlp_find_page)
int32_t hlp_find_page_export(uint32_t topic_offset) {
    return hlp_find_page_by_topic_offset(topic_offset);
}

EXPORT(hlp_get_page_title)
const char* hlp_get_page_title_export(unsigned index) {
    return hlp_get_page_title(&g_hlp, index);
}

EXPORT(hlp_get_last_image_size)
void hlp_get_last_image_size_export(uint16_t* out_w, uint16_t* out_h, uint8_t* out_type) {
    hlp_get_last_image_size(out_w, out_h, out_type);
}

EXPORT(hlp_decode_image_png)
int32_t hlp_decode_image_png(uint32_t handle, uint32_t bm_index,
                             const uint8_t** out_ptr, size_t* out_len) {
    return hlp_decode_bitmap(&g_hlp, bm_index, out_ptr, out_len);
}

EXPORT(hlp_decode_image_wmf)
int32_t hlp_decode_image_wmf_export(uint32_t handle, uint32_t bm_index,
                                    const uint8_t** out_ptr, size_t* out_len) {
    return hlp_decode_image_wmf(&g_hlp, bm_index, out_ptr, out_len);
}

EXPORT(hlp_peek_image_type)
int32_t hlp_peek_image_type_export(uint32_t handle, uint32_t bm_index) {
    return hlp_peek_image_type(&g_hlp, bm_index);
}

EXPORT(hlp_search_keyword)
int32_t hlp_search_keyword_export(uint32_t handle, int32_t use_alink,
                                  const char* keyword,
                                  uint32_t* out_offsets, int32_t out_max) {
    return hlp_search_keyword(&g_hlp, use_alink, keyword, out_offsets, out_max);
}

EXPORT(hlp_get_keyword_count)
int32_t hlp_get_keyword_count_export(uint32_t handle) {
    return hlp_get_keyword_count(&g_hlp);
}

EXPORT(hlp_enum_keywords)
int32_t hlp_enum_keywords_export(uint32_t handle, uint32_t* out_ptrs, int32_t max) {
    return hlp_enum_keywords(&g_hlp, out_ptrs, max);
}

EXPORT(hlp_get_startup_macros)
const char* hlp_get_startup_macros(size_t* out_len) {
    *out_len = g_hlp.startup_macros_len;
    return g_hlp.startup_macros;
}

EXPORT(hlp_debug_lookup)
int32_t hlp_debug_lookup(uint32_t hash) {
    return hlp_hash_to_offset(&g_hlp, (int32_t)hash);
}

/* Debug: enumerate internal subfiles */
static void log_subfile(const char* name, uint32_t offset, void* ctx) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "  %s @ 0x%x", name, offset);
    js_log(buf, (uint32_t)len);
}

EXPORT(hlp_list_files)
void hlp_list_files(void) {
    js_log("Internal files:", 15);
    hlp_enum_subfiles(&g_hlp, log_subfile, 0);
}

EXPORT(hlp_dump_topics)
void hlp_dump_topics(void) {
    char buf[256];
    int len;

    len = snprintf(buf, sizeof(buf), "Topic map: %u blocks, compressed=%d",
                   g_hlp.topic_maplen, g_hlp.compressed);
    js_log(buf, len);

    len = snprintf(buf, sizeof(buf), "Phrases: %s, count=%u",
                   g_hlp.has_phrases ? "v3.0" : (g_hlp.has_phrases40 ? "v4.0" : "none"),
                   g_hlp.num_phrases);
    js_log(buf, len);

    /* Walk the first few topic links */
    uint32_t ref = 0x0C; /* first topic link offset */
    for (int count = 0; count < 10; count++) {
        unsigned index, offset;
        if (g_hlp.version <= 16) {
            index  = (ref - 0x0C) / g_hlp.dsize;
            offset = (ref - 0x0C) % g_hlp.dsize;
        } else {
            index  = (ref - 0x0C) >> 14;
            offset = (ref - 0x0C) & 0x3FFF;
        }
        if (index >= g_hlp.topic_maplen) break;

        uint8_t* block = g_hlp.topic_map[index] + offset;
        if (block + 0x15 >= g_hlp.topic_end) break;

        uint32_t blocksize = get_u32(block, 0);
        uint32_t datalen   = get_u32(block, 0x10);
        uint8_t  rec_type  = block[0x14];
        uint32_t next_ref  = get_u32(block, 0x0C);

        len = snprintf(buf, sizeof(buf),
            "  [%d] ref=0x%x idx=%u+%u type=0x%02x bsize=%u dlen=%u next=0x%x",
            count, ref, index, offset, rec_type, blocksize, datalen, next_ref);
        js_log(buf, len);

        /* If topic header (type 2), try to read the title */
        if (rec_type == 0x02 && datalen > 0) {
            uint8_t* title_src = block + datalen;
            uint32_t title_size = get_u32(block, 4);
            if (title_size > 0 && title_size < 256) {
                char title[256];
                if (title_size > blocksize - datalen) {
                    /* Need phrase decompression */
                    if (g_hlp.has_phrases)
                        hlp_phrase_decompress2(&g_hlp, title_src, block + blocksize,
                                              (uint8_t*)title, (uint8_t*)title + title_size);
                    else if (g_hlp.has_phrases40)
                        hlp_phrase_decompress3(&g_hlp, title_src, block + blocksize,
                                              (uint8_t*)title, (uint8_t*)title + title_size);
                } else {
                    memcpy(title, title_src, title_size);
                }
                title[title_size] = '\0';
                len = snprintf(buf, sizeof(buf), "    title: %s", title);
                js_log(buf, len);
            }
        }

        if (next_ref == 0xFFFFFFFF) break;
        if (g_hlp.version <= 16) {
            if (next_ref == 0) break;
            ref += next_ref;
        } else {
            ref = next_ref;
        }
    }
}
