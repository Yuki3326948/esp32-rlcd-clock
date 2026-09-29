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
 *
 * ★ 两种"电脑别再出声"的办法,别搞混:
 *     SetYield(true)   让出喇叭。连接保留、电脑端程序一直活着,
 *                      板子只收不放。切回来【立刻】有声。
 *     RequestStop()    真停止。往同一根 TCP 发 STOP,电脑端程序收到就退出,
 *                      用户再想听就得重新双击那个 exe。
 *   切源用前者,用户明确按"停止推流"才用后者。
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

/* 电脑正在【占用喇叭】时返回 true —— MusicTask 靠它决定要不要让出播放通道。
   注意:光"连着"不算,让出通道期间返回 false(喇叭已经还给 TF 卡了)。 */
bool NetAudio_IsStreaming(void);

/* 电脑连着(不管板子现在放不放它的声音)。
   用户按"停止推流"时看的是这个 —— 让出期间连接还在,
   但仍然该让用户能把电脑端程序关掉。 */
bool NetAudio_IsConnected(void);

/* 让出 / 收回播放通道。
 *   true  = 把喇叭交出去:连接和接收都留着,但不写 I2S、DAC 静音。
 *   false = 把喇叭要回来:立刻按 48k 重新配置并解除静音。
 *
 * ★ 为什么必须有这一档:
 *   以前切走时发的是 STOP,而电脑端程序收到 STOP 是【干净退出】的。
 *   于是用户切回"电脑推流"时电脑端程序早就没了 —— 屏幕上写着
 *   "电脑推流",耳朵里一点声音都没有,还得自己想起来重新双击那个 exe。
 *   改成"让出"之后,电脑端程序一直活着,切回来立刻有声。
 *
 * 没连接时返回 false(什么都不用做)。 */
bool NetAudio_SetYield(bool yield_it);

/* 板子现在想不想要电脑的声音?由 user_app 注册一个回调,
   net_audio 在【建立连接时】问一次,决定这条新连接要不要独占喇叭。
   用回调是为了不让 net_audio 反过来依赖界面模块。 */
void NetAudio_SetWantCallback(bool (*fn)(void));

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
