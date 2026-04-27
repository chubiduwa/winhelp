#include "hlp.h"

/*
 * |CONTEXT B+ tree: maps 4-byte hash → 4-byte topic offset
 * Index page entry: key(4) + page_num(2) = 6 bytes
 * Leaf page entry:  key(4) + offset(4) = 8 bytes
 *
 * For index pages, the child page to follow is the 2 bytes BEFORE
 * the entry whose key compared greater (or after the last entry if
 * none did). The initial child page number, used when the target key
 * is smaller than every entry, is at offset 4 in the page (after the
 * 6-byte header: unknown(2) + entries(2) + prevpage(2)).
 */
static uint8_t* context_search(uint8_t* tree, int32_t hash) {
    uint16_t page_size = get_u16(tree, 4);
    uint16_t cur_page  = get_u16(tree, 26);
    uint16_t level     = get_u16(tree, 32);
    uint8_t* pages     = tree + 38;

    /* Walk index pages */
    while (--level > 0) {
        uint8_t* page = pages + cur_page * page_size;
        int16_t entries = get_i16(page, 2);
        /* First child page is at page+4 (PreviousPage field) */
        uint8_t* ptr = page + 6;
        int i;
        for (i = 0; i < entries; i++) {
            int32_t test = get_i32(ptr, 0);
            if (test > hash) break;
            ptr += 4 + 2; /* key + page_num */
        }
        /* The page to descend into: if we broke early, it's the page_num
           before this entry. If we went through all, it's after the last. */
        if (i == 0)
            cur_page = get_u16(page, 4); /* PreviousPage */
        else
            cur_page = get_u16(ptr - 2, 0);
    }

    /* Search leaf page */
    uint8_t* page = pages + cur_page * page_size;
    int16_t entries = get_i16(page, 2);
    uint8_t* ptr = page + 8; /* leaf header: unknown(2) + entries(2) + prev(2) + next(2) */
    for (int i = 0; i < entries; i++) {
        int32_t test = get_i32(ptr, 0);
        if (test == hash) return ptr;
        if (test > hash) return 0;
        ptr += 4 + 4; /* key + topic_offset */
    }
    return 0;
}

int hlp_load_context(HlpFile* hlp) {
    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, "|CONTEXT", &buf, &end) != 0)
        return -1;

    size_t len = end - buf;
    hlp->context = malloc(len);
    if (!hlp->context) return -1;
    memcpy(hlp->context, buf, len);
    return 0;
}

/* Resolve a hash to a topic offset via |CONTEXT */
int32_t hlp_hash_to_offset(HlpFile* hlp, int32_t hash) {
    if (!hlp->context) return -1;
    /* B+ tree starts after the FILEHEADER (9 bytes) */
    uint8_t* ptr = context_search(hlp->context + 9, hash);
    if (!ptr) return -1;
    return get_i32(ptr, 4);
}

/* Load |TOMAP for v3.0 files */
int hlp_load_tomap(HlpFile* hlp) {
    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, "|TOMAP", &buf, &end) != 0)
        return -1;

    size_t len = end - buf - 9;
    hlp->tomap = malloc(len);
    if (!hlp->tomap) return -1;
    memcpy(hlp->tomap, buf + 9, len);
    hlp->tomap_len = len / 4;
    return 0;
}

/* Load |AWBTREE + |AWDATA for associative keywords */
int hlp_load_alinks(HlpFile* hlp) {
    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, "|AWBTREE", &buf, &end) != 0)
        return -1;
    size_t len = end - buf;
    hlp->awbtree = malloc(len);
    if (!hlp->awbtree) return -1;
    memcpy(hlp->awbtree, buf, len);

    if (hlp_find_subfile(hlp, "|AWDATA", &buf, &end) != 0) {
        free(hlp->awbtree); hlp->awbtree = 0;
        return -1;
    }
    len = end - buf;
    hlp->awdata = malloc(len);
    if (!hlp->awdata) return -1;
    memcpy(hlp->awdata, buf, len);
    return 0;
}

/*
 * Search a keyword B+ tree for a keyword string.
 * Leaf entries: STRINGZ keyword + u16 count + u32 KWDataOffset
 * Returns pointer to (count, KWDataOffset) after the matched string, or NULL.
 */
static uint8_t* kw_btree_search(uint8_t* tree, const char* keyword) {
    uint16_t page_size = get_u16(tree, 4);
    uint16_t cur_page  = get_u16(tree, 26);
    uint16_t level     = get_u16(tree, 32);
    uint8_t* pages     = tree + 38;

    /* Walk index pages */
    while (--level > 0) {
        uint8_t* page = pages + cur_page * page_size;
        int16_t entries = get_i16(page, 2);
        uint8_t* ptr = page + 6;
        int i;
        for (i = 0; i < entries; i++) {
            int cmp = strcmp((const char*)ptr, keyword);
            if (cmp > 0) break;
            size_t slen = strlen((const char*)ptr);
            ptr += slen + 1 + 2; /* string + NUL + page_num */
        }
        if (i == 0)
            cur_page = get_u16(page, 4); /* PreviousPage */
        else
            cur_page = get_u16(ptr - 2, 0);
    }

    /* Search leaf page */
    uint8_t* page = pages + cur_page * page_size;
    int16_t entries = get_i16(page, 2);
    uint8_t* ptr = page + 8; /* unknown(2) + entries(2) + prev(2) + next(2) */
    for (int i = 0; i < entries; i++) {
        int cmp = strcmp((const char*)ptr, keyword);
        size_t slen = strlen((const char*)ptr);
        if (cmp == 0) return ptr + slen + 1;
        if (cmp > 0) return 0;
        ptr += slen + 1 + 2 + 4; /* string + NUL + count(2) + KWDataOffset(4) */
    }
    return 0;
}

/*
 * Search keyword tree and return topic offsets from |KWDATA.
 * Returns count, fills out_offsets (max out_max entries).
 */
int hlp_search_keyword(HlpFile* hlp, int use_alink, const char* keyword,
                       uint32_t* out_offsets, int out_max) {
    uint8_t* tree = use_alink ? hlp->awbtree : hlp->kwbtree;
    uint8_t* data = use_alink ? hlp->awdata  : hlp->kwdata;
    if (!tree || !data) return 0;

    uint8_t* result = kw_btree_search(tree + 9, keyword);
    if (!result) return 0;

    uint16_t count = get_u16(result, 0);
    uint32_t kwdata_offset = get_u32(result, 2);
    if (count > (uint16_t)out_max) count = (uint16_t)out_max;

    /* Read topic offsets from KWDATA (skip 9-byte file header) */
    uint8_t* base = data + 9;
    for (uint16_t i = 0; i < count; i++) {
        out_offsets[i] = get_u32(base, kwdata_offset + i * 4);
    }
    return count;
}

/* Load keywords |KWBTREE + |KWDATA */
int hlp_load_keywords(HlpFile* hlp) {
    uint8_t *buf, *end;

    if (hlp_find_subfile(hlp, "|KWBTREE", &buf, &end) != 0)
        return -1;
    size_t len = end - buf;
    hlp->kwbtree = malloc(len);
    if (!hlp->kwbtree) return -1;
    memcpy(hlp->kwbtree, buf, len);

    if (hlp_find_subfile(hlp, "|KWDATA", &buf, &end) != 0) {
        free(hlp->kwbtree);
        hlp->kwbtree = 0;
        return -1;
    }
    len = end - buf;
    hlp->kwdata = malloc(len);
    if (!hlp->kwdata) return -1;
    memcpy(hlp->kwdata, buf, len);

    return 0;
}

/* Return total keyword count from |KWBTREE header */
int32_t hlp_get_keyword_count(HlpFile* hlp) {
    if (!hlp->kwbtree) return 0;
    return get_i32(hlp->kwbtree + 9, 34); /* total_entries */
}

/*
 * Enumerate all keywords by walking leaf pages.
 * Fills out_ptrs with pointers to NUL-terminated keyword strings inside kwbtree.
 * Returns actual count written.
 */
int hlp_enum_keywords(HlpFile* hlp, uint32_t* out_ptrs, int max) {
    if (!hlp->kwbtree) return 0;
    uint8_t* tree = hlp->kwbtree + 9;
    uint16_t page_size = get_u16(tree, 4);
    uint16_t cur_page  = get_u16(tree, 26);
    uint16_t level     = get_u16(tree, 32);
    uint8_t* pages     = tree + 38;
    int count = 0;

    /* Find leftmost leaf by following PreviousPage at each level */
    while (--level > 0) {
        uint8_t* page = pages + cur_page * page_size;
        cur_page = get_u16(page, 4); /* PreviousPage = leftmost child */
    }

    /* Walk leaf pages via next_page links */
    while (cur_page != 0xFFFF) {
        uint8_t* page = pages + cur_page * page_size;
        int16_t entries = get_i16(page, 2);
        uint16_t next = get_u16(page, 6);
        uint8_t* ptr = page + 8;

        for (int i = 0; i < entries && count < max; i++) {
            out_ptrs[count++] = (uint32_t)(uintptr_t)ptr;
            size_t slen = strlen((const char*)ptr);
            ptr += slen + 1 + 2 + 4; /* string + NUL + count(2) + KWDataOffset(4) */
        }

        cur_page = next;
    }
    return count;
}
