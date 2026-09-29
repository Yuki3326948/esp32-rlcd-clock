/*
 * TF 卡(SDMMC 1-bit)挂载
 *
 * 硬件: 4.2 寸 RLCD 板上的 TF 座,只走 1 根数据线
 *       CLK=GPIO38  CMD=GPIO21  D0=GPIO39
 *       (ESP32-S3-WROOM-1 N16R8 的 GPIO33~37 被 8 位 PSRAM 占了,别碰)
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 本文件会被 C++ 的 user_app.cpp include,但实现是 C 编的,
   不加这段链接时找不到符号(name mangling 对不上)。 */
#ifdef __cplusplus
extern "C" {
#endif

/* FATFS 挂载点,访问文件就写 "/sdcard/xxx.wav" */
#define SDCARD_MOUNT_POINT  "/sdcard"

esp_err_t Sdcard_Mount(void);
void      Sdcard_Unmount(void);
bool      Sdcard_IsMounted(void);

/* 调试用:把目录内容打到日志里,方便确认卡和文件都在 */
void      Sdcard_List(const char *dir, int max_items);

#ifdef __cplusplus
}
#endif
