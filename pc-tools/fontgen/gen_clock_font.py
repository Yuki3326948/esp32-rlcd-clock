# -*- coding: utf-8 -*-
"""
生成等宽时钟数字字体(48px / 1bpp),并验证数字确实等宽。

为什么需要:
  Montserrat 是比例字体,数字宽度不等,时钟用居中标签时会左右抖动。
  这里只用 simhei 的 ASCII 数字,并验证 adv_w 一致。
"""
import os
import re
import subprocess

PROJ = r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test"
OUT_DIR = os.path.join(PROJ, "components", "ui_fonts")
TTF = r"C:\Windows\Fonts\simhei.ttf"
NODE = r"L:\ESP32\tools\fontgen\node_modules\lv_font_conv\lv_font_conv.js"

CHARS = "0123456789:-. APM"      # 数字/冒号/短横(占位符 --:--:-- 要用)/点/空格/AM/PM
OUT = os.path.join(OUT_DIR, "lv_font_clock_48.c")

cmd = [
    "node", NODE,
    "--font", TTF,
    "--symbols", CHARS,
    "--size", "48",
    "--bpp", "1",
    "--format", "lvgl",
    "--no-compress",
    "--lv-include", "lvgl.h",
    "--lv-font-name", "lv_font_clock_48",
    "-o", OUT,
]
print("生成 lv_font_clock_48 ...")
subprocess.run(cmd, check=True)
print(f"  -> {OUT}  ({os.path.getsize(OUT) // 1024} KB)")

# ---------------- 验证等宽性 ----------------
src = open(OUT, encoding="utf-8").read()
advs = [int(m) for m in re.findall(r"\.adv_w = (\d+)", src)]

# glyph_dsc 按码点升序排列,去掉开头的保留项(adv_w = 0)
while advs and advs[0] == 0:
    advs.pop(0)

ordered = sorted(CHARS)          # ' ' '-' '0'..'9' ':' 'A' 'M' 'P'
print("\n字形顺序:", ordered)
print("adv_w   :", advs)

if len(advs) >= len(ordered):
    widths = {c: advs[i] for i, c in enumerate(ordered)}
    digit_w = {widths[c] for c in "0123456789" if c in widths}
    print("\n数字宽度:", sorted(digit_w))
    print("结论:", "数字等宽 ✓" if len(digit_w) == 1 else "数字不等宽 ✗")
else:
    print("adv_w 数量不足,无法验证")
