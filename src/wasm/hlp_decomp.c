#include "hlp.h"

/* LZ77 decompressed size */
static size_t lz77_size(const uint8_t* ptr, const uint8_t* end) {
    size_t newsize = 0;
    while (ptr < end) {
        int mask = *ptr++;
        for (int i = 0; i < 8 && ptr < end; i++, mask >>= 1) {
            if (mask & 1) {
                uint16_t code = get_u16(ptr, 0);
                newsize += 3 + (code >> 12);
                ptr += 2;
            } else {
                newsize++;
                ptr++;
            }
        }
    }
    return newsize;
}

/* LZ77 decompress into dst, returns pointer past end of written data */
static uint8_t* lz77_decompress(const uint8_t* ptr, const uint8_t* end, uint8_t* dst) {
    while (ptr < end) {
        int mask = *ptr++;
        for (int i = 0; i < 8 && ptr < end; i++, mask >>= 1) {
            if (mask & 1) {
                uint16_t code = get_u16(ptr, 0);
                int len = 3 + (code >> 12);
                int offset = code & 0xFFF;
                /* byte-by-byte copy (overlapping allowed) */
                for (; len > 0; len--, dst++)
                    *dst = *(dst - offset - 1);
                ptr += 2;
            } else {
                *dst++ = *ptr++;
            }
        }
    }
    return dst;
}

/* RLE decompress */
void hlp_rle_decompress(const uint8_t* src, const uint8_t* end,
                        uint8_t* dst, size_t dstsz) {
    uint8_t* sdst = dst + dstsz;
    while (src < end) {
        uint8_t ch = *src++;
        if (ch & 0x80) {
            ch &= 0x7F;
            if (dst + ch <= sdst)
                memcpy(dst, src, ch);
            src += ch;
        } else {
            if (dst + ch <= sdst)
                memset(dst, *src, ch);
            src++;
        }
        dst += ch;
    }
}

/* Load v3.0 phrase table from |Phrases */
int hlp_load_phrases(HlpFile* hlp) {
    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, "|Phrases", &buf, &end) != 0)
        return -1;

    uint8_t* ref = buf + 9;
    unsigned head_size = (hlp->version <= 16) ? 4 : 8;

    hlp->num_phrases = get_u16(ref, 0);
    unsigned num = hlp->num_phrases;

    hlp->phrases_offsets = malloc(sizeof(unsigned) * (num + 1));
    if (!hlp->phrases_offsets) return -1;

    /* Offset table starts after the count (and optional extra header bytes) */
    for (unsigned i = 0; i <= num; i++)
        hlp->phrases_offsets[i] = get_u16(ref, head_size + 2 * i) - 2 * num - 2;

    /* Phrase data follows the offset table */
    const uint8_t* phrase_data = ref + head_size + 2 * (num + 1);
    size_t data_size;

    if (hlp->version <= 16) {
        data_size = end - buf - 9 - head_size - 2 * (num + 1);
        hlp->phrases_buffer = malloc(data_size);
        if (!hlp->phrases_buffer) return -1;
        memcpy(hlp->phrases_buffer, phrase_data, data_size);
    } else {
        data_size = lz77_size(phrase_data, end - buf + buf);
        hlp->phrases_buffer = malloc(data_size);
        if (!hlp->phrases_buffer) return -1;
        lz77_decompress(phrase_data, end - buf + buf, (uint8_t*)hlp->phrases_buffer);
    }

    hlp->has_phrases = 1;
    return 0;
}

/* Load v4.0 phrase table from |PhrIndex + |PhrImage */
int hlp_load_phrases40(HlpFile* hlp) {
    uint8_t *buf_idx, *end_idx, *buf_phs, *end_phs;

    if (hlp_find_subfile(hlp, "|PhrIndex", &buf_idx, &end_idx) != 0 ||
        hlp_find_subfile(hlp, "|PhrImage", &buf_phs, &end_phs) != 0)
        return -1;

    uint8_t* idx = buf_idx + 9;
    uint16_t bc = get_u16(idx, 24) & 0x0F;
    unsigned num = hlp->num_phrases = get_u32(idx, 4);
    int32_t dec_size = get_i32(idx, 12);
    int32_t cpr_size = get_i32(idx, 16);

    hlp->phrases_offsets = malloc(sizeof(unsigned) * (num + 1));
    hlp->phrases_buffer = malloc(dec_size);
    if (!hlp->phrases_offsets || !hlp->phrases_buffer) return -1;

    /* Decode bit-packed phrase lengths */
    uint32_t* ptr = (uint32_t*)(idx + 28);
    uint32_t mask = 0;

    #define getbit() ((mask <<= 1) ? (*ptr & mask) != 0 : (*++ptr & (mask = 1)) != 0)

    hlp->phrases_offsets[0] = 0;
    ptr--;
    for (unsigned i = 0; i < num; i++) {
        unsigned short n;
        for (n = 1; getbit(); n += 1 << bc);
        if (bc > 0 && getbit()) n++;
        if (bc > 1 && getbit()) n += 2;
        if (bc > 2 && getbit()) n += 4;
        if (bc > 3 && getbit()) n += 8;
        if (bc > 4 && getbit()) n += 16;
        hlp->phrases_offsets[i + 1] = hlp->phrases_offsets[i] + n;
    }
    #undef getbit

    /* Decompress phrase image */
    uint8_t* phs_data = buf_phs + 9;
    if (dec_size == cpr_size)
        memcpy(hlp->phrases_buffer, phs_data, dec_size);
    else
        lz77_decompress(phs_data, end_phs, (uint8_t*)hlp->phrases_buffer);

    hlp->has_phrases40 = 1;
    return 0;
}

/* Phrase-substitute decompression v3.0 */
void hlp_phrase_decompress2(HlpFile* hlp, const uint8_t* src, const uint8_t* src_end,
                            uint8_t* dst, const uint8_t* dst_end) {
    while (src < src_end && dst < dst_end) {
        if (!*src || *src >= 0x10) {
            *dst++ = *src++;
        } else {
            uint16_t code = 0x100 * src[0] + src[1];
            unsigned idx = (code - 0x100) / 2;
            unsigned len = hlp->phrases_offsets[idx + 1] - hlp->phrases_offsets[idx];
            if (dst + len <= dst_end)
                memcpy(dst, hlp->phrases_buffer + hlp->phrases_offsets[idx], len);
            dst += len;
            if (code & 1) *dst++ = ' ';
            src += 2;
        }
    }
}

/* Phrase-substitute decompression v4.0 */
void hlp_phrase_decompress3(HlpFile* hlp, const uint8_t* src, const uint8_t* src_end,
                            uint8_t* dst, const uint8_t* dst_end) {
    unsigned idx, len;
    for (; src < src_end; src++) {
        if ((*src & 1) == 0) {
            idx = *src / 2;
            len = (idx <= hlp->num_phrases)
                ? hlp->phrases_offsets[idx + 1] - hlp->phrases_offsets[idx] : 0;
            if (dst + len <= dst_end)
                memcpy(dst, hlp->phrases_buffer + hlp->phrases_offsets[idx], len);
        } else if ((*src & 0x03) == 0x01) {
            idx = (*src + 1) * 64;
            src++;
            idx += *src;
            len = (idx <= hlp->num_phrases)
                ? hlp->phrases_offsets[idx + 1] - hlp->phrases_offsets[idx] : 0;
            if (dst + len <= dst_end)
                memcpy(dst, hlp->phrases_buffer + hlp->phrases_offsets[idx], len);
        } else if ((*src & 0x07) == 0x03) {
            len = (*src / 8) + 1;
            if (dst + len <= dst_end)
                memcpy(dst, src + 1, len);
            src += len;
        } else {
            len = (*src / 16) + 1;
            if (dst + len <= dst_end)
                memset(dst, ((*src & 0x0F) == 0x07) ? ' ' : 0, len);
        }
        dst += len;
    }
}

/* Decompress |TOPIC into a topic map (array of decompressed block pointers) */
int hlp_decompress_topic(HlpFile* hlp) {
    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, "|TOPIC", &buf, &end) != 0) {
        hlp_set_error("missing |TOPIC");
        return -1;
    }

    buf += 9; /* skip FILEHEADER */
    size_t topic_size = end - buf;

    if (hlp->compressed) {
        hlp->topic_maplen = (topic_size - 1) / hlp->tbsize + 1;

        /* First pass: compute total decompressed size */
        size_t newsize = 0;
        for (unsigned i = 0; i < hlp->topic_maplen; i++) {
            uint8_t* ptr = buf + i * hlp->tbsize;
            if (ptr + 0x44 > end) ptr = end - 0x44;
            uint8_t* block_end = buf + (i + 1) * hlp->tbsize;
            if (block_end > end) block_end = end;
            newsize += lz77_size(ptr + 0x0C, block_end);
        }

        /* Allocate map + decompressed data together */
        hlp->topic_map = malloc(hlp->topic_maplen * sizeof(uint8_t*) + newsize);
        if (!hlp->topic_map) return -1;

        uint8_t* newptr = (uint8_t*)(hlp->topic_map + hlp->topic_maplen);
        hlp->topic_end = newptr + newsize;

        /* Second pass: decompress */
        for (unsigned i = 0; i < hlp->topic_maplen; i++) {
            uint8_t* ptr = buf + i * hlp->tbsize;
            if (ptr + 0x44 > end) ptr = end - 0x44;
            uint8_t* block_end = buf + (i + 1) * hlp->tbsize;
            if (block_end > end) block_end = end;

            hlp->topic_map[i] = newptr;
            newptr = lz77_decompress(ptr + 0x0C, block_end, newptr);
        }
    } else {
        hlp->topic_maplen = (topic_size - 1) / hlp->tbsize + 1;
        hlp->topic_map = malloc(hlp->topic_maplen * (sizeof(uint8_t*) + hlp->dsize));
        if (!hlp->topic_map) return -1;

        uint8_t* newptr = (uint8_t*)(hlp->topic_map + hlp->topic_maplen);
        hlp->topic_end = newptr + topic_size;

        for (unsigned i = 0; i < hlp->topic_maplen; i++) {
            hlp->topic_map[i] = newptr + i * hlp->dsize;
            memcpy(hlp->topic_map[i], buf + i * hlp->tbsize + 0x0C, hlp->dsize);
        }
    }

    return 0;
}
