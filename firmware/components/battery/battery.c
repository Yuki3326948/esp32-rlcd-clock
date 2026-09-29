#include "battery.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "battery";

#define BATT_ADC_UNIT      ADC_UNIT_1
#define BATT_ADC_CHANNEL   ADC_CHANNEL_3   /* ESP32-S3: CH3 = GPIO4 */
#define BATT_DIVIDER       3               /* 板载 1/3 分压 */
#define BATT_SAMPLES       16              /* 取平均的采样次数 */

#define BATT_NONE_MV       2500            /* 低于此值认为没装电池 */
#define BATT_EMPTY_MV      3000
#define BATT_FULL_MV       4120

/* 趋势判定的窗口。Poll() 由 SensorTask 每 3 秒调一次,10 次 = 30 秒。 */
#define BATT_TREND_POLLS   10
#define BATT_FALL_MV       8               /* 30 秒内降超过 8mV 就算在放电 */

/* 一阶低通。ADC 读数有 ±10mV 抖动,不滤的话百分比会在 70/71 之间
   来回跳,顶栏每 3 秒白重画一次。右移 3 位 = 时间常数约 8 次采样 ≈ 24 秒;
   真实变化(放电约 50mV/分钟)比这慢得多,滤掉不影响趋势判断。 */
#define BATT_LP_SHIFT      3

/* 百分比迟滞:换档要离开上次换档点 12mV(≈1%)才认 */
#define BATT_LEVEL_HYS_MV  12

static adc_oneshot_unit_handle_t s_adc   = NULL;
static adc_cali_handle_t         s_cali  = NULL;
static bool                      s_ready = false;

static float         s_volts = 0.0f;
static bool          s_has   = false;
static int           s_level = 0;
static Battery_Power s_power = BATTERY_POWER_UNKNOWN;

/* 电压趋势:拿当前值和一分钟前的参考值比。
   纯电池供电时屏幕 + WiFi 这个负载一定在持续掉压,
   所以“没在降”就是接在外部电源上。 */
static int s_ref_mv    = 0;
static int s_ref_cnt   = 0;
static int s_mv_filt   = 0;   /* 低通后的电压 mV */
static int s_trend     = 0;   /* 最近一个完整窗口的变化量 mV */
static int s_windows   = 0;   /* 已经走完的趋势窗口数 */
static int s_level_ref = 0;   /* 上次换档时的电压 mV(迟滞用) */

esp_err_t Battery_Init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BATT_ADC_UNIT };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC 单元初始化失败");
        return ESP_FAIL;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    if (adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败");
        return ESP_FAIL;
    }

    /* 曲线拟合校准能把原始值换算成 mV。芯片不支持就退回线性估算。 */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BATT_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "没有曲线拟合校准,电压只能粗略估算");
    }

    s_ready = true;
    ESP_LOGI(TAG, "电池检测就绪 (GPIO4 / ADC1_CH3 / 1:%d 分压)", BATT_DIVIDER);
    return ESP_OK;
}

esp_err_t Battery_Poll(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    int mv_sum = 0;
    int n      = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (s_cali) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
                continue;
            }
        } else {
            mv = raw * 3100 / 4095;        /* 12dB 量程约 3.1V */
        }
        mv_sum += mv;
        n++;
    }
    if (n == 0) {
        return ESP_FAIL;
    }

    /* 先还原成电池端电压再算,少一次取整损失 */
    int batt_mv = mv_sum * BATT_DIVIDER / n;

    /* 一阶低通。除法要四舍五入,否则差值小于 8 时会被整除成 0 卡住不动。 */
    if (s_mv_filt == 0) {
        s_mv_filt = batt_mv;
    } else {
        int d = batt_mv - s_mv_filt;
        s_mv_filt += (d >= 0)
                   ? ((d + (1 << (BATT_LP_SHIFT - 1))) >> BATT_LP_SHIFT)
                   : -(((-d) + (1 << (BATT_LP_SHIFT - 1))) >> BATT_LP_SHIFT);
    }
    int mv = s_mv_filt;

    s_volts = 0.001f * (float)mv;
    s_has   = (mv >= BATT_NONE_MV);

    int lvl;
    if (mv <= BATT_EMPTY_MV) {
        lvl = 0;
    } else if (mv >= BATT_FULL_MV) {
        lvl = 100;
    } else {
        lvl = (mv - BATT_EMPTY_MV) * 100 / (BATT_FULL_MV - BATT_EMPTY_MV);
    }

    /* 百分比迟滞。电压正好卡在某个整数百分比的边界上时(实测 3.810V
       就是 72/71 的分界),低通后剩下的 1~2mV 纹波还是会来回跨线,
       顶栏就每 3 秒重画一次。所以换档要离开上次换档点 12mV 才认。 */
    if (lvl != s_level) {
        int d = mv - s_level_ref;
        if (d < 0) {
            d = -d;
        }
        if (s_level_ref == 0 || d >= BATT_LEVEL_HYS_MV) {
            s_level     = lvl;
            s_level_ref = mv;
        }
    }

    /* ---------------- 供电 / 充电状态 ---------------- */
    /* 依据一:USB 主机连着。插电脑时立刻为真,这条最可靠。
       插充电头(不枚举数据)不会为真,那种情况靠下面的趋势。 */
    bool usb_host = usb_serial_jtag_is_connected();

    /* 依据二:电压趋势。每 30 秒结一次账,没降就当作外接。 */
    if (++s_ref_cnt >= BATT_TREND_POLLS) {
        s_ref_cnt = 0;
        if (s_ref_mv != 0) {
            s_trend = mv - s_ref_mv;
            s_windows++;
            ESP_LOGI(TAG, "电压趋势: %d -> %d mV (%+d / %d 秒)%s",
                     s_ref_mv, mv, s_trend,
                     BATT_TREND_POLLS * 3,
                     usb_host ? "  [USB 主机已连接]" : "");
        }
        s_ref_mv = mv;
    }
    /* 还没走完一个完整窗口之前不动用趋势(否则启动瞬间会误判成外接) */
    bool external = usb_host || (s_windows > 0 && s_trend > -BATT_FALL_MV);

    if (!s_has) {
        s_power = BATTERY_POWER_EXTERNAL_ONLY;   /* 没装电池,靠外接供电 */
    } else if (external) {
        s_power = (mv >= BATT_FULL_MV) ? BATTERY_POWER_FULL
                                       : BATTERY_POWER_CHARGING;
    } else {
        s_power = BATTERY_POWER_INTERNAL;
    }

    return ESP_OK;
}

bool Battery_HasBattery(void)
{
    return s_has;
}

float Battery_GetVoltage(void)
{
    return s_volts;
}

int Battery_GetLevel(void)
{
    return s_has ? s_level : 0;
}

Battery_Power Battery_GetPower(void)
{
    return s_power;
}
