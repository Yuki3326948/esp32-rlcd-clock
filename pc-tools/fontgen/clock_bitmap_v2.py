# -*- coding: utf-8 -*-
"""
方块点阵时钟 v2 —— 数字的"像素" = 2x2 个背景格。

和 v1 的区别:
    v1: 每个设计点 = 1 个网格格 = 3x3 屏幕像素(所以字形要大,8x16)
    v2: 每个设计点 = 2x2 个网格格 = 6x6 屏幕像素(字形缩到 4x8)

    为什么这样更好:数字的方块和背景网格是同一套节拍 ——
    每个数字像素正好盖住 2x2 个格子,整块看起来严丝合缝,而不是
    "数字自己一套密度、网格另一套密度"。

代价:
    4x8 只有 32 个点,比 8x16 的 128 个点少 4 倍,字形必然更简化。
    4 宽也画不出"4"的斜线那种细节 —— 只能靠暗示。

输出: tools/fontgen/_clock_bitmap48.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

FONTS = r"C:\Windows\Fonts"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "_clock_bitmap48.png")

# ----------------------------------------------------------------------
# 4 宽 x 8 高。笔画只有 1 点,放大 6 倍后就是 6x6 的方块链。
# ----------------------------------------------------------------------
DIGITS = {
    "0": [".##.",
          "#..#",
          "#..#",
          "#..#",
          "#..#",
          "#..#",
          "#..#",
          ".##."],

    "1": ["..#.",
          ".##.",
          "..#.",
          "..#.",
          "..#.",
          "..#.",
          "..#.",
          ".###"],

    "2": [".##.",
          "#..#",
          "...#",
          "..#.",
          ".#..",
          "#...",
          "#...",
          "####"],

    "3": [".##.",
          "#..#",
          "...#",
          "..##",
          "...#",
          "...#",
          "#..#",
          ".##."],

    "4": ["..##",
          ".#.#",
          "#..#",
          "#..#",
          "####",
          "...#",
          "...#",
          "...#"],

    "5": ["####",
          "#...",
          "#...",
          ".###",
          "...#",
          "...#",
          "#..#",
          ".##."],

    "6": [".##.",
          "#..#",
          "#...",
          "#...",
          ".###",
          "#..#",
          "#..#",
          ".##."],

    "7": ["####",
          "...#",
          "...#",
          "..#.",
          "..#.",
          ".#..",
          ".#..",
          ".#.."],

    "8": [".##.",
          "#..#",
          "#..#",
          ".##.",
          "#..#",
          "#..#",
          "#..#",
          ".##."],

    "9": [".##.",
          "#..#",
          "#..#",
          "####",
          "...#",
          "...#",
          "#..#",
          ".##."],
}

GLYPH_W, GLYPH_H = 4, 8

SCALE = 6            # 每个设计点 -> 6x6 像素 = 2x2 个（3px 的）网格格
GRID = 3             # 背景网格步距
GAP = 0              # 方块之间不留缝:1bpp 上留缝=掺白=发灰


def draw_grid(d, x0, y0, x1, y1, step, ink):
    for x in range(x0, x1 + 1, step):
        d.line([(x, y0), (x, y1)], fill=ink)
    for y in range(y0, y1 + 1, step):
        d.line([(x0, y), (x1, y)], fill=ink)


def draw_glyph(d, ch, x, y, ink):
    for r, row in enumerate(DIGITS[ch]):
        for c, v in enumerate(row):
            if v == "#":
                d.rectangle([x + c * SCALE, y + r * SCALE,
                             x + c * SCALE + SCALE - 1 - GAP,
                             y + r * SCALE + SCALE - 1 - GAP], fill=ink)


def draw_colon(d, x, y, ink):
    """两个方点,边长 = 2 个设计点。和数字同一套节拍"""
    s = SCALE * 2
    for row in (2.5, 4.5):
        py = int(y + row * SCALE)
        d.rectangle([x, py, x + s - 1 - GAP, py + s - 1 - GAP], fill=ink)


def draw_clock(d, text, cx, base_y, ink):
    dw = GLYPH_W * SCALE
    cw = SCALE * 4                 # 冒号占位(2 点宽 + 左右各 1 点)
    pad = SCALE                    # 数字之间留 1 个设计点

    total = sum(cw if ch == ":" else dw for ch in text)
    for i in range(len(text) - 1):
        if text[i] != ":" and text[i + 1] != ":":
            total += pad

    x = cx - total // 2
    top = base_y - GLYPH_H * SCALE
    for i, ch in enumerate(text):
        if ch == ":":
            draw_colon(d, x + SCALE, top, ink)
            x += cw
        else:
            draw_glyph(d, ch, x, top, ink)
            x += dw
            if i + 1 < len(text) and text[i + 1] != ":":
                x += pad
    return total, top


def main():
    W, RH = 400, 150
    AREA_TOP, AREA_BOT = 20, 144         # 模拟时钟区(白底范围)

    rows = [
        ("v1  8x16 x3  (每个点=1格)", 3, 8, 16),
        ("v2  4x8  x6  (每个点=2x2格)", 6, 4, 8),
    ]

    img = Image.new("1", (W, RH * len(rows)), 0)
    d = ImageDraw.Draw(img)
    lab = ImageFont.truetype(os.path.join(FONTS, "consola.ttf"), 11)

    global GLYPH_W, GLYPH_H, SCALE
    for i, (name, scale, gw, gh) in enumerate(rows):
        SCALE = scale
        y0 = i * RH
        if i:
            d.line([(0, y0), (W, y0)], fill=1)
        d.text((4, y0 + 3), name, font=lab, fill=1)

        top, bot = y0 + AREA_TOP, y0 + AREA_BOT
        # 白底 + 黑网格(1px 线在白底上像浅灰,和粗黑数字拉开层次)
        d.rectangle([0, top, W - 1, bot], fill=1)
        draw_grid(d, 0, top, W - 1, bot, GRID, 0)

        if i == 0:
            # v1 用旧的 8x16 字形 —— 这里不重复贴一遍,直接跳过
            d.text((200, (top + bot) // 2), "(v1 见上一版脚本)", fill=0,
                   anchor="mm", font=lab)
            continue

        bottom = top + (bot - top + GLYPH_H * SCALE) // 2
        draw_clock(d, "12:34:56", 200, bottom, 0)

    img.save(OUT)
    print(f"已保存 {OUT}  {img.size[0]}x{img.size[1]}")


def export_c_header():
    """导出成固件用的 C 头文件(4x8,每行一个字节,MSB 在左)"""
    path = os.path.join(
        r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF"
        r"\11_U8G2_Test\components\ui_draw",
        "ui_clock_bits.h")

    L = [
        "/*",
        " * 方块点阵时钟的数字字形 —— 由 tools/fontgen/clock_bitmap_v2.py 生成,别手改。",
        " *",
        " * 4 宽 x 8 高,每行一个字节(只用高 4 位)。显示时整数字放大 6 倍 =>",
        " * 每个设计点 = 6x6 屏幕像素 = 【2x2 个背景网格格】。",
        " *",
        " * 为什么是 2x2 格:数字的方块和背景网格共用同一套节拍,整块看起来",
        " * 严丝合缝,而不是\"数字一种密度、网格另一种密度\"。",
        " * 另外点阵天然等宽,秒跳时不会左右抖(以前要靠强制 adv_w 才不抖)。",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "#define CLK_BM_W   4",
        "#define CLK_BM_H   8",
        "",
        "static const uint8_t kClkDigit[10][CLK_BM_H] = {",
    ]

    for ch in "0123456789":
        L.append(f"    {{  /* {ch} */")
        vals = []
        for row in DIGITS[ch]:
            v = 0
            for c, p in enumerate(row):
                if p == "#":
                    v |= 0x80 >> c
            vals.append(f"0x{v:02X}")
        L.append("        " + ", ".join(vals) + ",")
        L.append("    },")

    L += ["};", ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    print(f"已写入 {path}")


if __name__ == "__main__":
    main()
