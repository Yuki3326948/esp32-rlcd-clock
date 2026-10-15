/*
 * usb_test —— Type-C(USB-Serial-JTAG)当实时数据口的实测。
 *
 * 目的:回答"这个 Type-C 口能不能当数据口、能跑多快、丢不丢包"。
 * 结论不靠猜,靠数:
 *   - RX:电脑全速往 COM5 灌数据,板子数收到多少 -> 吞吐 + 丢包率
 *   - TX:电脑发 "TX=<字节数>\n",板子回灌同样多的数据 -> 电脑量耗时
 *
 * 背景(已查证):
 *   本工程 CONFIG_ESP_CONSOLE_UART_DEFAULT=y(主 console 在 UART0),
 *   USB-Serial-JTAG 只是【次要】console —— 那条路径只 esp_vfs_register_fs
 *   ("/dev/secondary"),**不装驱动**,所以应用可以自己 usb_serial_jtag_driver_install()。
 *
 * ★ 这是临时测试代码。验完把 main.cpp 里的 USB_TEST 改成 0 即可,
 *   或者直接删掉 main/usb_test.c + CMakeLists 里那两处引用。
 */
#include "usb_test.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"

/* ★ 关键:让 console 的日志输出也走【驱动环形缓冲】,而不是它默认的
 * "裸寄存器 + 忙等"路径。
 *
 * 不调这一句的话,同一个 TX FIFO 会有【两个写者】:
 *   - console 日志:usb_serial_jtag_vfs 的原始路径,直接忙等寄存器
 *   - 应用数据:驱动环形缓冲 + ISR
 * 两者互不知情 —— 实测后果是 TX-RESULT 日志根本发不出来,
 * 而且回灌 4MB 时 20 秒只出去 2693 字节(卡死)。
 *
 * 该函数只在 esp_private 头里间接用到,没有公开声明,所以自己 extern。 */
extern void usb_serial_jtag_vfs_use_driver(void);

static const char *TAG = "usbtest";

/* 驱动自带的环形缓冲。开大一点,避免板子读得慢的时候本来能收下的也丢了。
   注意它从【内部 RAM】分配 —— 这个测试要吃掉 16KB,测完就还回去。 */
#define RT_RXBUF_BYTES   16384
#define RT_TXBUF_BYTES    4096

/* 读缓冲。故意放内部 RAM:PSRAM 的话测出来的是内存带宽不是 USB 带宽 */
static uint8_t s_rxbuf[4096];

/* 回灌测试用 */
#define TX_CHUNK         4096
static uint8_t s_txbuf[TX_CHUNK];

static void UsbTestTask(void *arg)
{
    (void)arg;

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = RT_TXBUF_BYTES,
        .rx_buffer_size = RT_RXBUF_BYTES,
    };
    esp_err_t r = usb_serial_jtag_driver_install(&cfg);
    ESP_LOGW(TAG, "driver_install = %s", esp_err_to_name(r));
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "装不上,测试没法做");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "就绪:RX buf=%d B。现在可以往 COM5 灌数据了", RT_RXBUF_BYTES);

    /* ★ 把 console 的输出也切到驱动路径(见文件头那段注释)。
       必须在驱动装好之后调。 */
    usb_serial_jtag_vfs_use_driver();
    ESP_LOGW(TAG, "console 已切到驱动路径(单写者)");

    for (int i = 0; i < TX_CHUNK; i++) {
        s_txbuf[i] = (uint8_t)('A' + (i % 26));
    }

    uint32_t total     = 0;
    uint32_t last      = 0;
    int64_t  last_ms   = 0;
    int64_t  t0        = esp_timer_get_time();
    int      tx_state  = 0;      /* 0=空闲 1=准备回灌 */
    uint32_t tx_want   = 0;

    for (;;) {
        int n = usb_serial_jtag_read_bytes(s_rxbuf, sizeof(s_rxbuf),
                                           pdMS_TO_TICKS(50));
        if (n > 0) {
            total += (uint32_t)n;

            /* 在收到的数据里找 "TX=<数字>" 命令。找不到就算了 —— 
               这个测试就是要全速灌,顺便扫一下命令。 */
            if (tx_state == 0) {
                for (int i = 0; i + 3 < n; i++) {
                    if (s_rxbuf[i] == 'T' && s_rxbuf[i + 1] == 'X' &&
                        s_rxbuf[i + 2] == '=') {
                        uint32_t v = 0;
                        int j = i + 3;
                        while (j < n && s_rxbuf[j] >= '0' && s_rxbuf[j] <= '9') {
                            v = v * 10u + (uint32_t)(s_rxbuf[j] - '0');
                            j++;
                        }
                        if (v >= 1024u && v <= 8u * 1024u * 1024u) {
                            tx_want  = v;
                            tx_state = 1;
                        }
                        break;
                    }
                }
            }
        }

        /* ---- 回灌测试 ---- */
        if (tx_state == 1 && tx_want > 0) {
            tx_state = 2;
            int64_t ts = esp_timer_get_time();
            uint32_t left = tx_want;
            uint32_t sent = 0;
            while (left > 0) {
                size_t c = left < TX_CHUNK ? left : TX_CHUNK;
                int w = usb_serial_jtag_write_bytes(s_txbuf, c,
                                                    pdMS_TO_TICKS(1000));
                if (w <= 0) {
                    ESP_LOGE(TAG, "写失败,已发 %u", (unsigned)sent);
                    break;
                }
                sent += (uint32_t)w;
                left -= (uint32_t)w;
            }
            usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(2000));
            int64_t ms = (esp_timer_get_time() - ts) / 1000;
            if (ms < 1) ms = 1;
            ESP_LOGW(TAG, "[[TXRESULT]] 发出 %u B, 用时 %d ms = %u KB/s",
                     (unsigned)sent, (int)ms,
                     (unsigned)((uint64_t)sent * 1000u / (uint64_t)ms / 1024u));
            tx_want  = 0;
            tx_state = 0;      /* ★ 必须归零,否则第二次 TX 命令永远不会被识别 */
        }

        /* ---- 每秒报一次 RX ---- */
        int64_t now_ms = (esp_timer_get_time() - t0) / 1000;
        if (now_ms - last_ms >= 1000) {
            uint32_t d = total - last;
            ESP_LOGW(TAG, "RX 累计=%u B, 最近1秒=%u B (%u KB/s)",
                     (unsigned)total, (unsigned)d, (unsigned)(d / 1024u));
            last    = total;
            last_ms = now_ms;
        }
    }
}

void UsbTest_Start(void)
{
    xTaskCreatePinnedToCore(UsbTestTask, "usbtest", 4096, NULL, 3, NULL, 1);
}
