"""只生成界面真正用到的汉字 —— 替代之前 3755 字的巨大字体。

为什么要这样做:
  之前用 GB2312 一级字库(3755 字),16px + 20px 两个字号合计 2.5MB。
  但界面实际只用到 40 个不同的汉字,其余都是白占 flash。
  这里按"界面上真正会显示的字符串"精确挑字,两个字号合计只要几十 KB。

用法: python gen_ui_font.py
输出: components/ui_fonts/lv_font_cjk_16.c / lv_font_cjk_20.c
"""
import os
import re
import subprocess

PROJ = r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test"
OUT_DIR = os.path.join(PROJ, "components", "ui_fonts")
TTF = r"C:\Windows\Fonts\simhei.ttf"
NODE = r"L:\ESP32\tools\fontgen\node_modules\lv_font_conv\lv_font_conv.js"

# ---------------------------------------------------------------
# 界面显示用字(从 user_app.cpp 里逐条核对出来的,不含日志文字)
# ---------------------------------------------------------------
USED = (
    # 星期:星期日/一/二/三/四/五/六
    "一二三四五六日星期"
    # 日期:%d年%d月%d日
    "年月日"
    # 状态:正在连接 / 时间已同步 / 正在同步 / 正在启动 / 正在获取时间
    "正在连接时间已同步启动获取"
    # 传感器卡片:室内温度 / 相对湿度
    "室内温度相对湿"
    # 音乐页:音乐播放 / 播放中 / 已暂停 / 播放/暂停 / 切页
    "音乐播放中暂停切页"
)

# 预留一点常用字,省得以后改文案又要重新生成(才几 KB)
# 长/按/源 是"长按切源"这条按键提示要用的
SPARE = "设置音量高低大小开关无有不是的好了频谱文件麦克长按源置换曲卡下首共上双击"

# 只保留唯一的汉字
CJK = "".join(dict.fromkeys(USED + SPARE))
print(f"共 {len(CJK)} 个汉字(其中界面必用 {len(set(USED))} 个)")
print(f"  {CJK}")

# 需要生成的字号:16 用于标签,20 用于日期和标题
for size, name in ((16, "lv_font_cjk_16"), (20, "lv_font_cjk_20")):
    out = os.path.join(OUT_DIR, name + ".c")
    cmd = ["node", NODE,
           "--font", TTF,
           "-r", "0x20-0x7E",            # ASCII(hint 里有 BOOT / KEY)
           "--symbols", CJK,
           "--size", str(size), "--bpp", "1",
           "--format", "lvgl", "--no-compress",
           "--lv-include", "lvgl.h",
           "--lv-font-name", name,
           "-o", out]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    src = open(out, encoding="utf-8").read()
    n = len(re.findall(r"\.bitmap_index = \d+,", src))
    advs = {int(m) for m in re.findall(r"\.adv_w = (\d+)", src)}
    print(f"\n{name}: {size}px, {n} 个字形, "
          f"{os.path.getsize(out) // 1024} KB, adv_w 取值 {sorted(advs)[:4]}...")

# 清理不再使用的字体文件
for dead in ("lv_font_clock_48.c", "lv_font_pix_16.c", "lv_font_pix_28.c"):
    p = os.path.join(OUT_DIR, dead)
    if os.path.exists(p):
        os.remove(p)
        print(f"已删除 {dead}")

print("\n完成")
