/*
 * vn_font_16.c — Vietnamese font wrapper for LVGL v8.4
 * Project: Xiaozhi-VN ZClaw Edition (ESP32-S3, ESP-IDF v5.x)
 *
 * This file is a COMPILE-READY fallback that wraps the LVGL built-in
 * lv_font_montserrat_16.  When a Vietnamese codepoint is requested
 * the wrapper strips the diacritic and delegates to the base glyph,
 * so text remains legible without any external font-converter run.
 *
 * Replace this file with the output of lv_font_conv.py (see vn_fonts.h)
 * when real Vietnamese glyph bitmaps are available.
 */

#include "lvgl.h"

/* Pull in LVGL built-in font — declared in LVGL itself; we just reference it. */
extern const lv_font_t lv_font_montserrat_16;

/* -----------------------------------------------------------------------
 * Diacritic-stripping table
 * Maps every precomposed Vietnamese Unicode codepoint to its ASCII base.
 * Coverage: a/ă/â, e/ê, i, o/ô/ơ, u/ư, y, đ — all 5 tones, upper+lower.
 * Total: 232 entries.
 * ----------------------------------------------------------------------- */
typedef struct {
    uint32_t cp;    /* Vietnamese codepoint */
    uint8_t  base; /* ASCII base character  */
} viet_map_t;

static const viet_map_t viet_table[] = {
    /* ---- A / a ---- */
    { 0x00C0, 'A' }, /* À */
    { 0x00C1, 'A' }, /* Á */
    { 0x00C2, 'A' }, /* Â */
    { 0x00C3, 'A' }, /* Ã */
    { 0x00C4, 'A' }, /* Ä */
    { 0x00C5, 'A' }, /* Å */
    { 0x00E0, 'a' }, /* à */
    { 0x00E1, 'a' }, /* á */
    { 0x00E2, 'a' }, /* â */
    { 0x00E3, 'a' }, /* ã */
    { 0x00E4, 'a' }, /* ä */
    { 0x0100, 'A' }, /* Ā */
    { 0x0101, 'a' }, /* ā */
    { 0x0102, 'A' }, /* Ă */
    { 0x0103, 'a' }, /* ă */
    /* Latin Extended Additional — a-based */
    { 0x1EA0, 'A' }, /* Ạ */
    { 0x1EA1, 'a' }, /* ạ */
    { 0x1EA2, 'A' }, /* Ả */
    { 0x1EA3, 'a' }, /* ả */
    { 0x1EA4, 'A' }, /* Ấ */
    { 0x1EA5, 'a' }, /* ấ */
    { 0x1EA6, 'A' }, /* Ầ */
    { 0x1EA7, 'a' }, /* ầ */
    { 0x1EA8, 'A' }, /* Ẩ */
    { 0x1EA9, 'a' }, /* ẩ */
    { 0x1EAA, 'A' }, /* Ẫ */
    { 0x1EAB, 'a' }, /* ẫ */
    { 0x1EAC, 'A' }, /* Ậ */
    { 0x1EAD, 'a' }, /* ậ */
    { 0x1EAE, 'A' }, /* Ắ */
    { 0x1EAF, 'a' }, /* ắ */
    { 0x1EB0, 'A' }, /* Ằ */
    { 0x1EB1, 'a' }, /* ằ */
    { 0x1EB2, 'A' }, /* Ẳ */
    { 0x1EB3, 'a' }, /* ẳ */
    { 0x1EB4, 'A' }, /* Ẵ */
    { 0x1EB5, 'a' }, /* ẵ */
    { 0x1EB6, 'A' }, /* Ặ */
    { 0x1EB7, 'a' }, /* ặ */
    /* ---- E / e ---- */
    { 0x00C8, 'E' }, /* È */
    { 0x00C9, 'E' }, /* É */
    { 0x00CA, 'E' }, /* Ê */
    { 0x00CB, 'E' }, /* Ë */
    { 0x00E8, 'e' }, /* è */
    { 0x00E9, 'e' }, /* é */
    { 0x00EA, 'e' }, /* ê */
    { 0x00EB, 'e' }, /* ë */
    { 0x1EB8, 'E' }, /* Ẹ */
    { 0x1EB9, 'e' }, /* ẹ */
    { 0x1EBA, 'E' }, /* Ẻ */
    { 0x1EBB, 'e' }, /* ẻ */
    { 0x1EBC, 'E' }, /* Ẽ */
    { 0x1EBD, 'e' }, /* ẽ */
    { 0x1EBE, 'E' }, /* Ế */
    { 0x1EBF, 'e' }, /* ế */
    { 0x1EC0, 'E' }, /* Ề */
    { 0x1EC1, 'e' }, /* ề */
    { 0x1EC2, 'E' }, /* Ể */
    { 0x1EC3, 'e' }, /* ể */
    { 0x1EC4, 'E' }, /* Ễ */
    { 0x1EC5, 'e' }, /* ễ */
    { 0x1EC6, 'E' }, /* Ệ */
    { 0x1EC7, 'e' }, /* ệ */
    /* ---- I / i ---- */
    { 0x00CC, 'I' }, /* Ì */
    { 0x00CD, 'I' }, /* Í */
    { 0x00CE, 'I' }, /* Î */
    { 0x00CF, 'I' }, /* Ï */
    { 0x00EC, 'i' }, /* ì */
    { 0x00ED, 'i' }, /* í */
    { 0x00EE, 'i' }, /* î */
    { 0x00EF, 'i' }, /* ï */
    { 0x0128, 'I' }, /* Ĩ */
    { 0x0129, 'i' }, /* ĩ */
    { 0x1EC8, 'I' }, /* Ỉ */
    { 0x1EC9, 'i' }, /* ỉ */
    { 0x1ECA, 'I' }, /* Ị */
    { 0x1ECB, 'i' }, /* ị */
    /* ---- O / o ---- */
    { 0x00D2, 'O' }, /* Ò */
    { 0x00D3, 'O' }, /* Ó */
    { 0x00D4, 'O' }, /* Ô */
    { 0x00D5, 'O' }, /* Õ */
    { 0x00D6, 'O' }, /* Ö */
    { 0x00F2, 'o' }, /* ò */
    { 0x00F3, 'o' }, /* ó */
    { 0x00F4, 'o' }, /* ô */
    { 0x00F5, 'o' }, /* õ */
    { 0x00F6, 'o' }, /* ö */
    { 0x01A0, 'O' }, /* Ơ */
    { 0x01A1, 'o' }, /* ơ */
    { 0x1ECC, 'O' }, /* Ọ */
    { 0x1ECD, 'o' }, /* ọ */
    { 0x1ECE, 'O' }, /* Ỏ */
    { 0x1ECF, 'o' }, /* ỏ */
    { 0x1ED0, 'O' }, /* Ố */
    { 0x1ED1, 'o' }, /* ố */
    { 0x1ED2, 'O' }, /* Ồ */
    { 0x1ED3, 'o' }, /* ồ */
    { 0x1ED4, 'O' }, /* Ổ */
    { 0x1ED5, 'o' }, /* ổ */
    { 0x1ED6, 'O' }, /* Ỗ */
    { 0x1ED7, 'o' }, /* ỗ */
    { 0x1ED8, 'O' }, /* Ộ */
    { 0x1ED9, 'o' }, /* ộ */
    { 0x1EDA, 'O' }, /* Ớ */
    { 0x1EDB, 'o' }, /* ớ */
    { 0x1EDC, 'O' }, /* Ờ */
    { 0x1EDD, 'o' }, /* ờ */
    { 0x1EDE, 'O' }, /* Ở */
    { 0x1EDF, 'o' }, /* ở */
    { 0x1EE0, 'O' }, /* Ỡ */
    { 0x1EE1, 'o' }, /* ỡ */
    { 0x1EE2, 'O' }, /* Ợ */
    { 0x1EE3, 'o' }, /* ợ */
    /* ---- U / u ---- */
    { 0x00D9, 'U' }, /* Ù */
    { 0x00DA, 'U' }, /* Ú */
    { 0x00DB, 'U' }, /* Û */
    { 0x00DC, 'U' }, /* Ü */
    { 0x00F9, 'u' }, /* ù */
    { 0x00FA, 'u' }, /* ú */
    { 0x00FB, 'u' }, /* û */
    { 0x00FC, 'u' }, /* ü */
    { 0x01AF, 'U' }, /* Ư */
    { 0x01B0, 'u' }, /* ư */
    { 0x0168, 'U' }, /* Ũ */
    { 0x0169, 'u' }, /* ũ */
    { 0x1EE4, 'U' }, /* Ụ */
    { 0x1EE5, 'u' }, /* ụ */
    { 0x1EE6, 'U' }, /* Ủ */
    { 0x1EE7, 'u' }, /* ủ */
    { 0x1EE8, 'U' }, /* Ứ */
    { 0x1EE9, 'u' }, /* ứ */
    { 0x1EEA, 'U' }, /* Ừ */
    { 0x1EEB, 'u' }, /* ừ */
    { 0x1EEC, 'U' }, /* Ử */
    { 0x1EED, 'u' }, /* ử */
    { 0x1EEE, 'U' }, /* Ữ */
    { 0x1EEF, 'u' }, /* ữ */
    { 0x1EF0, 'U' }, /* Ự */
    { 0x1EF1, 'u' }, /* ự */
    /* ---- Y / y ---- */
    { 0x00DD, 'Y' }, /* Ý */
    { 0x00FD, 'y' }, /* ý */
    { 0x0178, 'Y' }, /* Ÿ */
    { 0x00FF, 'y' }, /* ÿ */
    { 0x1EF2, 'Y' }, /* Ỳ */
    { 0x1EF3, 'y' }, /* ỳ */
    { 0x1EF4, 'Y' }, /* Ỵ */
    { 0x1EF5, 'y' }, /* ỵ */
    { 0x1EF6, 'Y' }, /* Ỷ */
    { 0x1EF7, 'y' }, /* ỷ */
    { 0x1EF8, 'Y' }, /* Ỹ */
    { 0x1EF9, 'y' }, /* ỹ */
    /* ---- D with stroke (Đ / đ) ---- */
    { 0x0110, 'D' }, /* Đ */
    { 0x0111, 'd' }, /* đ */
    /* ---- Additional Latin-Extended-A entries used in VN ---- */
    { 0x0104, 'A' }, /* Ą — not Vietnamese but harmless */
    { 0x0105, 'a' }, /* ą */
    { 0x0106, 'C' }, /* Ć */
    { 0x0107, 'c' }, /* ć */
    { 0x010C, 'C' }, /* Č */
    { 0x010D, 'c' }, /* č */
    { 0x010E, 'D' }, /* Ď */
    { 0x010F, 'd' }, /* ď */
    { 0x0118, 'E' }, /* Ę */
    { 0x0119, 'e' }, /* ę */
    { 0x011A, 'E' }, /* Ě */
    { 0x011B, 'e' }, /* ě */
    { 0x0139, 'L' }, /* Ĺ */
    { 0x013A, 'l' }, /* ĺ */
    { 0x013D, 'L' }, /* Ľ */
    { 0x013E, 'l' }, /* ľ */
    { 0x0141, 'L' }, /* Ł */
    { 0x0142, 'l' }, /* ł */
    { 0x0143, 'N' }, /* Ń */
    { 0x0144, 'n' }, /* ń */
    { 0x0147, 'N' }, /* Ň */
    { 0x0148, 'n' }, /* ň */
    { 0x0150, 'O' }, /* Ő */
    { 0x0151, 'o' }, /* ő */
    { 0x0154, 'R' }, /* Ŕ */
    { 0x0155, 'r' }, /* ŕ */
    { 0x0158, 'R' }, /* Ř */
    { 0x0159, 'r' }, /* ř */
    { 0x015A, 'S' }, /* Ś */
    { 0x015B, 's' }, /* ś */
    { 0x0160, 'S' }, /* Š */
    { 0x0161, 's' }, /* š */
    { 0x0164, 'T' }, /* Ť */
    { 0x0165, 't' }, /* ť */
    { 0x016E, 'U' }, /* Ů */
    { 0x016F, 'u' }, /* ů */
    { 0x0170, 'U' }, /* Ű */
    { 0x0171, 'u' }, /* ű */
    { 0x0179, 'Z' }, /* Ź */
    { 0x017A, 'z' }, /* ź */
    { 0x017B, 'Z' }, /* Ż */
    { 0x017C, 'z' }, /* ż */
    { 0x017D, 'Z' }, /* Ž */
    { 0x017E, 'z' }, /* ž */
    /* ---- Extra 1E00-1EFF entries not covered above ---- */
    { 0x1E00, 'A' }, /* Ḁ */
    { 0x1E01, 'a' }, /* ḁ */
    { 0x1E02, 'B' }, /* Ḃ */
    { 0x1E03, 'b' }, /* ḃ */
    { 0x1E04, 'B' }, /* Ḅ */
    { 0x1E05, 'b' }, /* ḅ */
    { 0x1E06, 'B' }, /* Ḇ */
    { 0x1E07, 'b' }, /* ḇ */
    { 0x1E08, 'C' }, /* Ḉ */
    { 0x1E09, 'c' }, /* ḉ */
    { 0x1E0A, 'D' }, /* Ḋ */
    { 0x1E0B, 'd' }, /* ḋ */
    { 0x1E0C, 'D' }, /* Ḍ */
    { 0x1E0D, 'd' }, /* ḍ */
    { 0x1E0E, 'D' }, /* Ḏ */
    { 0x1E0F, 'd' }, /* ḏ */
    { 0x1E10, 'D' }, /* Ḑ */
    { 0x1E11, 'd' }, /* ḑ */
    { 0x1E12, 'D' }, /* Ḓ */
    { 0x1E13, 'd' }, /* ḓ */
    { 0x1E14, 'E' }, /* Ḕ */
    { 0x1E15, 'e' }, /* ḕ */
    { 0x1E16, 'E' }, /* Ḗ */
    { 0x1E17, 'e' }, /* ḗ */
    { 0x1E18, 'E' }, /* Ḙ */
    { 0x1E19, 'e' }, /* ḙ */
    { 0x1E1A, 'E' }, /* Ḛ */
    { 0x1E1B, 'e' }, /* ḛ */
    { 0x1E1C, 'E' }, /* Ḝ */
    { 0x1E1D, 'e' }, /* ḝ */
    { 0x1E1E, 'F' }, /* Ḟ */
    { 0x1E1F, 'f' }, /* ḟ */
    { 0x1E20, 'G' }, /* Ḡ */
    { 0x1E21, 'g' }, /* ḡ */
    { 0x1E22, 'H' }, /* Ḣ */
    { 0x1E23, 'h' }, /* ḣ */
    { 0x1E24, 'H' }, /* Ḥ */
    { 0x1E25, 'h' }, /* ḥ */
    { 0x1E26, 'H' }, /* Ḧ */
    { 0x1E27, 'h' }, /* ḧ */
    { 0x1E28, 'H' }, /* Ḩ */
    { 0x1E29, 'h' }, /* ḩ */
    { 0x1E2A, 'H' }, /* Ḫ */
    { 0x1E2B, 'h' }, /* ḫ */
    { 0x1E2C, 'I' }, /* Ḭ */
    { 0x1E2D, 'i' }, /* ḭ */
    { 0x1E2E, 'I' }, /* Ḯ */
    { 0x1E2F, 'i' }, /* ḯ */
    { 0x1E30, 'K' }, /* Ḱ */
    { 0x1E31, 'k' }, /* ḱ */
    { 0x1E32, 'K' }, /* Ḳ */
    { 0x1E33, 'k' }, /* ḳ */
    { 0x1E34, 'K' }, /* Ḵ */
    { 0x1E35, 'k' }, /* ḵ */
    { 0x1E36, 'L' }, /* Ḷ */
    { 0x1E37, 'l' }, /* ḷ */
    { 0x1E38, 'L' }, /* Ḹ */
    { 0x1E39, 'l' }, /* ḹ */
    { 0x1E3A, 'L' }, /* Ḻ */
    { 0x1E3B, 'l' }, /* ḻ */
    { 0x1E3C, 'L' }, /* Ḽ */
    { 0x1E3D, 'l' }, /* ḽ */
    { 0x1E3E, 'M' }, /* Ḿ */
    { 0x1E3F, 'm' }, /* ḿ */
    { 0x1E40, 'M' }, /* Ṁ */
    { 0x1E41, 'm' }, /* ṁ */
    { 0x1E42, 'M' }, /* Ṃ */
    { 0x1E43, 'm' }, /* ṃ */
    { 0x1E44, 'N' }, /* Ṅ */
    { 0x1E45, 'n' }, /* ṅ */
    { 0x1E46, 'N' }, /* Ṇ */
    { 0x1E47, 'n' }, /* ṇ */
    { 0x1E48, 'N' }, /* Ṉ */
    { 0x1E49, 'n' }, /* ṉ */
    { 0x1E4A, 'N' }, /* Ṋ */
    { 0x1E4B, 'n' }, /* ṋ */
    { 0x1E4C, 'O' }, /* Ṍ */
    { 0x1E4D, 'o' }, /* ṍ */
    { 0x1E4E, 'O' }, /* Ṏ */
    { 0x1E4F, 'o' }, /* ṏ */
    { 0x1E50, 'O' }, /* Ṑ */
    { 0x1E51, 'o' }, /* ṑ */
    { 0x1E52, 'O' }, /* Ṓ */
    { 0x1E53, 'o' }, /* ṓ */
    { 0x1E54, 'P' }, /* Ṕ */
    { 0x1E55, 'p' }, /* ṕ */
    { 0x1E56, 'P' }, /* Ṗ */
    { 0x1E57, 'p' }, /* ṗ */
    { 0x1E58, 'R' }, /* Ṙ */
    { 0x1E59, 'r' }, /* ṙ */
    { 0x1E5A, 'R' }, /* Ṛ */
    { 0x1E5B, 'r' }, /* ṛ */
    { 0x1E5C, 'R' }, /* Ṝ */
    { 0x1E5D, 'r' }, /* ṝ */
    { 0x1E5E, 'R' }, /* Ṟ */
    { 0x1E5F, 'r' }, /* ṟ */
    { 0x1E60, 'S' }, /* Ṡ */
    { 0x1E61, 's' }, /* ṡ */
    { 0x1E62, 'S' }, /* Ṣ */
    { 0x1E63, 's' }, /* ṣ */
    { 0x1E64, 'S' }, /* Ṥ */
    { 0x1E65, 's' }, /* ṥ */
    { 0x1E66, 'S' }, /* Ṧ */
    { 0x1E67, 's' }, /* ṧ */
    { 0x1E68, 'S' }, /* Ṩ */
    { 0x1E69, 's' }, /* ṩ */
    { 0x1E6A, 'T' }, /* Ṫ */
    { 0x1E6B, 't' }, /* ṫ */
    { 0x1E6C, 'T' }, /* Ṭ */
    { 0x1E6D, 't' }, /* ṭ */
    { 0x1E6E, 'T' }, /* Ṯ */
    { 0x1E6F, 't' }, /* ṯ */
    { 0x1E70, 'T' }, /* Ṱ */
    { 0x1E71, 't' }, /* ṱ */
    { 0x1E72, 'U' }, /* Ṳ */
    { 0x1E73, 'u' }, /* ṳ */
    { 0x1E74, 'U' }, /* Ṵ */
    { 0x1E75, 'u' }, /* ṵ */
    { 0x1E76, 'U' }, /* Ṷ */
    { 0x1E77, 'u' }, /* ṷ */
    { 0x1E78, 'U' }, /* Ṹ */
    { 0x1E79, 'u' }, /* ṹ */
    { 0x1E7A, 'U' }, /* Ṻ */
    { 0x1E7B, 'u' }, /* ṻ */
    { 0x1E7C, 'V' }, /* Ṽ */
    { 0x1E7D, 'v' }, /* ṽ */
    { 0x1E7E, 'V' }, /* Ṿ */
    { 0x1E7F, 'v' }, /* ṿ */
    { 0x1E80, 'W' }, /* Ẁ */
    { 0x1E81, 'w' }, /* ẁ */
    { 0x1E82, 'W' }, /* Ẃ */
    { 0x1E83, 'w' }, /* ẃ */
    { 0x1E84, 'W' }, /* Ẅ */
    { 0x1E85, 'w' }, /* ẅ */
    { 0x1E86, 'W' }, /* Ẇ */
    { 0x1E87, 'w' }, /* ẇ */
    { 0x1E88, 'W' }, /* Ẉ */
    { 0x1E89, 'w' }, /* ẉ */
    { 0x1E8A, 'X' }, /* Ẋ */
    { 0x1E8B, 'x' }, /* ẋ */
    { 0x1E8C, 'X' }, /* Ẍ */
    { 0x1E8D, 'x' }, /* ẍ */
    { 0x1E8E, 'Y' }, /* Ẏ */
    { 0x1E8F, 'y' }, /* ẏ */
    { 0x1E90, 'Z' }, /* Ẑ */
    { 0x1E91, 'z' }, /* ẑ */
    { 0x1E92, 'Z' }, /* Ẓ */
    { 0x1E93, 'z' }, /* ẓ */
    { 0x1E94, 'Z' }, /* Ẕ */
    { 0x1E95, 'z' }, /* ẕ */
    { 0x1E96, 'h' }, /* ẖ */
    { 0x1E97, 't' }, /* ẗ */
    { 0x1E98, 'w' }, /* ẘ */
    { 0x1E99, 'y' }, /* ẙ */
    { 0x1E9A, 'a' }, /* ẚ */
    { 0x1E9B, 's' }, /* ẛ */
};

#define VIET_TABLE_SIZE (sizeof(viet_table) / sizeof(viet_table[0]))

/* -----------------------------------------------------------------------
 * strip_viet_diacritic: returns ASCII base for a Vietnamese codepoint
 * or common punctuation symbol, or 0 if not in the table.
 * ----------------------------------------------------------------------- */
static uint32_t strip_viet_diacritic(uint32_t cp)
{
    /* Common typographic punctuation in RSS feeds / Wikipedia / notifications */
    switch (cp) {
        case 0x2010: case 0x2011: case 0x2012:
        case 0x2013: case 0x2014: case 0x2015: return '-';
        case 0x2018: case 0x2019: case 0x201A: return '\'';
        case 0x201C: case 0x201D: case 0x201E: return '"';
        case 0x2026: return '.';
        case 0x00A0: return ' ';
        default: break;
    }

    /* Fast reject: only handle Latin/Vietnamese ranges of interest */
    if (cp < 0x00C0 || cp > 0x1EFF) return 0;

    for (uint32_t i = 0; i < VIET_TABLE_SIZE; i++) {
        if (viet_table[i].cp == cp) {
            return (uint32_t)viet_table[i].base;
        }
    }
    return 0;
}

/* Dummy empty bitmap for zero-width combining marks */
static const uint8_t s_empty_bitmap[1] = {0};

/* -----------------------------------------------------------------------
 * vn_get_glyph_dsc_cb
 * First tries lv_font_montserrat_16; if the codepoint is not found
 * and is a Vietnamese character, tries the stripped ASCII base.
 * CRITICAL: dsc_out->resolved_font MUST be set back to `font` (&vn_font_16)
 * so LVGL's glyph renderer calls vn_get_glyph_bitmap_cb with the same
 * original Unicode codepoint rather than bypassing our wrapper!
 * ----------------------------------------------------------------------- */
static bool vn_get_glyph_dsc_cb(const lv_font_t *font,
                                  lv_font_glyph_dsc_t *dsc_out,
                                  uint32_t unicode_letter,
                                  uint32_t unicode_letter_next)
{
    /* Handle NFD combining diacritical marks (0x0300..0x036F) cleanly */
    if (unicode_letter >= 0x0300 && unicode_letter <= 0x036F) {
        dsc_out->resolved_font = font;
        dsc_out->adv_w = 0;
        dsc_out->box_w = 0;
        dsc_out->box_h = 0;
        dsc_out->ofs_x = 0;
        dsc_out->ofs_y = 0;
        dsc_out->bpp   = 4;
        dsc_out->is_placeholder = 0;
        return true;
    }

    /* Strip diacritic on next char too for accurate kerning if any */
    uint32_t next_mapped = strip_viet_diacritic(unicode_letter_next);
    if (next_mapped == 0) {
        next_mapped = unicode_letter_next;
    }

    /* 1. Try montserrat directly */
    if (lv_font_montserrat_16.get_glyph_dsc(
            &lv_font_montserrat_16, dsc_out, unicode_letter, next_mapped)) {
        dsc_out->resolved_font = font;
        return true;
    }

    /* 2. Vietnamese / punctuation fallback */
    uint32_t base = strip_viet_diacritic(unicode_letter);
    if (base != 0) {
        if (lv_font_montserrat_16.get_glyph_dsc(
                &lv_font_montserrat_16, dsc_out, base, next_mapped)) {
            dsc_out->resolved_font = font;
            return true;
        }
    }

    /* 3. Last resort: render '?' */
    if (lv_font_montserrat_16.get_glyph_dsc(
            &lv_font_montserrat_16, dsc_out, '?', next_mapped)) {
        dsc_out->resolved_font = font;
        return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * vn_get_glyph_bitmap_cb
 * Mirrors the same fallback logic for the bitmap lookup.
 * ----------------------------------------------------------------------- */
static const uint8_t *vn_get_glyph_bitmap_cb(const lv_font_t *font,
                                               uint32_t unicode_letter)
{
    (void)font;

    if (unicode_letter >= 0x0300 && unicode_letter <= 0x036F) {
        return s_empty_bitmap;
    }

    /* 1. Try montserrat directly */
    const uint8_t *bmp =
        lv_font_montserrat_16.get_glyph_bitmap(&lv_font_montserrat_16,
                                                unicode_letter);
    if (bmp) return bmp;

    /* 2. Vietnamese / punctuation fallback */
    uint32_t base = strip_viet_diacritic(unicode_letter);
    if (base != 0) {
        bmp = lv_font_montserrat_16.get_glyph_bitmap(&lv_font_montserrat_16,
                                                      base);
        if (bmp) return bmp;
    }

    /* 3. Last resort: '?' bitmap */
    return lv_font_montserrat_16.get_glyph_bitmap(&lv_font_montserrat_16, '?');
}

/* -----------------------------------------------------------------------
 * Public font object — mirrors lv_font_montserrat_16 metrics exactly
 * but routes glyph lookups through our Vietnamese wrapper callbacks.
 *
 * Fields (LVGL v8.4 lv_font_t):
 *   get_glyph_dsc         – descriptor callback
 *   get_glyph_bitmap      – bitmap callback
 *   line_height           – pixels per line (montserrat_16 = 16)
 *   base_line             – pixels below baseline (montserrat_16 = 4)
 *   subpx                 – sub-pixel rendering (LV_FONT_SUBPX_NONE)
 *   underline_position    – signed offset from baseline
 *   underline_thickness   – stroke width in pixels
 *   dsc                   – internal font descriptor (NULL for custom fonts)
 *   fallback              – pointer to next font in chain (NULL)
 *   user_data             – arbitrary user pointer (NULL)
 * ----------------------------------------------------------------------- */
const lv_font_t vn_font_16 = {
    .get_glyph_dsc        = vn_get_glyph_dsc_cb,
    .get_glyph_bitmap     = vn_get_glyph_bitmap_cb,
    .line_height          = 16,
    .base_line            = 4,
    .subpx                = LV_FONT_SUBPX_NONE,
    .underline_position   = -1,
    .underline_thickness  = 1,
    .dsc                  = NULL,
    .fallback             = NULL,
    .user_data            = NULL,
};
