# -*- coding: utf-8 -*-
"""
时钟实景对比 —— 按屏幕真实几何(400x300、上下分隔线、日期行)摆一遍。

和 clock_styles.py 的区别:
    那个只比字体;这个把时钟放回它真正的位置和留白里。
    字体再好看,塞进 124px 高的格子也可能挤 —— 这张图才看得出。

关键几何(抄自 user_app.cpp):
    分隔线 #1  y=42
    时钟基线    y=112   (CLK_BASE)
    日期基线    y=144
    分隔线 #2  y=166
    时钟横占    x=94..305 (居中)

输出: tools/fontgen/_clock_page.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

W, ROW_H = 400, 130          # 每行 = 分隔线到分隔线之间的那 124px + 余量
FONTS = r"C:\Windows\Fonts"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "_clock_page.png")

# 行内的相对坐标(把真实的 42..166 平移到行的 0..124)
SEP_TOP = 2
CLK_BASE = 112 - 42 + SEP_TOP      # 72
DATE_BASE = 144 - 42 + SEP_TOP     # 104
SEP_BOT = 166 - 42 + SEP_TOP       # 126


def ft(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


def frame(d, y0, label, lab_font):
    """画这一行的上下分隔线 + 左上角标注"""
    d.line([(2, y0 + SEP_TOP), (W - 3, y0 + SEP_TOP)], fill=1)
    d.line([(2, y0 + SEP_BOT), (W - 3, y0 + SEP_BOT)], fill=1)
    d.text((4, y0 + SEP_TOP + 2), label, font=lab_font, fill=1)


def date_row(d, y0, font_name):
    """日期/星期行,给个参照物(真实界面里它在时钟下面)"""
    d.text((200, y0 + DATE_BASE), "2026年9月30日  星期三",
           font=ft(font_name, 20), fill=1, anchor="ms")


# ---- 各方案 ----

def now(d, y0):
    """当前:全字同大,simhei 48"""
    d.text((200, y0 + CLK_BASE), "12:34:56",
           font=ft("simhei.ttf", 48), fill=1, anchor="ms")
    date_row(d, y0, "msyh.ttc")


def split_console(d, y0):
    """Consolas Bold:时分 52 + 秒 22 贴在右下"""
    big, sm = ft("consolab.ttf", 52), ft("consolab.ttf", 22)
    t1, t2 = "12:34", "56"
    w1 = d.textlength(t1, font=big)
    w2 = d.textlength(t2, font=sm)
    x = (W - (w1 + 10 + w2)) / 2
    d.text((x, y0 + CLK_BASE), t1, font=big, fill=1, anchor="ls")
    d.text((x + w1 + 10, y0 + CLK_BASE), t2, font=sm, fill=1, anchor="ls")
    date_row(d, y0, "msyh.ttc")


def split_bahn(d, y0):
    """Bahnschrift:时分 54 + 秒 22"""
    big, sm = ft("bahnschrift.ttf", 54), ft("bahnschrift.ttf", 22)
    t1, t2 = "12:34", "56"
    w1 = d.textlength(t1, font=big)
    w2 = d.textlength(t2, font=sm)
    x = (W - (w1 + 10 + w2)) / 2
    d.text((x, y0 + CLK_BASE), t1, font=big, fill=1, anchor="ls")
    d.text((x + w1 + 10, y0 + CLK_BASE), t2, font=sm, fill=1, anchor="ls")
    date_row(d, y0, "msyh.ttc")


def split_segoe(d, y0):
    """Segoe UI Semibold:时分 52 + 秒 22"""
    big, sm = ft("seguisb.ttf", 52), ft("seguisb.ttf", 22)
    t1, t2 = "12:34", "56"
    w1 = d.textlength(t1, font=big)
    w2 = d.textlength(t2, font=sm)
    x = (W - (w1 + 10 + w2)) / 2
    d.text((x, y0 + CLK_BASE), t1, font=big, fill=1, anchor="ls")
    d.text((x + w1 + 10, y0 + CLK_BASE), t2, font=sm, fill=1, anchor="ls")
    date_row(d, y0, "msyh.ttc")


ROWS = [
    ("current  simhei 48",            now),
    ("A  Consolas Bold 52 + SS 22",   split_console),
    ("B  Bahnschrift 54 + SS 22",     split_bahn),
    ("C  Segoe UI Semibold 52 + SS22", split_segoe),
]


def main():
    img = Image.new("1", (W, ROW_H * len(ROWS)), 0)
    d = ImageDraw.Draw(img)
    lab = ft("consola.ttf", 11)

    for i, (name, fn) in enumerate(ROWS):
        y0 = i * ROW_H
        frame(d, y0, name, lab)
        fn(d, y0)

    img.save(OUT)
    print(f"已保存 {OUT}  {img.size[0]}x{img.size[1]}")


if __name__ == "__main__":
    main()
