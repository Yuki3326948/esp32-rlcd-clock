/*
 * 电池电压检测
 *
 * 硬件: 18650 -> 1/3 分压 -> GPIO4(ESP32-S3 上 GPIO4 = ADC1 通道 3)
 *       满电 4.2V 分压后 1.4V,落在 ADC 12dB 量程(约 0~3.1V)内。
 *
 * 电压换算电量沿用官方示例的直线: (V - 3.0) / 1.12,
 * 低于 3.0V 记 0%,高于 4.12V 记 100%。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t Battery_Init(void);

/* 读一次并缓存。内部多采几次取平均(ADC 单次读数能差几十 mV),
 * 所以别每帧都调 —— 3 秒左右一次足够。 */
esp_err_t Battery_Poll(void);

/* 是否接着电池。纯 Type-C 供电时(没装电池)分压点电压很低,据此判断。 */
bool  Battery_HasBattery(void);

float Battery_GetVoltage(void);   /* 伏,例 3.85 */
int   Battery_GetLevel(void);     /* 0~100 */

/* 供电 / 充电状态。
 *
 * 这块板子【没有把充电 IC 的状态脚接到 GPIO】(官方文档、官方例程、
 * 综合示例都只读电压一个信号),所以“外部电源”靠两个依据判断:
 *   1. USB 主机连着(usb_serial_jtag_is_connected)—— 插电脑时立刻就知道,
 *      这一条是可靠的;
 *   2. 插充电头时没有 USB 握手,只能看电压趋势 —— 纯电池供电时这个负载
 *      (屏幕 + WiFi)一定在持续掉压,所以“不降”就当作外接。
 * 第 2 条是推断,约 1 分钟才收敛,而且极端情况下可能不准。
 */
typedef enum {
    BATTERY_POWER_UNKNOWN = 0,    /* 还没读够数据 */
    BATTERY_POWER_INTERNAL,       /* 内部电池供电 */
    BATTERY_POWER_CHARGING,       /* 外接电源,充电中 */
    BATTERY_POWER_FULL,           /* 外接电源,已充满 */
    BATTERY_POWER_EXTERNAL_ONLY,  /* 只有外接电源,没装电池 */
} Battery_Power;

Battery_Power Battery_GetPower(void);

#ifdef __cplusplus
}
#endif
