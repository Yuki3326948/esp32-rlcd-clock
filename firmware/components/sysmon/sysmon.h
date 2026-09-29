/*
 * sysmon —— 接收 PC 推来的资源遥测,并保留最近一段历史给曲线页用。
 *
 * 数据来自 PC 上那个任务栏温度小工具(temp_widget),每 0.5 秒推一行:
 *     HOST=ZXS CPU=12 RAM=34 RAMU=16941 RAMT=32609 CPUT=27 GPU=3 GPUT=52
 *     VRAMU=1627 VRAMT=6144 NETRX=12 NETTX=3
 *
 * 存储(关键设计):
 *   环形缓冲,固定 SYSMON_HIST_MAX 个点,存在 PSRAM 里。
 *   ★ 空间是【构造上有界】的 —— PC 推得再快、推多久,占用都不变。
 *   0.5 秒 × 3 小时 = 21600 个点,每点 13 字节 ≈ 281 KB,
 *   8MB PSRAM 里完全无压力。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSMON_PORT       3334
#define SYSMON_HIST_MAX   21600     /* 0.5 秒 x 3 小时 */

/* 一个采样点。紧凑排布,13 字节 —— 曲线页只需要这些。 */
typedef struct __attribute__((packed)) {
    uint8_t  cpu;      /* CPU 占用 %     */
    uint8_t  gpu;      /* GPU 占用 %     */
    uint8_t  ram;      /* 内存占用 %     */
    uint8_t  cput;     /* CPU 温度 °C,0 = 无数据 */
    uint8_t  gput;     /* GPU 温度 °C,0 = 无数据 */
    /* 网速用 32 位:16 位上限只有 65535 KiB/s = 64 MB/s,
       2.5G 内网跑满时会被截成一个假的平顶 */
    uint32_t down;     /* 下行 KiB/s     */
    uint32_t up;       /* 上行 KiB/s     */
} SysSample_t;

/* 最新一帧的完整信息(含历史里没存的那些) */
typedef struct {
    bool     connected;         /* PC 是否连着 */
    char     host[24];          /* PC 主机名   */
    int      cpu, gpu, ram;     /* %,-1 = 无数据     */
    int      cput, gput;        /* °C,-1 = 无数据    */
    unsigned ram_used_mb, ram_total_mb;
    unsigned vram_used, vram_total;
    unsigned down_kbps, up_kbps;
    int      hist_count;        /* 历史里现有多少个点 */
    int      sample_ms;         /* 实测采样间隔(毫秒),用来算时间跨度 */
} SysStatus_t;

esp_err_t Sysmon_Start(void);

bool   Sysmon_IsConnected(void);
void   Sysmon_GetStatus(SysStatus_t *out);
const  char *Sysmon_GetPeer(void);

/*
 * 把整段历史压缩成 n 个点(每段取平均),按时间顺序(旧的在前)。
 * 不管历史里有几千个点,曲线页只画 n 个 —— 这样画图代码和
 * 历史长度完全解耦。
 * 返回实际填了几个点(0 = 还没数据)。
 */
int Sysmon_GetDecimated(SysSample_t *out, int n);

#ifdef __cplusplus
}
#endif
