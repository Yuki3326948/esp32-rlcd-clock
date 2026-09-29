/*******************************************************************************
 * Size: 24 px
 * Bpp: 1
 * Opts: --font L:\ESP32\Computerfont-1.ttf --symbols 0123456789: --size 24 --bpp 1 --format lvgl --no-compress --lv-include lvgl.h -o L:\ESP32\tools\fontgen\_probe\probe_24.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef PROBE_24
#define PROBE_24 1
#endif

#if PROBE_24

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0030 "0" */
    0xff, 0xf8, 0x1f, 0x3, 0xe0, 0x7c, 0x1f, 0x87,
    0xf0, 0xfe, 0x1f, 0xc3, 0xf8, 0x7f, 0xf, 0xe1,
    0xfc, 0x3f, 0xff, 0xc0,

    /* U+0031 "1" */
    0x39, 0xce, 0x77, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc,

    /* U+0032 "2" */
    0xff, 0xfc, 0xf, 0xc0, 0xfc, 0xf, 0xc0, 0xf0,
    0xf, 0x0, 0xf0, 0xf, 0xff, 0xff, 0x80, 0xf8,
    0xf, 0x83, 0xf8, 0x3f, 0xff,

    /* U+0033 "3" */
    0xff, 0xcc, 0x1c, 0xc1, 0xc0, 0x1c, 0x1, 0xe3,
    0xff, 0x1, 0xf0, 0x1f, 0x1, 0xf0, 0x1f, 0xc1,
    0xfc, 0x1f, 0xc1, 0xff, 0xff,

    /* U+0034 "4" */
    0xff, 0xe6, 0x7, 0x30, 0x39, 0x81, 0xcc, 0xe,
    0x60, 0x73, 0x3, 0x98, 0x1c, 0xff, 0xf8, 0x1f,
    0x0, 0xf8, 0x7, 0xc0, 0x3e, 0x1, 0xf0,

    /* U+0035 "5" */
    0xff, 0xfc, 0x3, 0x80, 0x70, 0xe, 0x1, 0xff,
    0xc0, 0xf8, 0x1f, 0x3, 0xe0, 0x7f, 0xf, 0xe1,
    0xfc, 0x3f, 0xff, 0xc0,

    /* U+0036 "6" */
    0xff, 0xf8, 0x1f, 0x3, 0xe0, 0xc, 0x1, 0xff,
    0xf0, 0xfe, 0x1f, 0xc3, 0xf8, 0x7f, 0xf, 0xe1,
    0xfc, 0x3f, 0xff, 0xc0,

    /* U+0037 "7" */
    0xff, 0xf8, 0x1f, 0x3, 0x80, 0x70, 0x1e, 0x7,
    0xc0, 0xf8, 0x1f, 0x3, 0xe0, 0x7c, 0xf, 0x81,
    0xf0, 0x3e, 0x3, 0xc0,

    /* U+0038 "8" */
    0x3f, 0x84, 0x70, 0x8e, 0x11, 0xc6, 0x3d, 0xff,
    0xf0, 0xfe, 0x1f, 0xc3, 0xf8, 0x7f, 0xf, 0xe1,
    0xfc, 0x3f, 0xff, 0xc0,

    /* U+0039 "9" */
    0xff, 0xf8, 0x1f, 0x3, 0xe0, 0x7c, 0xf, 0x81,
    0xf0, 0x3e, 0x7, 0xff, 0xe0, 0x7c, 0xf, 0x81,
    0xf0, 0x3e, 0x3, 0xc0,

    /* U+003A ":" */
    0xff, 0xfe, 0x0, 0x0, 0x0, 0xff, 0xfe
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 211, .box_w = 11, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 20, .adv_w = 106, .box_w = 5, .box_h = 14, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 29, .adv_w = 211, .box_w = 12, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 50, .adv_w = 211, .box_w = 12, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 71, .adv_w = 232, .box_w = 13, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 94, .adv_w = 211, .box_w = 11, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 114, .adv_w = 211, .box_w = 11, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 134, .adv_w = 211, .box_w = 11, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 154, .adv_w = 211, .box_w = 11, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 174, .adv_w = 211, .box_w = 11, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 194, .adv_w = 114, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 48, .range_length = 11, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t probe_24 = {
#else
lv_font_t probe_24 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 14,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -4,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if PROBE_24*/

