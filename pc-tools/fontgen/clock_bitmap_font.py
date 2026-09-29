# -*- coding: utf-8 -*-
"""
方块点阵时钟 —— 照着参考图(方块拼接 + 缝线质感)设计的数字字形预览。

设计思路:
    1. 手绘一套小尺寸点阵数字(8 宽 x 16 高,笔画 2 点粗)
    2. 显示时整数字【整数倍放大】—— 每个点变成一个方块
    3. 方块之间可选留 1px 缝 -> 就出现参考图那种"方块拼接/编织"的质感
    4. 冒号不依赖字体,直接画方点(更粗更醒目,也不受字体影响)

为什么走点阵而不是继续用 TTF:
    参考图的关键特征是"笔画由方块构成"。矢量字体放大再二值化,
    边缘会是光滑的直线;只有点阵放大才能得到方块拼接的感觉。
    而且点阵 = 天然等宽,秒跳时不会左右抖。

输出: tools/fontgen/_clock_bitmap.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

FONTS = r"C:\Windows\Fonts"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "_clock_bitmap.png")

# ----------------------------------------------------------------------
# 8 宽 x 16 高的点阵数字。'#' = 笔画,'.' = 空。
# 笔画统一 2 点粗,拐角处对齐 —— 放大后才会有参考图那种块面感。
# ----------------------------------------------------------------------
DIGITS = {
    "0": ["..####..",
          ".######.",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          ".######.",
          "..####.."],

    "1": ["...##...",
          "..###...",
          ".####...",
          "##.##...",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          "...##...",
          ".######.",
          ".######."],

    "2": ["..####..",
          ".######.",
          "##....##",
          "##....##",
          "......##",
          "......##",
          ".....##.",
          "....##..",
          "...##...",
          "..##....",
          ".##.....",
          "##......",
          "##......",
          "##......",
          "########",
          "########"],

    "3": ["..####..",
          ".######.",
          "##....##",
          "......##",
          "......##",
          "......##",
          "....###.",
          "....###.",
          "......##",
          "......##",
          "......##",
          "##....##",
          "##....##",
          "##....##",
          ".######.",
          "..####.."],

    "4": [".....##.",
          "....###.",
          "...####.",
          "..##.##.",
          ".##..##.",
          "##...##.",
          "##...##.",
          "##...##.",
          "########",
          "########",
          ".....##.",
          ".....##.",
          ".....##.",
          ".....##.",
          ".....##.",
          ".....##."],

    "5": ["########",
          "########",
          "##......",
          "##......",
          "##......",
          "##......",
          ".#####..",
          ".######.",
          "......##",
          "......##",
          "......##",
          "##....##",
          "##....##",
          "##....##",
          ".######.",
          "..####.."],

    "6": ["..####..",
          ".######.",
          "##....##",
          "##......",
          "##......",
          "##......",
          ".#####..",
          ".######.",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          ".######.",
          "..####.."],

    "7": ["########",
          "########",
          "......##",
          "......##",
          ".....##.",
          ".....##.",
          "....##..",
          "....##..",
          "...##...",
          "...##...",
          "..##....",
          "..##....",
          ".##.....",
          ".##.....",
          "##......",
          "##......"],

    "8": ["..####..",
          ".######.",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          ".######.",
          ".######.",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          ".######.",
          "..####.."],

    "9": ["..####..",
          ".######.",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          "##....##",
          ".######.",
          ".#####..",
          "......##",
          "......##",
          "......##",
          "##....##",
          "##....##",
          ".######.",
          "..####.."],
}

GLYPH_W, GLYPH_H = 8, 16


def draw_glyph(d, ch, x, y, scale, gap, ink=1):
    """把一个字形画到 (x, y),每个点画成 scale x scale 的方块。

    gap = 方块之间留的空隙(像素)。
        gap=0 -> 连成一片,纯粹的粗笔画
        gap=1 -> 每个点独立成块,出现参考图那种"方块拼接"的质感
    ink = 1 画白(黑底用),0 画黑(白底用)
    """
    for r, row in enumerate(DIGITS[ch]):
        for c, v in enumerate(row):
            if v != "#":
                continue
            px = x + c * scale
            py = y + r * scale
            d.rectangle([px, py,
                         px + scale - 1 - gap, py + scale - 1 - gap], fill=ink)


def draw_colon(d, x, y, scale, gap, h_cells, ink=1):
    """冒号自己画:两个方点。位置对齐字形的竖直中线(偏上/偏下各一点)。"""
    size = scale * 2                      # 方点边长 = 2 个点
    for row in (h_cells * 0.34, h_cells * 0.62):
        py = y + int(row) * scale
        d.rectangle([x, py, x + size - 1 - gap, py + size - 1 - gap], fill=ink)
    return size


def draw_grid(d, x0, y0, x1, y1, step, ink=1):
    """铺一层十字网格:每 step 像素一条 1px 线。

    ink 指定线的颜色(1=白、黑底用;0=黑、白底用)。
    """
    for x in range(x0, x1 + 1, step):
        d.line([(x, y0), (x, y1)], fill=ink)
    for y in range(y0, y1 + 1, step):
        d.line([(x0, y), (x1, y)], fill=ink)


def draw_dot_grid(d, x0, y0, x1, y1, step):
    """只点网格的【交叉点】(2x2 亮点),不画整条线。

    亮点总数是整线版的 1/step,对主体的干扰小得多,
    但还能让人看出"这里是格子纸"。
    """
    for x in range(x0, x1 + 1, step):
        for y in range(y0, y1 + 1, step):
            d.rectangle([x, y, x + 1, y + 1], fill=1)


def draw_clock(d, text, cx, base_y, scale, gap, ink=1, colon_gap_cells=4):
    """把 "12:34:56" 以 cx 为中心、底部对齐 base_y 画出来"""
    digit_w = GLYPH_W * scale
    colon_w = scale * 2 + colon_gap_cells      # 方点宽 + 左右各留白
    digit_pad = scale * 2                      # 相邻数字之间留 2 点

    # 先算总宽才能居中。间距只加在"数字挨着数字"的位置,
    # 冒号两侧本来就留了白,不再加。
    total = 0
    for ch in text:
        total += colon_w if ch == ":" else digit_w
    for i in range(len(text) - 1):
        if text[i] != ":" and text[i + 1] != ":":
            total += digit_pad

    x = cx - total // 2
    top = base_y - GLYPH_H * scale

    for i, ch in enumerate(text):
        if ch == ":":
            draw_colon(d, x + colon_gap_cells // 2, top, scale, gap,
                       GLYPH_H, ink)
            x += colon_w
        else:
            draw_glyph(d, ch, x, top, scale, gap, ink)
            x += digit_w
            if i + 1 < len(text) and text[i + 1] != ":":
                x += digit_pad
    return total


def main():
    W = 400
    RH = 150
    # 时钟区真实高度:第一根分隔线(42)到第二根(166) = 124px
    AREA_TOP, AREA_BOT = 20, 144
    # (标注, scale, 笔画内缝, 底板: "black"/"white", 网格 step)
    rows = [
        ("0  BLACK bg, no grid                 (baseline)", 3, 0, "black", 0),
        ("1  WHITE bg, no grid",                            3, 0, "white", 0),
        ("2  WHITE bg + black grid step=3",                 3, 0, "white", 3),
        ("3  WHITE bg + grid 3 + gap=1 (stitch)",           3, 1, "white", 3),
        ("4  WHITE bg + grid 6 + gap=1",                    3, 1, "white", 6),
    ]

    img = Image.new("1", (W, RH * len(rows)), 0)
    d = ImageDraw.Draw(img)
    lab = ImageFont.truetype(os.path.join(FONTS, "consola.ttf"), 11)

    for i, row in enumerate(rows):
        name, scale, gap, bg, step = row
        y0 = i * RH
        if i:
            d.line([(0, y0), (W, y0)], fill=1)
        d.text((4, y0 + 3), name, font=lab, fill=1)

        top, bot = y0 + AREA_TOP, y0 + AREA_BOT
        ink = 1 if bg == "black" else 0     # 字色

        if bg == "white":
            # 底板铺白
            d.rectangle([0, top, W - 1, bot], fill=1)
        if step:
            # 网格用【对比色】:白底上画黑线。
            # 1px 细线在白底上看着像浅灰,和粗体数字拉开层次 ——
            # 这就是参考图能成立的关键(黑白屏上只有线宽能表达"深浅")
            draw_grid(d, 0, top, W - 1, bot, step, 1 - ink)

        bottom = top + (bot - top + GLYPH_H * scale) // 2
        draw_clock(d, "12:34:56", 200, bottom, scale, gap, ink)

    img.save(OUT)
    print(f"已保存 {OUT}  {img.size[0]}x{img.size[1]}")


def export_c_header():
    """把点阵导出成固件直接能用的 C 头文件。

    每个数字 8 宽 x 16 高,每行一个字节,MSB 在左 ——
    这样固件里只需"取字节的第几位"就能判断一个点画不画,
    不需要任何解码或结构体。
    """
    path = os.path.join(
        r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF"
        r"\11_U8G2_Test\components\ui_draw",
        "ui_clock_bits.h")

    lines = [
        "/*",
        " * 方块点阵时钟的数字字形 —— 由 tools/fontgen/clock_bitmap_font.py 生成,别手改。",
        " *",
        " * 8 宽 x 16 高,每行一个字节,MSB 在左。显示时整数字放大 scale 倍,",
        " * 每个点画成一个 scale x scale 的方块,方块之间留 1px 缝 —— 就是参考图",
        " * 里那种\"方块拼接\"的质感。",
        " *",
        " * 为什么用点阵而不是矢量字体:参考图的特征就是\"笔画由方块构成\",",
        " * 矢量字体放大后边缘是光滑直线,出不来这个感觉;而且点阵天 然等宽,",
        " * 秒跳时不会左右抖(原来要靠强制 adv_w 才不抖)。",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "#define CLK_BM_W   8",
        "#define CLK_BM_H   16",
        "",
        "static const uint8_t kClkDigit[10][CLK_BM_H] = {",
    ]

    order = "0123456789"
    for n, ch in enumerate(order):
        lines.append(f"    {{  /* {ch} */")
        row_hex = []
        for row in DIGITS[ch]:
            v = 0
            for c, p in enumerate(row):
                if p == "#":
                    v |= 0x80 >> c
            row_hex.append(f"0x{v:02X}")
        for i in range(0, GLYPH_H, 8):
            lines.append("        " + ", ".join(row_hex[i:i + 8]) + ",")
        lines.append("    },")

    lines.append("};")
    lines.append("")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"已写入 {path}  ({len(order)} 个字形)")


if __name__ == "__main__":
    main()
    export_c_header()
