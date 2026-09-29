"""抓一段串口日志,只打印匹配正则的行 —— 免得刷屏。

用法: python capture.py [COM口] [秒数] [正则] [-n]

    -n / --no-reset  不复位板子,直接挂上去听。
                     要在板子【运行中】抓崩溃现场时用(默认会复位)。
"""
import re
import sys
import time

import serial

argv = [a for a in sys.argv[1:] if a not in ("-n", "--no-reset")]
no_reset = len(argv) < len(sys.argv) - 1

port = argv[0] if len(argv) > 0 else "COM5"
secs = float(argv[1]) if len(argv) > 1 else 25.0
pat = re.compile(argv[2] if len(argv) > 2 else ".", re.I)

s = serial.Serial(port, 115200, timeout=0.5)
# 打开串口本身可能会动 DTR/RTS,先把它们都放开(不触发复位)
s.setDTR(False)
s.setRTS(False)

if not no_reset:
    # 经典 ESP32 复位时序:DTR/RTS 配合拉一下 EN
    time.sleep(0.1)
    s.setRTS(True)
    time.sleep(0.15)
    s.setRTS(False)
    time.sleep(0.05)
else:
    print("[capture] 不复位,直接监听", flush=True)

buf = b""
t0 = time.time()
while time.time() - t0 < secs:
    data = s.read(8192)
    if not data:
        continue
    buf += data
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        txt = line.decode("utf-8", "replace").rstrip()
        if pat.search(txt):
            print(txt)
s.close()
