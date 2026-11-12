#pragma once

/*
 * vn_fonts.h — Vietnamese font declarations for LVGL v8.4
 * Project: S.W.Ni-Oh (ESP32-S3, ESP-IDF v5.x)
 *
 * Glyph ranges included in vn_font_16:
 *   0x0020–0x007E  Basic Latin (ASCII printable)
 *   0x0100–0x017E  Latin Extended-A (ă, Ă, ơ, Ơ, ư, Ư, etc.)
 *   0x0110, 0x0111 Đ / đ (Latin letter D with stroke)
 *   0x0300–0x036F  Combining Diacritical Marks (tone marks)
 *   0x1E00–0x1EFF  Latin Extended Additional (full precomposed Vietnamese)
 *
 * To regenerate vn_font_16.c with the real LVGL font converter:
 *   python3 lv_font_conv.py \
 *       --font Roboto-Regular.ttf \
 *       --size 16 \
 *       --format lvgl \
 *       --bpp 4 \
 *       --range 0x0020-0x007E,0x0100-0x017E,0x0110,0x0111,0x0300-0x036F,0x1E00-0x1EFF \
 *       -o vn_font_16.c
 *
 * Until the real glyph data is available, vn_font_16.c provides a
 * wrapper that falls back to lv_font_montserrat_16 (LVGL built-in)
 * with diacritic-stripping so Vietnamese text renders legibly.
 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL font declaration — defined in vn_font_16.c */
LV_FONT_DECLARE(vn_font_16)
extern const lv_font_t vn_font_16;

#ifdef __cplusplus
}
#endif

/* Convenience aliases */
#define VN_FONT_SMALL   (&vn_font_16)
#define VN_FONT_DEFAULT (&vn_font_16)
