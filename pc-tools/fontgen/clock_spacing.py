# -*- coding: utf-8 -*-
"""
时钟字距对比 —— HH / MM / SS 三组之间拉开多少。

【为什么挤】
"12:34:56" 的排布是:数字 - 间距 - 数字 - 冒号槽 - 数字 ...
组内两个数字之间是 CLK_PAD(6px),
而冒号槽 CLK_COLONW 只有 12px,方点自己就 6px 宽,
居中之后数字离方点只剩 3px —— 比组内的 6px 还小!
等于"组内松、组间更紧",看着当然挤。

所以要让三组分开,不是把整串拉长,而是把【冒号槽】加大:
方点 6px + 左右各留 12px = 30px,数字到方点的空档 12px,
是组内 6px 的两倍 —— 组和组的边界一眼就能看出来。

输出: tools/fontgen/_clock_spacing.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from clock_bitmap_v3 import DIG_8x16, draw_grid, draw_glyph   # noqa: E402

OUT = os.path.join(HERE, "_clock_spacing.png")
FONT_UI = os.path.join(r"C:\Windows\Fonts", "msyh.ttc")
W, H = 400, 330
BG, INK = 255, 0

SCALE, GRID, PAD = 3, 3, 6          # 数字放大 3 倍,网格 3px,组内间距 6px
BGX0, BGX1, BGY0, BGY1 = 30, 370, 44, 122


def draw_colon(d, x, y, colonw):
    s = SCALE * 2
    x0 = x + (colonw - s) // 2
    d.rectangle([x0, y + SCALE * 3, x0 + s - 1, y + SCALE * 5 - 1], fill=INK)
    d.rectangle([x0, y + SCALE * 11, x0 + s - 1, y + SCALE * 13 - 1], fill=INK)


def draw_clock(d, text, x, y, colonw):
    gw = len(DIG_8x16["0"][0]) * SCALE
    for i, ch in enumerate(text):
        if ch == ":":
            draw_colon(d, x, y, colonw)
            x += colonw
        else:
            draw_glyph(d, DIG_8x16[ch], x, y, SCALE)
            x += gw
            if i + 1 < len(text) and text[i + 1] != ":":
                x += PAD


def main():
    img = Image.new("L", (W, H), BG)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT_UI, 15)

    y = 8
    for colonw in (12, 24, 30, 36):
        total = 6 * 24 + 2 * colonw + 5 * PAD
        x0 = ((W - total) // 2 // GRID) * GRID
        note = "  <- 现在这版(挤)" if colonw == 12 else ""
        d.text((10, y), f"冒号槽 {colonw}px   数字到方点 {(colonw-6)//2}px"
                        f"   整串宽 {total}{note}", font=f, fill=INK)
        y += 20

        d.rectangle([BGX0, y, BGX1, y + 77], fill=BG)
        draw_grid(d, BGX0, y, BGX1, y + 77, GRID)
        gy = y + (77 - 48) // 2
        draw_clock(d, "12:34:56", x0, gy, colonw)
        d.rectangle([BGX0, y, BGX1, y + 77], outline=INK)

        y += 84

    img.save(OUT)
    print(f"已保存 {OUT}  {img.size[0]}x{img.size[1]}")


if __name__ == "__main__":
    main()
