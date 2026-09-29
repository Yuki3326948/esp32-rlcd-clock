# 按 user_app.cpp 里的 SYS_* 宏离线渲染曲线页,查六件事:
#   1) 四块面板的位置和大小,四周边距还剩多少(目标是"不留黑边")
#   2) 标题框 / 当前值框 / Y 轴刻度框 三者会不会互相重叠
#   3) 绘图区、轴线、刻度线、X 轴刻度值框 有没有跑到面板边框外面
#   4) X 轴刻度值框和面板下边框之间还剩几像素
#   5) 文字宽度够不够(尤其 "100" 要塞进 19px 的刻度框)
#   6) 点状竖网格 + 实心柱子画出来到底什么样
# 改 user_app.cpp 里的宏之后,把这里的同名常量同步一下再跑。
import math
from PIL import Image, ImageDraw, ImageFont

# ---- 和 user_app.cpp 保持一致 ----
SYS_M, SYS_GAP, SYS_N = 2, 6, 4
SYS_W, SYS_H, SYS_Y0 = 195, 130, 30
SYS_COL_X = lambda c: SYS_M + c * (SYS_W + SYS_GAP)
SYS_ROW_Y = lambda r: SYS_Y0 + r * (SYS_H + SYS_GAP)

SYS_TITLE_B = 18
SYS_GUT_X, SYS_GUT_W = 1, 30
SYS_PLOT_DX, SYS_PLOT_DY = 36, 26
SYS_PLOT_W, SYS_PLOT_H = 157, 82
SYS_TICK, SYS_XTICK = 3, 2
SYS_XLAB_B = SYS_PLOT_DY + SYS_PLOT_H + 16
SYS_XLAB_W = 40
SYS_BAR_W, SYS_BAR_STEP = 1, 2
SYS_NBAR = SYS_PLOT_W // SYS_BAR_STEP
SYS_TITLE_W, SYS_VAL_W = 56, 56

NAMES = ["CPU %", "GPU %", "MEM %", "NET↓"]

F = r"C:\Windows\Fonts\msyh.ttc"
f14 = ImageFont.truetype(F, 14)
f10 = ImageFont.truetype(F, 10)
f20 = ImageFont.truetype(F, 20)
L14_H, L14_B = 19, 15      # ascii14
L10_H, L10_B = 14, 11      # ascii10

HEADER_B = 19
SPAN_B = 18
DIVIDER_Y = 26

print("=== 四周还剩多少边距 ===")
print("  左 %d / 右 %d / 上(面板顶到屏顶) %d / 下 %d"
      % (SYS_COL_X(0), 400 - (SYS_COL_X(1) + SYS_W), SYS_Y0,
         300 - (SYS_ROW_Y(1) + SYS_H)))
print("  总分隔线 y=%d,面板从 y=%d -> 间隙 %d  %s"
      % (DIVIDER_Y, SYS_Y0, SYS_Y0 - DIVIDER_Y - 1,
         "OK" if SYS_Y0 > DIVIDER_Y else "分隔线横穿面板!"))

print("=== 面板位置 ===")
for k in range(SYS_N):
    px, py = SYS_COL_X(k % 2), SYS_ROW_Y(k // 2)
    print("  面板%d %-6s x %3d..%3d  y %3d..%3d"
          % (k, NAMES[k], px, px + SYS_W - 1, py, py + SYS_H - 1))

print("=== 面板内部:标题 / 刻度栏 / 值框 会不会撞 ===")
px, py = SYS_COL_X(0), SYS_ROW_Y(0)
gx, gy = px + SYS_PLOT_DX, py + SYS_PLOT_DY
gw, gh = SYS_PLOT_W, SYS_PLOT_H
bot = gy + gh - 1

t_l, t_r = gx + 2, gx + 2 + SYS_TITLE_W - 1
v_r = gx + gw - 1
v_l = v_r - SYS_VAL_W + 1
lab_r = px + SYS_GUT_X + SYS_GUT_W
lab_l = lab_r - SYS_GUT_W + 1
print("  标题框 x %d..%d,值框 x %d..%d -> 相隔 %d  %s"
      % (t_l, t_r, v_l, v_r, v_l - t_r - 1,
         "OK" if v_l > t_r else "重叠!"))
print("  刻度值框 x %d..%d,与标题框横向相隔 %d -> %s"
      % (lab_l, lab_r, t_l - lab_r - 1,
         "OK" if lab_r < t_l else "横向重叠!"))

t_top, t_bot = py + SYS_TITLE_B - L14_B - 2, py + SYS_TITLE_B - L14_B - 2 + L14_H + 4
y_top_box = gy + 3 - L10_B - 2
print("  标题擦拭框 y %d..%d,顶部刻度值框 y %d..%d"
      % (t_top, t_bot - 1, y_top_box, y_top_box + L10_H + 4 - 1))
print("    -> 纵向虽叠但横向分开(刻度栏 3..%d vs 标题 %d..%d),不打架  OK"
      % (lab_r, t_l, t_r))

print("=== 绘图区 / 刻度 / 刻度值 都在面板内吗 ===")
print("  绘图区 x %d..%d y %d..%d(面板 x %d..%d y %d..%d)"
      % (gx, gx + gw - 1, gy, bot, px, px + SYS_W - 1, py, py + SYS_H - 1))
print("  Y 轴轴线 x=%d,刻度线 x %d..%d,刻度值框 x %d..%d"
      % (gx - 1, gx - 1 - SYS_TICK, gx - 2, lab_l, lab_r))
xl_top = py + SYS_XLAB_B - L10_B - 2
xl_bot = xl_top + L10_H + 4 - 1
print("  X 刻度线 y %d..%d,刻度值框 y %d..%d,面板下边框 y %d -> 剩 %d"
      % (bot + 2, bot + 1 + SYS_XTICK, xl_top, xl_bot, py + SYS_H - 1,
         py + SYS_H - 1 - xl_bot))
print("  柱子:共 %d 根,最后一根右边界 x=%d,绘图区右边界 x=%d -> %s"
      % (SYS_NBAR, gx + (SYS_NBAR - 1) * SYS_BAR_STEP + SYS_BAR_W - 1,
         gx + gw - 1,
         "OK" if (SYS_NBAR - 1) * SYS_BAR_STEP + SYS_BAR_W <= gw else "超出!"))

print("=== 文字宽度 ===")
for s, lim, tag in [(x, SYS_TITLE_W, "标题") for x in NAMES] + \
                   [(x, SYS_VAL_W, "值") for x in
                    ["0", "42", "100", "--", "4.9M", "860K", "976.6M", "64.0M"]] + \
                   [(x, SYS_GUT_W, "Y刻度") for x in
                    ["100", "50", "0", "940K", "470K", "298M", "149M"]] + \
                   [(x, SYS_XLAB_W, "X刻度") for x in
                    ["-2.4h", "-1.2h", "-45m", "0"]]:
    w = (f14 if tag in ("标题", "值") else f10).getlength(s)
    print("  %-5s %-7s %5.1f / %d  %s"
          % (tag, s, w, lim, "OK" if w <= lim else "溢出!"))


def dot_v(d, x, y, ln):
    for i in range(ln):
        if (i & 3) == 0:
            d.point((x, y + i), fill=255)


def bars(d, gx, gy, val, n, scale):
    for i in range(n):
        if val[i] < 0:
            continue
        v = min(val[i], scale)
        hgt = v * (SYS_PLOT_H - 1) // scale
        if val[i] > 0 and hgt < 2:
            hgt = 2
        x = gx + i * SYS_BAR_STEP
        d.line([x, gy + SYS_PLOT_H - 1 - hgt, x + SYS_BAR_W - 1,
                gy + SYS_PLOT_H - 1], fill=255, width=SYS_BAR_W)


def gen(k, n):
    out = []
    for i in range(n):
        t = i / float(n)
        if k == 0:
            out.append(int(40 + 25 * math.sin(t * 9)))
        elif k == 1:
            out.append(int(20 + 35 * max(0.0, math.sin(t * 4))))
        elif k == 2:
            out.append(int(75 + 4 * math.sin(t * 3)))
        else:
            out.append(int(abs(math.sin(t * 13)) ** 3 * 900 + 20))
    return out


def fmt_short(kbs):
    if kbs < 1024:
        return "%uK" % kbs
    t = (kbs * 10 + 512) // 1024
    return "%u.%uM" % (t // 10, t % 10)


def fmt_tiny(kbs):
    """Y 轴刻度用的极短写法(复刻 C 的 FmtSpeedTiny)"""
    if kbs < 1024:
        return "%uK" % kbs
    if kbs < 1024 * 1024:
        return "%uM" % (kbs // 1024)
    return "%uG" % (kbs // (1024 * 1024))


img = Image.new("L", (400, 300), 0)
d = ImageDraw.Draw(img)

d.text((16, HEADER_B), "历史曲线", font=f20, fill=255, anchor="ls")
d.line([16, DIVIDER_Y, 383, DIVIDER_Y], fill=255, width=1)
d.text((384, SPAN_B), "2.4h", font=f14, fill=255, anchor="rs")

net_max = 940
SPAN_S = 2 * 3600 + 24 * 60      # 2.4h

for k in range(SYS_N):
    px, py = SYS_COL_X(k % 2), SYS_ROW_Y(k // 2)
    gx, gy = px + SYS_PLOT_DX, py + SYS_PLOT_DY
    bot = gy + SYS_PLOT_H - 1
    ax = gx - 1
    lab_r = px + SYS_GUT_X + SYS_GUT_W
    scale = net_max if k == SYS_N - 1 else 100
    val = gen(k, SYS_NBAR)

    d.rounded_rectangle([px, py, px + SYS_W - 1, py + SYS_H - 1],
                        radius=4, outline=255, width=1)
    d.text((gx + 2, py + SYS_TITLE_B), NAMES[k], font=f14, fill=255,
           anchor="ls")
    cur = fmt_short(940) if k == SYS_N - 1 else "%d" % val[-1]
    d.text((gx + SYS_PLOT_W - 1, py + SYS_TITLE_B), cur, font=f14, fill=255,
           anchor="rs")

    if k == SYS_N - 1:
        yhi, ymid = fmt_tiny(scale), fmt_tiny(scale // 2)
    else:
        yhi, ymid = "100", "50"
    d.text((lab_r, gy + 3), yhi, font=f10, fill=255, anchor="rs")
    d.text((lab_r, gy + SYS_PLOT_H // 2 + 3), ymid, font=f10, fill=255,
           anchor="rs")
    d.text((lab_r, bot + 3), "0", font=f10, fill=255, anchor="rs")

    for i in range(3):
        y = min(gy + SYS_PLOT_H * i // 2, bot)
        d.line([ax - SYS_TICK, y, ax - 1, y], fill=255, width=1)
    d.line([ax, gy, ax, bot + 1], fill=255, width=1)
    d.line([ax, bot + 1, ax + SYS_PLOT_W, bot + 1], fill=255, width=1)

    def span(secs):
        if secs >= 3600:
            return "%d.%dh" % (secs // 3600, (secs % 3600) // 360)
        return "%dm" % (secs // 60)
    d.text((gx, py + SYS_XLAB_B), "-" + span(SPAN_S), font=f10, fill=255,
           anchor="ls")
    d.text((gx + SYS_PLOT_W // 2 - SYS_XLAB_W // 2, py + SYS_XLAB_B),
           "-" + span(SPAN_S // 2), font=f10, fill=255, anchor="ls")
    d.text((gx + SYS_PLOT_W - 1, py + SYS_XLAB_B), "0", font=f10, fill=255,
           anchor="rs")

    for x in (gx - 1, gx + SYS_PLOT_W // 2, gx + SYS_PLOT_W - 1):
        d.line([x, bot + 2, x, bot + 1 + SYS_XTICK], fill=255, width=1)

    for i in range(1, 4):
        dot_v(d, gx + SYS_PLOT_W * i // 4, gy, SYS_PLOT_H)

    bars(d, gx, gy, val, SYS_NBAR, scale)

out = r"L:\ESP32\tools\_syspage_mock.png"
img.convert("1").save(out)
print("已保存", out)
