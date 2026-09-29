#pragma once

/*
 * 板载音频驱动(ESP32-S3-RLCD-4.2)
 *   播放: ES8311 (DAC)  -> 功放 GPIO46 -> 喇叭
 *   录音: ES7210 (ADC)  <- 麦克风          (待接入)
 *   总线: I2S0 播放/采集,控制走 I2C0(与 SHTC3/PCF85063 共用)
 *
 * 引脚取自官方 codec_board/board_cfg.txt 的 S3_RLCD_4_2 段:
 *   i2s: {mclk: 16, bclk: 9, ws: 45, din: 10, dout: 8}
 *   out: {codec: ES8311, pa: 46, use_mclk: 1}
 * 与 LCD(11/12/5/40/41/6)、I2C(13/14)、按键(18/0)都不冲突。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 板级引脚 ---------------- */
#define AUDIO_I2S_MCLK_PIN   16
#define AUDIO_I2S_BCLK_PIN    9
#define AUDIO_I2S_WS_PIN     45
#define AUDIO_I2S_DOUT_PIN    8   /* ESP -> ES8311 */
#define AUDIO_I2S_DIN_PIN    10   /* ES7210 -> ESP  */
#define AUDIO_PA_PIN         46   /* 功放使能,不拉高就是静音 */

/* ---------------- 音频参数 ---------------- */
/* 48kHz:要覆盖到 20kHz 频段,Nyquist 至少得 40kHz。
   而且 48000/24000 是整数 2 倍,原来 24kHz 的音乐能精确重采样上来。 */
#define AUDIO_SAMPLE_RATE    48000
#define AUDIO_CHANNELS       2
#define AUDIO_BITS           16

/* I2S + ES8311 初始化。失败返回错误码,可用 Audio_IsReady() 查询。 */
esp_err_t Audio_Init(void);

/* 打开播放通道(打开时才会拉高功放使能脚) */
esp_err_t Audio_Open(void);
esp_err_t Audio_Close(void);

/* 音量 0~100 */
esp_err_t Audio_SetVolume(int vol);
int       Audio_GetVolume(void);

/* 运行时改采样率(播 TF 卡不同采样率的 WAV 时要跟着变)。
   内部会把 DAC(以及已打开的麦克风)关掉重开,顺带重配 MCLK 分频。 */
esp_err_t Audio_SetSampleRate(uint32_t hz);
uint32_t  Audio_GetSampleRate(void);

/* 写一段 PCM 到喇叭(阻塞,数据入 DMA 即返回) */
esp_err_t Audio_PlayPcm(const void *data, size_t bytes);

/* 从麦克风读 PCM(ES7210,阻塞直到收满) */
esp_err_t Audio_ReadMic(void *buf, size_t bytes);

/* 只开麦克风通道(不播音乐时用,比如做麦克风频谱) */
esp_err_t Audio_OpenMic(void);

/* 麦克风增益(dB) */
esp_err_t Audio_SetMicGain(float db);

esp_err_t Audio_Mute(bool mute);

bool Audio_IsReady(void);
bool Audio_IsOpened(void);
bool Audio_IsMicOpen(void);

#ifdef __cplusplus
}
#endif
