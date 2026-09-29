/*
 * 从 TF 卡读 WAV 播放
 *
 * 只吃「16-bit PCM」的 WAV —— 这是最常见的一种,也是嵌入音乐用的格式。
 * 8/24/32-bit 和压缩过的 WAV(ADPCM 之类)会明确拒绝并打日志,
 * 免得播出来是噪声还找不到原因。
 *
 * 单声道会自动复制成双声道,因为 I2S 是按立体声配的。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;       /* 原文件的声道数(1 或 2) */
    uint16_t bits;           /* 固定 16 */
    uint32_t data_bytes;     /* PCM 数据长度 */
    uint32_t duration_ms;
} WavInfo;

/* 打开一个 WAV。失败时日志里会说清是哪一条不满足。 */
esp_err_t Wav_Open(const char *path);
void      Wav_Close(void);
bool      Wav_IsOpen(void);

const WavInfo *Wav_GetInfo(void);
const char    *Wav_GetPath(void);

/* 读一段 PCM,输出一定是 16-bit 立体声。返回实际写入的字节数,0 = 读完了。 */
size_t Wav_Read(void *buf, size_t bytes);

/* 回到开头,循环播放用 */
esp_err_t Wav_Rewind(void);

/* ---------------- 目录扫描 ---------------- */
#define WAV_MAX_FILES   32
#define WAV_PATH_LEN    96

/* 扫一层目录里所有 .wav,返回找到的数量(内部记着清单) */
int         Wav_ScanDir(const char *dir);
int         Wav_FileCount(void);
const char *Wav_FilePath(int idx);    /* 完整路径,喂给 Wav_Open */
const char *Wav_FileName(int idx);    /* 只有文件名,显示用 */

#ifdef __cplusplus
}
#endif
