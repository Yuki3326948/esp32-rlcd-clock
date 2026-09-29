"""把 TTF 点阵化成界面用的 1bpp 字体(自定义格式,不再依赖 LVGL)。

和上一版(gen_ui_font_lvgl_old.py)的区别:
  上一版用 Node 的 lv_font_conv 输出 LVGL 的字体结构。现在把 LVGL 整个拆掉了,
  所以自己用 Pillow 光栅化,输出一份最简单的表:

      UiGlyph { code, adv, w, h, ox, oy, off }
      UiFont  { line_h, base, cnt, glyphs, bits }

为什么要自己写而不是继续用 lv_font_conv:
  1. 去掉 Node 依赖,一条 Python 就够
  2. 时钟那个 48px 大字以前只能借 LVGL 内置的 montserrat —— 现在能自己生成
  3. 每个字号只带真正用到的字符,不带 cmap / kerning 那些表

用法: python gen_ui_font.py
输出: components/ui_fonts/ui_font_data.c
"""
import os

from PIL import ImageFont

PROJ = r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test"
OUT = os.path.join(PROJ, "components", "ui_draw", "ui_font_data.c")

# 中文用【微软雅黑】:笔画末端和转折都是圆的,比黑体(SimHei)的方头笔锋圆润得多。
# 关键是总笔画量差不多(实测 16px 下雅黑 302 像素 / 黑体 288),所以只是形状变圆,
# 不会因为变细而在 1bpp 屏上糊掉。对比图: tools\_fontcmp.png
FONT_CJK = r"C:\Windows\Fonts\msyh.ttc"
# 小字号(坐标轴刻度 10px)继续用 Segoe UI —— 雅黑在 10px 二值化后会糊成一团
FONT_ASCII = r"C:\Windows\Fonts\segoeui.ttf"
# 中大号西文(温湿度数值、电量百分比)跟着中文一起换雅黑,整个界面才是一个调子
FONT_LATIN = r"C:\Windows\Fonts\msyh.ttc"

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))

# 非 ASCII 的符号。温度那句 "%.1f°C" 里的度符号 U+00B0 不在 ASCII 范围里,
# 漏了它就会在屏幕上画成一个实心方块(缺字提示)。
# 以后界面上要用到新的特殊符号,加到这里。
#   °  = U+00B0 度
#   ↓  = U+2193 下行(网络下载)
#   ↑  = U+2191 上行(网络上传)
SYMBOLS = "°↓↑"

# ---------------------------------------------------------------
# 界面上真正会画出来的中文。改文案就要同步改这里,否则会画成空白!
# 下面按"一句话"列出来,方便对照。
# ---------------------------------------------------------------
CJK_SAMPLES = [
    "一二三四五六日星期",        # 星期
    "年月日",                    # 日期
    "正在连接时间已同步启动",    # WiFi / NTP 状态
    "室内温度相对湿",            # 传感器卡片
    "音乐播放中暂停",            # 标题与播放状态
    "切页长按源换曲",            # 按键提示
    "内置无文件麦克风频谱",      # 曲目名 / 空清单 / 麦克风状态
    "双击下一首上共卡",          # 按键提示与曲目序号
    "设置音量高低大小开关",      # 预留
    "不是的好了有",              # 预留
    "电池内部外充满未",          # 顶栏电源 / 充电状态标签
    "历史曲线",                  # 新页:曲线页标题
    "来自电脑推流",              # 推流模式状态字 + 曲目名占位("来自电脑")
    "继续止",                    # 按键提示里的"继续 / 停止推流"
    "还没有数据",                # 曲线页样本不足时的空状态
    "未知",                      # 电池电源类型(实际只进日志,顺手带上)
    "松手定",                    # 按键提示里的"长按换源(松手定)"
    "本地",                      # 中间那行:频谱来源="来自本地"
]
CJK = "".join(dict.fromkeys("".join(CJK_SAMPLES)))
print(f"共用 {len(CJK)} 个汉字")
print(f"  {CJK}")

# 每个字号:字符集按实际用途给,不多带
JOBS = [
    # 变量名,            字号, 字体,       字符集
    ("ui_font_cjk16",    16, FONT_CJK,   ASCII + SYMBOLS + CJK),
    ("ui_font_cjk20",    20, FONT_CJK,   ASCII + SYMBOLS + CJK),
    ("ui_font_ascii14",  14, FONT_LATIN, ASCII + SYMBOLS),
    ("ui_font_ascii28",  28, FONT_LATIN, ASCII + SYMBOLS),
    ("ui_font_ascii10",  10, FONT_ASCII, ASCII + SYMBOLS),
    ("ui_font_clock48",  48, FONT_ASCII, "0123456789:-"),
]


def rasterize(ttf, size, chars):
    """返回 (字形列表, 位图字节)。位图每行按字节对齐,MSB 在左。"""
    f = ImageFont.truetype(ttf, size)
    ascent, _descent = f.getmetrics()

    glyphs = []
    bits = bytearray()

    for ch in chars:
        mask, (dx, dy) = f.getmask2(ch, mode="1")
        w, h = mask.size
        adv = int(round(f.getlength(ch)))

        if w == 0 or h == 0:
            # 空白字符(空格之类):没有位图,只占宽度
            glyphs.append((ord(ch), adv, 0, 0, 0, 0, len(bits)))
            continue

        off = len(bits)
        for r in range(h):
            acc = 0
            for c in range(w):
                if mask.getpixel((c, r)):
                    acc |= 0x80 >> (c & 7)
                if (c & 7) == 7:
                    bits.append(acc)
                    acc = 0
            if w & 7:
                bits.append(acc)

        # oy:位图顶边相对基线的偏移(负数 = 在基线上方)
        glyphs.append((ord(ch), adv, w, h, dx, dy - ascent, off))

    return glyphs, bytes(bits)


def emit(fonts):
    """fonts: [(变量名, 字号, 字形表, 位图, 行高, 基线), ...]"""
    L = [
        "/*",
        " * 界面字体数据 —— 由 tools/fontgen/gen_ui_font.py 自动生成,别手改。",
        " *",
        " * 1bpp 位图,每行按字节对齐,MSB 在左。汉字取自黑体,",
        " * 数字与西文取自 Segoe UI。每个字号只带实际用到的字符。",
        " */",
        "#include \"ui_font.h\"",
        "",
    ]

    for name, size, glyphs, bits, line_h, base in fonts:
        tag = name.replace("ui_font_", "")
        L.append(f"/* ---------- {name}: {size}px, {len(glyphs)} 个字形 "
                 f"---------- */")
        L.append(f"static const UiGlyph g_{tag}[] = {{")
        for code, adv, w, h, ox, oy, off in glyphs:
            if w == 0:
                L.append(f"    {{0x{code:04X}, {adv}, 0, 0, 0, 0, 0}},"
                         f"        /* 空白 */")
            else:
                L.append(f"    {{0x{code:04X}, {adv}, {w}, {h}, "
                         f"{ox}, {oy}, {off}}},")
        L.append("};")
        L.append("")

        L.append(f"static const uint8_t b_{tag}[] = {{")
        for i in range(0, len(bits), 16):
            L.append("    " + ", ".join(f"0x{b:02X}" for b in bits[i:i + 16])
                     + ",")
        L.append("};")
        L.append("")

    L.append("/* ---------- 对外句柄 ---------- */")
    for name, size, glyphs, bits, line_h, base in fonts:
        tag = name.replace("ui_font_", "")
        L.append(f"const UiFont {name} = "
                 f"{{{line_h}, {base}, {len(glyphs)}, g_{tag}, b_{tag}}};")
    L.append("")

    return "\n".join(L)


def main():
    fonts = []
    for name, size, ttf, chars in JOBS:
        chars = "".join(sorted(set(chars)))      # 去重并排序,绘制时好二分
        glyphs, bits = rasterize(ttf, size, chars)

        f = ImageFont.truetype(ttf, size)
        ascent, descent = f.getmetrics()
        fonts.append((name, size, glyphs, bits, ascent + descent, ascent))

        print(f"  {name:16s} {size:2d}px  {len(glyphs):3d} 字形  "
              f"位图 {len(bits) // 1024} KB  行高 {ascent + descent}  "
              f"基线 {ascent}")

    with open(OUT, "w", encoding="utf-8") as fp:
        fp.write(emit(fonts))
    print(f"已写入 {OUT}")


if __name__ == "__main__":
    main()
