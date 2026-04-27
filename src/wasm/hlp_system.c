#include "hlp.h"

#define SYSTEM_MAGIC 0x036C

int hlp_parse_system(HlpFile* hlp) {
    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, "|SYSTEM", &buf, &end) != 0) {
        hlp_set_error("missing |SYSTEM");
        return -1;
    }

    uint8_t* ref = buf + 9; /* skip FILEHEADER */
    if (ref + 12 > end) { hlp_set_error("|SYSTEM too small"); return -1; }

    uint16_t magic = get_u16(ref, 0);
    uint16_t minor = get_u16(ref, 2);
    uint16_t major = get_u16(ref, 4);
    uint16_t flags = get_u16(ref, 10);

    if (magic != SYSTEM_MAGIC || major != 1) {
        hlp_set_error("bad |SYSTEM magic");
        return -1;
    }

    hlp->version = minor;
    hlp->flags = flags;
    hlp->charset = 0; /* DEFAULT_CHARSET */
    hlp->lcid = 0;
    hlp->contents_start = 0;
    hlp->title[0] = '\0';
    hlp->copyright[0] = '\0';

    /* Compression and topic block size */
    if (minor <= 16) {
        hlp->tbsize = 0x800;
        hlp->compressed = 0;
    } else if (flags == 0) {
        hlp->tbsize = 0x1000;
        hlp->compressed = 0;
    } else if (flags == 4) {
        hlp->tbsize = 0x1000;
        hlp->compressed = 1;
    } else {
        hlp->tbsize = 0x800;
        hlp->compressed = 1;
    }

    hlp->dsize = hlp->compressed ? 0x4000 : (hlp->tbsize - 0x0C);

    /* v3.0: title is inline after the header */
    if (minor <= 16) {
        const char* str = (const char*)ref + 12;
        strncpy(hlp->title, str, sizeof(hlp->title) - 1);
        return 0;
    }

    /* v3.1+: system records */
    uint8_t* ptr = ref + 12;
    while (ptr + 4 <= end) {
        uint16_t rec_type = get_u16(ptr, 0);
        uint16_t rec_size = get_u16(ptr, 2);
        const char* str = (const char*)ptr + 4;

        if (ptr + 4 + rec_size > end) break;

        switch (rec_type) {
        case 1: /* Title */
            strncpy(hlp->title, str, sizeof(hlp->title) - 1);
            break;
        case 2: /* Copyright */
            strncpy(hlp->copyright, str, sizeof(hlp->copyright) - 1);
            break;
        case 3: /* Contents topic offset */
            if (rec_size >= 4)
                hlp->contents_start = get_u32(ptr, 4);
            break;
        case 4: { /* Startup macro */
            size_t mlen = strlen(str);
            if (mlen > 0) {
                size_t old = hlp->startup_macros_len;
                size_t need = old + mlen + 1;
                char* buf2 = realloc(hlp->startup_macros, need + 1);
                if (buf2) {
                    memcpy(buf2 + old, str, mlen + 1);
                    buf2[need] = '\0';
                    hlp->startup_macros = buf2;
                    hlp->startup_macros_len = need;
                }
            }
            break;
        }
        case 6: /* Window definition (90 bytes) */
            if (rec_size >= 90) {
                uint16_t wflags = get_u16(ptr, 4);
                if (wflags & 0x0100) {
                    hlp->sr_color[0] = ptr[4 + 82];
                    hlp->sr_color[1] = ptr[4 + 83];
                    hlp->sr_color[2] = ptr[4 + 84];
                    hlp->has_sr_color = 1;
                }
                if (wflags & 0x0200) {
                    hlp->nsr_color[0] = ptr[4 + 86];
                    hlp->nsr_color[1] = ptr[4 + 87];
                    hlp->nsr_color[2] = ptr[4 + 88];
                    hlp->has_nsr_color = 1;
                }
            }
            break;
        case 9: /* LCID (language ID at offset 8 of record data) */
            if (rec_size >= 10)
                hlp->lcid = get_u16(ptr, 4 + 8);
            break;
        case 11: /* Per-font charset array: pick first non-zero, non-symbol(2) entry */
            for (uint16_t i = 0; i < rec_size; i++) {
                uint8_t c = ptr[4 + i];
                if (c != 0 && c != 2) { hlp->charset = c; break; }
            }
            break;
        }
        ptr += 4 + rec_size;
    }

    return 0;
}
