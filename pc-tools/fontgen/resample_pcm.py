"""把内嵌音乐从 24kHz 重采样到 48kHz。

为什么必须做:要覆盖到 20kHz 频段,采样率至少要 40kHz(Nyquist)。
采样率改成 48kHz 后,原本 24kHz 采样的音乐会以 2 倍速播放,
所以必须先离线把它重采样上去。

24000 -> 48000 正好是整数 2 倍,用线性插值即可:
    输出[2i]   = 原[i]
    输出[2i+1] = (原[i] + 原[i+1]) / 2
"""
import array
import os

ASSETS = (r"L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF"
          r"\11_U8G2_Test\components\audio_player\assets")
SRC = os.path.join(ASSETS, "canon.pcm")

src = array.array("h")
with open(SRC, "rb") as f:
    src.frombytes(f.read())

frames = len(src) // 2          # 立体声:每帧 2 个 int16
print(f"源文件: {len(src) * 2 / 1024 / 1024:.2f} MB, {frames} 帧, "
      f"{frames / 24000:.1f} 秒 @24kHz")

out = array.array("h")
append = out.append
for i in range(frames):
    l0 = src[2 * i]
    r0 = src[2 * i + 1]
    append(l0)
    append(r0)
    if i + 1 < frames:
        append((l0 + src[2 * i + 2]) // 2)
        append((r0 + src[2 * i + 3]) // 2)
    else:
        # 最后一帧没有下一帧,直接复制
        append(l0)
        append(r0)

with open(SRC, "wb") as f:
    out.tofile(f)

n_frames = len(out) // 2
print(f"输出文件: {len(out) * 2 / 1024 / 1024:.2f} MB, {n_frames} 帧, "
      f"{n_frames / 48000:.1f} 秒 @48kHz")
print("时长一致 ✓" if abs(n_frames / 48000 - frames / 24000) < 0.01 else "!! 时长不一致")
