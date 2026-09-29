# -*- coding: utf-8 -*-
"""分析 Computerfont-1.ttf 的字符覆盖范围(无第三方依赖,直接解析 TTF cmap)"""
import struct

PATH = r"L:\ESP32\Computerfont-1.ttf"
d = open(PATH, "rb").read()

num_tables = struct.unpack(">H", d[4:6])[0]
tables = {}
for i in range(num_tables):
    off = 12 + i * 16
    tag = d[off:off + 4].decode("latin1")
    toff, tlen = struct.unpack(">II", d[off + 8:off + 16])
    tables[tag] = (toff, tlen)

print("表:", ", ".join(sorted(tables)))
print("unitsPerEm:", struct.unpack(">H", d[tables['head'][0] + 18:tables['head'][0] + 20])[0])

co, _ = tables["cmap"]
n = struct.unpack(">H", d[co + 2:co + 4])[0]
cps = set()
fmt_used = []
for i in range(n):
    p = co + 4 + i * 8
    pid, eid, sub = struct.unpack(">HHI", d[p:p + 8])
    o = co + sub
    fmt = struct.unpack(">H", d[o:o + 2])[0]
    fmt_used.append(fmt)
    if fmt == 4:
        segX2 = struct.unpack(">H", d[o + 6:o + 8])[0]
        seg = segX2 // 2
        ends = struct.unpack(">%dH" % seg, d[o + 14:o + 14 + segX2])
        starts = struct.unpack(">%dH" % seg, d[o + 16 + segX2:o + 16 + segX2 * 2])
        for s, e in zip(starts, ends):
            if s == 0xFFFF:
                continue
            cps.update(range(s, e + 1))
    elif fmt == 12:
        ng = struct.unpack(">I", d[o + 12:o + 16])[0]
        for g in range(ng):
            q = o + 16 + g * 12
            s, e, _ = struct.unpack(">III", d[q:q + 12])
            cps.update(range(s, e + 1))

print("cmap 子表格式:", fmt_used)
print("字形总数:", len(cps))

# 压成区间打印
rngs, s0, p0 = [], None, None
for c in sorted(cps):
    if s0 is None:
        s0 = p0 = c
    elif c == p0 + 1:
        p0 = c
    else:
        rngs.append((s0, p0)); s0 = p0 = c
if s0 is not None:
    rngs.append((s0, p0))
print("覆盖区间:")
for a, b in rngs:
    print(f"  U+{a:04X}-U+{b:04X}  ({a}-{b})")

# 测试界面需要的关键字符
need = "0123456789: APM-."
print("\n时钟所需字符:")
for ch in need:
    print(f"  '{ch}' U+{ord(ch):04X}  {'有 ✓' if ord(ch) in cps else '缺失 ✗'}")

cjk = "室内温度相对湿度星期一二三四五六日年月"
have = [c for c in cjk if ord(c) in cps]
print(f"\n中文测试: 有 {len(have)}/{len(cjk)}  ({''.join(have) if have else '全无'})")
