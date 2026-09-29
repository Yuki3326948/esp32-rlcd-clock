/*
 * 绘图基础层 —— 取代原来的 LVGL。
 *
 * 这块屏是 400x300 单色(1bpp),界面布局完全固定,没有滚动、
 * 没有动画、没有可交互控件。LVGL 那套对象树 / 脏区 / 样式系统
 * 在这里纯属负担(实测它占了 429KB flash 和 65KB 内部 DRAM),
 * 所以整个拆掉,直接往显存里画。
 *
 * 分工:
 *   ui_draw  —— 点/线/矩形/文字(本文件)
 *   user_app —— 两个页面的具体排版
 *   频谱柱  —— 仍然由 SpectrumTask 直接改显存(那段本来就绕开了 LVGL)
 *
 * 颜色只有亮/暗两种。底层 RLCD_SetPixel 只认"非零 = 亮",
 * 所以这里统一用 1 = 亮(白)、0 = 暗(黑)。
 */
#pragma once

#include "ui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 基础图元 ---------------- */
/* 整屏填一个色(改页时用) */
void Ui_Clear(int dark);

void Ui_Pixel(int x, int y, int dark);
void Ui_FillRect(int x, int y, int w, int h, int dark);
void Ui_HLine(int x, int y, int w, int dark);
void Ui_VLine(int x, int y, int h, int dark);

/* ---------------- 文字 ---------------- */
/*
 * 从 (x, baseline_y) 开始画一行 UTF-8 文本。
 * 只画"亮"的点 —— 也就是说背景得先自己清干净(见 Ui_FillRect)。
 * 遇到 '\n' 会换行(回到起始 x,基线加上行高)。
 * 返回画完后的笔位置(x)。
 */
int Ui_Text(int x, int baseline_y, const UiFont *font,
            const char *utf8, int dark);

/* 这一行文字占多宽(不画,只量)。多行时取最宽的那行。 */
int Ui_TextWidth(const UiFont *font, const char *utf8);

/* 以 cx 为中心画一行(多行时每行各自居中) */
void Ui_TextCentered(int cx, int baseline_y, const UiFont *font,
                     const char *utf8, int dark);

/* 行高,排版时算 y 用 */
#define UI_LINE_H(f)   ((f)->line_h)

#ifdef __cplusplus
}
#endif
