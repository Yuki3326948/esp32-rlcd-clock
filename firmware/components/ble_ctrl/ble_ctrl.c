#include "ble_ctrl.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

#define DEV_NAME            "ESP32-RLCD"
#define UUID_SVC            0xAB00
#define UUID_CHR_CTRL       0xAB01
#define UUID_CHR_VOL        0xAB02
#define UUID_CHR_STATUS     0xAB03

static BleCallbacks s_cb;
static uint8_t      s_addr_type = 0;
static uint16_t     s_conn      = BLE_HS_CONN_HANDLE_NONE;
static uint16_t     s_h_status  = 0;      /* 0xAB03 的 value handle,notify 要用 */
static uint8_t      s_last[8]   = {0xFF}; /* 上一次推过的内容,用于去重 */
static char         s_peer[20]  = "";

/* ============================================================
 *  特征值读写
 * ============================================================ */
static int ChrCtrl(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (OS_MBUF_PKTLEN(ctxt->om) < 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t cmd = 0;
    ble_hs_mbuf_to_flat(ctxt->om, &cmd, sizeof(cmd), NULL);
    ESP_LOGI(TAG, "收到控制命令 %u", (unsigned)cmd);

    if (s_cb.on_control != NULL) {
        s_cb.on_control(cmd);
    }
    return 0;
}

static int ChrVol(uint16_t conn, uint16_t attr,
                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t vol = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) < 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ble_hs_mbuf_to_flat(ctxt->om, &vol, sizeof(vol), NULL);
        if (vol > 100) {
            vol = 100;
        }
        ESP_LOGI(TAG, "设置音量 %u", (unsigned)vol);
        if (s_cb.on_volume != NULL) {
            s_cb.on_volume(vol);
        }
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t vol = (s_cb.get_volume != NULL) ? s_cb.get_volume() : 0;
        return os_mbuf_append(ctxt->om, &vol, sizeof(vol)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ChrStatus(uint16_t conn, uint16_t attr,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    BleStatus st = {0};
    if (s_cb.get_status != NULL) {
        s_cb.get_status(&st);
    }

    uint8_t buf[8] = {0};
    buf[0] = (uint8_t)((st.playing    ? 0x01 : 0) |
                       (st.mic_source ? 0x02 : 0) |
                       (st.clock_page ? 0x04 : 0) |
                       (st.use_24h    ? 0x08 : 0));
    buf[1] = st.track;
    buf[2] = st.track_cnt;
    buf[3] = (uint8_t)st.temp_c;
    buf[4] = st.humi;

    return os_mbuf_append(ctxt->om, buf, sizeof(buf)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ============================================================
 *  服务定义
 * ============================================================ */
static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid       = BLE_UUID16_DECLARE(UUID_CHR_CTRL),
        .access_cb  = ChrCtrl,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid       = BLE_UUID16_DECLARE(UUID_CHR_VOL),
        .access_cb  = ChrVol,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid       = BLE_UUID16_DECLARE(UUID_CHR_STATUS),
        .access_cb  = ChrStatus,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h_status,
    },
    { 0 }                                  /* 数组结尾 */
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(UUID_SVC),
        .characteristics = s_chrs,
    },
    { 0 }
};

/* ============================================================
 *  广播 / 连接
 * ============================================================ */
static int GapEvent(struct ble_gap_event *event, void *arg);   /* 下面定义 */

static void AdvStart(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params adv  = {0};

    fields.flags     = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name      = (uint8_t *)DEV_NAME;
    fields.name_len  = (uint8_t)strlen(DEV_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播内容失败: %d", rc);
        return;
    }

    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv,
                           GapEvent, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "开始广播失败: %d", rc);
    } else {
        ESP_LOGI(TAG, "开始广播,设备名 \"%s\"", DEV_NAME);
    }
}

static int GapEvent(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(s_conn, &d) == 0) {
                snprintf(s_peer, sizeof(s_peer),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         d.peer_ota_addr.val[5], d.peer_ota_addr.val[4],
                         d.peer_ota_addr.val[3], d.peer_ota_addr.val[2],
                         d.peer_ota_addr.val[1], d.peer_ota_addr.val[0]);
            }
            ESP_LOGI(TAG, "已连接: %s", s_peer);
        } else {
            AdvStart();                    /* 没连上就接着广播 */
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开(原因 %d),重新广播",
                 event->disconnect.reason);
        s_conn    = BLE_HS_CONN_HANDLE_NONE;
        s_last[0] = 0xFF;                  /* 强制下次重推一次状态 */
        AdvStart();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        AdvStart();
        return 0;

    default:
        return 0;
    }
}

/* ============================================================
 *  协议栈启动
 * ============================================================ */
static void OnReset(int reason)
{
    ESP_LOGW(TAG, "协议栈复位: %d", reason);
}

static void OnSync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    AdvStart();
}

static void HostTask(void *param)
{
    (void)param;
    nimble_port_run();                     /* 一直跑到 nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t Ble_Init(const BleCallbacks *cb)
{
    if (cb != NULL) {
        s_cb = *cb;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = OnSync;
    ble_hs_cfg.reset_cb = OnReset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEV_NAME);

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "服务计数失败: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册服务失败: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(HostTask);

    ESP_LOGI(TAG, "BLE 就绪  服务=0x%04X  控制=0x%04X  音量=0x%04X  状态=0x%04X",
             UUID_SVC, UUID_CHR_CTRL, UUID_CHR_VOL, UUID_CHR_STATUS);
    return ESP_OK;
}

bool Ble_IsConnected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

const char *Ble_GetPeer(void)
{
    return s_peer;
}

void Ble_Update(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_h_status == 0) {
        return;
    }

    BleStatus st = {0};
    if (s_cb.get_status != NULL) {
        s_cb.get_status(&st);
    }

    uint8_t buf[8] = {0};
    buf[0] = (uint8_t)((st.playing    ? 0x01 : 0) |
                       (st.mic_source ? 0x02 : 0) |
                       (st.clock_page ? 0x04 : 0) |
                       (st.use_24h    ? 0x08 : 0));
    buf[1] = st.track;
    buf[2] = st.track_cnt;
    buf[3] = (uint8_t)st.temp_c;
    buf[4] = st.humi;

    if (memcmp(buf, s_last, sizeof(buf)) == 0) {
        return;                            /* 没变就不发,省电也省手机流量 */
    }
    memcpy(s_last, buf, sizeof(buf));

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn, s_h_status, om);
    }
}
