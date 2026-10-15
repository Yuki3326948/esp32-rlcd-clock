/*
 * BLE 网关第一版:间歇扫描 + 解析广播 + 打日志。见 ble_gate.h 的说明。
 *
 * 关于扫描参数的两个坑(都踩过/查过源码):
 *
 * 1. ★ 不能把 itvl/window 留 0。
 *    留 0 的话 NimBLE 会用它的默认档,而默认档是
 *    BLE_GAP_SCAN_FAST_INTERVAL_MIN/MAX + FAST_WINDOW,
 *    window 等于 interval —— 也就是【100% 占空比】持续扫。
 *    那会把 WiFi 打到没脾气。必须自己显式给一个小窗口。
 *    但这里用的是"突发"而不是"小窗口常扫":见 SCAN_ITVL/SCAN_WIN 的注释。
 *
 * 2. ★ 去重必须关掉(filter_duplicates = 0)。
 *    sdkconfig 里控制器开了去重(BT_CTRL_BLE_SCAN_DUPL + SCAN_DUPL_TYPE_DEVICE,
 *    缓存 100,而 SCAN_DUPL_CACHE_REFRESH_PERIOD=0 表示【永不刷新】)。
 *    如果打开去重,同一个设备只会被报一次、而且这个"记忆"是跨扫描启停保留的,
 *    于是"扫 5 秒停 55 秒"从第二轮起就什么都收不到 —— 现象是
 *    "第一轮好好的,之后每轮都是 0 个",极难猜。
 *    关掉之后每轮都能拿到报告,重复由 host 这边自己按地址聚合成一张表。
 *    报告量有控制器侧的流控兜底(BT_CTRL_BLE_ADV_REPORT_FLOW_CTRL_SUPP,
 *    阈值 20、上限 100),不会把 host 淹掉。
 */
#include "ble_gate.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"      /* esp_get_free_internal_heap_size */

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

static const char *TAG = "blegate";

/* ============================================================
 *  参数
 * ============================================================ */
#define SCAN_ON_MS      5000     /* 一轮扫 5 秒 */
#define SCAN_OFF_MS    55000     /* 停 55 秒 -> 平均占空比约 8% */

/* 单位 0.625ms。160 = 100ms。
   窗口 = 间隔 => 扫描窗口【内】是 100% 占用,但只持续 5 秒。
   这正是本版要测的东西:这 5 秒里 WiFi 会不会抖。 */
#define SCAN_ITVL       160
#define SCAN_WIN        160

static volatile int  s_last_count = 0;
static volatile int  s_total      = 0;
static volatile bool s_scanning   = false;

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool  (*s_busy_fn)(void) = NULL;

/* ---- 临时诊断开关:把每轮前几份广播的原始字节打出来 ----
   已经查完了(2026-10):设备【真的不发名字】。
   现场看到的全是:
     0x02 0x01 0x06 | flags
     0x16 0x16 0x95 0xFE ... 小米服务数据(0xFE95)
     0x0A 0xFF 0x4C 0x00 ... Apple 厂商数据(Nearby Info)
   而扫描响应要么只有厂商数据、要么干脆是空的(len=0)。
   0x08/0x09(设备名)一个都没有 —— iPhone / Mac / 米家都故意不发。
   => 结论:名字在 BLE 网关里【不可靠】,认设备得靠 MAC OUI + 服务 UUID。
   以后要再看现场就把这个改成 1。 */
#define BLEGATE_DUMP_RAW   0
#define BLEGATE_DUMP_MAX   8
static int s_dumped = 0;

/* ★ 推流时要不要暂停扫描。
   0 = 不暂停 —— 这是【故意的】:要量“扫描和 WiFi 推流能不能共存”,
       扫描就必须在推流时真的跑起来,否则量到的是假的。
       busy 状态会打在每轮的汇总日志里,方便把卡顿和扫描窗口对上。
   1 = 暂停 —— 三方共存里最省事的一招:听音乐时零干扰。
   等实测结论出来再决定定哪个。 */
#define BLEGATE_PAUSE_WHEN_BUSY  0

/* 表本身只有 ~700 字节,但按项目惯例还是放 PSRAM:
   内部 RAM 只剩 80KB 出头,能省就省。 */
EXT_RAM_BSS_ATTR static BleGateDev s_dev[BLEGATE_MAX_DEV];

/* ============================================================
 *  设备表
 * ============================================================ */
static void AddrStr(const uint8_t *val, char *out, size_t cap)
{
    /* NimBLE 里 val[0] 是最低位字节,打印时反过来 */
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             val[5], val[4], val[3], val[2], val[1], val[0]);
}

/* 按地址找;找不到返回第一个空位;表满返回 NULL */
static BleGateDev *FindOrAdd(const uint8_t *val)
{
    BleGateDev *empty = NULL;
    for (int i = 0; i < BLEGATE_MAX_DEV; i++) {
        if (!s_dev[i].valid) {
            if (empty == NULL) {
                empty = &s_dev[i];
            }
            continue;
        }
        if (memcmp(s_dev[i].val, val, 6) == 0) {
            return &s_dev[i];
        }
    }
    return empty;
}

/* ============================================================
 *  GAP 事件
 * ============================================================ */
static int DiscEvent(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_scanning = false;
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    struct ble_gap_disc_desc *d = &event->disc;

    BleGateDev *e = FindOrAdd(d->addr.val);
    if (e == NULL) {
        return 0;                       /* 表满,后面的直接不管 */
    }

    bool is_new   = !e->valid;
    bool had_name = !is_new && (e->name[0] != '\0');

    if (is_new) {
        memset(e, 0, sizeof(*e));
        memcpy(e->val, d->addr.val, 6);
        e->valid = true;
    }
    e->seen++;
    e->rssi = d->rssi;

    /* 解析广播内容。
       ★ fields 里的指针直接指在 d->data 里面(不拷贝),
         所以名字必须【当场】抄出来 —— 事件返回后那块内存就无效了。 */
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) == 0 &&
        fields.name != NULL && fields.name_len > 0) {
        size_t n = fields.name_len;
        if (n > BLEGATE_NAME_MAX) {
            n = BLEGATE_NAME_MAX;
        }
        memcpy(e->name, fields.name, n);
        e->name[n] = '\0';
    }

#if BLEGATE_DUMP_RAW
    if (s_dumped < BLEGATE_DUMP_MAX) {
        char hex[3 * 40 + 4];
        int  hn  = 0;
        int  len = (d->length_data > 40) ? 40 : d->length_data;
        hex[0] = '\0';
        for (int i = 0; i < len; i++) {
            hn += snprintf(hex + hn, sizeof(hex) - (size_t)hn,
                           "%02X ", d->data[i]);
        }
        ESP_LOGI(TAG, "  [raw] evt=%u len=%u  %s",
                 (unsigned)d->event_type, (unsigned)d->length_data, hex);
        s_dumped++;
    }
#endif

    /* 每个设备每轮只打一行,不然广播密集时日志会刷屏。
       ★ 但名字往往【不在】第一份报告里 —— 普通广播常常不带名字,
         名字在之后单独的扫描响应里。所以还要补一条“名字来了”的日志,
         否则永远只看到“(无名)”,会以为是解析没生效。 */
    if (is_new) {
        char addr[20];
        AddrStr(e->val, addr, sizeof(addr));
        ESP_LOGI(TAG, "  发现 %s  RSSI=%d  \"%s\"",
                 addr, (int)d->rssi, e->name[0] ? e->name : "(无名)");
    } else if (!had_name && e->name[0] != '\0') {
        char addr[20];
        AddrStr(e->val, addr, sizeof(addr));
        ESP_LOGI(TAG, "  %s 名字补上了: \"%s\"", addr, e->name);
    }
    return 0;
}

/* ============================================================
 *  扫描任务
 * ============================================================ */
static void GateTask(void *arg)
{
    (void)arg;

    /* 等协议栈同步(ble_ctrl 也在等,谁先到都行) */
    while (!ble_hs_synced()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.itvl              = SCAN_ITVL;
    p.window            = SCAN_WIN;
    p.passive           = 0;   /* 主动扫描:发 SCAN_REQ 换 SCAN_RSP,设备名在里面 */
    p.filter_duplicates = 0;   /* 见文件头 ★2 */
    p.filter_policy     = 0;
    p.limited           = 0;

    ESP_LOGI(TAG, "BLE 网关就绪:每 %d 秒扫 %d 秒(平均占空比约 %d%%)",
             (SCAN_ON_MS + SCAN_OFF_MS) / 1000, SCAN_ON_MS / 1000,
             SCAN_ON_MS * 100 / (SCAN_ON_MS + SCAN_OFF_MS));

    uint32_t round = 0;

    for (;;) {
        /* 忙不忙只记下来,要不要因此跳过往后看 BLEGATE_PAUSE_WHEN_BUSY 的注释 */
        bool busy = (s_busy_fn != NULL && s_busy_fn());
#if BLEGATE_PAUSE_WHEN_BUSY
        if (busy) {
            ESP_LOGI(TAG, "第 %u 轮跳过(正在推流,不抢射频)",
                     (unsigned)(round + 1));
            vTaskDelay(pdMS_TO_TICKS(SCAN_OFF_MS));
            continue;
        }
#endif

        for (int i = 0; i < BLEGATE_MAX_DEV; i++) {
            s_dev[i].seen = 0;         /* 本轮计数清零,valid 保留 */
        }
        s_dumped = 0;                  /* 临时诊断:每轮重新 dump */

        int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p,
                              DiscEvent, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "启动扫描失败: %d,等一轮再来", rc);
            vTaskDelay(pdMS_TO_TICKS(SCAN_OFF_MS));
            continue;
        }
        s_scanning = true;
        round++;

        vTaskDelay(pdMS_TO_TICKS(SCAN_ON_MS));

        ble_gap_disc_cancel();         /* 会触发 DISC_COMPLETE */
        s_scanning = false;

        int n = 0, tot = 0;
        for (int i = 0; i < BLEGATE_MAX_DEV; i++) {
            if (!s_dev[i].valid) {
                continue;
            }
            tot++;
            if (s_dev[i].seen > 0) {
                n++;
            }
        }
        s_last_count = n;
        s_total      = tot;

        ESP_LOGI(TAG, "第 %u 轮: 本轮 %d 个设备(累计见过 %d 个)%s",
                 (unsigned)round, n, tot,
                 busy ? "   ← 本轮和推流重叠" : "");

        vTaskDelay(pdMS_TO_TICKS(SCAN_OFF_MS));
    }
}

/* ============================================================
 *  对外接口
 * ============================================================ */
esp_err_t BleGate_Start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(GateTask, "blegate", 3072,
                                           NULL, 2, NULL, 0);
    if (ok != pdPASS) {
        /* 任务栈只能从内部 RAM 切 —— 创建失败几乎一定是内存不够。
           把余量打出来,别让它静悄悄地不工作(本项目有过这个教训)。 */
        ESP_LOGE(TAG, "扫描任务创建失败!内部 RAM 只剩 %u 字节",
                 (unsigned)esp_get_free_internal_heap_size());
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int BleGate_LastCount(void)
{
    return s_last_count;
}

int BleGate_TotalSeen(void)
{
    return s_total;
}

bool BleGate_Scanning(void)
{
    return s_scanning;
}

const BleGateDev *BleGate_Devices(int *n)
{
    if (n != NULL) {
        int c = 0;
        for (int i = 0; i < BLEGATE_MAX_DEV; i++) {
            if (s_dev[i].valid) {
                c++;
            }
        }
        *n = c;
    }
    return s_dev;
}

void BleGate_SetBusyCallback(bool (*fn)(void))
{
    s_busy_fn = fn;
}
