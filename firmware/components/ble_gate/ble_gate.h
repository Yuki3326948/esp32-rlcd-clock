/*
 * BLE 网关(第一版:只扫描,不连接)
 *
 * ★ 这一版的目的【不是功能,是验证】:
 *   回答一个问题 —— 把 BLE 扫描开起来之后,WiFi 音频推流还稳不稳。
 *
 *   所以它只做三件事:间歇扫描、解析广播、打日志。
 *   不建立连接、不解析任何具体传感器的格式(那是下一版的事)。
 *
 * 射频占用靠三件事控制:
 *   1. 突发扫描:开 5 秒 / 停 55 秒(平均约 8%),不是低占空比常扫 ——
 *      BLE 传感器每 1~2 秒广播一次,5 秒的窗口足够抓到,而且抓得干净。
 *   2. 广播间隔从 NimBLE 默认的 30ms 调到 300ms(改在 ble_ctrl.c)。
 *   3. 留了"忙时暂停"的钩子:BleGate_SetBusyCallback()。
 *      注册进去的函数返回 true 时,本轮扫描直接跳过、不占射频。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLEGATE_MAX_DEV   24
#define BLEGATE_NAME_MAX  20

typedef struct {
    uint8_t val[6];                        /* NimBLE 原始顺序(val[0] 是最低位字节) */
    char    name[BLEGATE_NAME_MAX + 1];    /* 没解析到名字就是空串 */
    int8_t  rssi;                          /* 最近一次收到的信号强度 */
    uint8_t seen;                          /* 本轮收到几次广播 */
    bool    valid;                         /* 表项被占用 */
} BleGateDev;

/* 起扫描任务。非阻塞,失败也不影响其它功能。 */
esp_err_t BleGate_Start(void);

/* 上一轮扫到几个设备 */
int BleGate_LastCount(void);
/* 累计见过几个不同的设备 */
int BleGate_TotalSeen(void);
/* 此刻在不在扫描窗口内(用来核对占空比) */
bool BleGate_Scanning(void);

/* 只读的设备表;*n 返回有效的项数。表里 rssi/seen 每轮更新。 */
const BleGateDev *BleGate_Devices(int *n);

/* 注册"现在忙不忙"的回调。返回 true 时跳过本轮扫描。
   典型用法:注册 NetAudio_IsStreaming —— 听音乐时不抢射频。 */
void BleGate_SetBusyCallback(bool (*fn)(void));

#ifdef __cplusplus
}
#endif
