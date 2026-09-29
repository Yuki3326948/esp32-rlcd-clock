/*
 * Waveshare ESP32-S3-RLCD-4.2  ——  联网气象时钟(深色版)
 *
 *  界面:  黑底白字 / 单色现代的极简布局
 *  时间:  开机先读 PCF85063 RTC(瞬间有数) -> NTP 校准 -> 回写 RTC
 *  环境:  SHTC3 温湿度,每 3 秒刷新
 *  按键:  KEY(GPIO18)  短按 = 切页(时钟 / 音乐频谱)
 *                      长按 = 时钟页切 12/24 小时制,音乐页 = 下一首;
 *                              电脑推流时 = 停止推流
 *                      双击 = 音乐页上一首
 *         BOOT(GPIO0)  短按 = 音乐播放 / 暂停
 *                              电脑推流时 = 本地暂停/继续推流(连接不断)
 *                      长按 = 按住 2 秒开始轮换频谱源(每 2 秒换一个,
 *                              松手停在当前这个)。三个源:TF 卡音乐 /
 *                              麦克风 / 电脑推流
 *                      长按 = 切换频谱源(播放中的音乐 / 麦克风采音)
 *
 *  说明:  本板没有触摸屏(只有上面这两个物理键),
 *         所以播放控制全部由按键完成,音乐页底部会列出操作方式。
 *
 *  音乐:  TF 卡上的 WAV(16bit PCM,44.1k/48k 都行)-> ES8311 -> 功放 -> 喇叭
 *         原来还内嵌了一段 3.67MB 的 PCM 在 flash 里,能读卡之后就删了
 *  频谱:  esp-dsp 512 点 FFT -> 90 个对数频段(94Hz~20kHz) -> 直写显存的柱状图
 *         BOOT 长按可把频谱源从"播放的音乐"切到"ES7210 麦克风采音"
 *
 * 注意: RLCD 为单色屏,flush 时按 0x7fff 阈值二值化,
 *       因此界面只用纯黑 / 纯白,中文使用自生成的 1bpp 点阵字体。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <nvs_flash.h>

#include "ui_draw.h"
#include "audio_player.h"
#include "battery.h"
#include "ble_ctrl.h"
#include "net_audio.h"
#include "sysmon.h"
#include "ui_clock_bits.h"      /* 方块点阵时钟的数字字形(工具生成) */
#include "board_sensors.h"
#include "display_bsp.h"
#include "spectrum.h"
#include "sdcard.h"
#include "wav_player.h"
#include "user_app.h"
#include "wifi_cfg_parse.h"

/* LCD 显存句柄(main.cpp 里的全局对象)。频谱要直接写显存,绕过 LVGL。 */
extern DisplayPort RlcdPort;

/* ==================== 用户配置 ==================== */

/* ---- 默认 WiFi 凭据(编译进固件)----
 *
 * 凭据【不写在源码里】,而是放在同目录的 `wifi_secrets.h`:
 *     #define WIFI_SSID      "我家的WiFi"
 *     #define WIFI_PASSWORD  "密码"
 *
 * 为什么这么绕:这个文件是要公开的。密码一旦进了 git,就算你后来删掉,
 * 它也永远留在历史提交里(git log -p 谁都能翻出来)。所以真实凭据
 * 必须从一开始就不进版本库 —— 仓库里只放 `wifi_secrets.h.example` 模板,
 * 真实文件写进 .gitignore。
 *
 * 没有这个文件也能正常编译:凭据为空,靠 TF 卡根目录的 wifi.txt 配网
 * (见 WifiLoadFromSd)。所以克隆下来直接编、直接烧,一样能用。
 *
 * 另外还有个小开关:编"给别人用"的固件时用
 *     idf.py -DWIFI_CREDENTIALS_EMPTY=ON build
 * 强制留空,双保险。定义在 components/user_app/CMakeLists.txt。 */
#if defined(__has_include)
#  if __has_include("wifi_secrets.h")
#    include "wifi_secrets.h"
#  endif
#endif

#ifdef WIFI_CREDENTIALS_EMPTY
#undef  WIFI_SSID
#undef  WIFI_PASSWORD
#endif
#ifndef WIFI_SSID
#define WIFI_SSID        ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD    ""
#endif

/* ---- 实际生效的 WiFi 凭据 ---- */
#define WIFI_CFG_PATH    SDCARD_MOUNT_POINT "/wifi.txt"

static char s_wifi_ssid[33] = WIFI_SSID;      /* 802.11 上限 32 字节 + '\0' */
static char s_wifi_pass[65] = WIFI_PASSWORD;  /* 上限 64 字节 + '\0' */
static bool s_wifi_cfg_tried_sd = false;      /* 是否已经尝试过读卡 */

#define NTP_SERVER       "ntp.aliyun.com"
#define TIMEZONE_STR     "CST-8"        /* 中国标准时间 UTC+8 */

#define KEY_PIN          GPIO_NUM_18    /* KEY  键,低电平有效 */
#define BOOT_PIN         GPIO_NUM_0     /* BOOT 键,低电平有效 */

#define TIME_VALID_THRESHOLD  1700000000L   /* 早于此值视为未对时 */

/* 调试埋点:1 = 每秒打印刷新调度数据(定位时序问题用);0 = 关闭 */
#define CLOCK_DBG_LOG         1

static const char *TAG = "clock";

/* ==================== 界面状态 ====================
   LVGL 拆掉之后,界面就是"一块字符串缓存 + 一个固定位置",
   内容变了才重画那一小块。不再有 lv_obj_t。

   时钟仍然拆成 8 个固定格子 —— 这一条不能丢:
   整串居中时秒数出现 '1' 会让整串宽度变化,屏幕上的数字就会左右滑动。
   现在用的是点阵字形(天然等宽),格子定位仍然保留,一是习惯,
   二是整块重画时的坐标计算简单。 */
#define CLK_NCHARS      8

/* ---- 时钟两侧:温度(左) / 湿度(右)两根小柱 ----
 * 时钟本体占 x = 99..296(见下面 CLK_STR_W),左右各剩 70px 空档,
 * 正好一边放一个。样式和下面几根大柱一致:圆角外框 + 自底向上填的内芯;
 * 旁边只写数值 —— 单位靠 °C 和 % 两个符号表达,不写中文。
 *
 * 宽度都要卡死,因为两边都是靠视觉擦除框刷新的,压线就会互相擦:
 *   左数值框 32..89  ,离时钟白底(从 30 开始但只到 y=122)不冲突
 *   右数值框 306..368 ,同理
 * 竖向都在白底之下(基线 144),所以不会和网格撞上。
 */
#define SIDE_W          24                   /* 柱宽(下面大柱是 32) */
#define SIDE_H          74                   /* 柱高:从日期行上方一直填到第二根分隔线 */
#define SIDE_R          4                    /* 外框圆角 */
#define SIDE_INSET      3                    /* 内芯缩进,和大柱一致 */
/* 柱顶 = 92:柱底(92+74=166)正好贴住第二根分隔线。
   原来顶在第一根分隔线下面(44),下面空了一大截。 */
#define SIDE_Y          92
/* 数值基线。原来是柱的竖直中心(SIDE_Y + 42 = 134),
   再下移 10px 到 144 —— 正好和日期行的基线齐平,一排看过去更稳。 */
#define SIDE_TXT_B      (SIDE_Y + 52)

#define SIDE_T_X        2                    /* 左柱(温度)左边:贴边 */
#define SIDE_T_TXT_X    (SIDE_T_X + SIDE_W + 6)   /* 32 */
#define SIDE_T_TXT_W    58                        /* 右边界 89,离时钟左边界 94 留 5px */

#define SIDE_H_X        (400 - 2 - SIDE_W)   /* 右柱(湿度)左边 = 374:贴边 */
#define SIDE_H_TXT_R    (SIDE_H_X - 6)       /* 湿度数值框右边界 = 368 */
#define SIDE_H_TXT_W    62                   /* 左边界 306,正好在时钟右边界 305 之外 */

/* AM/PM 从 12 小时制切回 24 时要擦这一块,所以位置必须写死;
   基线 60 -> 擦除区 y=43..65,正好避开分隔线(y=42)、
   上面状态行(到 37 为止)和下面的湿度数值(擦除区从 69 开始) */
/* AM/PM:12 小时制才显示。
   原来画在时钟右侧(306..368),但时钟区现在是【白底】,
   而白底每个秒跳都要重铺一次 —— 画在里面的东西会被盖掉。
   改放到日期行右边(那里正好空着,日期居中到 312 就结束了)。 */
#define AMPM_TXT_R     396                   /* 右边界,和顶栏状态字对齐 */
#define AMPM_TXT_B     140                   /* 基线:和日期行(144)基本齐平 */
#define AMPM_TXT_W     40

#define SIDE_TEMP_FULL  50.0f                /* 温度满刻度:0~50°C 折成 0~100% 柱高 */

static char  s_clk_cache[CLK_NCHARS] = {0};

/* 每个字段上一次画的内容:一样就跳过 */
static char s_p_ampm[8]    = "";
static char s_p_date[64]   = "";
/* 实际生效的 IP(连上 WiFi 后才有值)。顶栏直接显示它,
   这样屏上的地址永远是真的在用的那个,不是写死的常量。 */
static char s_ip_str[24]   = "";
static char s_p_track[40]  = "";
static char s_p_mstate[32] = "";

/* 纵轴刻度个数。位置固定 6 等分,数字随显示下限重写。 */
#define YDB_TICKS       6
static float s_axis_floor = -55.0f;   /* 刻度当前反映的下限 */

/* 频谱柱子仍然直接写显存(原因见文件头):
   只重画"高度变化的那几行",一帧不到 1000 个像素。 */
static int  s_bar_h[SPECTRUM_BANDS] = {0};
static bool s_bars_need_clear = true;

/* 谁往显存里画就持这把锁。以前是 Lvgl_lock;LVGL 走了之后
   时钟任务、频谱任务、按键切页仍然会同时碰显存,所以锁留着。 */
static SemaphoreHandle_t s_disp_lock = NULL;
static bool DispLock(int wait_ms);
static void DispUnlock(void);
static void ReportStacks(void);        /* 栈水位体检,定义在后面 */

/* 拼好一整页再显示(replace 掉 LVGL 的整屏刷新) */
static void RenderMusicPage(void);
static void RenderSysPage(void);
static void RenderPage(void);
static void DrawTopIndicators(bool force);
static void UpdateResBars(bool force);
static void UpdateSideBars(bool force);
static void DrawTextRightBox(int right_x, int baseline, int box_w,
                             const UiFont *f, const char *txt);
static void DrawTextCenterBox(int center_x, int baseline, int box_w,
                              const UiFont *f, const char *txt);
static void DrawIpLabel(void);
static void DrawSsidLabel(void);
static void DrawClockIcon(bool synced);
static void DrawPlugIcon(bool external);

/* 音乐页顶部那两个会变长的字段,必须用【定宽框】擦。
   右对齐的状态字尤其明显:"麦克风频谱"(5 字)切到"播放中"(3 字)
   时,新字只盖住右边一段,左边"麦克"两个字的笔画会留在屏上。
   宽度按各自最长的内容取。 */
#define MSTATE_BOX_W   92    /* 最长是"麦克风频谱" 5 字 */
#define TRACK_BOX_W   180    /* 文件名按 %.20s 截,西文 20 字符约 160px */

/* ---------- 频谱区几何 ----------
   分工:坐标轴/刻度/刻度值 用 LVGL 画(静态内容,只画一次),
         柱子区里不放任何 LVGL 对象,由 SpectrumTask 直接写显存。 */
#define PLOT_TOP       44                    /* 绘图区上边 = 0dB */
#define PLOT_H        184                    /* 绘图区高。往上顶到分隔线、往下铺到横轴标注之前,上下都不留黑边 */
#define PLOT_BOT      (PLOT_TOP + PLOT_H)    /* 绘图区下边 = -55dB    */
#define PLOT_X0        32                    /* 柱子区左边界(紧贴纵轴刻度) */
#define PLOT_W        366                    /* 柱子区宽 -> 32..398,几乎贴到屏幕右边 */

/* 90 根柱子 x (3px 柱 + 1px 缝) = 359px,画在 366px 里,左右各留 3px */
#define BAR_W           3
#define BAR_GAP         1                    /* 步距 4px */
#define BAR_MAX_H    PLOT_H
#define BAR_BOTTOM   PLOT_BOT
#define BAR_TOTAL    (SPECTRUM_BANDS * BAR_W + (SPECTRUM_BANDS - 1) * BAR_GAP)
#define BAR_X0       (PLOT_X0 + (PLOT_W - BAR_TOTAL) / 2)

#define YLABEL_RX      24                    /* 纵轴刻度值右对齐位置 */
#define YTICK_X        26                    /* 纵轴刻度线左端       */
#define XTICK_Y      (PLOT_BOT + 2)          /* 横轴刻度线           */
#define XLABEL_Y     (XTICK_Y + 6)           /* 横轴刻度值           */

#define AXIS_BOX_W     24                    /* 刻度值容器的定宽(8 号字很小) */

/* ---- dB 横向网格线 ----
 *  纵向 6 等分,每道刻度都拉一条横向点状线,长度和 X 轴一样
 *  (底下那排频率刻度正好也是 BAR_X0 起、BAR_TOTAL 宽)。
 *
 *  ⚠ 不能只在 RenderMusicPage 里画一次:
 *  频谱柱子是【直接写显存 + 只重画高度变化的那几行】的,
 *  柱子变矮时那段会被填黑,先画好的点跟着被抹掉且无人补。
 *  所以真正保证它一直存在的是 SpectrumDrawDirect 里每帧那次补画,
 *  RenderMusicPage 里画的那次只是让"切页那一瞬间"不缺线。
 */
static int DbTickY(int k)
{
    float frac = 1.0f - (float)k / (float)(YDB_TICKS - 1);
    return PLOT_BOT - (int)((float)PLOT_H * frac + 0.5f);
}

static void DrawDbGrid(void)
{
    for (int k = 0; k < YDB_TICKS; k++) {
        int y = DbTickY(k);
        if (y < PLOT_TOP || y > PLOT_BOT) {
            continue;
        }
        for (int i = 0; i < BAR_TOTAL; i += 4) {   /* 每 4px 一个点 = 25% 占空 */
            Ui_Pixel(BAR_X0 + i, y, 1);
        }
    }
}

/* ---- 时钟页顶栏:板子 IP:端口 + 电池 + 电源状态 + TF 卡 + 同步图标 ----
   从左到右依次排开。原来最右边是 5 个汉字的"时间已同步"(84px),
   换成 15px 的秒表图标后腾出 69px,正好把 IP:端口 放进来 ——
   外面那个推流程序要照着这个地址连,它比 WiFi 名字更值得占屏幕。
   每段都有定宽擦除区,免得新旧文字宽度不同时留下残笔。 */
/* 顶栏文字的基线。两个字体的 ascent 不同(cjk16 的 base=17,
   ascii14 的 base=15),分别写死才能让视觉中心都落在 y≈20。 */
#define TOP_BASE_ASCII  26
#define TOP_BASE_CJK    27

/* 左边的 WiFi 名字 + 板子地址。两个都是会变的字符串,
   而且长度不定(wifi.txt 里的名字可以很长),必须用定宽框擦。
   没连上就显示 "no wifi"。 */
#define SSID_TXT_X      2
#define SSID_BOX_W      62                    /* 7 个 ASCII 字符 */

#define IP_TXT_X        70
#define IP_BOX_W        148                   /* "192.168.1.182:3333" 18 字符 */

#define BATT_ICON_X     224
#define BATT_ICON_Y     13
#define BATT_ICON_W     26                    /* 电池身体宽(不含右侧触点) */
#define BATT_ICON_H     15
#define BATT_ICON_R     3                     /* 圆角半径 */
#define BATT_INSET      3                     /* 电量条距外框 */
#define BATT_TXT_X      258
#define BATT_TXT_W      32

/* TF 卡图标:照 Material Symbols / MDI 的 sd-card 形状画 ——
   正方形卡片轮廓 + 左上角斜切 + 卡内顶部 3 根竖触点。
   没插卡就整个不画(只把那块擦黑)。 */
#define SD_ICON_X       320
#define SD_ICON_Y       13
#define SD_ICON_W       15
#define SD_ICON_H       15

/* 外接电源:插头图标(替掉原来"外接充电"4 个汉字)。
   注意它和电池图标里那个闪电分工不同:
   插头 = "插着外接电源",闪电 = "正在往里充"。 */
#define PLUG_X          300
#define PLUG_Y          13
#define PLUG_W          13
#define PLUG_H          15

/* 时间同步状态:秒表图标,占 15px(原来是 84px 的文字) */
#define CLK_ICON_X      381
#define CLK_ICON_Y      13
#define CLK_ICON_R      6                     /* 表盘半径,直径 13 */

/* ---- 固定 IP(可选)----
   为什么要固定:电脑端的推流程序要按地址连过来。走 DHCP 的话地址可能变,
   一变就得两边重新改配置。注意 IP 别落在路由器的 DHCP 池里,
   否则可能和别的设备撞车(路由器分配时不会考虑已经在用的人)。

   ★ 默认【不定义】-> 走 DHCP。
     公开仓库的默认值必须能直接用在任何人的网络上,所以这里不能写死
     作者家里的网段 —— 那样别人编译出来会拿不到网关(NTP 失败),甚至撞地址。

   想用自己的固定地址,就在 wifi_secrets.h 里定义这 6 个数
   (那个文件不进版本库,所以你家网段不会漏出去):

       #define STATIC_IP_0  192
       #define STATIC_IP_1  168
       #define STATIC_IP_2  1
       #define STATIC_IP_3  182
       #define STATIC_GW_0  192
       #define STATIC_GW_1  168
       #define STATIC_GW_2  1
       #define STATIC_GW_3  1

   6 个数齐全才生效 —— 下面用 STATIC_IP_3 当哨兵,缺一个就当没定义。 */

#define PAGE_CLOCK    0
#define PAGE_MUSIC    1
#define PAGE_SYS      2                /* 历史曲线页 */

/* ==================== 运行状态 ==================== */
static EventGroupHandle_t s_wifi_events = NULL;
static volatile int  s_page          = PAGE_CLOCK;
/* 曲线页里长按 KEY = 让它立刻重画一次(平时是 2 秒一次) */
static volatile bool s_sys_refresh   = false;
#define WIFI_RETRY_BIT      BIT0

static volatile bool s_wifi_connected = false;
static volatile bool s_time_synced    = false;
static volatile bool s_use_24h        = true;
static bool          s_sntp_started   = false;

/* ==================== 音乐播放 ==================== */
/* 只放 TF 卡上的 WAV */
#define MUSIC_CHUNK_BYTES  4096     /* 每次从卡里读这么多,≈21ms 的音频 */

static volatile bool s_music_playing = false;   /* 开机不自动播,等用户按 BOOT */
static volatile int  s_music_pos     = 0;       /* 当前块号,频谱用 */

/* 频谱的来源。BOOT 长按按住不放就在这三个之间轮换,松手定格。
   注意"电脑推流"是个外部事件 —— 电脑没连的时候选它,频谱就是静止的,
   这是正常的(来源确实没数据),不是坏了。 */
#define SRC_PLAYBACK   0    /* TF 卡里的 WAV */
#define SRC_MIC        1    /* ES7210 麦克风 */
#define SRC_STREAM     2    /* 电脑推流过来的声音 */
#define SRC_COUNT      3
static volatile int  s_spec_src      = SRC_PLAYBACK;   /* BOOT 长按轮换 */

/* 播放清单:0 = 内嵌 PCM,1..N = TF 卡上第 N 个 WAV 文件。
   没插卡时就只有第 0 首。 */
static volatile int  s_track      = 0;      /* 想要的曲目(按键任务写) */
static int           s_track_loaded = -1;   /* 已经打开的曲目(音乐任务写) */
static int           s_track_cnt  = 0;      /* 卡上有几个 WAV */
static char          s_track_disp[28] = "无音乐文件";   /* 界面上显示的名字 */

/* 从 TF 卡读出来的一块 PCM。放静态区,不压任务栈。 */
static int16_t       s_pcm[MUSIC_CHUNK_BYTES / 2];

static float         s_temp      = 0.0f;
static float         s_humi      = 0.0f;
static bool          s_sensor_ok = false;

/* 顶栏两个指示器的数据源(SensorTask 每3秒刷一次) */
static int           s_batt_level = 0;      /* 0~100 */
static bool          s_batt_ok    = false;  /* 是否真接着电池 */
static bool          s_sd_mounted = false;
static Battery_Power s_power      = BATTERY_POWER_UNKNOWN;

/* ---------------- 主界面 4 根竖向资源柱 ----------------
   左两组:CPU / GPU;右两组:↓ 下载 / ↑ 上传。
   柱高就是数值,右手边摆三行文字(名称 / 数值 / 单位)。

   为什么名称写 ASCII 的 CPU/GPU 而不是汉字:字库里 ASCII 是全量的
   (0x20~0x7E)，汉字是“只带真正用到的”，用 CPU/GPU 就不必改字库重跑生成器。

   柱体 = 外框 + 从底往上填的内芯。内芯必须先擦干净再填，
   否则数值从 80% 掉到 20% 会留一截旧柱子。
   柱高和两行文字都缓存上一帧画了什么，一样就一个像素都不动。 */
/* 名字统一用 VBAR_ 前缀 —— 频谱区已经用了 BAR_W / BAR_GAP / BAR_BOTTOM
   那一套，撞名会直接报 'BAR_W redefined' */
#define VBAR_N        5                  /* 一共几根柱子:CPU/GPU/MEM + ↓/↑ */
/* 上下左右都不留黑边:上边顶到日期下面那条分隔线,下边离屏底 5px;
   左右铺满(列坐标见下面 s_col_x)。
   加到第 5 根时把柱宽 32->28、间距 8->6 才塞得下 ——
   文字框 40px 不能动("100°C" 就要 39px),只能从柱子和间距里抠。 */
#define VBAR_Y        170                /* 柱条顶边 */
#define VBAR_H        126
#define VBAR_W        28
#define VBAR_R        4                  /* 外框圆角 */
#define VBAR_INSET    3                  /* 内芯相对外框缩进 */
#define VBAR_GAP      6                  /* 柱子与右边文字的间距 */
#define VBAR_TXT_W    40                 /* 文字块宽。14px 下最宽的是 "100°C"(39px) */

/* 三行等大,按柱高比例摆开(约 1/4、1/2、3/4 处)。
   每行擦除框高 23px,三个框之间隔着十几个像素,不会互相擦。 */
#define VBAR_L1_B     201                /* "CPU" / "↓"     这一行基线 */
#define VBAR_L2_B     240                /* "42%" / "4110"  数值基线   */
#define VBAR_L3_B     278                /* "27°C" / "KB/s" 这一行基线 */

/* 数值字号 = 和名称同号。
   试过 ascii28(墨迹 21px) 和 cjk20(15px)，在 400x300 上都嫌大、太抢眼；
   三行等大反而最整齐。 */
#define VBAR_NUM_FONT (&ui_font_ascii14)

/* 网速柱的满刻度。网速没有天然的 0~100%，得给个上限才能算柱高。
   ★ 上下行必须分开给 —— 它们本来就不对称，用一个数的话
     要么下载一动就顶格，要么上传永远只有一丝。

     下行 = 内网 2.5GbE。单位要注意:小工具推过来的数除过 1024，
     是 KiB/s 而不是十进制的 KB/s，所以不能直接写 312500:
         2.5Gbps / 8 = 312.5 MB/s(十进制)
         312.5e6 / 1024 = 305175 KiB/s
     满柱时屏上显示 305175/1024 = 298.0 MB/s —— 这里的 MB/s 也按 1024 算，
     和“2.5G”的十进制定义差 2.4%，对柱高无所谓。

     上行 = 宽带套餐 5 MB/s = 5000 KiB/s(如果以后换了套餐改这里) */
#define NET_FULL_DN_KBPS  305175
#define NET_FULL_UP_KBPS    5000

/* 网速显示的数值和单位都是自适应的:
   低于 1024 KiB/s 用 KB/s(整数)，涓流也能看得见;
   高了换 MB/s —— 2.5G 是 305175 KiB/s，6 位数在 40px 的文字框里放不下。
   分界用 1024 不是 1000:数据本来就是按 1024 除过来的。 */

/* 版本标记。改这块的排版就换个号。
   烧录后串口会打出来 —— 用来确定“板子上跑的到底是哪一版”，
   免得“改了但看起来没变”时分不清是没烧进去还是真没变。 */
#define RESBAR_TAG "resbar-H9"

typedef struct {
    const char *title;              /* 静态的第一行,整页重画时画一次 */
    char        l2[12];             /* 上一帧的第二行,"42%" / "4110"  */
    char        l3[12];             /* 上一帧的第三行,"27°C" / "KB/s"  */
    int         pct;                /* 上一帧的柱高百分比             */
} ResBar_t;

/* 五根柱子左边对齐的 x。单元宽 74 = 柱 28 + 间距 6 + 文字 40,
   步进 80,最后一根 322+74 = 396,右边缘留 4px。
   三根占用率(CPU/GPU/MEM)在左,两根网速在右。 */
static const int s_col_x[VBAR_N] = { 2, 82, 162, 242, 322 };

/* 每个成员都要写出来:漏写会报 -Werror=missing-field-initializers。
   pct = -1 表示“屏上还没画过柱子”，第一帧一定会重画。 */
static ResBar_t s_card[VBAR_N] = {
    { "CPU", "", "", -1 },
    { "GPU", "", "", -1 },
    { "MEM", "", "", -1 },
    { "↓",   "", "", -1 },
    { "↑",   "", "", -1 },
};

/* 上一次显示的文本,内容不变就不重绘。
   时钟不用这个缓存 —— 它由 s_clk_cache[] 逐位缓存(见 SetClockText)。 */
static int  s_last_log_min = -1;

static const char *kWeek[] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
};

/* 下面第 6 节里定义的,这里先用 */
static void DrawText(int anchor_x, int baseline, const UiFont *f,
                     const char *txt, int align, bool erase);
static bool Changed(char *cache, size_t cap, const char *txt);

/* ============================================================
 *  方块点阵时钟
 * ============================================================
 *  样式照用户给的参考图:白底 + 细网格,笔画由小方块拼成。
 *
 *  为什么白底能成立(黑白屏只有两个色,没有"灰"可用):
 *      白底(亮) -> 1px 黑网格线(看着像浅灰) -> 粗黑数字(最黑)
 *      三级层次全靠【线宽】撑开。参考图靠的是灰度,这里换成线宽,
 *      效果等价。反过来斜(黑底 + 白网格)就不行 —— 网格会和字同色,
 *      一起抢注意力,数字反而看不清。
 */
/* 每个设计点放大成 3x3 屏幕像素 —— 恰好盖住 1 个网格格。
   ★ 这个值直接决定笔画粗细:笔画厚度 / 数字高度 = 1 / 点阵行数。
     8 行 x 6 = 笔画 6px(数字才 24px 宽,一块墨就占 1/4 -> 胖)
    16 行 x 3 = 笔画 3px(同样 48px 高,笔画减半 -> 正常)
   数字高度被时钟区锁在 48px,所以行数翻倍 = 倍数减半,二者等价。 */
#define CLK_SCALE     3
/* 方块之间的缝。★ 必须为 0:
   留缝时每个格只填一部分,黑像素占比掉到 44%,远看整串数字会混成灰色
   (白底上尤其明显)。1bpp 屏没有"浅一点的缝"可用,缝只能是纯白,
   等于拿对比度去换纹理 —— 数字是信息主体,不该这么换。 */
#define CLK_GAP       0
#define CLK_DW        (CLK_BM_W * CLK_SCALE)   /* 数字宽 24 */
#define CLK_DH        (CLK_BM_H * CLK_SCALE)   /* 数字高 48 */
#define CLK_PAD       (CLK_SCALE * 2)          /* 相邻数字间距 6(2 个设计点):组内 */
/* 冒号槽。★ 这个值控制的是【组与组之间的距离】,不能只按冒号本体算:
   方点自己 6px 宽,槽只有 12px 时居中后数字离方点只剩 3px ——
   比组内的 6px 还小,等于"组内松、组间紧",所以看着挤。
   给到 30px(方点 6 + 左右各 12),数字到方点的空档就是组内的 2 倍,
   HH / MM / SS 三块的边界靠空档自己就分开了,不用加多余的东西。 */
#define CLK_COLONW    (CLK_SCALE * 10)         /* 冒号槽 30 */
#define CLK_GRID      3                        /* 背景网格步距固定 3px,不跟放大倍数走 */

/* 白底 + 网格的范围。左右避开两根温湿度柱(2..26 / 374..398),
   上下:上边离第一根分隔线 2px,下边到 122(不去碰日期行)。 */
#define CLK_BG_X0     30
#define CLK_BG_X1     370
#define CLK_BG_Y0     44
#define CLK_BG_Y1     122

/* 整串 "HH:MM:SS" 的宽 234 = 6*24 + 2*30 + 5*6 */
#define CLK_STR_W     (6 * CLK_DW + 2 * CLK_COLONW + 5 * CLK_PAD)
/* 起点必须对齐到网格,否则数字方块和背景格子错开几个像素,
   "严丝合缝"就没了。CLK_BG_X0 / CLK_BG_Y0 本身就是 3 的倍数,
   所以只要把相对偏移取整到 CLK_GRID 的整数倍就行。
   这里取【四舍五入】而不是直接截断 —— 截断会让整串偏左 2px 左右。 */
#define CLK_STR_X     ((((400 - CLK_STR_W) / 2 + CLK_GRID / 2) / CLK_GRID) \
                       * CLK_GRID)
#define CLK_STR_Y     (CLK_BG_Y0 + (((CLK_BG_Y1 - CLK_BG_Y0) - CLK_DH) / 2 \
                                    / CLK_GRID) * CLK_GRID)

/* 一个方块数字。填【黑】(0)—— 白底上用黑字 */
static void DrawClockDigit(char ch, int x, int y)
{
    if (ch < '0' || ch > '9') {
        return;
    }
    const uint8_t *g = kClkDigit[ch - '0'];
    for (int r = 0; r < CLK_BM_H; r++) {
        uint8_t bits = g[r];
        if (bits == 0) {
            continue;
        }
        for (int c = 0; c < CLK_BM_W; c++) {
            if (bits & (0x80 >> c)) {
                Ui_FillRect(x + c * CLK_SCALE, y + r * CLK_SCALE,
                            CLK_SCALE - CLK_GAP, CLK_SCALE - CLK_GAP, 0);
            }
        }
    }
}

/* 冒号自己画:两个方点。不依赖任何字体,粗细自己说了算。也是黑的。
   竖向位置压在【设计点】上(CLK_SCALE 的整数倍):16 行高里取第 3 行和第 11 行,
   大致是竖直方向的 1/4 和 3/4 处,不和数字错位 */
static void DrawClockColon(int x, int y)
{
    const int d  = CLK_SCALE * 2;               /* 方点边长 6 = 2x2 设计点 */
    const int x0 = x + (CLK_COLONW - d) / 2;
    Ui_FillRect(x0, y + CLK_SCALE * 3,  d - CLK_GAP, d - CLK_GAP, 0);
    Ui_FillRect(x0, y + CLK_SCALE * 11, d - CLK_GAP, d - CLK_GAP, 0);
}

/* 铺白底 + 黑网格。先把整块刷白,数字没盖到的地方就是"格子纸" */
static void DrawClockBackdrop(void)
{
    Ui_FillRect(CLK_BG_X0, CLK_BG_Y0, CLK_BG_X1 - CLK_BG_X0,
                CLK_BG_Y1 - CLK_BG_Y0, 1);
    for (int x = CLK_BG_X0; x < CLK_BG_X1; x += CLK_GRID) {
        Ui_VLine(x, CLK_BG_Y0, CLK_BG_Y1 - CLK_BG_Y0, 0);
    }
    for (int y = CLK_BG_Y0; y < CLK_BG_Y1; y += CLK_GRID) {
        Ui_HLine(CLK_BG_X0, y, CLK_BG_X1 - CLK_BG_X0, 0);
    }
}

/* 画整串时间。text 必须是 CLK_NCHARS 个字符 "HH:MM:SS" */
static void DrawClockBig(const char *text)
{
    int x = CLK_STR_X;
    for (int i = 0; i < CLK_NCHARS; i++) {
        if (text[i] == ':') {
            DrawClockColon(x, CLK_STR_Y);
            x += CLK_COLONW;
        } else {
            DrawClockDigit(text[i], x, CLK_STR_Y);
            x += CLK_DW;
            if (i + 1 < CLK_NCHARS && text[i + 1] != ':') {
                x += CLK_PAD;
            }
        }
    }
}

/* ============================================================
 *  工具:刷新时钟
 * ============================================================ */
/* text 必须是 8 个字符 "HH:MM:SS"。
   只比整串,变了就整块重画 —— 样式里带了底板和网格,逐个格子擦的话
   还得把被擦掉的网格线补回来,反而复杂又容易出错。 */
static void SetClockText(const char *text, const char *ampm)
{
    bool changed = false;
    for (int i = 0; i < CLK_NCHARS; i++) {
        if (s_clk_cache[i] != text[i]) {
            changed = true;
            break;
        }
    }
    if (changed) {
        memcpy(s_clk_cache, text, CLK_NCHARS);
        DrawClockBackdrop();
        DrawClockBig(text);
    }

    /* AM/PM 挪到右柱数值上方那一行。
       原来画在 (312, 110) 用 ascii28,正好压在湿度数值那块(308..355),
       两边一画就互擦。现改用 14px、右对齐到 356、基线 62:
       擦除区 y=45..67,而湿度数值的擦除区从 y=74 才开始。 */
    if (ampm == NULL) {
        if (s_p_ampm[0]) {          /* 从 12 小时制切回 24 小时制,擦掉 AM/PM */
            DrawTextRightBox(AMPM_TXT_R, AMPM_TXT_B, AMPM_TXT_W,
                             &ui_font_ascii14, "");
            s_p_ampm[0] = '\0';
        }
        return;
    }
    if (Changed(s_p_ampm, sizeof(s_p_ampm), ampm)) {
        DrawTextRightBox(AMPM_TXT_R, AMPM_TXT_B, AMPM_TXT_W,
                         &ui_font_ascii14, s_p_ampm);
    }
}

/* ============================================================
 *  1. 时间: NTP 对时成功 -> 回写 RTC
 * ============================================================ */
static void OnTimeSynced(struct timeval *tv)
{
    s_time_synced = true;

    time_t t = tv->tv_sec;
    struct tm ti;
    localtime_r(&t, &ti);
    ESP_LOGI(TAG, "NTP 对时成功: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);

    /* 把准确时间写进 RTC,之后断电重启也能立即走时 */
    if (Rtc_SyncFromSystem()) {
        ESP_LOGI(TAG, "时间已写入 PCF85063 RTC");
    } else {
        ESP_LOGW(TAG, "写入 RTC 失败");
    }
}

static void StartSntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    cfg.sync_cb     = OnTimeSynced;
    cfg.smooth_sync = false;

    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
    ESP_LOGI(TAG, "SNTP 已启动  服务器:%s", NTP_SERVER);
}

/* ============================================================
 *  2. WiFi 事件
 * ============================================================ */
static void OnWifiEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_wifi_ssid[0] == '\0') {
            /* 空 SSID 时 esp_wifi_connect() 会直接报错,叫了也没用 */
            ESP_LOGW(TAG, "没配过 WiFi:把 wifi.txt 放进 TF 卡根目录后重启"
                          "(格式:ssid=xxx / pass=xxx)");
        } else {
            ESP_LOGI(TAG, "正在连接 WiFi \"%s\" ...", s_wifi_ssid);
            esp_wifi_connect();
        }

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_time_synced    = false;
        s_ip_str[0]      = '\0';     /* 断线就不该再显示旧地址 */
        ESP_LOGW(TAG, "WiFi 断开");
        /* 不能在这里 delay,会阻塞系统事件任务 */
        xEventGroupSetBits(s_wifi_events, WIFI_RETRY_BIT);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_wifi_connected = true;
        /* 记下实际生效的地址给顶栏用。固定 IP 也走这条路径,
           所以屏上显示的一定是真正在用的那个。 */
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "WiFi 已连接,IP = %s", s_ip_str);

        if (!s_sntp_started) {
            s_sntp_started = true;
            StartSntp();
        }
    }
}

/* ---------- WiFi 凭据:优先用 TF 卡里的 wifi.txt ---------- */

/*
 * 从 TF 卡读 wifi.txt。格式:一行一条,# 开头算注释
 *
 *     ssid=我家的WiFi
 *     pass=密码
 *
 * 至少要有 ssid= 才算成功;成功就用它覆盖 s_wifi_ssid / s_wifi_pass。
 *
 * 为什么是在这里读、而不是 WifiInit() 里:
 *   TF 卡是 WifiInit() 之后才挂载的,那时侯 fopen 一定失败。
 *   本任务是在挂载之后才创建的,所以由它来补读。
 */
static bool WifiLoadFromSd(void)
{
    int r = WifiCfgParse(WIFI_CFG_PATH,
                         s_wifi_ssid, sizeof(s_wifi_ssid),
                         s_wifi_pass, sizeof(s_wifi_pass));

    if (r == 1) {
        ESP_LOGI(TAG, "已从 TF 卡读到 WiFi 配置: \"%s\"", s_wifi_ssid);
        return true;
    }
    if (r == 0) {
        /* 把实际生效的 SSID 一并打出来 —— 只写"继续用默认凭据"是不够的:
           万一默认值被清空了,这句话就是假的,日志会骗人。 */
        ESP_LOGI(TAG, "TF 卡里没有 %s,继续用默认凭据 \"%s\"",
                 WIFI_CFG_PATH, s_wifi_ssid);
    } else {
        ESP_LOGW(TAG, "%s 里没找到可用的 ssid=,忽略", WIFI_CFG_PATH);
    }
    return false;
}

/* 把 s_wifi_ssid / s_wifi_pass 写回驱动并重连(此时 WiFi 已经 start 过了) */
static void WifiApplyConfig(void)
{
    wifi_config_t wifi_config = {};

    strncpy((char *)wifi_config.sta.ssid, s_wifi_ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_wifi_pass,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    /* 没连着的时候 disconnect 会返回错误,正常,不用管 */
    esp_wifi_disconnect();
    esp_wifi_connect();
}

static void WifiRetryTask(void *arg)
{
    for (;;) {
        /* 还没读过卡里配置时,等待要带超时:
           SSID 为空时 esp_wifi_connect() 直接返回错误、连断开事件都不产生,
           这任务要是死等事件就永远不醒,卡里的配置也就永远读不到。 */
        TickType_t wait = s_wifi_cfg_tried_sd ? portMAX_DELAY
                                              : pdMS_TO_TICKS(3000);
        xEventGroupWaitBits(s_wifi_events, WIFI_RETRY_BIT,
                            pdTRUE, pdFALSE, wait);

        /* 卡里的配置优先,而且必须放在"已连上"判断之前 ——
           否则默认凭据连得太快(1 秒多就连上了),这里永远轮不到读卡。 */
        if (!s_wifi_cfg_tried_sd) {
            s_wifi_cfg_tried_sd = true;
            if (WifiLoadFromSd()) {
                WifiApplyConfig();          /* 用卡里的凭据重连 */
                continue;
            }
        }

        if (s_wifi_connected) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
        /* SSID 为空 = 还没配过网,重试也是白搭,省点日志 */
        if (!s_wifi_connected && s_wifi_ssid[0] != '\0') {
            ESP_LOGI(TAG, "重连 WiFi ...");
            esp_wifi_connect();
        }
    }
}

/* ============================================================
 *  3. 刷新界面(需持有 Lvgl 锁)
 * ============================================================ */
static void UiRefreshUnderLock(void)
{
    char buf[64];
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);

    /* ---------- 埋点:检测"秒跳变"(正常应该每次 +1) ---------- */
    {
        static int dbg_last_sec = -1;
        if (now >= TIME_VALID_THRESHOLD && dbg_last_sec >= 0 && ti.tm_sec != dbg_last_sec) {
            int d = (ti.tm_sec - dbg_last_sec + 60) % 60;
            if (d != 1) {
                ESP_LOGW(TAG, ">>> 秒跳变异常: %d -> %d (间隔 %d 秒)",
                         dbg_last_sec, ti.tm_sec, d);
            }
        }
        dbg_last_sec = ti.tm_sec;
    }

    /* ---------- 时钟 ---------- */
    if (now < TIME_VALID_THRESHOLD) {
        SetClockText("00:00:00", NULL);
    } else {
        char       clk[16];
        const char *ampm = NULL;
        if (s_use_24h) {
            snprintf(clk, sizeof(clk), "%02d:%02d:%02d",
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
        } else {
            int h12 = ti.tm_hour % 12;
            if (h12 == 0) {
                h12 = 12;
            }
            snprintf(clk, sizeof(clk), "%02d:%02d:%02d",
                     h12, ti.tm_min, ti.tm_sec);
            ampm = (ti.tm_hour < 12) ? "AM" : "PM";
        }
        SetClockText(clk, ampm);

        snprintf(buf, sizeof(buf), "%d年%d月%d日  %s",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 kWeek[ti.tm_wday]);
        if (Changed(s_p_date, sizeof(s_p_date), buf)) {
            DrawText(200, 122 + ui_font_cjk20.base, &ui_font_cjk20,
                     s_p_date, 0, true);
        }

        if (ti.tm_min != s_last_log_min) {
            s_last_log_min = ti.tm_min;
            ESP_LOGI(TAG, "%04d-%02d-%02d %02d:%02d:%02d  温度%.1f℃ 湿度%.0f%%",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                     ti.tm_hour, ti.tm_min, ti.tm_sec, s_temp, s_humi);
        }
    }

    /* ---------- 左上:WiFi 名字(从 wifi.txt 换过名字时要重画) ---------- */
    {
        static char last_ssid[33] = "\x01";
        if (strncmp(last_ssid, s_wifi_ssid, sizeof(last_ssid) - 1) != 0) {
            snprintf(last_ssid, sizeof(last_ssid), "%s", s_wifi_ssid);
            DrawSsidLabel();
        }
    }

    /* ---------- 左上:IP:端口(连上 / 断开时要换) ---------- */
    {
        static char last_ip[24] = "\x01";   /* 初值故意不等,保证首次会画 */
        if (strncmp(last_ip, s_ip_str, sizeof(last_ip) - 1) != 0) {
            snprintf(last_ip, sizeof(last_ip), "%s", s_ip_str);
            DrawIpLabel();
        }
    }

    /* ---------- 右上:时间同步状态图标 ---------- */
    {
        static int last_st = -1;
        int st = !s_wifi_connected ? 0 : (s_time_synced ? 2 : 1);
        if (st != last_st) {
            last_st = st;
            DrawClockIcon(st == 2);
        }
    }

    /* ---------- 电池 + TF 卡(内容变了才重画) ---------- */
    DrawTopIndicators(false);

    /* ---------- 两根竖向资源柱:CPU / GPU(PC 推过来的) ---------- */
    UpdateResBars(false);

    /* ---------- 时钟两侧的温度 / 湿度柱 ---------- */
    UpdateSideBars(false);

    /* ============================================================
     * 关键:改完文字后立刻同步刷新到屏幕,不要等 LVGL 的定时器
     * ============================================================
     *
     * 原因:LVGL 的渲染循环周期 = lv_timer_handler() 的返回值 + 本次实际渲染耗时。
     *       平时没有内容变化时几乎不耗时;但每秒钟"秒数变化"那一次要重绘并进行
     *       一次全屏 flush(约 20ms),于是那一轮循环周期明显变长。
     *       这个周期性抖动让"渲染时刻"相对"整秒边界"不断漂移,
     *       最终表现就是秒的跳动时快时慢。
     *
     * 以前这里必须调 lv_refr_now() 立即渲染,目的是把 LVGL 的渲染相位
     * 和整秒边界对齐 —— LVGL 的循环周期会被"这一秒要重绘"拖长,
     * 导致渲染时刻相对整秒边界漂移,表现为秒的跳动时快时慢。
     *
     * 现在没有渲染循环了:改哪个字就画哪一块,画完直接推屏,
     * 严格落在整秒边界上。抖动的根子从源头上没了。
     */
    RlcdPort.RLCD_Display();
}

static void ClockTask(void *arg)
{
    /* ============================================================
     * 用【轮询】而不是"睡到整秒边界" —— 简单,且从原理上不可能漏秒
     * ============================================================
     *
     * 踩过的坑(实测埋点数据):
     *   一次刷新耗时 139ms(旧代码用 FULL 整屏渲染,每次都要重绘 12 万像素)。
     *   这占了 1 秒的 14%!所有"睡到下一个整秒再刷新"的算法都因此不可靠 ——
     *   只要采样点漂到整秒末尾,139ms 的刷新就会跨过一秒,
     *   下一次目标变成"下下个整秒",中间那一秒完全没刷新,
     *   屏幕上表现为 19 -> 21(跳 2 秒)。
     *
     * 现在:每 20ms 检查一次"秒数是否变化",变了就刷新 —— 每秒检查 50 次,
     *   任何一秒都不可能被漏掉。配合已改成的 PARTIAL 局部渲染
     *   (只重绘变化区域,单次约 20ms),端到端抖动 < 40ms,肉眼不可察。
     * ============================================================ */
    int last_sec = -1;

    for (;;) {
        time_t now = time(NULL);
        struct tm ti;
        localtime_r(&now, &ti);

        if (ti.tm_sec != last_sec) {
            last_sec = ti.tm_sec;

            /* 只在时钟页刷。
               以前这里不分页 —— 因为 LVGL 用 lv_obj_set_hidden() 把整页藏起来,
               写隐藏控件不会渲染到屏幕上。现在直接写显存,没有那层保护了,
               所以在音乐页上刷时钟会把温度/湿度/日期/时间全叠到频谱上。 */
            if (s_page != PAGE_CLOCK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            int64_t t0 = esp_timer_get_time();
            if (DispLock(-1)) {           /* 无限等锁,绝不丢帧 */
                UiRefreshUnderLock();
                DispUnlock();
            }
#if CLOCK_DBG_LOG
            ESP_LOGI(TAG, "[dbg] 刷新耗时=%dms",
                     (int)((esp_timer_get_time() - t0) / 1000));
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ============================================================
 *  4. 传感器任务(每 3 秒一次,避免自热)
 * ============================================================ */
static void SensorTask(void *arg)
{
    for (;;) {
        float t, h;
        if (Sensors_ReadTempHumi(&t, &h)) {
            s_temp      = t;
            s_humi      = h;
            s_sensor_ok = true;
        }

        /* 电池:ADC 采 16 次取平均,和温湿度一样 3 秒一次就够 */
        if (Battery_Poll() == ESP_OK) {
            bool has = Battery_HasBattery();
            int  lvl = Battery_GetLevel();
            Battery_Power pwr = Battery_GetPower();
            static bool logged = false;
            if (!logged || has != s_batt_ok || lvl != s_batt_level ||
                pwr != s_power) {
                logged = true;
                const char *ps = (pwr == BATTERY_POWER_INTERNAL)      ? "内部电池"
                               : (pwr == BATTERY_POWER_CHARGING)      ? "外接充电"
                               : (pwr == BATTERY_POWER_FULL)          ? "外接充满"
                               : (pwr == BATTERY_POWER_EXTERNAL_ONLY) ? "未接电池"
                                                                      : "未知";
                ESP_LOGI(TAG, "电池 %.2fV -> %d%%  电源=%s",
                         Battery_GetVoltage(), lvl, ps);
            }
            s_batt_ok    = has;
            s_batt_level = lvl;
            s_power      = pwr;
        }
        s_sd_mounted = Sdcard_IsMounted();

        /* 每 60 秒体检一次(传感器本来就是 3 秒一轮)。
           放在这里而不是时钟任务里 —— 时钟任务只在时钟页干活,
           停在音乐页就永远看不到报告。 */
        {
            static int health = 0;
            if (++health >= 20) {
                health = 0;
                ReportStacks();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ============================================================
 *  5. 按键任务(连续 3 次读到同一电平才认账 = 30ms 消抖)
 * ============================================================ */
/* 切页:直接重画整页 */
static void SwitchPage(int page)
{
    s_page = page;
    if (DispLock(-1)) {
        RenderPage();
        DispUnlock();
    }
    ESP_LOGI(TAG, "切到%s页", page == PAGE_MUSIC ? "音乐"
                            : page == PAGE_SYS  ? "曲线" : "时钟");
}

/* 往显存里填一个纯白/纯黑矩形。
   SpectrumDrawDirect 传的是底层色值(ColorBlack/ColorWhite),这里转一下。 */
static void RlcdFillRect(int x, int y, int w, int h, uint8_t color)
{
    Ui_FillRect(x, y, w, h, color ? 1 : 0);
}

/* 画频谱。调用者必须已持有 LVGL 锁。
   只重画"高度变化的那几行":柱子长高就填白那一段,变矮就把多出来的填黑。 */
static void SpectrumDrawDirect(void)
{
    float bands[SPECTRUM_BANDS];
    Spectrum_GetBands(bands);

    const int x0 = BAR_X0;

    /* 刚切到本页:LVGL 刚把这块刷成黑底,先把整块清干净再按 0 高度重画 */
    if (s_bars_need_clear) {
        s_bars_need_clear = false;
        RlcdFillRect(x0, PLOT_TOP, BAR_TOTAL, PLOT_H, ColorBlack);
        memset(s_bar_h, 0, sizeof(s_bar_h));
    }

    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        int h = (int)(bands[i] * BAR_MAX_H + 0.5f);
        if (h < 1) {
            h = 1;                      /* 静音时留一条 1px 基线 */
        }

        const int old = s_bar_h[i];
        if (h == old) {
            continue;                   /* 没变就不碰,这是快的关键 */
        }

        const int x = x0 + i * (BAR_W + BAR_GAP);
        if (h > old) {
            RlcdFillRect(x, BAR_BOTTOM - h, BAR_W, h - old, ColorWhite);
        } else {
            RlcdFillRect(x, BAR_BOTTOM - old, BAR_W, old - h, ColorBlack);
        }
        s_bar_h[i] = h;
    }

    /* ★ 网格点必须在柱子【之后】补。
       柱子是增量重画的:变矮那一段被填黑,之前画好的网格点会一起被
       抹掉。放这里补就不用做任何裁剪判断 —— 点落在柱子里就是白点
       (本来就白),落在空白处才显形,天然就是"柱子压在网格上"。
       代价:每帧 6 条线 x 90 个点 = 540 次单像素写,
       相对这块 359x185 = 6.6 万像素可以忽略。 */
    DrawDbGrid();

    RlcdPort.RLCD_Display();
}

/* ============================================================
 *  频谱刷新任务
 * ============================================================ */
static void SpectrumTask(void *arg)
{
    for (;;) {
        /* 直写显存后一帧只要 7~10ms,所以可以跑到 ~30fps。
           主要是 RLCD_Display() 要往 SPI 推 30KB,所以不宜再快。 */
        vTaskDelay(pdMS_TO_TICKS(33));

        /* BLE 状态推送放在页判断【之前】 —— 不然在时钟页就永远不推了。
           内部自己比对,内容没变不会真的发。 */
        Ble_Update();

        /* 不在音乐页就不白费力气 */
        if (s_page != PAGE_MUSIC) {
            continue;
        }

        /* 麦克风源:自己取一段 PCM 喂给 FFT。
           放在 LVGL 锁外面 —— 读 I2S 会阻塞几到几十毫秒,不能占着锁做。 */
        if (s_spec_src == SRC_MIC) {
            Spectrum_Enable(true);      /* 暂停音乐时被关过,这里确保开着 */
            static int16_t mic_buf[2048];
            if (Audio_ReadMic(mic_buf, sizeof(mic_buf)) == ESP_OK) {
                /* 头几次把原始波形统计打出来,确认 ES7210 真的在出数 */
                static int probe = 0;
                if (probe < 5) {
                    probe++;
                    int peak = 0;
                    for (int i = 0; i < 2048; i++) {
                        int v = mic_buf[i] < 0 ? -mic_buf[i] : mic_buf[i];
                        if (v > peak) {
                            peak = v;
                        }
                    }
                    ESP_LOGI(TAG, "麦克风采样自检: 峰值=%d/32767", peak);
                }
                Spectrum_Feed(mic_buf, sizeof(mic_buf) / 4);
            } else {
                ESP_LOGE(TAG, "麦克风读取失败");
            }
        }

        /* 临时诊断:约每秒打一次。打印最响的 5 个频段 ——
           只有 3 个纯音的话应该看到 3 根孤立的高柱,其余接近 0;
           如果一整片都在 0.5 以上,那就是进来噪声了而不是音频。
           自检 WAV 是 440Hz / 1kHz / 4kHz,对应频段约 26 / 40 / 63。 */
        {
            static int dbg_cnt = 0;
            if (++dbg_cnt >= 16) {
                dbg_cnt = 0;
                float b[SPECTRUM_BANDS];
                Spectrum_GetBands(b);

                int top[5] = {0, 0, 0, 0, 0};
                for (int i = 1; i < SPECTRUM_BANDS; i++) {
                    for (int k = 0; k < 5; k++) {
                        if (b[i] > b[top[k]]) {
                            for (int m = 4; m > k; m--) {
                                top[m] = top[m - 1];
                            }
                            top[k] = i;
                            break;
                        }
                    }
                }
                ESP_LOGI(TAG,
                         "[dbg] 源=%s 总=%.3f 峰值=%.1fdB @%uHz  top: %d=%.2f %d=%.2f %d=%.2f %d=%.2f %d=%.2f",
                         s_spec_src == SRC_MIC ? "麦克风" : "音乐",
                         (double)Spectrum_GetLevel(),
                         (double)Spectrum_GetPeakDb(),
                         (unsigned)Spectrum_GetSampleRate(),
                         top[0], (double)b[top[0]], top[1], (double)b[top[1]],
                         top[2], (double)b[top[2]], top[3], (double)b[top[3]],
                         top[4], (double)b[top[4]]);
            }
        }

        /* 麦克风模式要把显示下限拉深,否则远处的声音全被截成 0。
           实测(34.5dB 增益、安静房间):麦克风的 FFT 峰值在 -58~-71dB,
           平均约 -64dB —— 原来 -55dB 的下限等于把它整个滤掉了,
           所以远一点就完全不动。取 -72dB 时噪声占屏幕约 10%,
           而比噪声高 14dB 的声音能到 30% 左右,对比很清楚。

           注意:光加模拟增益解决不了这个问题 —— 信号和噪声一起抬高,
           信噪比没变。真正起作用的是这个显示下限。 */
        {
            float want = (s_spec_src == SRC_MIC) ? -72.0f : -55.0f;
            if (want != Spectrum_GetFloor()) {
                Spectrum_SetFloor(want);
            }
        }

        if (DispLock(200)) {
            /* 显示下限变了 -> 纵轴刻度数字要跟着重画,否则尺子是假的 */
            if (s_axis_floor != Spectrum_GetFloor()) {
                s_axis_floor = Spectrum_GetFloor();
                if (s_page == PAGE_MUSIC) {
                    RenderMusicPage();      /* 整页重画(会把柱子区清掉) */
                    s_bars_need_clear = true;
                }
            }

            /* 推流开始/结束 -> 底部按键提示得跟着换(整页重画一次) */
            {
                static bool last_stream = false;
                bool stream_now = NetAudio_IsStreaming();
                if (stream_now != last_stream) {
                    last_stream = stream_now;
                    RenderMusicPage();
                    s_bars_need_clear = true;
                }
            }

            int64_t t0 = esp_timer_get_time();

            SpectrumDrawDirect();

            /* 状态字显示的是"当前选中的来源" —— 用户按住 BOOT 轮换时,
               就靠它确认松手会停在哪一个 */
            const char *st;
            if (s_spec_src == SRC_MIC) {
                st = "麦克风频谱";
            } else if (s_spec_src == SRC_STREAM) {
                /* "等电脑连接"这一档很重要:源选在"电脑推流"但电脑端程序
                   没开(或者刚被"停止推流"关掉)时,屏幕必须把"在等"说清楚。
                   不然就是"选了电脑推流却一片安静",只能靠猜 ——
                   用户的第一反应会是"固件坏了"。 */
                st = !NetAudio_IsConnected() ? "等电脑连接" :
                     NetAudio_IsPaused()     ? "推流暂停"   : "电脑推流";
            } else {
                st = s_music_playing ? "播放中" : "已暂停";
            }
            if (Changed(s_p_mstate, sizeof(s_p_mstate), st)) {
                DrawTextRightBox(398, 13 + ui_font_cjk16.base, MSTATE_BOX_W,
                                 &ui_font_cjk16, s_p_mstate);
            }
            /* 中间这行写的是"现在这个频谱是从哪儿来的",跟着选中的来源走。
               它和右上角的状态字分的是两件事:
               状态字说"在不在放、放得对不对",这里说"看的是谁"。 */
            const char *track =
                (s_spec_src == SRC_STREAM) ? "来自电脑" :
                (s_spec_src == SRC_MIC)    ? "麦克风"   : "来自本地";
            if (Changed(s_p_track, sizeof(s_p_track), track)) {
                DrawTextCenterBox(200, 13 + ui_font_cjk16.base, TRACK_BOX_W,
                                  &ui_font_cjk16, s_p_track);
            }

            RlcdPort.RLCD_Display();

            int ms = (int)((esp_timer_get_time() - t0) / 1000);
            DispUnlock();

            static int cnt = 0;
            if (++cnt % 8 == 0) {
                ESP_LOGI(TAG, "[dbg] 频谱刷新耗时=%dms", ms);
            }
        }
    }
}

/* 换曲:下标只在 0..s_track_cnt-1 之间转。
   加 s_track_cnt 是为了 delta=-1 时不会出现负数取模。 */
static void MusicStep(int delta)
{
    s_track = (s_track + s_track_cnt + delta) % s_track_cnt;
    ESP_LOGI(TAG, "%s -> 第 %d/%d 首", (delta > 0) ? "下一首" : "上一首",
             s_track + 1, s_track_cnt);
}

/* 板子现在想不想要电脑的声音?net_audio 建连时会问一句。
   答案就是用户选的频谱源 —— 只有"电脑推流"才要。
   这样电脑一开始推流不会把正在放的 TF 卡音乐顶掉,
   而是安静地等着,用户切到"电脑推流"那一刻立刻有声。 */
static bool WantPcAudio(void)
{
    return s_spec_src == SRC_STREAM;
}

/* 切换频谱来源。集中在一处改:除了 s_spec_src 自己,还要通知推流任务
   "现在轮到你喂频谱了吗" —— 三个来源同时往同一个 FFT 里灌数据的话,
   柱子会是三者的混合物,谁也看不准。 */
static void SetSpecSource(int src)
{
    s_spec_src = ((src % SRC_COUNT) + SRC_COUNT) % SRC_COUNT;
    NetAudio_SetSpectrumEnabled(s_spec_src == SRC_STREAM);
    ESP_LOGI(TAG, "频谱源 -> %s(松手定在这里)",
             s_spec_src == SRC_MIC    ? "麦克风" :
             s_spec_src == SRC_STREAM ? "电脑推流" : "播放音乐");
}

/* 松手"定在这里"之后的收尾:把播放通道要回来。
 *
 * ★ 为什么必须有这一步:
 *   三个声源同时只能有一个在出声,但"选源"以前【只改了频谱显示来源】,
 *   没管播放通道 —— 电脑一推流就一直占着 DAC。于是用户看着屏幕上写着
 *   "频谱源 -> 播放音乐",按 BOOT 却什么都不动(那一下被当成"暂停推流"了),
 *   而且看日志也看不出问题。
 *
 * ★ 为什么放在【松手】而不是切源那一刻:
 *   长按轮换会一路经过三个源。要是每经过一个就把推流断一次,
 *   wascap 会不停重连,体验很差。只有最终停在哪个才算数。
 *
 * 停在"电脑推流"上就什么都不做 —— 通道本来就该是它的。
 * (发 STOP 后 wascap 是【干净退出】的,不会自己重连,所以通道能稳定交还。) */
static void CommitSpecSource(void)
{
    /* 停在"电脑推流"上:把喇叭要回来。
       连接一直留着(wascap 从头到尾没退出过),所以这里【立刻】有声,
       不需要用户重新双击电脑上那个 exe。 */
    if (s_spec_src == SRC_STREAM) {
        if (NetAudio_SetYield(false)) {
            ESP_LOGI(TAG, "定在电脑推流,准备收回喇叭");
        }
        return;
    }

    /* 停在别的源上:让出喇叭,但【不发 STOP】。
     *
     * ★ 这里以前发的是 STOP,而 wascap 收到 STOP 是"干净退出"的。
     *   于是用户切回"电脑推流"时电脑端程序早就没了 —— 屏幕上写着"电脑推流",
     *   耳朵里一点声音都没有,还得自己想起来重新双击那个 exe 才行。
     *   现象看起来像"固件坏了",实际上只是两边对"停止"的理解不一致。
     *
     *   现在只"让出":连接和接收都保留,wascap 一直活着,切回来立刻有声。
     *   真要关掉电脑端程序是 KEY 长按("停止推流"),那是用户的明确意思。 */
    if (NetAudio_SetYield(true)) {
        ESP_LOGI(TAG, "定在 %s,已让电脑让出喇叭(它不用重开)",
                 s_spec_src == SRC_MIC ? "麦克风" : "播放音乐");
    }
}

#define KEY_LONG_TICKS   80     /* 80 x 10ms = 800ms 算长按 */
#define KEY_DBL_TICKS    28     /* 280ms 内再按一下算双击 */

/* BOOT 的按住时长(单位:还是 10ms 的那种循环次数)。
   短按 = 不到 2 秒就松手;过了 2 秒就进入"轮换选源":每 2 秒换一个源,
   松手停在当时那个。原来 800ms 就切源,经常"想切源却按太短"变成短按,
   或者反过来想播放/暂停却按久了把源切走 —— 拉长到 2 秒、并且让用户
   看着屏幕选,两头都稳。 */
#define BOOT_HOLD_TICKS  200    /* 2 秒:短按 / 长按的分界 */
#define BOOT_CYCLE_TICKS 200    /* 长按期间每 2 秒换一个源 */
#define BOOT_HOLD_MAX    3000   /* 按住计时上限(30 秒),防止一直累加 */

static void ButtonTask(void *arg)
{
    int key_stable = 1, key_cnt = 0, key_hold = 0;
    int boot_stable = 1, boot_cnt = 0, boot_hold = 0;

    bool key_long_fired = false;    /* 这一轮按住已经发过长按 */
    bool key_dbl_used   = false;    /* 这次松手已经被双击吃掉了 */
    bool key_pending    = false;    /* 有一次短按正在等第二击 */
    int  key_timer      = 0;

    for (;;) {
        int k = gpio_get_level(KEY_PIN);
        int b = gpio_get_level(BOOT_PIN);

        /* ---- KEY:短按切页,长按(≥800ms)切 12/24 小时制 ---- */
        if (k == key_stable) {
            key_cnt = 0;
            if (k == 0 && key_hold < 1000) {
                key_hold++;
                if (key_hold == KEY_LONG_TICKS) {
                    key_long_fired = true;
                    /* 先看有没有电脑在推流 —— 有就是"停止推流"。
                       其余时候的含义跟当前页面走:
                       时钟页 = 12/24 小时制,音乐页 = 下一首,
                       曲线页 = 让它立刻重画一次(不用等 2 秒周期)。
                       为何放 KEY 上:推流时 TF 卡已经让出播放通道,
                       "下一首"本来就无意义,正好空着;而停止推流是不可逆的,
                       不该跟"轮换频谱源"抢同一个键。 */
                    /* 只有停在"电脑推流"上,KEY 长按才是"停止推流"。
                       别的源上它去干本页的活(下一首 / 12-24 小时制 / 刷新),
                       免得"切源"和"停推流"又抢同一个动作。
                       注意判断用的是 IsConnected 而不是 IsStreaming:
                       喇叭让出去之后连接还在,用户仍然应该能把电脑端程序关掉。 */
                    if (s_spec_src == SRC_STREAM && NetAudio_IsConnected()) {
                        if (NetAudio_RequestStop()) {
                            ESP_LOGI(TAG, "已通知电脑停止推流");
                        }
                    } else if (s_page == PAGE_MUSIC) {
                        MusicStep(+1);
                    } else if (s_page == PAGE_SYS) {
                        s_sys_refresh = true;
                        ESP_LOGI(TAG, "曲线页立刻刷新");
                    } else {
                        s_use_24h = !s_use_24h;
                        for (int i = 0; i < CLK_NCHARS; i++) {
                            s_clk_cache[i] = 0;      /* 强制下一次全量重画 */
                        }
                        ESP_LOGI(TAG, "切换为 %s 小时制", s_use_24h ? "24" : "12");
                    }
                }
            }
        } else if (++key_cnt >= 3) {
            key_cnt = 0;
            key_stable = k;
            if (k == 0) {
                key_hold = 0;                   /* 按下:开始计时 */
                key_long_fired = false;
            } else {
                /* 松开 */
                if (!key_long_fired && !key_dbl_used) {
                    if (s_page == PAGE_MUSIC) {
                        /* 音乐页的短按要等一下 —— 它可能是双击(上一首)的前半。
                           时钟页/曲线页没双击,所以那边切页仍然是即时的。 */
                        key_pending = true;
                        key_timer   = KEY_DBL_TICKS;
                    } else {
                        /* 切页循环:时钟 -> 音乐 -> 曲线 -> 时钟。
                           音乐页在上面那个分支里,走不到这儿 */
                        SwitchPage(s_page == PAGE_CLOCK ? PAGE_MUSIC : PAGE_CLOCK);
                    }
                }
                key_dbl_used = false;
            }
        }

        /* 双击窗口:再按一下 = 上一首;超时 = 当成单次短按(切页) */
        if (key_pending) {
            if (k == 0) {
                key_pending = false;
                key_dbl_used = true;            /* 后面那次松手不要重复触发 */
                key_hold     = 1000;            /* 也别把后半程当成又一次长按 */
                MusicStep(-1);
            } else if (--key_timer <= 0) {
                key_pending = false;
                SwitchPage(PAGE_SYS);        /* 音乐页短按 -> 曲线页 */
            }
        }

        /* ---- BOOT:短按播放/暂停,长按(≥2s)轮换频谱源 ---- */
        if (b == boot_stable) {
            boot_cnt = 0;
            if (b == 0 && boot_hold < BOOT_HOLD_MAX) {
                boot_hold++;
                /* 到 2 秒开始换,之后每 2 秒再换一个,松手停在当时那个。
                   推流时也一样 —— 这里不再管推流了,免得"想切源"和
                   "停止推流"抢同一个动作(那正是之前误触的根源)。 */
                if (boot_hold >= BOOT_HOLD_TICKS &&
                    (boot_hold - BOOT_HOLD_TICKS) % BOOT_CYCLE_TICKS == 0) {
                    SetSpecSource(s_spec_src + 1);
                }
            }
        } else if (++boot_cnt >= 3) {
            boot_cnt = 0;
            boot_stable = b;
            if (b == 0) {
                boot_hold = 0;                      /* 按下:开始计时 */
            } else if (boot_hold < BOOT_HOLD_TICKS) {
                /* 短按:推流中 -> 本地暂停/继续(可逆,连接不断);
                   平时   -> 播放/暂停 TF 卡音乐。
                   "停止推流"在 KEY 长按上,BOOT 长按专心切源。 */
                if (NetAudio_IsStreaming()) {
                    bool want_pause = !NetAudio_IsPaused();
                    if (NetAudio_SetPaused(want_pause)) {
                        ESP_LOGI(TAG, "推流%s", want_pause ? "已暂停" : "继续");
                    }
                } else {
                    s_music_playing = !s_music_playing;
                    ESP_LOGI(TAG, "%s音乐", s_music_playing ? "继续播放" : "暂停");
                }
            } else {
                /* 长按松手:源已经定下来了,收尾 ——
                   如果定的不是"电脑推流",就把播放通道从电脑手里要回来,
                   否则按 BOOT 根本放不了本地音乐(通道还在电脑那儿)。 */
                CommitSpecSource();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ============================================================
 *  5.5 音乐播放(TF 卡 WAV)
 * ============================================================ */
/* 打开第 idx 首(从 0 起)。只能在 MusicTask 里调 —— 它会改 I2S 的采样率,
   而同时只有音乐任务在往播放通道写数据。 */
static void MusicOpenTrack(int idx)
{
    if (idx < 0 || idx >= s_track_cnt || Wav_FilePath(idx) == NULL) {
        Wav_Close();
        s_track_loaded = idx;      /* 标成已处理,否则每轮都会重试 */
        snprintf(s_track_disp, sizeof(s_track_disp), "无音乐文件");
        return;
    }

    if (Wav_Open(Wav_FilePath(idx)) != ESP_OK) {
        /* 这首有问题(格式不对 / 文件坏了)。照样标成已加载,
           这样 Wav_Read 会返回 0,自然跳到下一首,不会卡死。 */
        ESP_LOGW(TAG, "第 %d 首打不开,跳过", idx + 1);
        s_track_loaded = idx;
        return;
    }

    const WavInfo *in = Wav_GetInfo();
    Audio_SetSampleRate(in->sample_rate);        /* 44100 / 48000 ... */
    Spectrum_SetSampleRate(in->sample_rate);

    /* 界面只显示文件名。%.20s 一是不想把标题栏撑爆,
       二是避开 -Werror=format-truncation(编译器假定 %s 能到 255 字节)。 */
    snprintf(s_track_disp, sizeof(s_track_disp), "%.20s", Wav_FileName(idx));
    s_track_loaded = idx;
    ESP_LOGI(TAG, "播放第 %d/%d 首: %s", idx + 1, s_track_cnt, s_track_disp);
}

static void MusicTask(void *arg)
{
    bool playing = false;

    for (;;) {
        /* 没插卡 / 卡里没 WAV 就歇着,别空转 */
        if (s_track_cnt <= 0) {
            if (playing) {
                Audio_Mute(true);
                Spectrum_Enable(false);
                playing = false;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* ★ 本地音乐【只在"播放音乐"这个源下】才放。
           以前这里只挡了麦克风和推流:
               !s_music_playing || s_spec_src == SRC_MIC || NetAudio_IsStreaming()
           于是"切到电脑推流、但电脑还没连上"时三条全不成立,TF 卡继续响 ——
           屏幕上的源写着"电脑推流",耳朵里却是本地音乐,谁都看不明白。

           改成只认一个正条件,规则就一句话:**源就是你听到的声音**。
             源 = 播放音乐 + 电脑没占着喇叭  -> 放 TF 卡
             源 = 麦克风 / 电脑推流          -> 静音(哪怕电脑还没连上也一样)
             电脑占着喇叭                    -> 让位(它的 DAC 不能和播放抢)
           麦克风那条还多一层原因:喇叭的声音会被自己的麦克风收进去,变成回授。

           注意 NetAudio_IsStreaming() 问的是"电脑【占着喇叭】吗",
           而不是"电脑连着吗" —— 让出通道期间连接还在,但它返回 false,
           喇叭就该还给 TF 卡(见 net_audio.c 里 s_yield 的说明)。 */
        if (!s_music_playing || s_spec_src != SRC_PLAYBACK ||
            NetAudio_IsStreaming()) {
            if (playing) {
                /* ⚠️ 因为"电脑占着喇叭"而停的这一次,【不能 mute DAC】:
                   DAC 此时归 net_audio 管 —— 它拿走通道时刚 unmute 过,
                   我们后手再 mute 一下就静音了,而且它不会再 unmute,
                   现象是"电脑接上了但没声音",查起来完全想不到是播放任务干的。
                   net_audio 断开或让出时自己会静音,不需要我们代劳。 */
                if (!NetAudio_IsStreaming()) {
                    Audio_Mute(true);      /* 暂停 = 把 DAC 静音 */
                }
                Spectrum_Enable(false);    /* 柱子归零 */
                playing = false;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        if (!playing) {
            Audio_Mute(false);
            Spectrum_Enable(true);
            playing = true;
            ESP_LOGI(TAG, "开始播放");
        }

        /* 换曲:打开文件,并把 I2S / 频谱都切到这首的采样率 */
        if (s_track_loaded != s_track) {
            MusicOpenTrack(s_track);
        }

        size_t got = Wav_Read(s_pcm, MUSIC_CHUNK_BYTES);
        if (got == 0) {
            if (!Wav_IsOpen()) {
                /* 根本没打开(文件有问题)。等一下吧,不然会在几首坏文件
                   之间飞速空转 */
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            /* 这首放完 -> 下一首。取模保证下标始终在 0..cnt-1 里 */
            s_track = (s_track + 1) % s_track_cnt;
            continue;
        }

        if (Audio_PlayPcm(s_pcm, got) != ESP_OK) {
            ESP_LOGW(TAG, "播放失败,500ms 后重试");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* 只有轮到"本地播放"这个来源时才喂 —— 否则会和麦克风/推流掺在一起 */
        if (s_spec_src == SRC_PLAYBACK) {
            Spectrum_Feed((const int16_t *)s_pcm, got / 4);
        }
    }
}

/* ============================================================
 *  6. 界面绘制(深色 / 极简,不再用 LVGL)
 * ============================================================ */
bool DispLock(int wait_ms)
{
    if (s_disp_lock == NULL) {
        return true;
    }
    return xSemaphoreTake(s_disp_lock,
                          (wait_ms < 0) ? portMAX_DELAY
                                        : pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void DispUnlock(void)
{
    if (s_disp_lock != NULL) {
        xSemaphoreGive(s_disp_lock);
    }
}

/* 一条 1px 分隔线。左右各留 2px —— 和上下内容一样贴边 */
static void DrawDivider(int y)
{
    Ui_HLine(2, y, 396, 1);
}

/*
 * 在 (anchor_x, baseline) 处画一行字。
 * align: -1 = 左对齐(anchor_x 是左端), 0 = 居中(anchor_x 是中心),
 *         1 = 右对齐(anchor_x 是右端)。
 * erase = true 时先擦掉这一行的底色 —— 定宽字段更新时必须擦,
 *         否则新字比旧字短就会留下残笔。
 */
static void DrawText(int anchor_x, int baseline, const UiFont *f,
                     const char *txt, int align, bool erase)
{
    int w = Ui_TextWidth(f, txt);
    int x = (align > 0) ? (anchor_x - w)
          : (align == 0) ? (anchor_x - w / 2)
          : anchor_x;
    if (erase) {
        Ui_FillRect(x - 2, baseline - f->base, w + 4, f->line_h, 0);
    }
    Ui_Text(x, baseline, f, txt, 1);
}

/* 内容变了才返回 true,并把新内容存进 cache */
static bool Changed(char *cache, size_t cap, const char *txt)
{
    if (strncmp(cache, txt, cap - 1) == 0) {
        return false;
    }
    snprintf(cache, cap, "%s", txt);
    return true;
}

/* ============================================================
 *  顶栏指示器:电池 + TF 卡
 * ============================================================
 *  都是"图标 + 短文字"。文字部分用定宽框重画 —— 不定宽的话,
 *  比如 "100%" 变成 "9%" 时新字比旧字窄,旧笔迹会留在屏上。
 */

/* 在定宽框里重画一行字:x 是左边界。
   擦除区从基线往上推算(base + 2),而不是写死一个 y —— 换字体时
   line_h / base 会变(黑体→雅黑,cjk16 的 base 就从 14 变成 17),
   写死的话字会跑到擦除区外面,旧笔迹就留在屏上了。 */
static void DrawTextInBox(int x, int baseline, int box_w,
                          const UiFont *f, const char *txt)
{
    Ui_FillRect(x, baseline - f->base - 2, box_w, f->line_h + 4, 0);
    Ui_Text(x, baseline, f, txt, 1);
}

/* 右对齐版本:right_x 是右边界 */
static void DrawTextRightBox(int right_x, int baseline, int box_w,
                             const UiFont *f, const char *txt)
{
    Ui_FillRect(right_x - box_w, baseline - f->base - 2, box_w,
                f->line_h + 4, 0);
    int w = Ui_TextWidth(f, txt);
    Ui_Text(right_x - w, baseline, f, txt, 1);
}

/* 居中版本:center_x 是中心。曲目名用它,理由和上面一样。 */
static void DrawTextCenterBox(int center_x, int baseline, int box_w,
                              const UiFont *f, const char *txt)
{
    Ui_FillRect(center_x - box_w / 2, baseline - f->base - 2, box_w,
                f->line_h + 4, 0);
    int w = Ui_TextWidth(f, txt);
    Ui_Text(center_x - w / 2, baseline, f, txt, 1);
}

/* 定宽框里右对齐版的兄弟:这里不需要了，柱条旁边的数字用 DrawTextInBox。 */

/* ---- 圆角图元 ----
   半径很小时(2~3px),单色屏上的圆角就退化成一条 45 度斜线,
   一圈点上去就是干干净净的圆角矩形。 */
static void RoundFrame(int x, int y, int w, int h, int r)
{
    Ui_HLine(x + r, y, w - 2 * r, 1);
    Ui_HLine(x + r, y + h - 1, w - 2 * r, 1);
    Ui_VLine(x, y + r, h - 2 * r, 1);
    Ui_VLine(x + w - 1, y + r, h - 2 * r, 1);
    for (int i = 0; i < r; i++) {
        Ui_Pixel(x + r - 1 - i, y + i, 1);
        Ui_Pixel(x + w - r + i, y + i, 1);
        Ui_Pixel(x + r - 1 - i, y + h - 1 - i, 1);
        Ui_Pixel(x + w - r + i, y + h - 1 - i, 1);
    }
}

/* 实心圆角矩形:逐行算左右缩进,比画圆弧简单也不容易出错 */
static void RoundFill(int x, int y, int w, int h, int r)
{
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r) {
            inset = r - 1 - j;
        } else if (j >= h - r) {
            inset = r - 1 - (h - 1 - j);
        }
        if (inset < 0) {
            inset = 0;
        }
        Ui_FillRect(x + inset, y + j, w - 2 * inset, 1, 1);
    }
}

/* ---------------- 竖向资源柱 ---------------- */

/* 网速的"数值 + 单位"自适应格式化:低于 1024 KiB/s 用 KB/s(整数)，
   高了换 MB/s 保留一位小数。
   为什么必须分两档:固定 KB/s 时 2.5G 是 "305175"，6 位数在文字框里放不下;
   固定 MB/s 时涓流又全变成 "0.0"。分界取 1024 不是 1000 —— 数据本来就
   是按 1024 除过来的。
   MB/s 用纯整数算，不碰 %f —— 万一 IDF 开了 newlib nano 格式，%f 会直接打不出来 */
static void FmtSpeed(unsigned kbs, char *num, size_t num_cap, const char **unit)
{
    if (kbs < 1024u) {
        snprintf(num, num_cap, "%u", kbs);
        *unit = "KB/s";
    } else {
        unsigned tenths = (kbs * 10u + 512u) / 1024u;      /* 0.1 MB，四舍五入 */
        snprintf(num, num_cap, "%u.%u", tenths / 10u, tenths % 10u);
        *unit = "MB/s";
    }
}

/* 网速的紧凑写法:只留 K/M 后缀、不带斜杠。
   子图左上角只有 10px 小字的位置，"4.9 MB/s" 那种写法放不下 */
static void FmtSpeedShort(unsigned kbs, char *out, size_t cap)
{
    if (kbs < 1024u) {
        snprintf(out, cap, "%uK", kbs);
    } else {
        unsigned tenths = (kbs * 10u + 512u) / 1024u;
        snprintf(out, cap, "%u.%uM", tenths / 10u, tenths % 10u);
    }
}

/* 柱体:外框 + 从底往上填的内芯。
   必须先把内芯整块擦干净再填，否则柱高变矮时会留一截旧柱子。 */
static void DrawVBar(int bx, int pct)
{
    const int ix = bx + VBAR_INSET;
    const int iy = VBAR_Y + VBAR_INSET;
    const int iw = VBAR_W - 2 * VBAR_INSET;   /* 26 */
    const int ih = VBAR_H - 2 * VBAR_INSET;   /* 86 */
    int fh;

    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }

    Ui_FillRect(ix, iy, iw, ih, 0);
    RoundFrame(bx, VBAR_Y, VBAR_W, VBAR_H, VBAR_R);

    fh = ih * pct / 100;
    if (pct > 0 && fh < 3) {
        fh = 3;             /* 1~2% 也得看得见有这么一截 */
    }
    if (fh > 0) {
        Ui_FillRect(ix, iy + ih - fh, iw, fh, 1);
    }
}

/* 侧柱(时钟旁边的温度/湿度):和大柱同一个画法,只是尺寸小一号。
   同样必须先把内芯整块擦掉再填,否则柱高变矮时会留一截旧柱子。 */
static void DrawSideBar(int bx, int pct)
{
    const int ix = bx + SIDE_INSET;
    const int iy = SIDE_Y + SIDE_INSET;
    const int iw = SIDE_W - 2 * SIDE_INSET;   /* 18 */
    const int ih = SIDE_H - 2 * SIDE_INSET;   /* 56 */
    int fh;

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    Ui_FillRect(ix, iy, iw, ih, 0);
    RoundFrame(bx, SIDE_Y, SIDE_W, SIDE_H, SIDE_R);

    fh = ih * pct / 100;
    if (pct > 0 && fh < 3) {
        fh = 3;             /* 1~2% 也得看得见有这么一截 */
    }
    if (fh > 0) {
        Ui_FillRect(ix, iy + ih - fh, iw, fh, 1);
    }
}

/* 时钟两侧的两根小柱:左温度、右湿度。
   温度按 0~50°C 折成柱高,湿度本来就是 0~100%。
   数值没变就不重画(传感器 3 秒才读一次,时钟每秒都刷,不挡一下就白刷)。
   force = 整页刚清成黑的,必须无条件全画。 */
static void UpdateSideBars(bool force)
{
    static int last_t = -9999;
    static int last_h = -9999;
    char txt[16];
    int  t  = (int)(s_temp + (s_temp >= 0.0f ? 0.5f : -0.5f));
    int  h  = (int)(s_humi + 0.5f);
    int  tp;

    if (!force && t == last_t && h == last_h) {
        return;
    }
    last_t = t;
    last_h = h;

    /* ---- 左:温度。° 写成 UTF-8 的 C2 B0,并和 "C" 拼开 ——
       不拼开的话 \xB0C 会被当成一个十六进制转义( C 也是十六进制数字) ---- */
    tp = (int)((float)t / SIDE_TEMP_FULL * 100.0f + 0.5f);
    DrawSideBar(SIDE_T_X, tp);
    snprintf(txt, sizeof(txt), "%d\xC2\xB0" "C", t);
    DrawTextInBox(SIDE_T_TXT_X, SIDE_TXT_B, SIDE_T_TXT_W,
                  &ui_font_ascii14, txt);

    /* ---- 右:湿度 ---- */
    DrawSideBar(SIDE_H_X, h);
    snprintf(txt, sizeof(txt), "%d%%", h);
    DrawTextRightBox(SIDE_H_TXT_R, SIDE_TXT_B, SIDE_H_TXT_W,
                     &ui_font_ascii14, txt);
}

/* 增量重画一根柱子旁边会变的两行字和柱高。
   第一行(CPU/GPU/↓/↑)是静态的，由 RenderClockPage 画一次。
   force = 整页刚重画过(切页/开机)，屏上是空白，两行字和柱子都得全画。 */
static void DrawVBarDynamic(int k, const SysStatus_t *st, bool ok, bool force)
{
    ResBar_t *c  = &s_card[k];
    const int bx = s_col_x[k];
    const int tx = bx + VBAR_W + VBAR_GAP;
    char buf[24];
    int  pct;

    if (k < 3) {
        /* ---- 前三根:CPU / GPU / MEM,值都是占用率。
               第三行 CPU/GPU 显示温度(只有它们有温度),
               MEM 显示已用容量 —— 下面单独格式化。 ---- */
        int load, temp;
        if (k == 2) {
            load = ok ? st->ram : 0;
            temp = -1;
        } else {
            load = ok ? (k == 0 ? st->cpu : st->gpu) : 0;
            temp = ok ? (k == 0 ? st->cput : st->gput) : -1;
        }

        if (load < 0) {
            load = 0;
        }
        if (load > 100) {
            load = 100;
        }
        pct = load;

        snprintf(buf, sizeof(buf), "%d%%", load);
        if (force || strcmp(c->l2, buf) != 0) {
            /* 精度写死到目标缓冲区大小:直接 "%s" 的话 GCC 无法证明
               源有 24 字节、目标更小时不截断，会报 format-truncation */
            snprintf(c->l2, sizeof(c->l2), "%.11s", buf);
            DrawTextInBox(tx, VBAR_L2_B, VBAR_TXT_W, VBAR_NUM_FONT, c->l2);
        }

        if (k == 2) {
            /* 内存第三行:已用容量。取整到 GB —— 带小数就 5 字符,
               正好顶满 40px 的框,再宽一点就压到下一根柱子上了。 */
            if (ok && st->ram_total_mb > 0) {
                snprintf(buf, sizeof(buf), "%uG", st->ram_used_mb / 1024);
            } else {
                snprintf(buf, sizeof(buf), "--");
            }
        } else if (temp >= 0) {
            snprintf(buf, sizeof(buf), "%d°C", temp);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        if (force || strcmp(c->l3, buf) != 0) {
            snprintf(c->l3, sizeof(c->l3), "%.11s", buf);
            DrawTextInBox(tx, VBAR_L3_B, VBAR_TXT_W, &ui_font_ascii14, c->l3);
        }
    } else {
        /* ---- 后两根:↓ 下载 / ↑ 上传。值换算成 MB/s,单位与数值都自适应 ---- */
        unsigned kbs  = ok ? (k == 3 ? st->down_kbps : st->up_kbps) : 0;
        unsigned full = (k == 3) ? NET_FULL_DN_KBPS : NET_FULL_UP_KBPS;
        const char *unit;

        /* 上限卡住:既防 kbs*10 溢出，也防数字跑出文字框 */
        if (kbs > 999999u) {
            kbs = 999999u;
        }

        /* 先比再乘，避免 kbs 很大时 kbs*100 溢出 */
        if (kbs >= full) {
            pct = 100;              /* 超过满刻度就顶格，数值照样照实显示 */
        } else {
            pct = (int)(kbs * 100u / full);
            if (kbs > 0 && pct == 0) {
                /* 有流量但不到 1%。不让柱子完全空 —— 否则"有涓流"和
                   "链路断了"在屏上长得一模一样。1% 的柱高刚好看得见 */
                pct = 1;
            }
        }

        /* MB/s 保留一位小数。纯整数运算，不碰 %f ——
           万一 IDF 开了 newlib nano 格式，%f 会直接打不出来 */
        if (ok) {
            FmtSpeed(kbs, buf, sizeof(buf), &unit);
        } else {
            snprintf(buf, sizeof(buf), "-");
            unit = "KB/s";
        }
        if (force || strcmp(c->l2, buf) != 0) {
            snprintf(c->l2, sizeof(c->l2), "%.11s", buf);
            DrawTextInBox(tx, VBAR_L2_B, VBAR_TXT_W, VBAR_NUM_FONT, c->l2);
        }

        if (force || strcmp(c->l3, unit) != 0) {
            snprintf(c->l3, sizeof(c->l3), "%.11s", unit);
            DrawTextInBox(tx, VBAR_L3_B, VBAR_TXT_W, &ui_font_ascii14, c->l3);
        }
    }

    /* 柱高 */
    if (force || pct != c->pct) {
        c->pct = pct;
        DrawVBar(bx, pct);
    }
}

/* 取一次状态，四根柱子都刷一遍 */
static void UpdateResBars(bool force)
{
    SysStatus_t st;
    bool ok = Sysmon_IsConnected();

    if (ok) {
        Sysmon_GetStatus(&st);
    } else {
        /* PC 没接上来:显示 0 和 --，不把上一次的旧值留在屏上冒充实时数据 */
        memset(&st, 0, sizeof(st));
        st.cput = -1;
        st.gput = -1;
    }

    for (int k = 0; k < VBAR_N; k++) {
        DrawVBarDynamic(k, &st, ok, force);
    }
}

/* 充电时电池里那个小闪电,7 宽 x 9 高 */
static const char *kBolt[9] = {
    "....###",
    "...##..",
    "..##...",
    ".##....",
    "#######",
    "...##..",
    "..##...",
    ".##....",
    "##.....",
};

/* 电池图标:x,y 左上角。
   charging 时内部画闪电(而不是电量条) —— 电量靠旁边的百分比显示;
   否则按电量填一根圆角条。 */
static void DrawBatteryIcon(int x, int y, int level, bool charging)
{
    /* 连右侧触点一起清出一块黑底 */
    Ui_FillRect(x, y, BATT_ICON_W + 4, BATT_ICON_H, 0);

    RoundFrame(x, y, BATT_ICON_W, BATT_ICON_H, BATT_ICON_R);

    /* 正极触点 */
    RoundFill(x + BATT_ICON_W + 2, y + 4, 3, 6, 1);

    int ix = x + BATT_INSET;
    int iy = y + BATT_INSET;
    int iw = BATT_ICON_W - 2 * BATT_INSET;    /* 20 */
    int ih = BATT_ICON_H - 2 * BATT_INSET;    /* 9  */

    if (charging) {
        int bx = ix + (iw - 7) / 2;
        int by = iy + (ih - 9) / 2;
        for (int j = 0; j < 9; j++) {
            for (int i = 0; i < 7; i++) {
                if (kBolt[j][i] == '#') {
                    Ui_Pixel(bx + i, by + j, 1);
                }
            }
        }
        return;
    }

    if (level > 0) {
        int w = (level * iw + 99) / 100;
        if (w < 3) {
            w = 3;   /* 还剩电就至少画一点,免得看着像空壳 */
        }
        RoundFill(ix, iy, w, ih, 2);
    }
}

/* TF 卡图标。mounted=false 时把这块擦黑就走人 —— 不画任何东西,
   也不画“无卡”文字(用户要求)。 */
static void DrawSdIcon(int x, int y, bool mounted)
{
    Ui_FillRect(x, y, SD_ICON_W, SD_ICON_H, 0);
    if (!mounted) {
        return;
    }

    /* 轮廓:左上角斜切 5x3,其余三个角圆角 r=2 */
    Ui_HLine(x + 5, y, 8, 1);                    /* 上边 x+5..x+12   */
    Ui_Pixel(x + 3, y + 1, 1);                   /* 斜切台阶         */
    Ui_Pixel(x + 1, y + 2, 1);
    Ui_Pixel(x + 13, y + 1, 1);                  /* 右上圆角         */
    Ui_VLine(x, y + 3, 10, 1);                   /* 左边 y+3..y+12   */
    Ui_VLine(x + 14, y + 2, 11, 1);              /* 右边 y+2..y+12   */
    Ui_Pixel(x + 1, y + 13, 1);                  /* 左下圆角         */
    Ui_Pixel(x + 13, y + 13, 1);                 /* 右下圆角         */
    Ui_HLine(x + 2, y + 14, 11, 1);              /* 下边 x+2..x+12   */

    /* 卡内顶部的 3 根竖触点 —— 这是让图标一眼看出是“卡”的关键 */
    for (int i = 0; i < 3; i++) {
        Ui_FillRect(x + 4 + i * 3, y + 2, 2, 4, 1);
    }
}

/* 顶栏左边的 WiFi 名字。也走定宽框 —— 名字是从 wifi.txt 读进来的,
   长度没法预先知道。 */
static void DrawSsidLabel(void)
{
    DrawTextInBox(SSID_TXT_X, TOP_BASE_ASCII, SSID_BOX_W, &ui_font_ascii14,
                  s_wifi_ssid[0] != '\0' ? s_wifi_ssid : "no wifi");
}

/* 顶栏左边那个 IP:端口。用定宽擦除 —— "no wifi"(7 字符)和
   完整地址(18 字符)宽度差一倍,不定宽就会留残笔。 */
static void DrawIpLabel(void)
{
    char buf[32];
    if (s_ip_str[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s:%d", s_ip_str, NET_AUDIO_PORT);
    } else {
        snprintf(buf, sizeof(buf), "no wifi");
    }
    DrawTextInBox(IP_TXT_X, TOP_BASE_ASCII, IP_BOX_W,
                  &ui_font_ascii14, buf);
}

/* 秒表图标:圆表盘 + 顶部按钮,同步好了在圆心点一下("表在走"),
   没同步就划一道斜杠 —— 和 TF 卡"没插卡打斜杠"是同一套视觉语言。
   为什么换图标:原来"时间已同步"占 5 个汉字(84px),
   而 IP:端口 要 148px,顶栏根本塞不下;图标只占 15px,腾出 69px。 */
static void DrawClockIcon(bool synced)
{
    const int cx = CLK_ICON_X + CLK_ICON_R;
    const int cy = CLK_ICON_Y + 2 + CLK_ICON_R;   /* 下移,给顶部按钮让位 */

    Ui_FillRect(CLK_ICON_X, CLK_ICON_Y, CLK_ICON_R * 2 + 3,
                CLK_ICON_R * 2 + 4, 0);

    /* 表盘:逐行左右各点一个像素。半径才 6,不需要中点圆算法 */
    for (int dy = -CLK_ICON_R; dy <= CLK_ICON_R; dy++) {
        int dx = (int)(sqrtf((float)(CLK_ICON_R * CLK_ICON_R - dy * dy)) + 0.5f);
        Ui_Pixel(cx - dx, cy + dy, 1);
        Ui_Pixel(cx + dx, cy + dy, 1);
    }
    /* 顶部按钮:贴着表盘正上方的一小段横线,秒表的标志特征 */
    Ui_HLine(cx - 2, cy - CLK_ICON_R - 2, 5, 1);

    if (synced) {
        Ui_HLine(cx - 1, cy, 3, 1);               /* 圆心一点 = 表在走 */
    } else {
        for (int i = -CLK_ICON_R; i <= CLK_ICON_R; i++) {
            Ui_Pixel(cx + i, cy + i, 1);          /* 斜杠 = 还没同步 */
        }
    }
}

/* 插头图标 = "插着外接电源"。两脚朝上、下面一根线,照插头原样画。
   没接外接电源(纯电池供电)就什么都不画 —— 和 TF 卡"没插卡不画"一致,
   空着比画个"否"的图标好认。 */
static void DrawPlugIcon(bool external)
{
    Ui_FillRect(PLUG_X, PLUG_Y, PLUG_W, PLUG_H, 0);
    if (!external) {
        return;
    }

    const int bx = PLUG_X;
    const int by = PLUG_Y;

    /* 两根插脚 */
    Ui_FillRect(bx + 3, by,     2, 4, 1);
    Ui_FillRect(bx + 8, by,     2, 4, 1);

    /* 本体画成 1px 空壳,实心会太黑,比旁边的电池图标还压眼 */
    Ui_HLine(bx + 1, by + 4, 11, 1);
    Ui_HLine(bx + 1, by + 9, 11, 1);
    Ui_VLine(bx + 1, by + 5,  4, 1);
    Ui_VLine(bx + 11, by + 5, 4, 1);

    /* 拖出去的那根线 */
    Ui_FillRect(bx + 5, by + 10, 3, 5, 1);
}

/* 画顶栏那几个指示器。force=true 时无条件重画 —— 整页重绘之后必须传 true,
   因为整个屏幕刚被清成黑的,而函数内部的"内容没变就不画"会把它漏掉。 */
static void DrawTopIndicators(bool force)
{
    static int           last_lvl = -99;
    static bool          last_has = false;
    static bool          last_sd  = false;
    static Battery_Power last_pwr = BATTERY_POWER_UNKNOWN;
    static bool          started  = false;

    int           lvl = s_batt_ok ? s_batt_level : -1;
    bool          has = s_batt_ok;
    bool          sd  = s_sd_mounted;
    Battery_Power pwr = s_power;

    if (!force && started && lvl == last_lvl && has == last_has &&
        sd == last_sd && pwr == last_pwr) {
        return;
    }
    started  = true;
    last_lvl = lvl;
    last_has = has;
    last_sd  = sd;
    last_pwr = pwr;

    /* ---- 电池(充电时里面画闪电) ---- */
    DrawBatteryIcon(BATT_ICON_X, BATT_ICON_Y, lvl,
                    pwr == BATTERY_POWER_CHARGING);

    char btxt[16];
    if (has) {
        snprintf(btxt, sizeof(btxt), "%d%%", lvl);
    } else {
        snprintf(btxt, sizeof(btxt), "USB");    /* 没装电池,靠 Type-C 供电 */
    }
    DrawTextInBox(BATT_TXT_X, TOP_BASE_ASCII, BATT_TXT_W,
                  &ui_font_ascii14, btxt);

    /* ---- 外接电源:插头图标(原来是"外接充电"四个汉字) ---- */
    DrawPlugIcon(pwr == BATTERY_POWER_CHARGING ||
                 pwr == BATTERY_POWER_FULL ||
                 pwr == BATTERY_POWER_EXTERNAL_ONLY);

    /* ---- TF 卡(没卡就不画;DrawSdIcon 会先把那块擦黑) ---- */
    DrawSdIcon(SD_ICON_X, SD_ICON_Y, sd);
}

/* ---------------- 页 0:时钟 ---------------- */
static void RenderClockPage(void)
{
    char buf[64];

    Ui_Clear(0);

    /* 顶部状态行:WiFi 名字 | IP:端口 | 电池 | TF 卡 | 同步图标 */
    DrawSsidLabel();
    DrawIpLabel();
    DrawTopIndicators(true);
    DrawClockIcon(s_time_synced);

    DrawDivider(42);

    /* 方块点阵时钟:白底 + 网格 + 粗笔画数字 */
    {
        char txt[CLK_NCHARS + 1];
        for (int i = 0; i < CLK_NCHARS; i++) {
            txt[i] = s_clk_cache[i] ? s_clk_cache[i] : '0';
        }
        txt[CLK_NCHARS] = '\0';
        DrawClockBackdrop();
        DrawClockBig(txt);
    }
    if (s_p_ampm[0]) {
        DrawTextRightBox(AMPM_TXT_R, AMPM_TXT_B, AMPM_TXT_W,
                         &ui_font_ascii14, s_p_ampm);
    }

    /* 日期 + 星期 */
    DrawText(200, 122 + ui_font_cjk20.base, &ui_font_cjk20,
             s_p_date, 0, true);

    DrawDivider(166);

    /* 四根竖向资源柱:左两根 CPU/GPU,右两根 ↓/↑,中间留 24px 空隙分组。
       这里只画静态的第一行(以及网速柱固定的 "KB/s"),
       会变的内容交给 UpdateResBars 增量补。 */
    for (int k = 0; k < VBAR_N; k++) {
        const int bx = s_col_x[k];
        const int tx = bx + VBAR_W + VBAR_GAP;
        RoundFrame(bx, VBAR_Y, VBAR_W, VBAR_H, VBAR_R);
        DrawTextInBox(tx, VBAR_L1_B, VBAR_TXT_W, &ui_font_ascii14,
                      s_card[k].title);
    }
    UpdateResBars(true);    /* 整页刚重画过,柱子和数字都要补上 */
    UpdateSideBars(true);   /* 时钟两侧的温度/湿度柱同理 */
    (void)buf;
}

/* ---------------- 页 1:音乐 + 频谱 ---------------- */
static void RenderMusicPage(void)
{
    Ui_Clear(0);

    DrawText(2, 7 + ui_font_cjk20.base, &ui_font_cjk20,
             "音乐播放", -1, true);
    DrawTextRightBox(398, 13 + ui_font_cjk16.base, MSTATE_BOX_W,
                     &ui_font_cjk16, s_p_mstate);
    DrawTextCenterBox(200, 13 + ui_font_cjk16.base, TRACK_BOX_W,
                      &ui_font_cjk16, s_p_track);

    DrawDivider(38);

    /* 纵轴:0dB 在顶、下限在底,6 等分。
       注意 DrawText 的第二个参数是【基线】,不是盒子顶端 ——
       基线 = 想要的文字垂直中心 + 大半个字高。

       顺序也要紧:文字先画、刻度线后画。因为 DrawText(erase=true)
       会先擦一块底色再写字,刻度线要是先画就会被擦出缺口,看着粗细不一。 */
    float span = -Spectrum_GetFloor();
    for (int k = 0; k < YDB_TICKS; k++) {
        int   y    = DbTickY(k);        /* 和网格线共用同一个公式,别各算各的 */

        char txt[8];
        if (k == 0) {
            snprintf(txt, sizeof(txt), "0dB");
        } else {
            snprintf(txt, sizeof(txt), "-%d",
                     (int)(span * (float)k / (float)(YDB_TICKS - 1) + 0.5f));
        }
        /* 让数字的视觉中心落在刻度线那一行上 */
        DrawText(YLABEL_RX, y + ui_font_ascii10.base / 2, &ui_font_ascii10,
                 txt, 1, true);
    }

    /* 横轴频率标注的位置 */
    static const struct { int bar; const char *txt; } fx[] = {
        {  1, "100" }, { 13, "200" }, { 28, "500" }, { 40, "1k"  },
        { 51, "2k"  }, { 67, "5k"  }, { 78, "10k" }, { 89, "20k" },
    };

    /* 数值只标整数频点:90 个频段等比分布,相邻只差 6%,全标出来全是重复数字 */
    for (int k = 0; k < (int)(sizeof(fx) / sizeof(fx[0])); k++) {
        int cx = BAR_X0 + fx[k].bar * (BAR_W + BAR_GAP) + BAR_W / 2;
        DrawText(cx, XLABEL_Y + ui_font_ascii10.base, &ui_font_ascii10,
                 fx[k].txt, 0, true);
    }

    /* 刻度线最后画,谁也别想擦到它 */
    Ui_HLine(YTICK_X, PLOT_TOP, 5, 1);                 /* 纵轴上下两个端点 */
    Ui_HLine(YTICK_X, PLOT_BOT, 5, 1);
    for (int k = 0; k < YDB_TICKS; k++) {
        Ui_HLine(YTICK_X, DbTickY(k), 5, 1);
    }
    for (int i = 0; i < SPECTRUM_BANDS; i++) {         /* 每根柱子下面一个短刻度 */
        int cx = BAR_X0 + i * (BAR_W + BAR_GAP) + BAR_W / 2;
        Ui_VLine(cx, XTICK_Y, 3, 1);
    }
    for (int k = 0; k < (int)(sizeof(fx) / sizeof(fx[0])); k++) {
        int cx = BAR_X0 + fx[k].bar * (BAR_W + BAR_GAP) + BAR_W / 2;
        Ui_VLine(cx, XTICK_Y, 5, 1);                   /* 标了数字的用长刻度 */
    }

    /* dB 横向网格线。这里画一次是为了切页那一瞬间就有线,
       之后由 SpectrumTask 每帧补画(原因见 DrawDbGrid 上面的注释) */
    DrawDbGrid();

    /* 柱子区留空:那块由 SpectrumTask 直接改显存(SpectrumDrawDirect),
       它每帧除了画柱子,还会把刚被柱子盖掉/擦掉的网格点补回来 */

    DrawDivider(254);

    /* 底部按键说明。BOOT 的含义跟着状态走,提示也跟着换 */
    DrawText(200, 257 + ui_font_cjk16.base, &ui_font_cjk16,
             NetAudio_IsStreaming()
                 ? "BOOT 暂停/继续  长按换源\nKEY 切页  长按停止推流"
                 : (s_spec_src == SRC_STREAM)
                     /* 停在"电脑推流"但电脑还没连上来:别提示"长按停止推流",
                        那会儿根本没东西可停 —— 直接告诉用户去开电脑端程序 */
                     ? "源在电脑推流,等电脑端连过来\nBOOT 长按换源  KEY 切页"
                     : "BOOT 播放/暂停  长按换源(松手定)\nKEY 切页  长按下一首  双击上一首",
             0, true);
}

/* ============================================================
 *  页 2:历史曲线
 * ============================================================
 *  参考 C:\pi_monitor\client\rpi_mon_gui.pyw 里的 HistoryPanel:
 *  3x2 六个独立子图,每个子图 = 彩色标题 + 细线 + 淡网格 + 曲线下浅填充。
 *  挪到这块 400x300 单色屏上做了四处改动:
 *    1) 6 张改 4 张。去掉两个温度 —— 温度曲线在 0~100 的共用坐标下几乎是
 *       一条直线,占两格不划算。去完正好凑 2x2,图也能画得更大
 *    2) 没有颜色   -> 改用文字标题区分(CPU % / GPU % / MEM % / NET↓)
 *    3) 没有透明度 -> 用点状线模拟参考图里的"淡"网格。竖的 4 道,
 *       横的 3 道且【只在 Y 轴刻度那三个高度上】—— 横线不做满,是因为
 *       单色屏上满屏横线会和"走势平缓的柱子"抢注意力;只画刻度那三道,
 *       它就变成"尺子"而不是"网格",反而帮着读数
 *    4) 折线换成实心柱子(每根 1px 宽、步距 2px)。参考图曲线下面
 *       那层浅填充看着好看,但在单色屏上就是一整块白;只有做成
 *       "从底边填到数值高度"的柱子,才能既看出趋势又看出绝对量。
 *       顺便和频谱页是同一个路子
 *  量程照参考设计:百分比固定 0~100(四张图能横向比),
 *  只有网速按窗口内最大值自适应 —— 否则涓流和 2.5G 没法画在一起。
 *  网速那张画的是"窗口内上下行的包络"(取两者较大者),不是两组柱子:
 *  真要分开就得再引入点/线区分,而主界面那两根柱子已经分别显示 ↓↑ 了。
 */

#define SYS_M        2                      /* 面板区外边距:四边都只留 2px,吃满屏 */
#define SYS_GAP      6                      /* 面板之间的间距 */
#define SYS_N        4                      /* 四张子图:CPU / GPU / 内存 / 网速 */
#define SYS_W        195                    /* (400 - 2*2 - 6) / 2 */
#define SYS_H        130                    /* (296 - 30 - 6) / 2 */
/* 顶部只给页头文字和那条分隔线留必要的高度。以前写 48,白白空掉一大条;
   更早写 34 时那条分隔线(y=42)还正好横穿面板,被标题的擦除框擦成几段,
   看上去像"框中间多出几根横线" */
#define SYS_Y0       30

#define SYS_COL_X(c) (SYS_M + (c) * (SYS_W + SYS_GAP))
#define SYS_ROW_Y(r) (SYS_Y0 + (r) * (SYS_H + SYS_GAP))

/* ---- 面板内部布局 ----
   Y 轴刻度标签占最左边一条竖栏,栏右侧是刻度和轴线,再过去才是绘图区。
   注意标题必须从刻度栏右边开始 —— 顶上那个 "100" 的擦除框横向会盖到
   px+1..px+20,标题要是还从 px+10 起就会和它打架 */
#define SYS_TITLE_B  18                     /* 标题/当前值基线(相对面板顶) */
#define SYS_GUT_X    1                      /* 刻度标签框左边界(相对面板左边) */
/* 30px 是迁就网速那张图:它的量程自适应,刻度得写成 "298M" 这种
   (不带小数的极短写法),而 "100" 只要 18px。用同一列宽是为了
   四张图的绘图区左右对齐 */
#define SYS_GUT_W    30
#define SYS_PLOT_DX  36                     /* 绘图区相对面板左边(刻度栏 30 + 刻度 3 + 轴线 1) */
#define SYS_PLOT_DY  26                     /* 绘图区相对面板顶边 */
#define SYS_PLOT_W   157                    /* 到面板右边还剩 2px */
#define SYS_PLOT_H   82
#define SYS_TICK     3                      /* 刻度短线长 */
#define SYS_XTICK    2                      /* X 轴刻度线比 Y 的短:下面紧接着就是刻度值 */
#define SYS_XLAB_B   (SYS_PLOT_DY + SYS_PLOT_H + 16)   /* X 轴刻度值基线 */
#define SYS_XLAB_W   40                     /* X 轴三个刻度值的定宽框 */
#define SYS_BAR_W    1                      /* 一根柱子 1 像素宽(和频谱一致) */
/* 步距 = 柱 1 + 缝 1。全都紧挨着的话填出来是一整块实心面,
   根本看不出"一根根柱子" —— 频谱之所以像柱子,也是因为有缝 */
#define SYS_BAR_STEP 2
#define SYS_NBAR      (SYS_PLOT_W / SYS_BAR_STEP)   /* 一张图 83 根 */
#define SYS_TITLE_W  56                     /* 最宽的是 "MEM %"(52px) */
#define SYS_VAL_W    56                     /* 最宽的是 "976.6M"(49px) */

static const char *kSysName[SYS_N] = {
    "CPU %", "GPU %", "MEM %", "NET↓"
};

/* 点状线。1bpp 没有"半透明",隔四个点打一个点来模拟参考图里的淡网格 */
static void DotLine(int horizontal, int x, int y, int len)
{
    for (int i = 0; i < len; i++) {
        if ((i & 3) == 0) {
            Ui_Pixel(horizontal ? x + i : x, horizontal ? y : y + i, 1);
        }
    }
}

/* 网速的极短写法:不带小数,只留 K/M/G 后缀。
   只给子图 Y 轴刻度用 —— 刻度栏就那么宽,而 "64.0M" 在 10px 下
   要 30px。带小数的完整写法用在面板右上角的当前值上 */
static void FmtSpeedTiny(unsigned kbs, char *out, size_t cap)
{
    if (kbs < 1024u) {
        snprintf(out, cap, "%uK", kbs);
    } else if (kbs < 1024u * 1024u) {
        snprintf(out, cap, "%uM", kbs / 1024u);
    } else {
        snprintf(out, cap, "%uG", kbs / (1024u * 1024u));
    }
}

/* 把秒数写成短标签:"2.4h" / "45m" / "30s"。页头和 X 轴刻度都用它 */
static void FmtSpan(int secs, char *out, size_t cap)
{
    if (secs >= 3600) {
        snprintf(out, cap, "%d.%dh", secs / 3600, (secs % 3600) / 360);
    } else if (secs >= 60) {
        snprintf(out, cap, "%dm", secs / 60);
    } else {
        snprintf(out, cap, "%ds", secs < 0 ? 0 : secs);
    }
}

/* 画一张子图。
   val[] 是 n 个采样,-1 = 该点没数据(那一根留空)。
   scale 是量程,span_s 是这段历史覆盖的秒数(X 轴刻度用)。
   Y 轴刻度:百分比那三张固定 0/50/100,网速那张跟着自适应量程走 */
static void DrawSysPanel(int k, const int *val, int n, int scale, int span_s,
                         const char *cur_txt)
{
    const int px = SYS_COL_X(k % 2);
    const int py = SYS_ROW_Y(k / 2);
    const int gx = px + SYS_PLOT_DX;
    const int gy = py + SYS_PLOT_DY;
    const int gw = SYS_PLOT_W;
    const int gh = SYS_PLOT_H;
    const int bot = gy + gh - 1;            /* 绘图区底边 = 量程 0 的位置 */
    const int ax  = gx - 1;                 /* Y 轴轴线 */
    const int lab_r = px + SYS_GUT_X + SYS_GUT_W;   /* 刻度值框右边界 */
    /* lab 要能装下 "-" + xl(最多 11 字符) + '\0' = 13 字节,
       所以不能跟 xl/xm 一样是 12 */
    char yhi[12], ymid[12], xl[12], xm[12], lab[16];

    RoundFrame(px, py, SYS_W, SYS_H, 4);

    /* 标题必须从刻度栏右边开始:顶上那个 "100" 的擦除框横向会盖到
       px+1..px+20,标题要是还从更左边起就会和它打架 */
    DrawTextInBox(gx + 2, py + SYS_TITLE_B, SYS_TITLE_W,
                  &ui_font_ascii14, kSysName[k]);
    DrawTextRightBox(gx + gw - 1, py + SYS_TITLE_B, SYS_VAL_W,
                     &ui_font_ascii14, cur_txt);

    /* ---- Y 轴刻度值 ---- */
    if (k == SYS_N - 1) {
        /* 网速量程是自适应的,刻度就得跟着量程走,否则这刻度是假的。
           这里用不带小数的极短写法 —— 刻度栏比完整的 "976.6M" 窄 */
        FmtSpeedTiny((unsigned)scale, yhi, sizeof(yhi));
        FmtSpeedTiny((unsigned)(scale / 2), ymid, sizeof(ymid));
    } else {
        snprintf(yhi, sizeof(yhi), "100");
        snprintf(ymid, sizeof(ymid), "50");
    }

    /* 10px 的数字墨迹大致是 baseline-7..baseline+1,想让它的中心落在
       刻度线上,基线取 y+3 */
    DrawTextRightBox(lab_r, gy + 3, SYS_GUT_W, &ui_font_ascii10, yhi);
    DrawTextRightBox(lab_r, gy + gh / 2 + 3, SYS_GUT_W, &ui_font_ascii10, ymid);
    DrawTextRightBox(lab_r, bot + 3, SYS_GUT_W, &ui_font_ascii10, "0");

    /* ---- 刻度短线 + L 形轴线 ---- */
    for (int i = 0; i <= 2; i++) {
        int y = gy + gh * i / 2;
        if (y > bot) {
            y = bot;
        }
        Ui_HLine(ax - SYS_TICK, y, SYS_TICK, 1);
    }
    Ui_VLine(ax, gy, gh + 1, 1);
    Ui_HLine(ax, bot + 1, gw + 1, 1);

    /* ---- X 轴时间刻度:左端最旧、右端是现在 ---- */
    FmtSpan(span_s, xl, sizeof(xl));
    FmtSpan(span_s / 2, xm, sizeof(xm));

    snprintf(lab, sizeof(lab), "-%.11s", xl);
    DrawTextInBox(gx, py + SYS_XLAB_B, SYS_XLAB_W, &ui_font_ascii10, lab);
    snprintf(lab, sizeof(lab), "-%.11s", xm);
    DrawTextInBox(gx + gw / 2 - SYS_XLAB_W / 2, py + SYS_XLAB_B, SYS_XLAB_W,
                  &ui_font_ascii10, lab);
    DrawTextRightBox(gx + gw - 1, py + SYS_XLAB_B, SYS_XLAB_W,
                     &ui_font_ascii10, "0");

    Ui_VLine(gx - 1, bot + 2, SYS_XTICK, 1);
    Ui_VLine(gx + gw / 2, bot + 2, SYS_XTICK, 1);
    Ui_VLine(gx + gw - 1, bot + 2, SYS_XTICK, 1);

    /* 竖向点状网格 + 横向点状网格。
       横向那三道【对齐 Y 轴刻度】(0 / 50 / 100,网速图是 0 / 半量程 / 量程),
       起止点和 X 轴一样:从轴线 ax 画到 ax+gw,长度 gw+1。

       以前是故意不画横的 —— 怕和"走势平缓的柱子"混在一起。现在画了,
       靠两点区分开:
         1) 线是 1px 点状(每 4px 一个点,25% 占空),远看是浅灰;柱子是实心白,
            密度差得远,不可能认错;
         2) 三张百分比图的线位置固定不变(50 永远是 50),看两眼就当成
            "尺子"了,不会再去当数据读。
       顺序照旧:网格先画,柱子后画 —— 柱子压在网格上,量值才不会被线切碎。 */
    for (int i = 1; i <= 3; i++) {
        DotLine(0, gx + gw * i / 4, gy, gh);
    }
    for (int i = 0; i <= 2; i++) {
        int y = gy + gh * i / 2;
        if (y > bot) {
            y = bot;                    /* 和上面的刻度短线用同一套坐标 */
        }
        DotLine(1, ax, y, gw + 1);
    }

    if (n < 1 || scale <= 0) {
        return;
    }

    /* 一根根实心柱子,不是折线:每根从底边一直填到数值高度。
       点数就是柱数,横向按步距排,所以也不需要什么插值 */
    for (int i = 0; i < n; i++) {
        int v, hgt, x;

        if (val[i] < 0) {
            continue;                       /* 该点没数据:那一根留空 */
        }
        v = val[i] > scale ? scale : val[i];
        hgt = v * (gh - 1) / scale;
        if (val[i] > 0 && hgt < 2) {
            hgt = 2;    /* 有量但不到 2 像素也得看得见,否则和 0 分不出来 */
        }
        x = gx + i * SYS_BAR_STEP;
        for (int b = 0; b < SYS_BAR_W; b++) {
            Ui_VLine(x + b, gy + gh - 1 - hgt, hgt + 1, 1);
        }
    }
}

static void RenderSysPage(void)
{
    static SysSample_t dec[SYS_NBAR];       /* 降采样后的历史,四张图共用 */
    static int         val[SYS_NBAR];
    char  cur[24];
    char  buf[32];
    int   n, net_max = 1, span_s;
    SysStatus_t st;

    Ui_Clear(0);

    /* 页头贴到屏幕最上。基线不是拍的:Pillow 量过,20px 雅黑的
       "历史曲线" 墨迹顶端在行顶往下 5px 处,而行顶 = 基线 - base(22)。
       要让墨迹顶端落在 y=2,基线就得是 19。
       (擦除框会算出负数 y,但 RLCD_SetPixel 有 x/y 越界直接返回的裁剪,
        超出的行被丢掉,不会写到显存外面) */
    DrawText(16, 19, &ui_font_cjk20, "历史曲线", -1, true);
    /* 分隔线紧跟在页头下面。面板从 y=30 开始,所以不会再像以前那样
       被横穿(旧的 42 正好切在面板上) */
    DrawDivider(26);

    /* 取一次历史就够四张图用 —— 降采样只和"图有多宽"有关,
       和历史里存了几千个点无关 */
    n = Sysmon_GetDecimated(dec, SYS_NBAR);
    Sysmon_GetStatus(&st);

    /* 右上角标实际时间跨度。开机没多久时这个数会远小于 3 小时,照实显示。
       ★ 时间跨度 = 原始点数 x 采样间隔,两个坑都在这一行里:
         1) 不能用"柱数 n"乘 —— n 是降采样后的柱数(83),而 sample_ms 是
            原始采样间隔,一个柱子平均覆盖几十个原始点,乘出来差几十倍;
         2) 必须先乘后除。写成 sample_ms / 1000 * hist_count 的话,
            500/1000 在整数除法下直接是 0,于是整个跨度恒为 0 ——
            X 轴三个刻度就会全变成 "-0s"。 */
    span_s = (st.sample_ms > 0 && st.hist_count > 1)
           ? (int)((int64_t)st.sample_ms * st.hist_count / 1000)
           : 0;
    if (span_s > 0) {
        FmtSpan(span_s, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }

    /* 每 10 秒最多打一条点,方便从串口核对跨度到底算得对不对 */
    {
        static int64_t last_us;
        int64_t t = esp_timer_get_time();
        if (t - last_us > 10 * 1000000) {
            last_us = t;
            ESP_LOGI(TAG, "曲线跨度 %ds = %d 个原始点 x %dms",
                     span_s, st.hist_count, st.sample_ms);
        }
    }
    /* 右边这个时间跨度标签跟着页头一起上移。这里不跟页头"墨迹顶齐平",
       而是"基线对齐、西文基线再高 1px" —— 和时钟页顶栏那两个
       TOP_BASE_CJK/TOP_BASE_ASCII 用的是同一个约定 */
    DrawTextRightBox(384, 18, 70, &ui_font_ascii14, buf);

    if (n < 2) {
        DrawText(200, 150 + ui_font_cjk16.base, &ui_font_cjk16,
                 "还没有数据", 0, true);
        return;
    }

    /* 网速量程自适应:取窗口内上下行两个方向的最大值 */
    for (int i = 0; i < n; i++) {
        if (dec[i].down > net_max) {
            net_max = dec[i].down;
        }
        if (dec[i].up > net_max) {
            net_max = dec[i].up;
        }
    }
    for (int k = 0; k < SYS_N; k++) {
        int num, scale = 100;

        for (int i = 0; i < n; i++) {
            switch (k) {
            case 0:  num = dec[i].cpu;                                  break;
            case 1:  num = dec[i].gpu;                                  break;
            case 2:  num = dec[i].ram;                                  break;
            default: num = dec[i].down > dec[i].up ? dec[i].down
                                                  : dec[i].up;          break;
            }
            val[i] = num;
        }

        /* 右上角的当前值。网速用紧凑写法,不然框里放不下 */
        if (k == SYS_N - 1) {
            /* 标题已经写了 NET↓,这里的值就不再加箭头了 */
            FmtSpeedShort((unsigned)dec[n - 1].down, cur, sizeof(cur));
            scale = net_max;
        } else if (val[n - 1] < 0) {
            snprintf(cur, sizeof(cur), "--");
        } else {
            snprintf(cur, sizeof(cur), "%d", val[n - 1]);
        }

        DrawSysPanel(k, val, n, scale, span_s, cur);
    }
}

/* ============================================================
 *  曲线页刷新任务
 * ============================================================ */
static void CurveTask(void *arg)
{
    for (;;) {
        if (s_page == PAGE_SYS && DispLock(200)) {
            s_sys_refresh = false;
            RenderSysPage();
            RlcdPort.RLCD_Display();        /* 六张图一起推一次屏 */
            DispUnlock();
        }

        /* 平时 2 秒一次(时钟页是 1 秒)。曲线页要重绘六张图和网格,
           比时钟页重得多,没必要刷那么勤。切成 100ms 的小步是为了
           长按 KEY 时能马上响应,不用等满 2 秒 */
        for (int i = 0; i < 20 && !s_sys_refresh; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_sys_refresh = false;
    }
}

/* 拼一页 + 一次推屏 */
void RenderPage(void)
{
    if (s_page == PAGE_MUSIC) {
        RenderMusicPage();
        s_bars_need_clear = true;    /* 柱子区刚被清过,要整块重画 */
    } else if (s_page == PAGE_SYS) {
        RenderSysPage();
    } else {
        RenderClockPage();
    }
    RlcdPort.RLCD_Display();
}

/* ============================================================
 *  7. WiFi 初始化
 * ============================================================ */
static void WifiInit(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    /* ---- 固定 IP(可选)----
       要固定地址就在 wifi_secrets.h 里定义 STATIC_IP_0..3 / STATIC_GW_0..3,
       那段注释里有完整说明。默认不定义,走 DHCP。

       为什么默认走 DHCP:静态地址是【按具体网络写的】,公开仓库里写死一个
       就会让所有克隆下来的人拿不到网关 —— 现象是「WiFi 名字显示了,
       但时间永远不同步」,很难查。 */
#ifdef STATIC_IP_3
    if (sta != NULL) {
        /* 两个坑:
           1) 要写 = {} 而不是 = {0} —— C++ 下后者会触发
              -Werror=missing-field-initializers(结构体不止一个成员);
           2) IP4_ADDR 是 lwip 的宏,这里没包含它的头文件,改用 esp_netif
              自带的 ESP_IP4TOADDR,干的是同一件事且一定可见。 */
        esp_netif_ip_info_t ip = {};
        ip.ip.addr      = ESP_IP4TOADDR(STATIC_IP_0, STATIC_IP_1,
                                        STATIC_IP_2, STATIC_IP_3);
        ip.gw.addr      = ESP_IP4TOADDR(STATIC_GW_0, STATIC_GW_1,
                                        STATIC_GW_2, STATIC_GW_3);
        ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);

        esp_netif_dhcpc_stop(sta);      /* 先停 DHCP 才能设静态地址 */
        esp_err_t e1 = esp_netif_set_ip_info(sta, &ip);

        /* DNS 也得给,不然 NTP 的域名解析不了 —— 时间就同步不上 */
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(STATIC_GW_0, STATIC_GW_1,
                                               STATIC_GW_2, STATIC_GW_3);
        esp_err_t e2 = esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);

        if (e1 == ESP_OK && e2 == ESP_OK) {
            ESP_LOGI(TAG, "静态 IP: " IPSTR "  网关: " IPSTR,
                     IP2STR(&ip.ip), IP2STR(&ip.gw));
        } else {
            ESP_LOGW(TAG, "静态 IP 没设上(ip:%s dns:%s),继续走 DHCP",
                     esp_err_to_name(e1), esp_err_to_name(e2));
        }
    }
#else
    (void)sta;
    ESP_LOGI(TAG, "网络: DHCP(没定义静态 IP)");
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, OnWifiEvent, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, OnWifiEvent, NULL, NULL));

    /* 注意:这里只能拿到编译进去的默认值 —— TF 卡还没挂载。
       卡里若真有 wifi.txt,由 WifiRetryTask 稍后覆盖并重连。 */
    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, s_wifi_ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_wifi_pass,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* ★ SSID 为空时 esp_wifi_set_config 会返回 ESP_ERR_WIFI_SSID,
       而 ESP_ERROR_CHECK 遇到错误会直接 abort —— 给朋友那版默认就是空的,
       不挡这一下就是一开机无限重启。 */
    if (s_wifi_ssid[0] != '\0') {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ============================================================
 *  对外接口
 * ============================================================ */
void UserApp_AppInit(void)
{
    /* 时区必须先设,mktime/localtime 才正确 */
    setenv("TZ", TIMEZONE_STR, 1);
    tzset();

    /* WiFi 驱动需要 NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* I2C 传感器 + RTC */
    if (Sensors_Init() == ESP_OK) {
        if (Rtc_IsRunning() && Rtc_LoadToSystem()) {
            struct tm t;
            time_t now = time(NULL);
            localtime_r(&now, &t);
            ESP_LOGI(TAG, "已从 RTC 恢复时间: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
        } else {
            ESP_LOGW(TAG, "RTC 时间无效(可能掉过电),等待 NTP 对时");
        }
    } else {
        ESP_LOGW(TAG, "传感器总线初始化失败");
    }

    /* 音频:ES8311 挂在同一条 I2C 总线上,所以必须在 Sensors_Init 之后 */
    if (Audio_Init() == ESP_OK && Audio_Open() == ESP_OK) {
        Audio_SetVolume(70);
        ESP_LOGI(TAG, "音频就绪,按 BOOT 键可暂停/继续音乐");
    } else {
        ESP_LOGW(TAG, "音频初始化失败(喇叭不会有声音)");
    }

    /* 电池电压(ADC1_CH3 / GPIO4) */
    if (Battery_Init() != ESP_OK) {
        ESP_LOGW(TAG, "电池检测初始化失败(顶栏会显示 USB)");
    }

    /* 频谱:分配 FFT 表和汉宁窗 */
    Spectrum_Init();
}

void UserApp_UiInit(void)
{
    gpio_config_t cfg = {};
    cfg.intr_type    = GPIO_INTR_DISABLE;
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pin_bit_mask = (1ULL << KEY_PIN) | (1ULL << BOOT_PIN);
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* 显存互斥锁:时钟任务、频谱任务、切页都会写屏 */
    s_disp_lock = xSemaphoreCreateMutex();

    /* 先把首页画出来 */
    RenderPage();
}

/* 已停用的一次性自检:开机往卡里写一个测试 WAV 再播它,用来验证
   "读卡 -> 解析 WAV -> 出声 -> 频谱" 整条链。留着备用,想彻底删掉也行。 */
#if 0
static void WavWriteSelfTest(void)
{
    const char *path = SDCARD_MOUNT_POINT "/_test.wav";

    FILE *chk = fopen(path, "rb");
    if (chk != NULL) {
        fclose(chk);
        ESP_LOGI(TAG, "自检文件已存在");
        return;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "自检:写不了 %s", path);
        return;
    }

    const uint32_t sr     = 48000;
    const uint32_t frames = sr * 2;          /* 2 秒 */
    const uint32_t dbytes = frames * 4;      /* 16bit x 2 声道 */

    uint8_t h[44] = {0};
    uint32_t v;
    uint16_t u;
    memcpy(h, "RIFF", 4);
    v = 36 + dbytes; memcpy(h + 4,  &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;          memcpy(h + 16, &v, 4);
    u = 1;           memcpy(h + 20, &u, 2);   /* PCM */
    u = 2;           memcpy(h + 22, &u, 2);   /* 立体声 */
    v = sr;          memcpy(h + 24, &v, 4);
    v = sr * 4;      memcpy(h + 28, &v, 4);   /* 字节率 */
    u = 4;           memcpy(h + 32, &u, 2);   /* 块对齐 */
    u = 16;          memcpy(h + 34, &u, 2);   /* 位深 */
    memcpy(h + 36, "data", 4);
    v = dbytes;      memcpy(h + 40, &v, 4);
    fwrite(h, 1, sizeof(h), f);

    /* 三个纯音:440Hz + 1kHz + 4kHz。
       看一下频谱里是不是对应位置鼓起三根柱,就知道 FFT 通路没错。 */
    static int16_t buf[1024];
    for (uint32_t done = 0; done < frames; ) {
        uint32_t n = frames - done;
        if (n > 512) {
            n = 512;
        }
        for (uint32_t i = 0; i < n; i++) {
            float t = (float)(done + i) / (float)sr;
            float s = 0.25f * sinf(2.0f * (float)M_PI * 440.0f  * t)
                    + 0.20f * sinf(2.0f * (float)M_PI * 1000.0f * t)
                    + 0.15f * sinf(2.0f * (float)M_PI * 4000.0f * t);
            int16_t v16 = (int16_t)(s * 32767.0f);
            buf[2 * i]     = v16;
            buf[2 * i + 1] = v16;
        }
        fwrite(buf, 1, n * 4, f);
        done += n;
    }
    fclose(f);
    ESP_LOGI(TAG, "自检:已写入 %s(2 秒,440/1k/4k 三个纯音)", path);
}
#endif

/* ============================================================
 *  BLE:手机侧的控制与状态
 * ============================================================ */
static void BleOnControl(uint8_t cmd)
{
    switch (cmd) {
    case BLE_CMD_TOGGLE_PLAY:
        s_music_playing = !s_music_playing;
        ESP_LOGI(TAG, "BLE: %s音乐", s_music_playing ? "继续播放" : "暂停");
        break;
    case BLE_CMD_NEXT:
        MusicStep(+1);
        break;
    case BLE_CMD_PREV:
        MusicStep(-1);
        break;
    case BLE_CMD_SWITCH_PAGE:
        SwitchPage(s_page == PAGE_CLOCK ? PAGE_MUSIC : PAGE_CLOCK);
        break;
    case BLE_CMD_SWITCH_SRC:
        SetSpecSource(s_spec_src + 1);
        break;
    default:
        ESP_LOGW(TAG, "BLE: 未知命令 %u", (unsigned)cmd);
        break;
    }
}

static void BleOnVolume(uint8_t vol)
{
    Audio_SetVolume((int)vol);
}

static uint8_t BleGetVolume(void)
{
    return (uint8_t)Audio_GetVolume();
}

/* 打包给手机的 8 字节状态,格式见 ble_ctrl.h */
static void BleGetStatus(BleStatus *out)
{
    out->playing    = s_music_playing ? 1 : 0;
    out->mic_source = (s_spec_src == SRC_MIC) ? 1 : 0;
    out->clock_page = (s_page == PAGE_CLOCK) ? 1 : 0;
    out->use_24h    = s_use_24h ? 1 : 0;
    out->track      = (uint8_t)((s_track >= 0 && s_track < s_track_cnt)
                                ? s_track + 1 : 0);
    out->track_cnt  = (uint8_t)s_track_cnt;
    out->temp_c     = (int8_t)(s_temp + (s_temp >= 0.0f ? 0.5f : -0.5f));
    out->humi       = (uint8_t)(s_humi + 0.5f);
}

static const BleCallbacks kBleCb = {
    .on_control = BleOnControl,
    .on_volume  = BleOnVolume,
    .get_volume = BleGetVolume,
    .get_status = BleGetStatus,
};

/* LuaTask:建任务的包装 —— 失败必须吼一声。
 * xTaskCreate 返回 pdFAIL 时任务【根本不存在】,而这些又都是
 * "死循环 + vTaskDelay" 的常驻任务:不检查的话现场就是
 * "某个功能一声不响地不工作"——典型就是时钟页永远停在
 * 开机那一帧(00:00:00 / no wifi),而串口风平浪静。
 *
 * 特别提醒:任务栈是从【内部 RAM】切的,不是 PSRAM。
 * 所以往 .bss 里加几万字节的静态数组(比如 FFT 缓冲区),
 * 真的会把最后几个任务挤掉。 */
/* 已启动任务的句柄表,只给健康体检用 */
static struct {
    const char *name;
    TaskHandle_t h;
} s_tasks[10];
static int s_task_n = 0;

static void StartTask(TaskFunction_t fn, const char *name,
                      uint32_t stack, UBaseType_t prio, BaseType_t core)
{
    TaskHandle_t h = NULL;
    if (xTaskCreatePinnedToCore(fn, name, stack, NULL, prio, &h, core) != pdPASS) {
        ESP_LOGE(TAG, "★ 任务 %s 创建失败(要 %u 字节栈)!内部 RAM 只剩 %u 字节",
                 name, (unsigned)stack,
                 (unsigned)esp_get_free_internal_heap_size());
        return;
    }
    if (s_task_n < (int)(sizeof(s_tasks) / sizeof(s_tasks[0]))) {
        s_tasks[s_task_n].name = name;
        s_tasks[s_task_n].h    = h;
        s_task_n++;
    }
}

/* 体检:每个任务的栈【最少剩多少】+ 内部 RAM 余量。
   经验值:某个数字一直在 1KB 下面的话,就差一次深层调用就爆栈了;
   反过来如果都在一半以上,说明栈给多了,可以再压。 */
static void ReportStacks(void)
{
    for (int i = 0; i < s_task_n; i++) {
        UBaseType_t w = uxTaskGetStackHighWaterMark(s_tasks[i].h);
        ESP_LOGI(TAG, "  栈水位 %-9s 最少剩 %u", s_tasks[i].name, (unsigned)w);
    }
    ESP_LOGI(TAG, "  内部 RAM 剩 %u 字节",
             (unsigned)esp_get_free_internal_heap_size());
}

void UserApp_TaskInit(void)
{
    /* 先把资源柱的真实参数打出来 —— 对比“看到的效果”和“以为的配置” */
    ESP_LOGI(TAG, "资源柱 %s: 柱 %dx%d @ y=%d, 五根 x=%d/%d/%d/%d/%d",
             RESBAR_TAG, VBAR_W, VBAR_H, VBAR_Y,
             s_col_x[0], s_col_x[1], s_col_x[2], s_col_x[3], s_col_x[4]);
    ESP_LOGI(TAG, "网速满刻度: 下行 %d KiB/s (2.5G 内网) / 上行 %d KiB/s (宽带套餐), 单位自适应 KB/s↔MB/s",
             NET_FULL_DN_KBPS, NET_FULL_UP_KBPS);

    /* 曲线页:短按 KEY 循环 时钟 -> 音乐 -> 曲线 */
    ESP_LOGI(TAG, "曲线页: %d 张子图 %dx%d,柱 %dpx 步距 %d -> %d 根,面板 %dx%d 外边距 %d",
             SYS_N, SYS_PLOT_W, SYS_PLOT_H, SYS_BAR_W, SYS_BAR_STEP, SYS_NBAR,
             SYS_W, SYS_H, SYS_M);

    WifiInit();

    /* 扫一遍 I2C:确认板子上到底挂了哪些芯片(顺便证明没有触摸屏) */
    Sensors_ScanBus();

    /* TF 卡挂载。挂不上不算致命,内嵌那段音乐还能播 */
    if (Sdcard_Mount() == ESP_OK) {
        Sdcard_List(SDCARD_MOUNT_POINT, 40);
        Wav_ScanDir(SDCARD_MOUNT_POINT);
        s_track_cnt = Wav_FileCount();
        ESP_LOGI(TAG, "播放清单共 %d 首", s_track_cnt);
        if (s_track_cnt == 0) {
            ESP_LOGW(TAG, "卡里没有 .wav 文件");
        }
    } else {
        ESP_LOGW(TAG, "没有 TF 卡,无从播放");
    }

    /* 蓝牙:开一个 BLE 外设给手机连(nRF Connect / LightBlue 都能用)。
       失败也不致命,只是没有手机遥控。 */
    if (Ble_Init(&kBleCb) == ESP_OK) {
        ESP_LOGI(TAG, "蓝牙已开,手机可用 nRF Connect 搜 \"ESP32-RLCD\"");
    } else {
        ESP_LOGW(TAG, "蓝牙初始化失败,手机遥控不可用");
    }

    /* 网络音频:等电脑连过来推流(tools/stream_audio.py) */
    NetAudio_SetWantCallback(WantPcAudio);   /* 先注册,再起任务 */
    NetAudio_Start();
    Sysmon_Start();          /* PC 资源遥测:监听 3334 */

    StartTask(WifiRetryTask, "wifi_retry", 4096, 4, 0);
    StartTask(SensorTask,    "sensor",     4096, 2, 1);
    StartTask(ButtonTask,    "button",     3072, 3, 1);
    /* 音频任务优先级略高,避免送数据不及时导致断音 */
    StartTask(MusicTask,     "music",      6144, 5, 1);

    /* ⚠下面三个原来都是 12288 —— 那个数字是 LVGL 时代定的:
     * 当时 UiRefreshUnderLock() 里会调 lv_refr_now() 同步跑完
     * 整条 LVGL 渲染链,而那条链是在【调用者的栈】上跑的。
     * LVGL 拆掉之后只剩 ui_draw 那几层轻量调用,12KB 纯属浪费。
     * 而任务栈只能从【内部 RAM】切 —— 白白占掉 36KB,
     * 直接后果是推流时内部 RAM 见底、SD 卡 DMA 分配失败。 */
    StartTask(SpectrumTask,  "spectrum",   8192, 3, 1);
    StartTask(ClockTask,     "clock",      6144, 3, 1);
    StartTask(CurveTask,     "curve",      6144, 2, 1);

    /* 任务栈全在内部 RAM,后面没别的大开销了 ——
       把余量打出来,以后往 .bss 加东西时心里有数 */
    ESP_LOGI(TAG, "任务全部启动完毕,内部 RAM 剩余 %u 字节(堆总剩余 %u)",
             (unsigned)esp_get_free_internal_heap_size(),
             (unsigned)esp_get_free_heap_size());
}
