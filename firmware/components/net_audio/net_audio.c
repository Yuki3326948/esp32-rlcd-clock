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

/* 让出播放通道:连接和接收都留着,只是不写 I2S、DAC 静音。
   ★ 和 s_streaming 是两件不同的事,别混:
      s_streaming = "电脑连着"       (TCP 在)
      s_yield     = "喇叭交出去了"   (TF 卡 / 麦克风在用)
   NetAudio_IsStreaming() 回答的是"电脑在不在占喇叭"(= 连着且没让出),
   因为所有调用点真正想问的都是这个。 */
static volatile bool s_yield  = false;

/* 从"让出"切回"独占"。重配置采样率 + 解除静音这一步必须由推流任务自己做 ——
   只有它知道这会儿有没有别的任务正在写 I2S(从用户按键的上下文里改 I2S
   会和正在播放的 MusicTask 撞车)。 */
static volatile bool s_retake = false;

/* 板子现在想不想要电脑的声音?由 user_app 注册,见 net_audio.h。 */
static bool (*s_want_fn)(void) = NULL;

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

        /* 这条新连接要不要独占喇叭?问板子当前的"频谱源"。
           不是"电脑推流"就先让出 —— 否则电脑一连上来就把正在放的 TF 卡
           音乐顶掉:屏幕写着"播放音乐",耳朵里却是电脑的声音。
           这种"看到的和听到的对不上"是最难查的一类问题。 */
        s_yield  = (s_want_fn != NULL) && !s_want_fn();
        s_retake = false;

        /* 让 TF 卡那边先停手:MusicTask 每 30ms 查一次,
           等它把播放通道关掉再动采样率,免得两头同时写 I2S。 */
        s_streaming = true;
        s_paused    = false;        /* 新的一次推流总是从"在放"开始 */
        SetLowLatency(true);
        vTaskDelay(pdMS_TO_TICKS(200));

        /* 电脑端 ffmpeg 固定按 48k/2ch/16bit 输出,这里跟上 */
        Audio_SetSampleRate(48000);
        Spectrum_SetSampleRate(48000);
        Audio_Mute(s_yield);
        Spectrum_Enable(!s_yield);   /* MusicTask 暂停时会把它关掉,这里补回来 */
        if (s_yield) {
            ESP_LOGI(TAG, "但当前选的不是【电脑推流】,先只收不放");
        }

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
            /* 刚从"让出"切回"独占":先等一下让本地播放停手,再切采样率。
               不等的话 MusicTask 可能正好在 Audio_PlayPcm 里,
               两个任务同时改/写同一条 I2S,出来的就是一团噪声。 */
            if (s_retake) {
                s_retake = false;
                vTaskDelay(pdMS_TO_TICKS(200));
                Audio_SetSampleRate(48000);
                Spectrum_SetSampleRate(48000);
                Audio_Mute(s_paused);
                Spectrum_Enable(true);
                ESP_LOGI(TAG, "喇叭已收回:采样率 %u 音量 %d",
                         (unsigned)Audio_GetSampleRate(), Audio_GetVolume());
            }

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

            /* 让出通道期间:数据照收照丢。
               不写 I2S —— 喇叭这时候归 TF 卡或麦克风,两头一起写就是一锅粥;
               也不能不读 —— 不读的话 TCP 接收窗很快就满,wascap 的发送会阻塞,
               它的采集线程跟着卡住,WASAPI 缓冲一溢出声音就断了,
               等切回来还得重新缓冲。丢掉只是白花一点 WiFi 带宽(局域网 192KB/s)。 */
            if (s_yield) {
                continue;
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
        s_yield     = false;        /* 连接没了,"让出"也就无从谈起 */
        s_retake    = false;
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
    /* "电脑正在占用喇叭" —— 光连着不算。让出通道时返回 false,
       这样 MusicTask 会把喇叭拿回去给 TF 卡用,显示也跟着改。 */
    return s_streaming && !s_yield;
}

bool NetAudio_IsConnected(void)
{
    return s_streaming;
}

bool NetAudio_SetYield(bool yield_it)
{
    if (!s_streaming) {
        return false;       /* 没连着,没什么可让的 */
    }
    if (yield_it == s_yield) {
        return false;       /* 状态没变,别重复记日志 */
    }
    s_yield = yield_it;

    if (yield_it) {
        /* 让出 = 立刻把 DAC 静音并停写 I2S(在下面的接收循环里判断)。
           注意这里【不发 STOP】—— 电脑端程序要一直活着,
           用户切回来才能立刻有声。 */
        Audio_Mute(true);
        Spectrum_Enable(false);
        ESP_LOGI(TAG, "让出喇叭(连接保留,电脑端程序不用重开)");
    } else {
        /* 选回"电脑推流"就是"我要听电脑" —— 别让它停在暂停状态上。 */
        s_paused = false;
        /* 真正的重配置交给推流任务 —— 见 s_retake 的说明。 */
        s_retake = true;
        ESP_LOGI(TAG, "准备收回喇叭");
    }
    return true;
}

void NetAudio_SetWantCallback(bool (*fn)(void))
{
    s_want_fn = fn;
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
    if (!s_streaming || s_yield) {
        return false;       /* 喇叭已经让出去了,"暂停"就轮不到电脑来说 */
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
