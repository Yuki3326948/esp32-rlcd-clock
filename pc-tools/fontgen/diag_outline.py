"""把 Computerfont 的 '8' 字形的原始轮廓坐标打出来。

如果轮廓本身左右对称,说明屏幕上看到的"左细右粗"是
TrueType 网格对齐指令(非原生字号下)撕歪的 —— 那么关掉 hinting 就能修好。
如果轮廓本身就左右不对称,那就是字体设计如此,只能换字体。
"""
from fontTools.ttLib import TTFont

FONT = r"L:\ESP32\Computerfont-1.ttf"
f = TTFont(FONT)
glyf = f["glyf"]
upem = f["head"].unitsPerEm
print(f"unitsPerEm = {upem}")

name = f.getBestCmap()[0x38]        # '8'
g = glyf[name]
g.expand(glyf)
print(f"glyph '{name}'  轮廓数={g.numberOfContours}")

coords, endPts, flags = g.getCoordinates(glyf)
start = 0
for i, e in enumerate(endPts):
    pts = coords[start:e + 1]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    on = sum(1 for fl in flags[start:e + 1] if fl & 1)
    print(f"\n轮廓{i}: {len(pts)} 点   x:{min(xs)}~{max(xs)}   y:{min(ys)}~{max(ys)}"
          f"   (其中 {on} 个是直线点)")
    for p, fl in zip(pts, flags[start:e + 1]):
        print(f"    ({p[0]:5d},{p[1]:5d}) {'直线' if fl & 1 else '曲线'}")
    start = e + 1

# 对称性检查:把 x 取反后是否有对应的点
allx = sorted({p[0] for p in coords})
print(f"\n所有 x 坐标: {allx}")
mid = (min(allx) + max(allx)) / 2
print(f"x 范围中值 = {mid}")
mirrored = sorted({round(2 * mid - x) for x in allx})
print(f"镜像后    : {mirrored}")
print("左右对称" if mirrored == allx else "左右不对称 <-- 字体设计如此")
