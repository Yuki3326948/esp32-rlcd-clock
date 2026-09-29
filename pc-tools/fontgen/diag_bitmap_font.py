"""判断 Computerfont 是不是位图字体,以及它原生渲染在哪些字号下是锐利的。

如果字体里带 EBDT/EBLC/CBDT/sbix 这些表,说明它是"位图字体":
只在内置的那几个字号(strike)下才锐利,放大缩小都会糊/被抻粗。
这能解释屏幕上"字体太粗、数字认不出来"的现象。
"""
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

FONT = r"L:\ESP32\Computerfont-1.ttf"

f = TTFont(FONT)
print("字体包含的表:", sorted(t for t in f.keys() if t != "GlyphOrder"))
for t in ("EBDT", "EBLC", "CBDT", "CBLC", "sbix", "hdmx", "gasp", "glyf", "CFF "):
    print(f"  {t:6s}: {'有' if t in f else '无'}")

if "hdmx" in f:
    print("hdmx 覆盖的字号:", sorted(f["hdmx"].hdmx.keys()))


def shape(ch: str, size: int) -> list:
    """只裁出字形本身的点阵,便于对比不同字号。"""
    font = ImageFont.truetype(FONT, size)
    img = Image.new("L", (200, 200), 255)
    ImageDraw.Draw(img).text((20, 20), ch, font=font, fill=0)
    px = img.load()
    rows = []
    for y in range(200):
        row = "".join("#" if px[x, y] < 128 else "." for x in range(200))
        if "#" in row:
            rows.append(row)
    if not rows:
        return []
    left = min(r.index("#") for r in rows)
    right = max(r.rindex("#") for r in rows)
    return [r[left:right + 1] for r in rows]


for size in (12, 16, 20, 24, 28, 32, 40, 48):
    rows = shape("8", size)
    print(f"\n=== 字号 {size}: 字形 {len(rows[0]) if rows else 0}px 宽 "
          f"{len(rows)}px 高 ===")
    for r in rows:
        print("  " + r)
