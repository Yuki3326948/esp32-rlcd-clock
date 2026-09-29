# -*- coding: utf-8 -*-
"""
为 LVGL 生成中文点阵字体(1bpp,完美匹配单色 RLCD)

- 源字体: Windows 黑体 simhei.ttf
- 字符集: ASCII + GB2312 一级常用汉字(3755 个)
- 位深:   1bpp(黑白二值),单色屏无需抗锯齿,体积最小、笔画最锐利
"""
import os
import subprocess

PROJ = r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test"
OUT_DIR = os.path.join(PROJ, "components", "ui_fonts")
TTF = r"C:\Windows\Fonts\simhei.ttf"
NODE = r"L:\ESP32\tools\fontgen\node_modules\lv_font_conv\lv_font_conv.js"

os.makedirs(OUT_DIR, exist_ok=True)

# ---------- 构建字符集:GB2312 一级汉字(0xB0A1 - 0xD7F9) ----------
chars = []
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.append(bytes([hi, lo]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
symbols = "".join(dict.fromkeys(chars))
print(f"GB2312 一级汉字: {len(symbols)} 个")


def gen(size: int, name: str) -> None:
    out = os.path.join(OUT_DIR, f"{name}.c")
    cmd = [
        "node", NODE,
        "--font", TTF,
        "-r", "0x20-0x7F",          # ASCII
        "--symbols", symbols,        # 常用汉字
        "--size", str(size),
        "--bpp", "1",                # 单色
        "--format", "lvgl",
        "--no-compress",
        "--lv-include", "lvgl.h",
        "--lv-font-name", name,
        "-o", out,
    ]
    print(f"生成 {name} ({size}px, 1bpp) ...")
    subprocess.run(cmd, check=True)
    print(f"  -> {out}  ({os.path.getsize(out) // 1024} KB)")


gen(20, "lv_font_cjk_20")   # 日期 / 标签
gen(16, "lv_font_cjk_16")   # 小标签
print("完成")
