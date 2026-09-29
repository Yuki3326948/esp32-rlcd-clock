# -*- coding: utf-8 -*-
"""
时钟样式对比图 —— 改样式之前先在 PC 上"看一眼"。

为什么需要:
    板子烧一次固件才能看一次效果,而且还是 1bpp 二值化的。
    先把候选字体/排版按同样的字号渲染成 png,一比就淘汰掉大半,
    只把真正顺眼的那一两个烧进去试。

图上每行一个方案,都是黑底白字(和屏幕一致),最后统一做 1bpp 二值化。

输出: tools/fontgen/_clock_styles.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

W, RH = 400, 78                      # 一行一个方案
FONTS = r"C:\Windows\Fonts"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "_clock_styles.png")


def ft(name: str, size: int):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


# ---- 各方案。签名都是 (draw, y0),在 y0 起始的那一行里画 ----

def cur(d, y):
    """当前方案:整个 HH:MM:SS 同一个字号。"""
    d.text((200, y + 50), "12:34:56", font=ft("simhei.ttf", 48),
           fill=1, anchor="ms")


def hhmm_big(d, y):
    """时分大、秒小 —— 数字钟最经典的排版,一眼看出重点在时分。"""
    big, sm = ft("simhei.ttf", 56), ft("simhei.ttf", 24)
    t1, t2 = "12:34", "56"
    w1 = d.textlength(t1, font=big)
    w2 = d.textlength(t2, font=sm)
    x = (W - (w1 + 8 + w2)) / 2
    d.text((x, y + 50), t1, font=big, fill=1, anchor="ls")
    d.text((x + w1 + 8, y + 50), t2, font=sm, fill=1, anchor="ls")


def bahn(d, y):
    """Bahnschrift:工业/仪表那种窄长感。"""
    d.text((200, y + 50), "12:34:56", font=ft("bahnschrift.ttf", 50),
           fill=1, anchor="ms")


def consol_b(d, y):
    """Consolas Bold:等宽、方正,和代码字体一个调子。"""
    d.text((200, y + 50), "12:34:56", font=ft("consolab.ttf", 46),
           fill=1, anchor="ms")


def yahei_b(d, y):
    """微软雅黑 Bold:笔画圆润,和界面里的中文字体同源。"""
    d.text((200, y + 50), "12:34:56", font=ft("msyhbd.ttc", 46),
           fill=1, anchor="ms")


def arial_b(d, y):
    """Arial Bold:最"标准"的粗体数字,普适。"""
    d.text((200, y + 50), "12:34:56", font=ft("arialbd.ttf", 46),
           fill=1, anchor="ms")


def segoe_sb(d, y):
    """Segoe UI Semibold:系统 UI 字体,现代但不过分。"""
    d.text((200, y + 50), "12:34:56", font=ft("seguisb.ttf", 48),
           fill=1, anchor="ms")


ROWS = [
    ("1  current  simhei 48 (mono)", cur),
    ("2  HH:MM 56 + SS 24",          hhmm_big),
    ("3  Bahnschrift 50",            bahn),
    ("4  Consolas Bold 46",          consol_b),
    ("5  YaHei Bold 46",             yahei_b),
    ("6  Arial Bold 46",             arial_b),
    ("7  Segoe UI Semibold 48",      segoe_sb),
]


def main():
    img = Image.new("1", (W, RH * len(ROWS)), 0)   # 1bpp,黑底
    d = ImageDraw.Draw(img)
    lab = ft("consola.ttf", 12)

    for i, (name, fn) in enumerate(ROWS):
        y0 = i * RH
        if i:
            d.line([(0, y0), (W, y0)], fill=1)     # 行分隔线,方便指着说
        fn(d, y0)
        d.text((4, y0 + 4), name, font=lab, fill=1)

    img.save(OUT)
    print(f"已保存 {OUT}  {img.size[0]}x{img.size[1]}")


if __name__ == "__main__":
    main()
