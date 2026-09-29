# -*- coding: utf-8 -*-
"""
检查界面上用到的字是不是都在字库里 —— 防"改文案忘了加字"。

背景:
    gen_ui_font.py 只把 CJK_SAMPLES 里列出的字打进字库(为了省 Flash)。
    改了界面文案却忘了同步加字 -> 屏幕上直接是一片【实心方块】。
    这个坑已经踩过两次(第一次是度符号 °,第二次是推流状态字)。

用法:
    python check_ui_text.py
    退出码 0 = 全部覆盖,1 = 有缺字

注意(会误报的地方):
    脚本把源码里所有字符串字面量的非 ASCII 字符都算进去了,[日志文本] 也在内。
    日志不画屏幕、不需要字形 —— 所以报出来的"缺失字"里可能有几个其实无害。
    宁可多带几个字(每个字约 32 字节),也别漏掉真正要显示的那个。
"""
import os
import re
import sys

# 脚本在 pc-tools/fontgen/ 下,仓库根目录就是往上三级。
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen_ui_font.py")

SOURCES = [
    os.path.join(ROOT, "firmware", "components", "user_app", "user_app.cpp"),
    os.path.join(ROOT, "firmware", "components", "ui_draw", "ui_draw.cpp"),
]


def font_chars() -> set:
    """从 gen_ui_font.py 抠出实际打进字库的字符(不 import,免得顺带跑了生成)。"""
    src = open(GEN, encoding="utf-8").read()
    chars = set()

    m = re.search(r"CJK_SAMPLES\s*=\s*\[(.*?)\n\]", src, re.S)
    if not m:
        print("!! 在 gen_ui_font.py 里找不到 CJK_SAMPLES,脚本要跟着改")
        return chars
    for s in re.findall(r'"([^"]*)"', m.group(1)):
        chars |= set(s)

    m = re.search(r'^SYMBOLS\s*=\s*"([^"]*)"', src, re.M)
    if m:
        chars |= set(m.group(1))
    return chars


def ui_chars() -> dict:
    """界面源码里字符串字面量用到的非 ASCII 字符 -> {字: {出现在哪些文件}}。

    只要"要在屏幕上画出来"的字,所以三类要剔掉:
      1. 注释
      2. 日志宏的参数 —— 里面的中文只是打给串口看的,不需要字形
         (不剔的话会一口气报一百多个假缺失,真缺失就淹在里面了)
      3. printf 系列
    """
    used = {}
    for path in SOURCES:
        if not os.path.exists(path):
            continue
        src = open(path, encoding="utf-8").read()
        src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
        src = re.sub(r"//[^\n]*", "", src)
        src = re.sub(r"ESP_(?:EARLY_)?LOG[A-Z]\(.*?\);", "", src, flags=re.S)
        src = re.sub(r"\bprintf\(.*?\);", "", src, flags=re.S)
        # 断言/校验宏里的话也是给日志看的
        src = re.sub(r"ESP_(?:RETURN_ON_[A-Z_]+|GOTO|CHECK[A-Z_]*)\(.*?\);",
                     "", src, flags=re.S)
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for ch in lit:
                if ord(ch) > 0x7F:
                    used.setdefault(ch, set()).add(os.path.basename(path))
    return used


def main() -> int:
    have = font_chars()
    used = ui_chars()
    missing = sorted(ch for ch in used if ch not in have)

    print(f"字库有 {len(have)} 个字符,界面用到 {len(used)} 个非 ASCII 字符")
    if not missing:
        print("全部覆盖,OK")
        return 0

    print(f"\n缺 {len(missing)} 个字 —— 屏幕上会画成实心方块:")
    for ch in missing:
        where = ", ".join(sorted(used[ch]))
        print(f"  {ch}  U+{ord(ch):04X}   ({where})")
    print("\n加进 gen_ui_font.py 的 CJK_SAMPLES(建议按整句话加,方便 grep),")
    print("然后重新跑 gen_ui_font.py 生成字库。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
