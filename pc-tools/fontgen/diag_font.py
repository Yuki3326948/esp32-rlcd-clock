# -*- coding: utf-8 -*-
"""诊断:Computerfont 的设计尺寸 + 生成的 LVGL 字体实际参数"""
import os
import re
import struct

TTF = r"L:\ESP32\Computerfont-1.ttf"
PROJ = r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test"
FONTS = os.path.join(PROJ, "components", "ui_fonts")

# ---------------- 1. TTF 里记录的"点阵设计尺寸" ----------------
d = open(TTF, "rb").read()
n = struct.unpack(">H", d[4:6])[0]
tables = {}
for i in range(n):
    off = 12 + i * 16
    tag = d[off:off + 4].decode("latin1")
    toff, tlen = struct.unpack(">II", d[off + 8:off + 16])
    tables[tag] = (toff, tlen)

print("=== Computerfont-1.ttf ===")
if "hdmx" in tables:
    ho, _ = tables["hdmx"]
    num = struct.unpack(">h", d[ho + 2:ho + 4])[0]
    rec = struct.unpack(">i", d[ho + 4:ho + 8])[0]
    ppems = sorted({d[ho + 8 + i * rec] for i in range(num)})
    print(f"hdmx 记录的点阵尺寸(px): {ppems}")
    print(f"  -> 这是位图字体,最佳渲染尺寸是这些值的整数倍")
else:
    print("无 hdmx 表")

if "head" in tables:
    h = tables["head"][0]
    print(f"unitsPerEm = {struct.unpack('>H', d[h+18:h+20])[0]}, "
          f"macStyle = {struct.unpack('>H', d[h+44:h+46])[0]} (bit0=粗体)")

# ---------------- 2. 生成的 LVGL 字体 ----------------
GLYPH_RE = re.compile(
    r"\.adv_w = (-?\d+), \.box_w = (-?\d+), \.box_h = (-?\d+),"
    r" \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)")


def inspect(name: str, sample: str) -> None:
    p = os.path.join(FONTS, name + ".c")
    if not os.path.exists(p):
        print(f"\n{name}: 文件不存在")
        return
    src = open(p, encoding="utf-8").read()
    vals = [tuple(int(x) for x in m.groups()) for m in GLYPH_RE.finditer(src)]
    lh = re.search(r"\.line_height = (\d+)", src)
    bl = re.search(r"\.base_line = (-?\d+)", src)
    print(f"\n=== {name} ({os.path.getsize(p)//1024} KB) ===")
    print(f"字形数 = {len(vals)} (含保留项), line_height = {lh.group(1) if lh else '?'}"
          f", base_line = {bl.group(1) if bl else '?'}")
    body = vals[1:] if vals and vals[0][0] == 0 else vals
    if body:
        print(f"adv_w 集合 = {sorted({v[0] for v in body})}")
        print(f"box_w 集合 = {sorted({v[1] for v in body})}")
        print(f"box_h 集合 = {sorted({v[2] for v in body})}")
    # 按"0x20 起顺序"换算示例文本宽度
    if len(body) >= 95:
        adv = {chr(0x20 + i): body[i][0] / 16 for i in range(95)}
        w = sum(adv.get(c, 0) for c in sample)
        print(f"示例 {sample!r} 宽度 ≈ {w:.0f}px  (屏幕宽 400px)")


inspect("lv_font_clock_48", "19:58:13")
inspect("lv_font_clock_48", "07:26:00 PM")
inspect("lv_font_pix_28", "31.5\u00b0C")
