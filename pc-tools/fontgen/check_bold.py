"""把时钟字体渲染成 ASCII 图,直观判断笔画粗细。

Computerfont 本身是"粗笔画像素字":设计尺寸只有 9~24px。
在 1bpp(无抗锯齿)下渲染到 28px 高时,笔画会显得很粗。
这个脚本把真实位图打出来,顺便统计笔画宽度占比。
"""
from PIL import Image, ImageDraw, ImageFont

FONT = r"L:\ESP32\Computerfont-1.ttf"
SIZE = 48                       # 与 lv_font_clock_48 一致


def render(text: str) -> list:
    f = ImageFont.truetype(FONT, SIZE)
    img = Image.new("L", (600, 80), 255)
    ImageDraw.Draw(img).text((10, 10), text, font=f, fill=0)

    # 1bpp 阈值:和 lv_font_conv 的 1bpp 输出一致(覆盖率 50%)
    px = img.load()
    grid = []
    for y in range(80):
        row = "".join("#" if px[x, y] < 128 else "." for x in range(600))
        if "#" in row:
            grid.append(row)
    return grid


def strokes(row: str) -> list:
    """统计一行里连续 '#' 的长度(即笔画宽度)。"""
    out, n = [], 0
    for ch in row:
        if ch == "#":
            n += 1
        elif n:
            out.append(n)
            n = 0
    if n:
        out.append(n)
    return out


for s in ("8", "0", "20:08:48"):
    print(f"\n===== {s!r} =====")
    for row in render(s)[:40]:
        print("  " + row.rstrip("."))

    # 笔画宽度统计
    widths = []
    for row in render(s):
        widths += strokes(row.strip("."))
    if widths:
        from collections import Counter
        c = Counter(widths)
        print(f"  -> 笔画宽度分布 {dict(sorted(c.items()))}")

print("\n参考:字高约 28px。笔画 3~4px 算正常,5px 以上就偏粗。")
