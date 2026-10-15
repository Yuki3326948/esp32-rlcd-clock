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
 *   78 秒 ÷ 450ms ≈ 173 个点,每点 13 字节 ≈ 2.2 KB。
 *   (以前存 3 小时 = 274 KB,但曲线页只看 78 秒,多出来的纯属白占。)
 *   注意这跟 TF 卡上的 CSV 日志是两码事:那边保留 30 天,不受这里影响。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSMON_PORT       3334      /* PC(temp_widget)推的资源数据 */
#define SYSMON_RT_PORT    3335      /* 路由器推的 WAN 流量 */
/*
 * 历史缓冲保留多久 —— 只要够装曲线页要显示的那 78 秒就行(见 user_app
 * 的 SYS_WINDOW_S,两边是配套的:改那个就要一起看这里)。
 *
 * ★ 用【毫秒数 ÷ 最快采样间隔】反推点数,不直接写死 4000:
 *   写死的话,PC 那边一旦推慢一点(实测 472~510ms 一直在飘),
 *   78 秒就装不下了,曲线左端会莫名其妙地短一截。
 *   留 450ms 的余量,是因为 s_interval_ms 是实测的平均值,偶尔会更低。
 *   (真低到 450 以下也不会出错,只是窗口被截短、X 轴刻度照实写少一点)
 */
#define SYSMON_SAMPLE_MS_MIN  450       /* 实测最快采样间隔(temp_widget 每 0.5s 推一次) */
#define SYSMON_HIST_MAX  (78 * 1000 / SYSMON_SAMPLE_MS_MIN)    /* 173 点 x 13B ≈ 2.2KB */

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
    unsigned down_kbps, up_kbps;    /* PC 网卡速率(仍是 PC 那条流报的) */

    /* ---- 路由器(WAN)的数据,走 3335 ----
       为什么不用 PC 的网速:PC 网卡只看得见这台机器,而且内网 2.5G
       的流量也混在里面。路由器 WAN 才是"整栋房子出去多少"的真值,
       而且路由器 7x24 开着,PC 关了也照样有数据。
       口径 = pppoe-wan 自拨号以来累计,重拨/重启会归零。
       推它的脚本:路由器上 /usr/bin/router_traffic.sh */
    bool     rt_ok;             /* 路由器正在推,且数据没超时(10 秒) */
    unsigned rt_dn_mib, rt_up_mib;   /* 累计下行 / 上行,MiB */
    unsigned rt_ds_kbps, rt_us_kbps; /* 实时下行 / 上行,KiB/s */

    /* ---- 天气(和上面那条 3335 流一起推来,10 分钟才变一次)----
       为什么让路由器取、不让板子自己取:
         板子自己取要么走 TLS —— 内部 RAM 只剩 80 多 KB,而 TLS 握手是
         十几 KB 的尖峰,推流时 WiFi 还会再吃掉 30KB;要么走明文 HTTP,
         那要看服务商支不支持。路由器 7x24 在线、有 curl+OpenSSL、4GB 内存,
         而且这条 3335 通道本来就是它在推 —— 板子只要多解析两个字段,
         零网络开销、零内存开销。
       ★ 路由器上跑着 OpenClash 全局代理,所以它【不能】直接问"我在哪"
         (会按代理出口回答,实测答的是美国纽约州锡拉丘兹)。它是先本地读出
         pppoe-wan 的 IP,再查"这个 IP 在哪",这样跟代理无关。
         详见 pc-tools/router-push/router_traffic.sh 的头注释。 */
    bool     wx_ok;             /* 收到过 OTEMP 才算有数据 */
    int      out_temp;          /* 室外温度 °C(路由器那边已四舍五入) */
    int      wmo_code;          /* WMO 天气码:0=晴 1,2=少云多云 3=阴
                                   45,48=雾 51~57=毛毛雨 61~67,80~82=雨
                                   71~77,85,86=雪 95,96,99=雷 */

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

/*
 * 只把【最新 window 个原始点】压成 n 个点。
 * 曲线页要"只显示最近 78 秒"就是靠它:window = 78s / 采样间隔。
 * window <= 0 或比现有历史还长时,结果和 Sysmon_GetDecimated 一样。
 */
int Sysmon_GetDecimatedWindow(SysSample_t *out, int n, int window);

#ifdef __cplusplus
}
#endif
