# 按 user_app.cpp 里的 VBAR_* / NET_* 宏离线渲染主界面下半部分。
# 在看不到屏的情况下先自己看一眼 —— 主要查六件事:
#   1) 每根柱子右边的三行文字,会不会超出 VBAR_TXT_W 的定宽框
#   2) 同一栏里三行的擦除框会不会互相重叠(重叠会把先画的字擦掉一块)
#   3) 上一栏的文字会不会压到下一栏的柱子(栏与栏之间够不够)
#   4) 四根柱子整体占宽、左右边距对不对称
#   5) 自适应单位的换算有没有写错(整数除法的四舍五入最容易错一位)
#   6) 单位在 KB/s 与 MB/s 之间切换时,两种写法都得装得进文字框
# 改 user_app.cpp 里的宏之后,把这里的同名常量同步一下再跑。
from PIL import Image, ImageDraw, ImageFont

# ---- 和 user_app.cpp 保持一致 ----
VBAR_N = 4
COL_X = [2, 107, 213, 318]
VBAR_Y, VBAR_H, VBAR_W = 170, 126, 32
VBAR_R, VBAR_INSET, VBAR_GAP, VBAR_TXT_W = 4, 3, 8, 40
L1_B, L2_B, L3_B = 201, 240, 278
NET_FULL_DN_KBPS = 305175     # 2.5GbE
NET_FULL_UP_KBPS = 5000       # 宽带套餐 5 MB/s
SWITCH = 1024                 # 低于它就显示 KB/s

F = r"C:\Windows\Fonts\msyh.ttc"
FSIZE, FLH, FBASE = 14, 19, 15          # ui_font_ascii14
font = ImageFont.truetype(F, FSIZE)

COL_NAMES = ["CPU", "GPU", "↓", "↑"]


def fmt_auto(kbs):
    """复刻 C 那边的自适应显示,返回 (数值, 单位)。
       kbs < 0 表示没数据。"""
    if kbs < 0:
        return "-", "KB/s"
    if kbs > 999999:
        kbs = 999999
    if kbs < SWITCH:
        return "%u" % kbs, "KB/s"
    t = (kbs * 10 + 512) // 1024        # 0.1 MB,四舍五入
    return "%u.%u" % (t // 10, t % 10), "MB/s"


def pct_of(kbs, full):
    if kbs >= full:
        return 100
    p = kbs * 100 // full
    if kbs > 0 and p == 0:
        return 1
    return p


print("=== 自适应换算抽查(必须和 C 的整数写法逐位一致) ===")
for kbs in (0, 36, 512, 1023, 1024, 2048, 5000, 25000, 125000, 305175, 999999):
    v, u = fmt_auto(kbs)
    print("  %7d KiB/s -> %-7s %-4s  (浮点参照 %.3f)"
          % (kbs, v, u, kbs / 1024.0))

print("=== 单位切换边界 ===")
for kbs in (1023, 1024):
    v, u = fmt_auto(kbs)
    print("  %d KiB/s -> %s %s" % (kbs, v, u))
print("  注意 1023->1024 数字会变短(\"1023\"->\"1.0\"),"
      "因为擦除框是定宽 40px 所以不会留残笔")

print("=== 柱高抽查 ===")
for kbs in (0, 36, 5000, 25000, 305175):
    print("  下行 %7d KiB/s -> %3d%%" % (kbs, pct_of(kbs, NET_FULL_DN_KBPS)))
for kbs in (0, 36, 2500, 5000):
    print("  上行 %7d KiB/s -> %3d%%" % (kbs, pct_of(kbs, NET_FULL_UP_KBPS)))

print("=== 擦除框检查(每栏三行) ===")
for k in range(VBAR_N):
    tx = COL_X[k] + VBAR_W + VBAR_GAP
    end, bad = None, []
    for label, bl in [("第一行", L1_B), ("第二行", L2_B), ("第三行", L3_B)]:
        top = bl - FBASE - 2
        if end is not None and top < end:
            bad.append("%s 与前一行重叠 %dpx" % (label, end - top))
        end = top + FLH + 4
    print("  %-4s 文字 %3d..%3d  竖跨 %d..%d   %s"
          % (COL_NAMES[k], tx, tx + VBAR_TXT_W - 1, L1_B - FBASE - 2, end - 1,
             "OK" if not bad else "!! " + "; ".join(bad)))

print("=== 栏间碰撞检查 ===")
for k in range(VBAR_N - 1):
    tr = COL_X[k] + VBAR_W + VBAR_GAP + VBAR_TXT_W - 1
    print("  %s 文字右端 %d -> 下一栏柱左 %d,空隙 %d  %s"
          % (COL_NAMES[k], tr, COL_X[k + 1], COL_X[k + 1] - tr - 1,
             "OK" if tr < COL_X[k + 1] else "!! 压到柱子了"))

print("=== 文字宽度 (框宽 %d) ===")
samples = ["CPU", "GPU", "↓", "↑", "0%", "100%", "27°C", "100°C", "--", "-"]
for kbs in (0, 36, 1023, 1024, 25000, 305175, 999999):
    samples.append(fmt_auto(kbs)[0])
samples += ["KB/s", "MB/s"]
for s in samples:
    w = font.getlength(s)
    print("  %-7s %5.1f  %s" % (s, w, "OK" if w <= VBAR_TXT_W else "溢出!"))

left = COL_X[0]
right = 400 - (COL_X[VBAR_N - 1] + VBAR_W + VBAR_GAP + VBAR_TXT_W)
print("=== 横向 ===")
print("  左右边距 %d / %d,内容占宽 %.0f%%" % (left, right, (400 - left - right) / 4.0))


def render(name, cpu, gpu, dn, up, ok=True):
    img = Image.new("L", (400, 300), 0)
    d = ImageDraw.Draw(img)
    vd, ud = fmt_auto(dn if ok else -1)
    vu, uu = fmt_auto(up if ok else -1)
    cols = [
        ("CPU", "%d%%" % cpu[0], ("%d\u00b0C" % cpu[1]) if cpu[1] >= 0 else "--",
         cpu[0]),
        ("GPU", "%d%%" % gpu[0], ("%d\u00b0C" % gpu[1]) if gpu[1] >= 0 else "--",
         gpu[0]),
        ("\u2193", vd, ud, pct_of(dn, NET_FULL_DN_KBPS) if ok else 0),
        ("\u2191", vu, uu, pct_of(up, NET_FULL_UP_KBPS) if ok else 0),
    ]

    def vbar(bx, pct):
        ix, iy = bx + VBAR_INSET, VBAR_Y + VBAR_INSET
        iw, ih = VBAR_W - 2 * VBAR_INSET, VBAR_H - 2 * VBAR_INSET
        d.rounded_rectangle([bx, VBAR_Y, bx + VBAR_W - 1, VBAR_Y + VBAR_H - 1],
                            radius=VBAR_R, outline=255, width=1)
        fh = ih * pct // 100
        if pct > 0 and fh < 3:
            fh = 3
        if fh > 0:
            d.rectangle([ix, iy + ih - fh, ix + iw - 1, iy + ih - 1], fill=255)

    d.line([16, 168, 383, 168], fill=255, width=1)
    d.text((200, 139), "2026年9月30日  星期三", font=font, fill=255, anchor="ms")

    for k, (t, v, u, pct) in enumerate(cols):
        bx = COL_X[k]
        tx = bx + VBAR_W + VBAR_GAP
        vbar(bx, pct)
        d.text((tx, L1_B), t, font=font, fill=255, anchor="ls")
        d.text((tx, L2_B), v, font=font, fill=255, anchor="ls")
        d.text((tx, L3_B), u, font=font, fill=255, anchor="ls")

    out = r"L:\ESP32\tools\%s.png" % name
    img.convert("1").save(out)
    print("已保存", out)


# 低流量:下载/上传都在 KB/s 档,验证单位切换
render("_vbar_mock", (42, 27), (18, 54), 860, 120)
# 高流量:下载跑满 2.5G,上传跑满套餐 -> 都切到 MB/s
render("_vbar_mock2", (100, 99), (0, -1), 305175, 5000, ok=True)
# 断连
render("_vbar_mock3", (0, -1), (0, -1), 0, 0, ok=False)
