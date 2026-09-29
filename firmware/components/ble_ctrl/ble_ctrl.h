/*
 * BLE 控制接口(手机 <-> 板子)
 *
 * 芯片限制:S3 只有 BLE,**没有经典蓝牙**。所以 iPhone 的
 * 「设置 → 蓝牙」里不会出现这块板子 —— 那是 iOS 的设计,
 * 它只列经典设备和已配对的 BLE 配件。
 * 用 nRF Connect(或 LightBlue)这类 App 扫描、连接、读写即可。
 *
 * 服务结构:
 *   0xAB00  服务
 *     0xAB01  控制  WRITE  1 字节,见下面的 BleCmd
 *     0xAB02  音量  READ/WRITE  1 字节 0~100
 *     0xAB03  状态  READ/NOTIFY 8 字节,见 BleStatus
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0xAB01 写入的字节 */
enum {
    BLE_CMD_TOGGLE_PLAY = 0,   /* 播放 / 暂停 */
    BLE_CMD_NEXT        = 1,   /* 下一首 */
    BLE_CMD_PREV        = 2,   /* 上一首 */
    BLE_CMD_SWITCH_PAGE = 3,   /* 切页(时钟 <-> 音乐) */
    BLE_CMD_SWITCH_SRC  = 4,   /* 切换频谱源(音乐 / 麦克风) */
};

/* 0xAB03 的 8 个字节 */
typedef struct {
    uint8_t playing;      /* 1 = 正在播放          */
    uint8_t mic_source;   /* 1 = 频谱源是麦克风    */
    uint8_t clock_page;   /* 1 = 当前是时钟页      */
    uint8_t use_24h;      /* 1 = 24 小时制         */
    uint8_t track;        /* 当前第几首(1 起;0 = 无) */
    uint8_t track_cnt;    /* 总共有几首            */
    int8_t  temp_c;       /* 温度 ℃(整数)         */
    uint8_t humi;         /* 湿度 %                */
} BleStatus;

typedef struct {
    /* 手机写了 0xAB01 时回调(cmd 是 BleCmd) */
    void (*on_control)(uint8_t cmd);
    /* 手机写了 0xAB02 时回调 */
    void (*on_volume)(uint8_t vol);
    /* 读当前音量 */
    uint8_t (*get_volume)(void);
    /* 读当前状态,Ble_Update() 会调用 */
    void (*get_status)(BleStatus *out);
} BleCallbacks;

/* 初始化协议栈并开始广播。不成功也不影响其它功能。 */
esp_err_t Ble_Init(const BleCallbacks *cb);

/* 内容变了就把状态推给手机(没连手机时什么都不做)。
   放在主循环里随便调,内部自己比对,一样就不发。 */
void Ble_Update(void);

bool Ble_IsConnected(void);
/* 当前连接用的蓝牙地址(MAC),没连上返回空串 */
const char *Ble_GetPeer(void);

#ifdef __cplusplus
}
#endif
