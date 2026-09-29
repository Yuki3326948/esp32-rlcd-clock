/*
 * 板载传感器驱动实现
 *   SHTC3   温湿度  @0x70
 *   PCF85063 RTC    @0x51
 *
 * 协议细节对照官方例程 05_I2C_SHTC3 / 04_I2C_PCF85063 核实过。
 */

#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_sensors.h"

static const char *TAG = "sensors";

/* ---------------- I2C 配置 ---------------- */
#define I2C_SCL_PIN     GPIO_NUM_14
#define I2C_SDA_PIN     GPIO_NUM_13
#define I2C_FREQ_HZ     100000
#define I2C_TIMEOUT_MS  1000

/* ---------------- 器件地址 ---------------- */
#define SHTC3_ADDR      0x70
#define PCF85063_ADDR   0x51

/* SHTC3 命令 */
#define SHTC3_WAKEUP        0x3517
#define SHTC3_SLEEP         0xB098
#define SHTC3_MEAS_T_RH     0x7866   /* 先温度后湿度,轮询模式 */

/* PCF85063 寄存器 */
#define PCF85063_SEC_REG    0x04     /* 连续 7 字节:秒分时日月周年 */

/*
 * 温度校准偏移。板上传感器贴近屏幕与主控,会有自热。
 * 官方例程用 -4.0℃ 做补偿,这里默认不补偿(严格按数据手册公式)。
 * 如果实测偏高,把它改成 -4.0f 或按实际偏差微调。
 */
#define TEMP_CALIBRATION    (0.0f)

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_shtc3 = NULL;
static i2c_master_dev_handle_t s_rtc = NULL;

/* 让音频组件能把 ES8311/ES7210 挂到同一条 I2C 总线上 */
i2c_master_bus_handle_t Sensors_GetI2cBus(void)
{
    return s_bus;
}

void Sensors_ScanBus(void)
{
    if (s_bus == NULL) {
        ESP_LOGW(TAG, "I2C 总线还没建好");
        return;
    }

    ESP_LOGI(TAG, "---- I2C 扫描(SDA=GPIO%d SCL=GPIO%d) ----",
             I2C_SDA_PIN, I2C_SCL_PIN);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  应答: 0x%02X", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "---- 共 %d 个设备 ----", found);
}

/* ============================================================
 *  初始化
 * ============================================================ */
esp_err_t Sensors_Init(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port                     = I2C_NUM_0;
    bus_cfg.sda_io_num                   = I2C_SDA_PIN;
    bus_cfg.scl_io_num                   = I2C_SCL_PIN;
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz    = I2C_FREQ_HZ;

    dev_cfg.device_address = SHTC3_ADDR;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_shtc3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 SHTC3 失败: %s", esp_err_to_name(err));
        return err;
    }

    dev_cfg.device_address = PCF85063_ADDR;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_rtc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 PCF85063 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C 就绪 (SCL=14 SDA=13),SHTC3@0x70  PCF85063@0x51");
    return ESP_OK;
}

/* ============================================================
 *  SHTC3 温湿度
 * ============================================================ */
static uint8_t shtc3_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t shtc3_send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_master_transmit(s_shtc3, buf, sizeof(buf),
                               pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

bool Sensors_ReadTempHumi(float *temp_c, float *humi_pct)
{
    uint8_t buf[6] = {0};

    if (shtc3_send_cmd(SHTC3_WAKEUP) != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 唤醒失败");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));       /* 唤醒需要 ~1ms,留足余量 */

    if (shtc3_send_cmd(SHTC3_MEAS_T_RH) != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 触发测量失败");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));       /* 测量约需 12.1ms */

    if (i2c_master_receive(s_shtc3, buf, sizeof(buf),
                           pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 读取失败");
        return false;
    }

    shtc3_send_cmd(SHTC3_SLEEP);         /* 读完立即休眠,降低自热 */

    if (shtc3_crc8(&buf[0], 2) != buf[2] || shtc3_crc8(&buf[3], 2) != buf[5]) {
        ESP_LOGW(TAG, "SHTC3 CRC 校验失败");
        return false;
    }

    uint16_t raw_t = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t raw_h = (uint16_t)((buf[3] << 8) | buf[4]);

    if (temp_c)   *temp_c   = 175.0f * raw_t / 65536.0f - 45.0f + TEMP_CALIBRATION;
    if (humi_pct) *humi_pct = 100.0f * raw_h / 65536.0f;
    return true;
}

/* ============================================================
 *  PCF85063 RTC
 * ============================================================ */
static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool Rtc_IsRunning(void)
{
    uint8_t reg = PCF85063_SEC_REG;
    uint8_t sec = 0;

    if (i2c_master_transmit_receive(s_rtc, &reg, 1, &sec, 1,
                                    pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != ESP_OK) {
        return false;
    }
    /* 秒寄存器 bit7 = OS(振荡器停止)标志:0 表示时间有效 */
    return (sec & 0x80) == 0;
}

bool Rtc_Read(struct tm *out)
{
    uint8_t reg = PCF85063_SEC_REG;
    uint8_t b[7] = {0};

    if (i2c_master_transmit_receive(s_rtc, &reg, 1, b, sizeof(b),
                                    pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != ESP_OK) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd2dec(b[0] & 0x7F);
    out->tm_min  = bcd2dec(b[1] & 0x7F);
    out->tm_hour = bcd2dec(b[2] & 0x3F);
    out->tm_mday = bcd2dec(b[3] & 0x3F);
    out->tm_wday = b[4] & 0x07;
    out->tm_mon  = (int)bcd2dec(b[5] & 0x1F) - 1;   /* tm_mon: 0-11 */
    out->tm_year = (int)bcd2dec(b[6]) + 100;        /* tm_year: 自 1900 */
    out->tm_isdst = 0;
    return true;
}

bool Rtc_Write(const struct tm *t)
{
    uint8_t buf[8];

    buf[0] = PCF85063_SEC_REG;
    buf[1] = dec2bcd((uint8_t)t->tm_sec) & 0x7F;    /* 顺带清掉 OS 标志 */
    buf[2] = dec2bcd((uint8_t)t->tm_min);
    buf[3] = dec2bcd((uint8_t)t->tm_hour);
    buf[4] = dec2bcd((uint8_t)t->tm_mday);
    buf[5] = (uint8_t)(t->tm_wday & 0x07);
    buf[6] = dec2bcd((uint8_t)(t->tm_mon + 1));
    buf[7] = dec2bcd((uint8_t)((t->tm_year + 1900) % 100));

    return i2c_master_transmit(s_rtc, buf, sizeof(buf),
                               pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK;
}

bool Rtc_LoadToSystem(void)
{
    struct tm t;
    if (!Rtc_Read(&t)) {
        return false;
    }

    time_t epoch = mktime(&t);          /* TZ 已设为东八区,RTC 存本地时间 */
    if (epoch <= 0) {
        return false;
    }

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return true;
}

bool Rtc_SyncFromSystem(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return Rtc_Write(&t);
}
