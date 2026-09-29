# -*- coding: utf-8 -*-
"""把生成的 LVGL 字体还原成字符画,直观看清楚字形长什么样"""
import os
import re

PROJ = r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test"
FONTS = os.path.join(PROJ, "components", "ui_fonts")

DSC_RE = re.compile(
    r"\.adv_w = (-?\d+), \.box_w = (-?\d+), \.box_h = (-?\d+),"
    r" \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)")


def load(path):
    src = open(path, encoding="utf-8").read()
    dsc = [tuple(int(x) for x in m.groups()) for m in DSC_RE.finditer(src)]
    if dsc and dsc[0][0] == 0:
        dsc = dsc[1:]

    bm0 = src.index("glyph_bitmap[] = {")
    bm1 = src.index("};", bm0)
    block = src[bm0:bm1]
    parts = re.split(r'/\*\s*U\+([0-9A-Fa-f]{4,5})', block)

    glyphs = {}
    order = []
    for i in range(1, len(parts) - 1, 2):
        cp = int(parts[i], 16)
        data = parts[i + 1]
        raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", data))
        glyphs[cp] = raw
        order.append(cp)
    return dsc, glyphs, sorted(glyphs)


def render(name, text):
    dsc, bm, cps = load(os.path.join(FONTS, name + ".c"))
    dmap = dict(zip(cps, dsc))          # 按码点升序对应 glyph_dsc

    # 计算画布
    total_adv = sum(dmap.get(ord(c), (0, 0, 0, 0, 0))[0] for c in text) / 16
    rows = max((dmap.get(ord(c), (0, 0, 0, 0, 0))[2] for c in text), default=0)
    H = int(rows) + 4
    W = int(total_adv) + 2
    canvas = [[" "] * W for _ in range(H)]

    x_px = 0.0
    for c in text:
        cp = ord(c)
        if cp not in dmap:
            x_px += 10
            continue
        adv, bw, bh, ox, oy = dmap[cp]
        raw = bm[cp]
        row_bytes = (bw + 7) // 8
        gx = x_px + ox / 16
        # oy 是字形顶部相对基线的偏移(向上为正),基线在 H-3
        base = H - 3
        gy = base - oy / 16
        for r in range(bh):
            for col in range(bw):
                byte = raw[r * row_bytes + col // 8] if r * row_bytes + col // 8 < len(raw) else 0
                if byte & (0x80 >> (col % 8)):
                    yy = int(gy + r)
                    xx = int(gx + col)
                    if 0 <= yy < H and 0 <= xx < W:
                        canvas[yy][xx] = "#"
        x_px += adv / 16

    print(f"\n=== {name}  文本 {text!r} ===")
    print(f"字形高度 box_h 最大 = {rows}, 总宽度 ≈ {total_adv:.0f}px")
    for row in canvas:
        print("|" + "".join(row) + "|")


render("lv_font_clock_48", "19:58:13")
render("lv_font_pix_28", "31.5\u00b0C")

# 列出各字符的 box 尺寸
dsc, bm, cps = load(os.path.join(FONTS, "lv_font_clock_48.c"))
dmap = dict(zip(cps, dsc))
print("\n=== lv_font_clock_48 各字符 ===")
for c in "0123456789: APM-.":
    cp = ord(c)
    if cp in dmap:
        a, w, h, ox, oy = dmap[cp]
        print(f"  {c!r} U+{cp:04X}: adv={a/16:5.1f}px box={w}x{h} ofs_x={ox/16:5.1f} ofs_y={oy/16:5.1f}")
    else:
        print(f"  {c!r} U+{cp:04X}: 【缺失】")
