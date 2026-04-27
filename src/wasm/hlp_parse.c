#include "hlp.h"

static char g_error[256] = {0};

void hlp_set_error(const char* msg) {
    strncpy(g_error, msg, sizeof(g_error) - 1);
    g_error[sizeof(g_error) - 1] = '\0';
}

const char* hlp_get_error_str(void) {
    return g_error[0] ? g_error : (const char*)0;
}

/*
 * Parse and validate the HLP file header. Sets hlp->directory to
 * the start of the internal directory B+ tree (after its FILEHEADER).
 */
int hlp_parse_header(HlpFile* hlp) {
    if (hlp->file_buffer_size < 16) {
        hlp_set_error("file too small");
        return -1;
    }

    uint32_t magic = get_u32(hlp->file_buffer, 0);
    if (magic != HLP_MAGIC) {
        hlp_set_error("bad magic");
        return -1;
    }

    uint32_t dir_offset = get_u32(hlp->file_buffer, 4);
    uint32_t file_size  = get_u32(hlp->file_buffer, 12);

    if (dir_offset + 9 > hlp->file_buffer_size) {
        hlp_set_error("directory offset out of bounds");
        return -1;
    }

    /* Skip the FILEHEADER (9 bytes: 4 reserved + 4 used + 1 flags)
       to get to the B+ tree data */
    hlp->directory = hlp->file_buffer + dir_offset + 9;

    uint16_t btree_magic = get_u16(hlp->directory, 0);
    if (btree_magic != BTREE_MAGIC) {
        hlp_set_error("bad B+ tree magic in directory");
        return -1;
    }

    return 0;
}

/*
 * B+ tree search. The directory B+ tree maps filename strings to
 * file offsets. Layout (relative to hlp->directory):
 *   +0   u16 magic (0x293B)
 *   +2   u16 flags
 *   +4   u16 page_size
 *   +6   char[16] structure descriptor
 *   +22  i16 must_be_zero
 *   +24  i16 page_splits
 *   +26  i16 root_page
 *   +28  i16 must_be_neg_one
 *   +30  i16 total_pages
 *   +32  i16 n_levels
 *   +34  i32 total_entries
 *   +38  pages[]
 */
static uint8_t* btree_search(uint8_t* tree, const char* key) {
    uint16_t page_size = get_u16(tree, 4);
    uint16_t cur_page  = get_u16(tree, 26);
    uint16_t level     = get_u16(tree, 32);
    uint8_t* pages     = tree + 38;

    /* Walk index pages */
    while (--level > 0) {
        uint8_t* ptr = pages + cur_page * page_size;
        int16_t entries = get_i16(ptr, 2);
        ptr += 6;
        for (int i = 0; i < entries; i++) {
            int cmp = strcmp((const char*)ptr, key);
            /* Advance past string + 2-byte page number */
            uint8_t* next = ptr + strlen((const char*)ptr) + 1 + 2;
            if (cmp > 0) break;
            ptr = next;
        }
        /* Page number is the 2 bytes before current ptr */
        cur_page = get_u16(ptr - 2, 0);
    }

    /* Search leaf page */
    uint8_t* ptr = pages + cur_page * page_size;
    int16_t entries = get_i16(ptr, 2);
    ptr += 8; /* leaf header is 8 bytes (unknown, entries, prev_page, next_page) */
    for (int i = 0; i < entries; i++) {
        int cmp = strcmp((const char*)ptr, key);
        /* In leaf nodes: string + 4-byte file offset */
        uint8_t* next = ptr + strlen((const char*)ptr) + 1 + 4;
        if (cmp == 0) return ptr;
        if (cmp > 0) return (uint8_t*)0;
        ptr = next;
    }
    return (uint8_t*)0;
}

int hlp_find_subfile(HlpFile* hlp, const char* name,
                     uint8_t** buf, uint8_t** end) {
    uint8_t* ptr = btree_search(hlp->directory, name);

    /* Some subfiles are stored without the '|' prefix */
    if (!ptr && name[0] == '|')
        ptr = btree_search(hlp->directory, name + 1);

    if (!ptr) return -1;

    const char* found_name = (const char*)ptr;
    size_t name_len = strlen(found_name);
    uint32_t offset = get_u32(ptr, name_len + 1);

    if (offset + 9 > hlp->file_buffer_size) return -1;

    *buf = hlp->file_buffer + offset;
    uint32_t reserved = get_u32(*buf, 0);
    if (offset + reserved > hlp->file_buffer_size) return -1;
    *end = *buf + reserved;

    return 0;
}

/*
 * Enumerate all entries in the directory B+ tree.
 * Walks leaf pages via their next_page links.
 */
void hlp_enum_subfiles(HlpFile* hlp,
                       void (*cb)(const char* name, uint32_t offset, void* ctx),
                       void* ctx) {
    uint8_t* tree = hlp->directory;
    uint16_t page_size = get_u16(tree, 4);
    uint16_t cur_page  = get_u16(tree, 26);
    uint16_t level     = get_u16(tree, 32);
    uint8_t* pages     = tree + 38;

    /* Walk down to first leaf */
    while (--level > 0) {
        uint8_t* ptr = pages + cur_page * page_size;
        cur_page = get_u16(ptr, 4);
    }

    /* Walk leaf chain */
    while (cur_page != 0xFFFF) {
        uint8_t* page = pages + cur_page * page_size;
        int16_t entries = get_i16(page, 2);
        uint8_t* ptr = page + 8;
        for (int i = 0; i < entries; i++) {
            const char* name = (const char*)ptr;
            size_t name_len = strlen(name);
            uint32_t offset = get_u32(ptr, name_len + 1);
            cb(name, offset, ctx);
            ptr += name_len + 1 + 4;
        }
        cur_page = get_u16(page, 6);
    }
}
