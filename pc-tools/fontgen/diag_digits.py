"""看 Computerfont 在"原生字号"和"放大字号"下数字分别长什么样。

hdmx 只覆盖 9~24px —— 说明这个字体是手工为 9~24px 调过网格对齐的。
超出这个范围(比如 40px)时,轮廓没有被正确吸附,就会变成一团。
"""
from PIL import Image, ImageDraw, ImageFont

FONT = r"L:\ESP32\Computerfont-1.ttf"
DIGITS = "0123456789"


def draw(text: str, size: int):
    font = ImageFont.truetype(FONT, size)
    img = Image.new("L", (900, 120), 255)
    ImageDraw.Draw(img).text((5, 5), text, font=font, fill=0)
    px = img.load()
    rows = []
    for y in range(120):
        row = "".join("#" if px[x, y] < 128 else "." for x in range(900))
        if "#" in row:
            rows.append(row)
    if not rows:
        return
    left = min(r.index("#") for r in rows)
    right = max(r.rindex("#") for r in rows)
    return [r[left:right + 1] for r in rows]


for size in (16, 24, 40):
    rows = draw(DIGITS, size)
    print(f"\n########## 字号 {size}  整体 {len(rows[0])}px 宽 {len(rows)}px 高 ##########")
    for r in rows:
        print("  " + r)
