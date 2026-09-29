# -*- coding: utf-8 -*-
"""纯 ASCII 输出:列出字体实际包含的字符与宽度(避免控制台编码问题)"""
import os
import re

PROJ = r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test"
FONTS = os.path.join(PROJ, "components", "ui_fonts")

DSC_RE = re.compile(
    r"\.adv_w = (-?\d+), \.box_w = (-?\d+), \.box_h = (-?\d+),"
    r" \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)")


def load(name):
    src = open(os.path.join(FONTS, name + ".c"), encoding="utf-8").read()
    dsc = [tuple(int(x) for x in m.groups()) for m in DSC_RE.finditer(src)]
    if dsc and dsc[0][0] == 0:
        dsc = dsc[1:]
    bm0 = src.index("glyph_bitmap[] = {")
    bm1 = src.index("};", bm0)
    cps = sorted(int(x, 16) for x in re.findall(
        r"/\*\s*U\+([0-9A-Fa-f]{4,5})", src[bm0:bm1]))
    lh = re.search(r"\.line_height = (\d+)", src)
    return dsc, cps, int(lh.group(1)) if lh else -1


for name in ("lv_font_clock_48", "lv_font_pix_28"):
    dsc, cps, lh = load(name)
    print("=" * 62)
    print(f"{name}:  {len(cps)} glyphs, line_height={lh}")
    print(f"codepoints: {' '.join('U+%04X' % c for c in cps)}")
    chars = "".join(chr(c) if 32 < c < 127 else "?" for c in cps)
    print(f"as chars  : {chars}")
    missing = [c for c in "0123456789:" if ord(c) not in cps]
    print(f"digits+colon missing: {[f'U+{ord(c):04X}' for c in missing] or 'NONE'}")

    # 步进是否一致
    advs = sorted({d[0] for d in dsc})
    heights = sorted({d[2] for d in dsc})
    print(f"adv_w set = {advs}  (px: {[round(a/16,1) for a in advs]})")
    print(f"box_h set = {heights}")

    # 示例文本宽度(按码点顺序与 dsc 一一对应)
    cmap = {}
    for c in cps:
        i = c - cps[0] if len(cps) == dsc.__len__() else None
    # 更稳妥:按 "码点排序 == dsc 顺序" 假设;先验证字符数吻合
    if len(cps) == len(dsc):
        m = dict(zip(cps, dsc))
        for sample in ("19:58:13", "07:26:00 PM", "31.5\u00b0C"):
            need = [ord(ch) for ch in sample]
            bad = [ch for ch in sample if ord(ch) not in m]
            w = sum(m[ord(ch)][0] for ch in sample if ord(ch) in m) / 16
            print(f"  {sample!r}: width={w:5.0f}px  missing={bad}")
    else:
        print(f"  !! glyph count mismatch: {len(cps)} vs {len(dsc)}")
