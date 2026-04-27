#include "hlp.h"

/*
 * OLDFONT descriptor: 11 bytes (face_offset < 12)
 *   +0  u8  attributes (bit 0=bold, 1=italic, 2=underline, 3=strikeout, 4=dblunderline, 5=smallcaps)
 *   +1  u8  half_points
 *   +2  u8  font_family
 *   +3  u16 font_name_index
 *   +5  u8[3] fg_rgb
 *   +8  u8[3] bg_rgb
 *
 * NEWFONT descriptor: 34 bytes (face_offset >= 12)
 *   +0  u8  unknown
 *   +1  i16 font_name_index
 *   +3  u8[3] fg_rgb
 *   +6  u8[3] bg_rgb
 *   +9  5 bytes unknown
 *   +14 i32 height (negative = half-points)
 *   +18 12 bytes mostly zero
 *   +30 i16 weight
 *   +32 u8 unknown, u8 unknown
 *   +34 u8 italic, u8 underline, u8 strikeout, u8 dbl_underline, u8 small_caps
 *   +39 u8 unknown, u8 unknown, u8 pitch_and_family
 */

static void parse_old_font(HlpFont* out, const uint8_t* dscr, const uint8_t* faces,
                           unsigned face_len, unsigned face_num) {
    uint8_t flag = dscr[0];
    out->half_points = dscr[1];
    uint8_t family = dscr[2];
    uint16_t idx = get_u16(dscr, 3);

    out->weight = (flag & 0x01) ? 700 : 400;
    out->italic = (flag & 0x02) ? 1 : 0;
    out->underline = (flag & 0x04) ? 1 : 0;
    out->strikeout = (flag & 0x08) ? 1 : 0;
    out->small_caps = (flag & 0x20) ? 1 : 0;
    out->family = family;

    out->fg_rgb[0] = dscr[5];
    out->fg_rgb[1] = dscr[6];
    out->fg_rgb[2] = dscr[7];
    out->bg_rgb[0] = dscr[8];
    out->bg_rgb[1] = dscr[9];
    out->bg_rgb[2] = dscr[10];

    if (idx < face_num) {
        size_t copy_len = face_len < 31 ? face_len : 31;
        memcpy(out->face_name, faces + idx * face_len, copy_len);
        out->face_name[copy_len] = '\0';
    } else {
        strcpy(out->face_name, "Helv");
    }
}

static void parse_new_font(HlpFont* out, const uint8_t* dscr, const uint8_t* faces,
                           unsigned face_len, unsigned face_num) {
    uint16_t idx = get_u16(dscr, 1);

    out->fg_rgb[0] = dscr[3]; out->fg_rgb[1] = dscr[4]; out->fg_rgb[2] = dscr[5];
    out->bg_rgb[0] = dscr[6]; out->bg_rgb[1] = dscr[7]; out->bg_rgb[2] = dscr[8];

    int32_t height = get_i32(dscr, 14);
    /* NEWFONT height is in twips; convert to half-points (twips / 10) */
    int32_t abs_h = height < 0 ? -height : height;
    out->half_points = (uint16_t)(abs_h / 10);
    out->weight = (uint16_t)get_i16(dscr, 30);
    out->italic = dscr[34];
    out->underline = dscr[35];
    out->strikeout = dscr[36];
    out->small_caps = dscr[38];
    out->family = dscr[41] >> 4;

    if (idx < face_num) {
        size_t copy_len = face_len < 31 ? face_len : 31;
        memcpy(out->face_name, faces + idx * face_len, copy_len);
        out->face_name[copy_len] = '\0';
    } else {
        strcpy(out->face_name, "Helv");
    }
}

int hlp_parse_fonts(HlpFile* hlp) {
    uint8_t *buf, *end;
    if (hlp_find_subfile(hlp, "|FONT", &buf, &end) != 0) {
        hlp->num_fonts = 0;
        hlp->fonts = 0;
        return 0; /* not fatal */
    }

    uint8_t* ref = buf + 9;
    if (ref + 8 > end) return -1;

    uint16_t face_num    = get_u16(ref, 0);
    uint16_t dscr_num    = get_u16(ref, 2);
    uint16_t face_offset = get_u16(ref, 4);
    uint16_t dscr_offset = get_u16(ref, 6);

    if (face_num == 0) return 0;
    unsigned face_len = (dscr_offset - face_offset) / face_num;
    const uint8_t* faces = ref + face_offset;
    const uint8_t* dscrs = ref + dscr_offset;

    hlp->num_fonts = dscr_num;
    hlp->fonts = calloc(dscr_num, sizeof(HlpFont));
    if (!hlp->fonts) return -1;

    int is_new = (face_offset >= 12);
    unsigned dscr_size = is_new ? 34 : 11;

    /* Set scale factor: old fonts store values in half-points (scale 10 to twips),
       new fonts store in twips directly (scale 1) */
    hlp->scale = is_new ? 1 : 10;
    hlp->rounderr = is_new ? 0 : 5;

    for (unsigned i = 0; i < dscr_num; i++) {
        const uint8_t* d = dscrs + i * dscr_size;
        if (d + dscr_size > end) break;

        if (is_new)
            parse_new_font(&hlp->fonts[i], d, faces, face_len, face_num);
        else
            parse_old_font(&hlp->fonts[i], d, faces, face_len, face_num);
    }

    return 0;
}
