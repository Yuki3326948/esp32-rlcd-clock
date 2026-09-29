#include "spectrum.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "dsps_fft2r.h"

static const char *TAG = "spectrum";

#define SPECTRUM_SR    48000     /* 默认值,要与 audio_player 的 AUDIO_SAMPLE_RATE 一致 */
/* ★ FFT 点数 = 频率分辨率的唯一决定因素(分辨率 = sr/N)。
   原来是 512 -> bin 93.75Hz,后果是低频“根本分不开”:
     对数频段每个比前一个宽 6.14%,频段间距 = 0.0614*f;
     要让它追得上 bin,得 f >= 93.75/0.0614 = 1527Hz。
     所以 1.5kHz 以下所有柱子都在抢同一两个 bin ——
     实测 512 点时 bin1 被 7 个频段共用、bin2 被 9 个共用,
     屏上就是“100Hz 以下好几条一起上下”。
   4096 点 -> bin 11.7Hz,频段间距 >= 一个 bin 的界限降到 190Hz,
     整个低频段基本变成 1 个频段 1 个 bin。
   代价只有内存(64KB 静态)和每次 FFT 的时间 ——
     但 FFT 每秒只跑 sr/HOP 次,N 变大时帧率降,CPU 占比其实差不多。 */
#define FFT_N          4096
/* 每次 FFT 窗口往前挪多少样本。
   = FFT_N 就是“不重叠”,刷新率只有 11.7Hz,柱子会一跳一跳;
   取一半(50% 重叠,汉宁窗的标准用法)刷新率翻倍到 23.4Hz,
   接近显示帧率,动作才连得上。多出来的代价就是每帧多算一倍 FFT。 */
#define FFT_HOP        2048
#define SPECTRUM_F_MIN  94.0f    /* 最低频段起点 */
#define SPECTRUM_F_MAX  20000.0f /* 目标上限;44.1k/48k 都够得着 */
#define DB_FLOOR_DEFAULT (-55.0f) /* 默认显示下限(音乐用) */
/* 柱子下落系数(越小掉得越快)。
   ⚠ 这是【每次 FFT】的系数,不是每秒的 —— 它和刷新率 sr/FFT_HOP 绑在一起。
   现在 23.4Hz:0.72^n 到 10% 要 7 帧 ≈ 300ms,看着刚好。
   改 FFT_HOP 必须重算这个,否则下落快慢会跟着变。 */
#define DECAY            0.72f

/* ★ 内存分配:内部 RAM 是这块板子最紧的资源。
 * 任务栈、WiFi/BLE 的 DMA 缓冲全靠内部 RAM,而 PSRAM 有 8MB 闲着。
 *
 * 教训(真实事故):64KB 的 FFT 缓冲曾经全部【静态】放在内部 RAM,
 * 结果开机后内部 RAM 只剩 711 字节,music/spectrum/clock/curve
 * 【四个任务全部创建失败】—— 屏幕永远停在开机那一帧
 * (00:00:00 / no wifi),而串口风平浪静。
 *
 * 所以全部改成 PSRAM 动态分配 —— 关键是【不放在 .bss 里】:
 * 静态数组无论用不用都占内部 RAM,指针就不占。
 * s_fft 必须 16 字节对齐:esp-dsp 的 aes3 版本用 128 位加载。
 *
 * 剩下的内部占用只有 esp-dsp 自己 malloc 的旋转因子表 + 倒位表。
 */
static float *s_win  = NULL;            /* 汉宁窗 */
static float *s_ring = NULL;            /* 累积的单声道样本 */
static float *s_fft  = NULL;            /* 交错 re,im */

static void *PsramAlloc(size_t bytes)
{
    /* MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,并且 16 字节对齐 */
    void *p = heap_caps_aligned_alloc(16, bytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p;
}
static int   s_lo[SPECTRUM_BANDS];
static int   s_hi[SPECTRUM_BANDS];
static float s_band[SPECTRUM_BANDS];
static float s_level = 0.0f;
static int   s_fill  = 0;
static bool  s_inited = false;
static volatile bool s_enabled = true;
static float s_hp_x1 = 0.0f;            /* 高通隔直的状态量 */
static float s_hp_y1 = 0.0f;

/* 采样率不是写死的:播 TF 卡上 44.1kHz 的 WAV 时会改。 */
static uint32_t s_sr   = SPECTRUM_SR;
static float    s_fmax = 0.0f;          /* 实际覆盖到的最高频(Hz) */
static float    s_floor = DB_FLOOR_DEFAULT;   /* 当前显示下限(dBFS) */
static float    s_peak_db = -140.0f;    /* 上一帧最响频段的真实 dB */

/* 频段边界按 Hz 算 -> 换算成 bin。
   这样不管采样率是 44.1k 还是 48k,柱子的横坐标含义都一致。
   上限取 20kHz:44.1kHz 时 Nyquist 是 22.05k,还塞得下,
   所以横坐标刻度值不用跟着变。 */
static void RebuildBands(void)
{
    float nyq = (float)s_sr * 0.5f;
    s_fmax = SPECTRUM_F_MAX;
    if (s_fmax > nyq * 0.95f) {
        s_fmax = nyq * 0.95f;
    }

    float span = s_fmax / SPECTRUM_F_MIN;
    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        int lo = (int)lroundf(SPECTRUM_F_MIN * powf(span, (float)i /
                                                    SPECTRUM_BANDS) *
                              (float)FFT_N / (float)s_sr);
        int hi = (int)lroundf(SPECTRUM_F_MIN * powf(span, (float)(i + 1) /
                                                    SPECTRUM_BANDS) *
                              (float)FFT_N / (float)s_sr);
        if (lo < 1) {
            lo = 1;
        }
        if (hi <= lo) {
            hi = lo + 1;
        }
        if (hi > FFT_N / 2 - 1) {
            hi = FFT_N / 2 - 1;
        }
        s_lo[i] = lo;
        s_hi[i] = hi;
    }
}

/* ============================================================
 *  一次 FFT -> 32 个频段电平
 * ============================================================ */
static void RunFft(void)
{
    /* 计时用 esp_log_timestamp()(毫秒),不引 esp_timer 依赖 */
    const uint32_t t0 = esp_log_timestamp();

    /* 1. 先减掉这一帧的均值再加窗。
       有直流时,汉宁窗的频谱是个 3 抽头核,直流会以 -6dB 漏进
       ±1 号 bin,在 94Hz 附近顶出一根假柱。真实音乐里一帧的均值
       本来就接近 0,基本没影响;但麦克风那路是带直流偏置的,
       所以统一在这里处理一下。 */
    float mean = 0.0f;
    for (int i = 0; i < FFT_N; i++) {
        mean += s_ring[i];
    }
    mean /= (float)FFT_N;

    /* 2. 加窗,抑制频谱泄漏(不加窗的话旁瓣会把柱子糊成一片) */
    for (int i = 0; i < FFT_N; i++) {
        s_fft[2 * i]     = (s_ring[i] - mean) * s_win[i];
        s_fft[2 * i + 1] = 0.0f;
    }

    /* 3. 复数 FFT + 位反转,得到自然顺序的频谱 */
    dsps_fft2r_fc32(s_fft, FFT_N);
    dsps_bit_rev_fc32(s_fft, FFT_N);

    /* 4. 归一化:满幅正弦经汉宁窗后,峰值 bin 的幅度约为 N/4 */
    const float norm = 1.0f / (FFT_N * 0.25f);

    float peak     = 0.0f;
    float peak_db  = -140.0f;
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        float max_pow = 0.0f;
        for (int k = s_lo[b]; k < s_hi[b]; k++) {
            float re = s_fft[2 * k];
            float im = s_fft[2 * k + 1];
            float pw = re * re + im * im;
            if (pw > max_pow) {
                max_pow = pw;
            }
        }

        float amp = sqrtf(max_pow) * norm;
        float db  = 20.0f * log10f(amp + 1e-7f);
        if (db > peak_db) {
            peak_db = db;               /* 真实 dB,标定下限用 */
        }
        float lv  = (db - s_floor) / (0.0f - s_floor);
        if (lv < 0.0f) lv = 0.0f;
        if (lv > 1.0f) lv = 1.0f;

        /* 上升要快(跟得上鼓点),下落要慢(看着舒服) */
        if (lv > s_band[b]) {
            s_band[b] = lv;
        } else {
            s_band[b] = s_band[b] * DECAY + lv * (1.0f - DECAY);
        }
        if (s_band[b] > peak) {
            peak = s_band[b];
        }
    }

    s_level = (peak > s_level) ? peak : s_level * 0.85f;
    s_peak_db = peak_db;

    /* 实测耗时。4096 点比 512 点单次重得多,但每秒跑的帧数也少,
       所以真正要看的是“占用百分比”。每 64 帧(约 2.7 秒)报一条,不刷屏。
       毫秒级分辨率下单次可能量到 0——那就攒够 64 帧看平均。
       ---- 标定完之后这段可以删 ---- */
    {
        static int      n;
        static uint32_t sum_ms;
        sum_ms += esp_log_timestamp() - t0;      /* 毫秒级,单次多半是 0 */
        if (++n >= 64) {
            const double fps = (double)s_sr / (double)FFT_HOP;
            ESP_LOGI(TAG, "FFT %d 点 / hop %d:平均 %.2f ms/次,刷新 %.1f Hz",
                     FFT_N, FFT_HOP, (double)sum_ms / n, fps);
            n = 0;
            sum_ms = 0;
        }
    }
}

/* ============================================================
 *  对外接口
 * ============================================================ */
void Spectrum_Init(void)
{
    if (s_inited) {
        return;
    }

    /* 先分大缓冲。分不到就直接退出,别带着 NULL 指针往下跑 */
    s_win  = (float *)PsramAlloc(sizeof(float) * FFT_N);
    s_ring = (float *)PsramAlloc(sizeof(float) * FFT_N);
    s_fft  = (float *)PsramAlloc(sizeof(float) * FFT_N * 2);
    if (s_win == NULL || s_ring == NULL || s_fft == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配失败(共 %u 字节),频谱不可用",
                 (unsigned)(sizeof(float) * FFT_N * 4));
        return;
    }

    /* 汉宁窗 */
    for (int i = 0; i < FFT_N; i++) {
        s_win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (float)(FFT_N - 1)));
    }

    /* 对数分频段:低频窄、高频宽。边界按 Hz 算,已经和采样率无关了。 */
    RebuildBands();

    /* 表按最大 FFT 长度分配。★ 这里必须传 N,不能像以前那样传 N*2 ——
       esp-dsp 会校验 table_size <= CONFIG_DSP_MAX_FFT_SIZE(当前 4096),
       传 8192 直接返回 ESP_ERR_DSP_PARAM_OUTOFRANGE,整个频谱就起不来了。
       那个 *2 本来就是多余的"余量",在这里反而是坑。 */
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_N);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FFT 表初始化失败: %s(检查 CONFIG_DSP_MAX_FFT_SIZE >= %d)",
                 esp_err_to_name(err), FFT_N);
        return;
    }

    s_inited = true;
    ESP_LOGI(TAG, "频谱就绪: %d 点 FFT / %d 频段 / %.0f~%.0fHz @%uHz",
             FFT_N, SPECTRUM_BANDS, SPECTRUM_F_MIN, (double)s_fmax,
             (unsigned)s_sr);
    ESP_LOGI(TAG, "分辨率 %.2fHz/bin(汉宁主瓣约 %.0fHz),刷新 %.1fHz",
             (double)s_sr / FFT_N, 4.0 * (double)s_sr / FFT_N,
             (double)s_sr / FFT_HOP);
    ESP_LOGI(TAG, "缓冲: 共 %uKB 全在 PSRAM(.bss 不占内部 RAM)",
             (unsigned)(sizeof(float) * FFT_N * 4 / 1024));

    /* ---- FFT 自检:开机算 10 次,量真实耗时 ----
       在【真实缓冲】上跑,所以 PSRAM 的缓存代价、位反转的随机访问
       全都算进去了 —— 这才是能拿来做判断的数字。
       不用等"有歌在放"才看得到,也不依赖任何外部条件。
       代价是开机多花 10 x 7ms = 70ms,可以忽略。
       (动过 FFT_N / 缓冲位置之后,看这一行就知道代价变了多少。)
       耗时用 esp_log_timestamp 的毫秒级,10 次累计到几十毫秒,够看了。 */
    {
        const int reps = 10;
        uint32_t t0 = esp_log_timestamp();
        for (int i = 0; i < reps; i++) {
            RunFft();
        }
        uint32_t dt = esp_log_timestamp() - t0;
        const double per_ms = (double)dt / (double)reps;
        const double fps    = (double)s_sr / (double)FFT_HOP;
        ESP_LOGI(TAG, "FFT 自检: %d 点 x%d 次 = %ums,平均 %.2f ms/次",
                 FFT_N, reps, (unsigned)dt, per_ms);
        ESP_LOGI(TAG, "         按 %.1fHz 刷新算,占单核 %.1f%%",
                 fps, per_ms * fps / 10.0);
    }
}

void Spectrum_SetSampleRate(uint32_t hz)
{
    if (hz < 8000 || hz > 192000 || hz == s_sr) {
        return;
    }
    s_sr = hz;
    RebuildBands();
    ESP_LOGI(TAG, "频谱采样率改为 %uHz,频段 %.0f~%.0fHz",
             (unsigned)hz, SPECTRUM_F_MIN, (double)s_fmax);
}

uint32_t Spectrum_GetSampleRate(void)
{
    return s_sr;
}

void Spectrum_SetFloor(float db)
{
    /* 太靠近 0 会让整个屏幕都在削顶,太深则等于没范围 */
    if (db > -20.0f || db < -140.0f || db == s_floor) {
        return;
    }
    s_floor = db;
    ESP_LOGI(TAG, "显示下限改为 %.0fdB", (double)db);
}

float Spectrum_GetFloor(void)
{
    return s_floor;
}

float Spectrum_GetPeakDb(void)
{
    return s_peak_db;
}

bool Spectrum_IsReady(void)
{
    return s_inited;
}

void Spectrum_Enable(bool on)
{
    s_enabled = on;
    if (!on) {
        memset(s_band, 0, sizeof(s_band));
        s_level = 0.0f;
        s_fill  = 0;
        s_hp_x1 = 0.0f;
        s_hp_y1 = 0.0f;
    }
}

void Spectrum_Feed(const int16_t *pcm, size_t frames)
{
    if (!s_inited || !s_enabled || pcm == NULL) {
        return;
    }

    for (size_t i = 0; i < frames; i++) {
        /* 只取左声道,归一化到 -1.0~1.0 */
        float x = (float)pcm[2 * i] / 32768.0f;

        /* 一阶高通隔直(转折频率 ≈38Hz @48kHz)。
           麦克风输出带着直流偏置,不滤掉的话直流会泄到低频段,
           最左边那根柱会一直高着,看着像个假包。 */
        float y = x - s_hp_x1 + 0.995f * s_hp_y1;
        s_hp_x1 = x;
        s_hp_y1 = y;

        s_ring[s_fill++] = y;
        if (s_fill >= FFT_N) {
            RunFft();
            /* 50% 重叠:窗口只往前挪 FFT_HOP,后半段留着当下一次的头部。
               不这么做的话刷新率就是 sr/FFT_N = 11.7Hz,柱子会一顿一顿的。 */
            memmove(s_ring, s_ring + FFT_HOP,
                    (FFT_N - FFT_HOP) * sizeof(s_ring[0]));
            s_fill = FFT_N - FFT_HOP;
        }
    }
}

void Spectrum_GetBands(float out[SPECTRUM_BANDS])
{
    memcpy(out, s_band, sizeof(s_band));
}

float Spectrum_GetLevel(void)
{
    return s_level;
}
