#pragma once

/*
 * 板载传感器驱动(ESP32-S3-RLCD-4.2)
 *   总线: I2C0  SCL=GPIO14  SDA=GPIO13
 *   SHTC3    @0x70  温湿度
 *   PCF85063 @0x51  RTC 实时时钟
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 I2C 总线并挂载两个从设备 */
esp_err_t Sensors_Init(void);

/* 取出 I2C 总线句柄 —— 音频编解码器 ES8311(0x18)/ES7210 挂在同一条总线上 */
i2c_master_bus_handle_t Sensors_GetI2cBus(void);

/* 扫一遍 I2C 总线并把应答的地址打到日志。
   调试用:能一眼看出板子上到底挂了哪些芯片(比如确认有没有触摸芯片)。 */
void Sensors_ScanBus(void);

/* ---------------- SHTC3 温湿度 ---------------- */
/* 读取成功返回 true,结果写入 temp_c(℃)/ humi_pct(%RH) */
bool Sensors_ReadTempHumi(float *temp_c, float *humi_pct);

/* ---------------- PCF85063 RTC ---------------- */
/* RTC 晶振是否在走(掉电后为 false,说明时间不可信) */
bool Rtc_IsRunning(void);

/* 读 RTC(存的是本地时间,即东八区) */
bool Rtc_Read(struct tm *out);

/* 写 RTC */
bool Rtc_Write(const struct tm *t);

/* RTC -> 系统时间(开机时先拿到大致时间,不用等 NTP) */
bool Rtc_LoadToSystem(void);

/* 系统时间 -> RTC(NTP 对时成功后回写,断电重启也能走时) */
bool Rtc_SyncFromSystem(void);

#ifdef __cplusplus
}
#endif
