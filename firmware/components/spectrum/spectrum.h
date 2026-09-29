#pragma once

/*
 * 音频频谱分析(FFT)
 *
 *   用 esp-dsp 对播放中的 PCM 做 4096 点 FFT,归到 90 个对数频段,
 *   输出 0.0~1.0 的电平,给界面画柱状图。
 *
 *   频率分辨率 = 采样率 / FFT 点数,这是唯一决定"低频能不能分开"的量:
 *     48kHz + 4096 点  ->  11.7Hz/bin,覆盖 94Hz~20kHz
 *   频段按对数分布(每段比前一段宽 6.14%),和听感一致。
 *
 *   为什么不能用 512 点(以前就是):
 *     512 点 -> bin 93.75Hz,而低频段间距只有 0.0614*f Hz,
 *     要 f >= 1527Hz 频段间距才追得上一个 bin。
 *     结果 1.5kHz 以下所有柱子共用同一两个 bin,屏上是"好几条一起上下"。
 *
 *   窗口 50% 重叠(FFT_HOP = N/2),刷新率 23.4Hz,柱子动作才连得上。
 *
 *   采样率不是写死的 —— 播 TF 卡上 44.1kHz 的 WAV 时,
 *   调 Spectrum_SetSampleRate() 后频段边界会按 Hz 重算。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPECTRUM_BANDS   90

/* 分配 FFT 表和汉宁窗。开机调一次。 */
void Spectrum_Init(void);

/* 喂一段 16bit 立体声 PCM。累积够 512 帧就自动做一次 FFT。 */
void Spectrum_Feed(const int16_t *pcm, size_t frames);

/* 取各频段电平(0.0~1.0)。内部带快升慢降,柱子会有自然的下落感。 */
void Spectrum_GetBands(float out[SPECTRUM_BANDS]);

/* 整体电平(0.0~1.0) */
float Spectrum_GetLevel(void);

/* 关掉后 Spectrum_Feed 直接返回,省 CPU */
void Spectrum_Enable(bool on);

/* 播放源采样率变了时调这个,频段边界会按 Hz 重算 */
void     Spectrum_SetSampleRate(uint32_t hz);
uint32_t Spectrum_GetSampleRate(void);

/* 显示下限(dBFS)。下面这段是动态范围:
     音乐是用 -55dB —— 信号强、噪底低,范围够用
     麦克风用 -85dB —— 远处的声音在 FFT 里可能只有 -70~-80dB,
                        -55 会直接把它截成 0,柱子一动不动
   改了这个就要把界面上的纵轴刻度也重写,否则刻度是假的。 */
void  Spectrum_SetFloor(float db);
float Spectrum_GetFloor(void);

/* 当前这一帧最响频段的真实 dB(未经下限映射)。
   用来标定下限:房间静止时读一下,就能知道该设多少。 */
float Spectrum_GetPeakDb(void);

bool Spectrum_IsReady(void);

#ifdef __cplusplus
}
#endif
