"""直接解码 lv_font_conv 生成的 .c,看设备上实际画出来的是什么。

这是唯一的"地面真相" —— 前面用 PIL 渲染只是推测,
这里解的是真正进固件的那份点阵数据。
"""
import os
import re
import subprocess

FONT = r"L:\ESP32\Computerfont-1.ttf"
NODE = r"L:\ESP32\tools\fontgen\node_modules\lv_font_conv\lv_font_conv.js"
TMP = r"L:\ESP32\tools\fontgen\_probe"
CHARS = "0123456789:"

DSC = re.compile(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
    r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}")


def gen(size: int) -> str:
    os.makedirs(TMP, exist_ok=True)
    out = os.path.join(TMP, f"probe_{size}.c")
    subprocess.run(
        ["node", NODE, "--font", FONT, "--symbols", CHARS,
         "--size", str(size), "--bpp", "1", "--format", "lvgl",
         "--no-compress", "--lv-include", "lvgl.h", "-o", out],
        check=True, stdout=subprocess.DEVNULL)
    return out


def load(path: str):
    src = open(path, encoding="utf-8").read()
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", src, re.S)
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)   # 去掉注释里的数字
    data = []
    for v in re.findall(r"0[xX][0-9a-fA-F]+|\d+", body):
        data.append(int(v, 16) if v[:2].lower() == "0x" else int(v, 10))
    glyphs = {}
    # dsc 顺序 = 码点顺序(0x30..0x3A),第 0 项是保留位
    for i, (bmi, adv, bw, bh, ox, oy) in enumerate(DSC.findall(src)):
        if 0 < i <= len(CHARS):
            glyphs[CHARS[i - 1]] = (int(bmi) * 8, int(adv), int(bw),
                                    int(bh), int(ox), int(oy))
    return data, glyphs


def art(data, bmi, bw, bh, bits=True):
    """按"连续比特流"解出点阵(1bpp)。"""
    rows = []
    for y in range(bh):
        row = ""
        for x in range(bw):
            i = bmi + y * bw + x
            byte = data[i >> 3] if bits else data[i]
            bit = (byte >> (7 - (i & 7))) & 1 if bits else (byte & 1)
            row += "#" if bit else "."
        rows.append(row)
    return rows


for size in (24, 32, 40):
    path = gen(size)
    data, glyphs = load(path)
    print(f"\n########## lv_font_conv --size {size} ##########")
    for ch in "081":
        if ch not in glyphs:
            print(f"  '{ch}' 不在字体里")
            continue
        bmi, adv, bw, bh, ox, oy = glyphs[ch]
        print(f"\n  '{ch}'  adv={adv}/16={adv / 16:.1f}px  box={bw}x{bh}"
              f"  ofs=({ox},{oy})  bitmap_index={bmi}")
        for r in art(data, bmi, bw, bh):
            print("     " + r)
