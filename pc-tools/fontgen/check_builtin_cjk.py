"""检查界面用到的汉字,LVGL 内置的 source_han_sans_sc_16_cjk 是否都认识。

自定义中文字体有 3755 字,内置的只有 ~1300 字。如果去自定义字体,
必须先确认界面上的字一个都不缺,否则会显示成方块。
"""
import re

FONT_C = (r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF"
          r"\11_U8G2_Test\managed_components\lvgl__lvgl\src\font"
          r"\lv_font_source_han_sans_sc_16_cjk.c")
APP_C = (r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF"
         r"\11_U8G2_Test\components\user_app\user_app.cpp")

# ---------- 1. 内置字体覆盖了哪些码点 ----------
src = open(FONT_C, encoding="utf-8", errors="ignore").read()

# 取出所有 unicode_list_X
lists = {}
for m in re.finditer(r"static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};",
                     src, re.S):
    lists[m.group(1)] = [int(v, 16) for v in
                         re.findall(r"0x([0-9a-fA-F]+)", m.group(2))]
print("unicode_list:", {k: len(v) for k, v in lists.items()})

# 解析 cmaps,按正确规则展开
# 注意:字段之间跨行,所以用 \s* 而不是写死空格
covered = set()
for m in re.finditer(r"\.range_start = (\d+), \.range_length = (\d+),"
                     r"\s*\.glyph_id_start = (\d+),\s*"
                     r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+),"
                     r" \.list_length = (\d+), \.type = ([\w_]+)",
                     src, re.S):
    rs, rl, gs, ulist, ofslist, llen, typ = m.groups()
    rs, rl, llen = int(rs), int(rl), int(llen)

    if ulist == "NULL":
        covered |= set(range(rs, rs + rl))          # FORMAT0_TINY: 绝对区间
    else:
        # SPARSE:列表里是相对 range_start 的偏移,不是绝对码点!
        covered |= {rs + v for v in lists.get(ulist, [])}

print(f"内置字体覆盖 {len(covered)} 个码点")
cjk = sorted(c for c in covered if 0x4E00 <= c <= 0x9FFF)
print(f"其中汉字 {len(cjk)} 个,例: {''.join(chr(c) for c in cjk[:20])}")

# ---------- 2. 界面里用到的字符 ----------
app = open(APP_C, encoding="utf-8", errors="ignore").read()
used = set()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', app):
    used |= set(lit)

# 只关心非 ASCII(中文/符号)
nonascii = {c for c in used if ord(c) > 127}
ascii_used = {c for c in used if 32 <= ord(c) < 127}

print(f"界面用到的非 ASCII 字符 {len(nonascii)} 个: {''.join(sorted(nonascii))}")

missing = sorted(c for c in nonascii if ord(c) not in covered)
if missing:
    print(f"\n!! 内置字体缺少 {len(missing)} 个字:")
    for c in missing:
        print(f"    '{c}'  U+{ord(c):04X}")
else:
    print("\nOK:内置字体覆盖了界面用到的全部汉字")

miss_ascii = sorted(c for c in ascii_used if ord(c) not in covered)
print(f"\nASCII 缺失: {miss_ascii if miss_ascii else '无'}")
