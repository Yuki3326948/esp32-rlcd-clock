#include "net_audio.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_player.h"
#include "spectrum.h"

static const char *TAG = "net_audio";

static volatile bool s_streaming = false;
static char          s_peer[20]  = "";
/* 当前连接的套接字。板子上的"停止键"靠它在同一根 TCP 上反向叫电脑别推了。 */
static volatile int  s_cli_fd    = -1;
/* 推流中本地暂停(只静音 DAC,连接照旧)。和"停止"不同,可逆。 */
static volatile bool s_paused    = false;
/* 推流的数据要不要喂频谱。默认 false:开机时频谱源是"播放音乐"。 */
static volatile bool s_spec_on   = false;

/* 诊断用:上一次写 I2S 的结果,以及有没有成功写过第一次。
   原来写失败是静默 continue,一声不响 —— 出问题时完全看不出卡在哪。 */
static esp_err_t s_last_wr      = ESP_OK;
static bool      s_logged_first = false;

/* 收到的 PCM 峰值。真实音乐几千到三万多;要是持续贴着 0,
   说明电脑那头压根没在出声(不是板子的问题),日志里点一句省得误判。 */
static int32_t    s_peak       = 0;
static int        s_silent_sec = 0;
static TickType_t s_next_rep   = 0;

#define CHUNK_BYTES   4096
static uint8_t s_buf[CHUNK_BYTES];

/* 推流期间把 WiFi 省电关掉。
   省电模式(MIN_MODEM)会让射频在信标之间打盹,往返延迟从几毫秒涨到
   上百毫秒。而 TCP 吞吐 = 接收窗口 / 往返延迟:5760 字节的窗口配上
   100ms 延迟只有 57KB/s,而 48k/2ch/16bit 要 192KB/s —— 音频自然
   就断续了。断开后恢复省电,免得白白耗电(板子是电池供电的)。 */
static void SetLowLatency(bool on)
{
    esp_err_t err = esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置 WiFi 省电模式失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WiFi %s", on ? "省电已关(低延迟)" : "已恢复省电");
    }
}

static void StreamTask(void *arg)
{
    (void)arg;

    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) {
        ESP_LOGE(TAG, "创建套接字失败: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(NET_AUDIO_PORT);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(srv, 1) != 0) {
        ESP_LOGE(TAG, "监听 %d 端口失败: errno=%d", NET_AUDIO_PORT, errno);
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "音频流已就绪:监听 %d 端口,等电脑连过来", NET_AUDIO_PORT);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int fd = accept(srv, (struct sockaddr *)&cli, &clen);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        snprintf(s_peer, sizeof(s_peer), "%s", inet_ntoa(cli.sin_addr));
        ESP_LOGI(TAG, "电脑 %s 已连接,开始接收音频", s_peer);
        s_cli_fd = fd;

        /* 让 TF 卡那边先停手:MusicTask 每 30ms 查一次 s_streaming,
           等它把播放通道关掉再动采样率,免得两头同时写 I2S。 */
        s_streaming = true;
        s_paused    = false;        /* 新的一次推流总是从"在放"开始 */
        SetLowLatency(true);
        vTaskDelay(pdMS_TO_TICKS(200));

        /* 电脑端 ffmpeg 固定按 48k/2ch/16bit 输出,这里跟上 */
        Audio_SetSampleRate(48000);
        Spectrum_SetSampleRate(48000);
        Audio_Mute(false);
        Spectrum_Enable(true);       /* MusicTask 暂停时会把它关掉,这里补回来 */

        /* 把播放链路的真实状态打出来 —— 出问题时一眼就能看出是不是
           通道没开 / 音量为 0 / 采样率不对 */
        ESP_LOGI(TAG, "音频通道: opened=%d 音量=%d 采样率=%u",
                 (int)Audio_IsOpened(), Audio_GetVolume(),
                 (unsigned)Audio_GetSampleRate());
        s_logged_first = false;
        s_last_wr      = ESP_OK;
        s_peak         = 0;
        s_silent_sec   = 0;
        s_next_rep     = xTaskGetTickCount() + pdMS_TO_TICKS(1000);

        bool eof = false;
        while (!eof) {
            /* TCP 是字节流,recv 可能只给一半,而且长度不保证是 4 的倍数。
               攒满一整块再送,顺便把长度对齐到"一帧立体声 = 4 字节",
               否则左右声道会错位。 */
            int have = 0;
            while (have < CHUNK_BYTES) {
                int got = recv(fd, s_buf + have, CHUNK_BYTES - have, 0);
                if (got > 0) {
                    have += got;
                    continue;
                }
                eof = true;                    /* 0 = 对端关闭,<0 = 出错 */
                break;
            }
            have -= have % 4;
            if (have <= 0) {
                break;
            }

            /* 阻塞写,自然限速在 48k 的实时速率上 */
            esp_err_t wr = Audio_PlayPcm(s_buf, (size_t)have);
            if (wr != ESP_OK) {
                if (s_last_wr == ESP_OK) {
                    ESP_LOGW(TAG, "Audio_PlayPcm 失败: %s", esp_err_to_name(wr));
                }
                s_last_wr = wr;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (s_last_wr != ESP_OK) {
                ESP_LOGI(TAG, "Audio_PlayPcm 恢复正常");
            }
            s_last_wr = ESP_OK;
            if (!s_logged_first) {
                s_logged_first = true;
                ESP_LOGI(TAG, "已向 I2S 写入第一块音频: %d 字节", have);
            }

            /* 每秒看一眼峰值:一直贴着 0 就说明电脑那头没在出声 */
            {
                const int16_t *pcm = (const int16_t *)s_buf;
                int            n   = have / 2;   /* 16bit -> 样本数 */
                for (int i = 0; i < n; i++) {
                    int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                    if (v > s_peak) {
                        s_peak = v;
                    }
                }
            }
            if (xTaskGetTickCount() >= s_next_rep) {
                s_next_rep = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
                if (s_peak < 100) {
                    if (++s_silent_sec == 3) {
                        ESP_LOGW(TAG, "连续 3 秒收到的都是静音 —— 电脑那边没在出声"
                                      "(查一下播放器和它的输出设备)");
                    }
                } else {
                    if (s_silent_sec >= 3) {
                        ESP_LOGI(TAG, "收到声音了,峰值 %d/32767", (int)s_peak);
                    }
                    s_silent_sec = 0;
                }
                s_peak = 0;
            }

            /* 只有轮到"电脑推流"这个来源时才喂频谱 */
            if (s_spec_on) {
                Spectrum_Feed((const int16_t *)s_buf, (size_t)have / 4);
            }
        }

        close(fd);
        s_cli_fd    = -1;
        s_streaming = false;
        s_peer[0]   = '\0';
        Audio_Mute(true);
        Spectrum_Enable(false);
        SetLowLatency(false);
        ESP_LOGI(TAG, "电脑断开,交还播放通道");
    }
}

esp_err_t NetAudio_Start(void)
{
    /* 钉在核 0:WiFi 协议栈也在那边,减少跨核搬运 */
    if (xTaskCreatePinnedToCore(StreamTask, "net_audio", 8192, NULL, 4,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "创建网络音频任务失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool NetAudio_IsStreaming(void)
{
    return s_streaming;
}

bool NetAudio_RequestStop(void)
{
    int fd = s_cli_fd;
    if (fd < 0) {
        return false;
    }
    /* 只把命令发出去,【千万不能】在这里顺手 shutdown / close:
       板子的接收缓冲里还堆着电脑推来的音频没读完,这时候关连接,
       TCP 发的是 RST 而不是 FIN —— RST 会把刚送出去的 STOP 一起冲掉,
       电脑那边只看到"远程主机强迫关闭了一个现有的连接",然后乖乖重连,
       等于白按一次。
       让电脑读到命令后自己收尾退出,它关连接时我们再走正常断开流程。 */
    (void)send(fd, NET_AUDIO_STOP_CMD, strlen(NET_AUDIO_STOP_CMD), 0);
    return true;
}

bool NetAudio_SetPaused(bool paused)
{
    if (!s_streaming) {
        return false;
    }
    s_paused = paused;
    /* 只静音 DAC,I2S 照收 —— 所以"暂停"随时能反悔,
       不像"停止"会把电脑端程序关掉。 */
    Audio_Mute(paused);
    return true;
}

bool NetAudio_IsPaused(void)
{
    return s_paused;
}

void NetAudio_SetSpectrumEnabled(bool on)
{
    s_spec_on = on;
}

const char *NetAudio_GetPeerIp(void)
{
    return s_peer;
}
