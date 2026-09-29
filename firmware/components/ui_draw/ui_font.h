/*
 * 界面字体格式 —— 极简版,只保留单色屏画字需要的东西。
 *
 * 为什么自己定格式(而不是继续用 LVGL 的):
 *   LVGL 的字体结构带 cmap 表、kerning 表、压缩位图等一大堆东西,
 *   我们这块 400x300 单色屏、固定布局的界面一样都用不上。
 *   这里就是一个"码点 -> 一张 1bpp 小图"的平表,查表用二分,
 *   一颗字节都不浪费。
 *
 * 位图排布:每行按字节对齐,MSB 在左(和一般点阵字库一致)。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t code;      /* Unicode 码点            */
    uint16_t adv;       /* 步进宽度(像素)          */
    uint8_t  w;         /* 位图宽(0 = 空白字符)   */
    uint8_t  h;         /* 位图高                  */
    int8_t   ox;        /* 位图左边相对笔位置的偏移 */
    int8_t   oy;        /* 位图顶边相对基线的偏移(负数在基线上方) */
    uint32_t off;       /* 在位图数组里的字节偏移   */
} UiGlyph;

typedef struct {
    uint8_t        line_h;   /* 行高                    */
    uint8_t        base;     /* 基线距行顶的距离        */
    uint16_t       cnt;      /* 字形个数(按 code 升序) */
    const UiGlyph *glyphs;
    const uint8_t *bits;
} UiFont;

/* 具体数据在 ui_font_data.c(由 gen_ui_font.py 生成) */
extern const UiFont ui_font_cjk16;      /* 标签 / 提示 / 正文 */
extern const UiFont ui_font_cjk20;      /* 标题 / 日期        */
extern const UiFont ui_font_ascii14;    /* SSID 等小字        */
extern const UiFont ui_font_ascii28;    /* 传感器读数 / AM-PM */
extern const UiFont ui_font_ascii10;    /* 坐标轴刻度数字     */
extern const UiFont ui_font_clock48;    /* 时钟大字           */

#ifdef __cplusplus
}
#endif
