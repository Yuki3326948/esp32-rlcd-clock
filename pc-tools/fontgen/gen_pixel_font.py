# -*- coding: utf-8 -*-
"""
用 Computerfont-1.ttf(像素风字体)生成 LVGL 1bpp 点阵字体,并强制等宽。

两个必须做的处理:
 1. 【限定字符集】Computerfont 是比例字体,最宽的字母(M/W)会撑大步进。
    时钟只用固定几种字符,限定后步进才不会过宽。
 2. 【强制等宽】比例字体在居中标签里会导致整串左右抖动(每秒跳一下),
    所以统一 adv_w 并按差值重新居中 ofs_x。
 3. 【自动选字号】保证 12 小时制 "07:26:00 PM"(11 字符)也能放进 400px。

Computerfont 无中文,中文标签继续用 lv_font_cjk_*。
"""
import os
import re
import subprocess

PROJ = r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test"
OUT_DIR = os.path.join(PROJ, "components", "ui_fonts")
FONT = r"L:\ESP32\Computerfont-1.ttf"
NODE = r"L:\ESP32\tools\fontgen\node_modules\lv_font_conv\lv_font_conv.js"

EXTRA = "\u00b0\u00b1\u2022"                  # ° ± •
# 注意:不要把 '-' 放进时钟字符集!Computerfont 的 '-' 极宽(22px),
# 会把等宽步进撑到 31.7px,而字形只有 24px 高 —— 字被拉得又扁又散。
# 占位符改用 "00:00:00",不含短横。
CLOCK_CHARS = "0123456789: APM."              # 时钟可能出现的全部字符
CLOCK_MAX_CHARS = 11                          # "07:26:00 PM"

# Computerfont 的 'M' 比数字宽 30%。若拿它当等宽基准,整个时钟会被
# 撑得很散(31.7px 步进配 24px 字形)。这里取"除 M 之外"的最宽字符当
# 基准,M 保留原始宽度 —— 它只出现在末尾的 AM/PM,不影响 HH:MM:SS 对齐。
WIDE_RATIO = 11                               # 1/16px 单位 / ppem,用来把 M 挑出来


def base_adv(vals, size: int) -> int:
    """等宽基准(1/16 px):剔除异常宽的字符(如 M)。"""
    narrow = [v[0] for v in vals if v[0] < WIDE_RATIO * size]
    return max(narrow) if narrow else max(v[0] for v in vals)
SCREEN_W = 400
SAFE_W = 380                                  # 左右各留 10px

GLYPH_RE = re.compile(
    r"\.adv_w = (-?\d+), \.box_w = (-?\d+), \.box_h = (-?\d+),"
    r" \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)")


def gen(size: int, name: str, symbols: str = None) -> str:
    out = os.path.join(OUT_DIR, f"{name}.c")
    cmd = ["node", NODE, "--font", FONT]
    if symbols:
        cmd += ["--symbols", symbols]
    else:
        cmd += ["-r", "0x20-0x7E", "--symbols", EXTRA]
    cmd += ["--size", str(size), "--bpp", "1", "--format", "lvgl",
            "--no-compress", "--lv-include", "lvgl.h",
            "--lv-font-name", name, "-o", out]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return out


def glyphs(path: str):
    """返回 [(adv_w, box_w, box_h, ofs_x, ofs_y), ...](已去掉开头保留项)"""
    src = open(path, encoding="utf-8").read()
    ms = list(GLYPH_RE.finditer(src))
    vals = [tuple(int(x) for x in m.groups()) for m in ms]
    if vals and vals[0][0] == 0:
        vals = vals[1:]
    return src, ms, vals


def force_monospace(path: str, common: int, wide_min: int) -> int:
    src, ms, vals = glyphs(path)
    start = 1 if int(ms[0].group(1)) == 0 else 0

    out, last = [], 0
    for n, m in enumerate(ms):
        out.append(src[last:m.start()])
        adv, bw, bh, ox, oy = (int(x) for x in m.groups())
        if n >= start and adv < wide_min:        # 异常宽的字符保持原样
            ox += (common - adv) // 2            # 在新格子里居中
            adv = common
        out.append(f".adv_w = {adv}, .box_w = {bw}, .box_h = {bh}, "
                   f".ofs_x = {ox}, .ofs_y = {oy}")
        last = m.end()
    out.append(src[last:])
    open(path, "w", encoding="utf-8").write("".join(out))
    return common


# ---------------- 时钟字体:自动选一个能放下 11 字符的字号 ----------------
print("时钟字体(字符集: %r)" % CLOCK_CHARS)
clock_path = None
for size in (48, 44, 40, 36, 32):
    p = gen(size, "lv_font_clock_48", CLOCK_CHARS)
    _, _, vals = glyphs(p)
    common = base_adv(vals, size)
    need_px = CLOCK_MAX_CHARS * common / 16
    print(f"  {size}px -> 单字 {common / 16:5.1f}px, "
          f"{CLOCK_MAX_CHARS} 字符共 {need_px:5.0f}px "
          f"{'✓ 放得下' if need_px <= SAFE_W else '✗ 超出'}")
    if need_px <= SAFE_W:
        clock_path = p
        break

common = force_monospace(clock_path, common, WIDE_RATIO * size)
print(f"  => 选定 {size}px,等宽化后单字 {common / 16:.1f}px")

# 各字符原始宽度(便于排查)
_, _, vals = glyphs(gen(28, "lv_font_clock_48_dbg", CLOCK_CHARS))
print("\n  28px 下各字符原始 adv_w:")
for i, ch in enumerate(CLOCK_CHARS):
    print(f"    '{ch}' = {vals[i][0]:4d} ({vals[i][0] / 16:.1f}px)", end="")
    if (i + 1) % 4 == 0:
        print()
print()
os.remove(os.path.join(OUT_DIR, "lv_font_clock_48_dbg.c"))

# ---------------- 传感器数值 / SSID ----------------
for size, name in ((28, "lv_font_pix_28"), (16, "lv_font_pix_16")):
    p = gen(size, name)
    _, _, vals = glyphs(p)
    # 这两个字体字符集全 ASCII,统一用最宽字符做基准(1<<30 表示不剔除任何字符)
    c = force_monospace(p, max(v[0] for v in vals), 1 << 30)
    print(f"\n{name}: {size}px, 单字 {c / 16:.1f}px, {os.path.getsize(p) // 1024} KB")
print("\n完成")
