/*
 * 网络音频:把电脑上的音乐通过 WiFi 推给板子
 *
 * 为什么走 WiFi 而不是蓝牙:
 *   ESP32-S3 没有经典蓝牙,A2DP(手机/电脑推音频)用不了。
 *   而 WiFi 这条路有个很大的好处 —— 解码可以放在电脑端做。
 *
 * 协议(故意做得极简):
 *   TCP,默认 3333 端口。连上之后就是一路裸 PCM:
 *   48000Hz / 2 声道 / 16bit 小端,没有包头,一直推到断开为止。
 *
 *   电脑端(见 tools/wascap)用 WASAPI 回环直接采"默认输出设备"的样本,
 *   转成这个格式推过来。所以板子这边【不需要任何解码器】,
 *   电脑上能出声的东西(播放器 / 浏览器 / 视频)都能原样推过来。
 *
 * 播放期间 TF 卡的播放会自动让出(见 MusicTask 里的判断),断开后恢复。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_AUDIO_PORT   3333

/* 板子 -> 电脑 的反向控制消息。电脑端(wascap)读到就停止推流并退出,
   不会去重连。复用同一条 TCP,不额外占端口。 */
#define NET_AUDIO_STOP_CMD  "STOP"

/* 起一个后台任务监听端口。失败不影响其它功能。 */
esp_err_t NetAudio_Start(void);

/* 有电脑正在推流时返回 true —— MusicTask 靠它决定要不要让出播放通道 */
bool NetAudio_IsStreaming(void);

/* 正在推流的电脑 IP,没连返回空串 */
const char *NetAudio_GetPeerIp(void);

/* 推流的数据要不要喂给频谱?
   BOOT 长按会在三个来源之间轮换,只有轮到"电脑推流"时才喂,
   否则三个来源会互相掺和(比如麦克风 + 推流同时往 FFT 里灌)。 */
void NetAudio_SetSpectrumEnabled(bool on);

/* 叫电脑停止推流(板子上长按停止键时调)。没在推流返回 false。 */
bool NetAudio_RequestStop(void);

/* 推流中本地暂停/继续(只把 DAC 静音,连接和接收都保持)。
   跟"停止"是两回事:暂停随时能反悔,停止会把电脑端程序关掉。 */
bool NetAudio_SetPaused(bool paused);
bool NetAudio_IsPaused(void);

#ifdef __cplusplus
}
#endif
