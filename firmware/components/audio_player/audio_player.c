/*
 * 板载音频驱动实现 —— I2S0 + ES8311(DAC) + ES7210(ADC)
 *
 * 为什么要用 esp_codec_dev:
 *   ES8311 的寄存器初始化跟 MCLK 分频、采样率强相关(要写几十个寄存器),
 *   自己写容易出错。espressif/esp_codec_dev 里带的是官方验证过的驱动,
 *   我们只负责把 I2S 通道和 I2C 总线准备好,然后直接用它播放。
 */
#include "audio_player.h"

#include <string.h>

#include "board_sensors.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"   /* 提供 audio_codec_new_* / es8311_codec_new */

static const char *TAG = "audio";

/* ES7210 麦克风增益。分档是 0/3/6/.../33/34.5/36/37.5 dB,
   驱动会把传入的 dB 向下取整到档位(所以官方写的 25 实际是 24dB)。
   原来跟随官方用 24dB,但远处的声音在频谱上根本看不见;
   提到 34.5dB 后仍有 +3dB 余量,只有贴着喇叭喊才可能削顶。 */
#define MIC_GAIN_DB                 34.5f

static i2s_chan_handle_t            s_tx       = NULL;
static i2s_chan_handle_t            s_rx       = NULL;
static const audio_codec_ctrl_if_t *s_ctrl_if  = NULL;
static const audio_codec_gpio_if_t *s_gpio_if  = NULL;
static const audio_codec_data_if_t *s_data_if  = NULL;
static const audio_codec_if_t      *s_codec_if = NULL;
static esp_codec_dev_handle_t       s_dac      = NULL;

/* 麦克风(ES7210)。和 ES8311 挂同一条 I2C、共用同一条 I2S。 */
static const audio_codec_if_t      *s_mic_if   = NULL;
static esp_codec_dev_handle_t       s_adc      = NULL;
static bool                         s_mic_open = false;

static bool s_ready  = false;
static bool s_opened = false;
static int  s_volume = 70;
/* 当前采样率。默认 48000,播 TF 卡上 44.1kHz 的 WAV 时会临时改掉。 */
static uint32_t s_rate = AUDIO_SAMPLE_RATE;

/* ============================================================
 *  I2S 通道
 * ============================================================ */
static esp_err_t InitI2s(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 网络推流最怕 DMA 饿死。原来 6 x 240 帧 @48kHz 只有 30ms,
       WiFi 一抖就成静音间隙(听着就是"断断续续")。
       改成 8 x 512 帧 = 每方向约 106ms,给网络留足抖动余量。
       TX/RX 各占 16KB 内部 DMA 内存。 */
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear    = true;   /* 没数据时输出 0,避免喇叭沙沙声 */

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx),
                        TAG, "创建 I2S 通道失败");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_PIN,
            .bclk = AUDIO_I2S_BCLK_PIN,
            .ws   = AUDIO_I2S_WS_PIN,
            .dout = AUDIO_I2S_DOUT_PIN,
            .din  = AUDIO_I2S_DIN_PIN,
            .invert_flags = { .mclk_inv = false,
                              .bclk_inv = false,
                              .ws_inv   = false },
        },
    };
    /* ES8311 需要 MCLK = 256 x LRCK,即 24000 * 256 = 6.144MHz */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg),
                        TAG, "TX 通道初始化失败");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg),
                        TAG, "RX 通道初始化失败");

    ESP_LOGI(TAG, "I2S 就绪  %dHz/%dch/%dbit  MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d",
             AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS,
             AUDIO_I2S_MCLK_PIN, AUDIO_I2S_BCLK_PIN, AUDIO_I2S_WS_PIN,
             AUDIO_I2S_DOUT_PIN, AUDIO_I2S_DIN_PIN);
    return ESP_OK;
}

/* ============================================================
 *  ES8311 编解码器
 * ============================================================ */
static esp_err_t InitCodec(void)
{
    i2c_master_bus_handle_t bus = Sensors_GetI2cBus();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG,
                        "I2C 总线还没初始化(要先调 Sensors_Init)");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port           = I2C_NUM_0,
        .addr           = ES8311_CODEC_DEFAULT_ADDR,   /* 0x30 = 7bit 0x18 */
        .bus_handle     = bus,
        .clock_speed_hz = 400000,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if, ESP_FAIL, TAG, "创建 I2C 控制接口失败");

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    ESP_RETURN_ON_FALSE(s_data_if, ESP_FAIL, TAG, "创建 I2S 数据接口失败");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if, ESP_FAIL, TAG, "创建 GPIO 接口失败");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = s_ctrl_if,
        .gpio_if     = s_gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = AUDIO_PA_PIN,   /* 打开播放时自动拉高 */
        .pa_reverted = false,
        .master_mode = false,          /* ESP32 作 I2S 主机发 BCLK/WS */
        .use_mclk    = true,           /* 用 ESP32 输出的 MCLK */
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = { .pa_voltage        = 5.0f,
                         .codec_dac_voltage = 3.3f,
                         .pa_gain           = 6.0f },
        .no_dac_ref  = false,
        .mclk_div    = 256,
    };
    s_codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if, ESP_FAIL, TAG, "ES8311 初始化失败");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec_if,
        .data_if  = s_data_if,
    };
    s_dac = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dac, ESP_FAIL, TAG, "创建播放设备失败");

    ESP_LOGI(TAG, "ES8311 就绪  I2C=0x%02X  PA=GPIO%d",
             ES8311_CODEC_DEFAULT_ADDR, AUDIO_PA_PIN);
    return ESP_OK;
}

/* ============================================================
 *  ES7210 麦克风
 * ============================================================ */
static esp_err_t InitMicCodec(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port           = I2C_NUM_0,
        .addr           = ES7210_CODEC_DEFAULT_ADDR,   /* 0x80 = 7bit 0x40 */
        .bus_handle     = Sensors_GetI2cBus(),
        .clock_speed_hz = 400000,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl, ESP_FAIL, TAG, "创建 ES7210 控制接口失败");

    /* 官方 STD(非 TDM)模式选的是 MIC1|MIC3,正好凑成双声道 */
    es7210_codec_cfg_t mic_cfg = {
        .ctrl_if      = ctrl,
        .master_mode  = false,        /* ESP32 作 I2S 主机 */
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC3,
        .mclk_src     = ES7210_MCLK_FROM_PAD,
        .mclk_div     = 256,
    };
    s_mic_if = es7210_codec_new(&mic_cfg);
    ESP_RETURN_ON_FALSE(s_mic_if, ESP_FAIL, TAG, "ES7210 初始化失败");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_mic_if,
        .data_if  = s_data_if,        /* 和播放共用同一条 I2S(全双工) */
    };
    s_adc = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_adc, ESP_FAIL, TAG, "创建麦克风设备失败");

    ESP_LOGI(TAG, "ES7210 就绪  I2C=0x%02X  MIC1|MIC3", ES7210_CODEC_DEFAULT_ADDR);
    return ESP_OK;
}

/* ============================================================
 *  对外接口
 * ============================================================ */
esp_err_t Audio_Init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_err_t err = InitI2s();
    if (err != ESP_OK) {
        return err;
    }
    err = InitCodec();
    if (err != ESP_OK) {
        return err;
    }
    /* 麦克风初始化失败不算致命,只是没有麦克风频谱 */
    if (InitMicCodec() != ESP_OK) {
        ESP_LOGW(TAG, "麦克风初始化失败,麦克风频谱不可用");
    }
    s_ready = true;
    return ESP_OK;
}

bool Audio_IsReady(void)  { return s_ready; }
bool Audio_IsOpened(void) { return s_opened; }

esp_err_t Audio_Open(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "还没初始化");
    if (s_opened) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel         = AUDIO_CHANNELS,
        .channel_mask    = 0,
        .sample_rate     = s_rate,
        .mclk_multiple   = 256,
    };
    int ret = esp_codec_dev_open(s_dac, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "打开播放通道失败: %d", ret);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_dac, s_volume);
    s_opened = true;
    ESP_LOGI(TAG, "播放通道已打开,音量 %d", s_volume);
    return ESP_OK;
}

esp_err_t Audio_Close(void)
{
    if (!s_opened) {
        return ESP_OK;
    }
    esp_codec_dev_close(s_dac);   /* 里面会拉低功放使能 */
    s_opened = false;
    return ESP_OK;
}

esp_err_t Audio_SetVolume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_opened) {
        esp_codec_dev_set_out_vol(s_dac, vol);
    }
    return ESP_OK;
}

int Audio_GetVolume(void)
{
    return s_volume;
}

esp_err_t Audio_PlayPcm(const void *data, size_t bytes)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "还没初始化");
    ESP_RETURN_ON_FALSE(data && bytes, ESP_ERR_INVALID_ARG, TAG, "空数据");

    if (!s_opened) {
        esp_err_t err = Audio_Open();
        if (err != ESP_OK) {
            return err;
        }
    }
    int ret = esp_codec_dev_write(s_dac, (void *)data, (int)bytes);
    return (ret == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t Audio_OpenMic(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "还没初始化");
    ESP_RETURN_ON_FALSE(s_adc, ESP_ERR_NOT_SUPPORTED, TAG, "麦克风不可用");
    if (s_mic_open) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel         = AUDIO_CHANNELS,
        .channel_mask    = 0,
        .sample_rate     = s_rate,
        .mclk_multiple   = 256,
    };
    int ret = esp_codec_dev_open(s_adc, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "打开麦克风失败: %d", ret);
        return ESP_FAIL;
    }
    s_mic_open = true;

    /* 官方例程用的是 25dB,先用同一个值 */
    if (esp_codec_dev_set_in_gain(s_adc, MIC_GAIN_DB) == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "麦克风通道已打开  增益=%.0fdB", (double)MIC_GAIN_DB);
    } else {
        ESP_LOGW(TAG, "麦克风通道已打开,但增益设置失败");
    }
    return ESP_OK;
}

esp_err_t Audio_SetMicGain(float db)
{
    ESP_RETURN_ON_FALSE(s_mic_open, ESP_ERR_INVALID_STATE, TAG, "麦克风未打开");
    int ret = esp_codec_dev_set_in_gain(s_adc, db);
    return (ret == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}

uint32_t Audio_GetSampleRate(void)
{
    return s_rate;
}

esp_err_t Audio_SetSampleRate(uint32_t hz)
{
    ESP_RETURN_ON_FALSE(s_dac != NULL, ESP_ERR_INVALID_STATE, TAG, "还没打开播放通道");
    ESP_RETURN_ON_FALSE(hz >= 8000 && hz <= 96000, ESP_ERR_INVALID_ARG,
                        TAG, "采样率不支持: %u", (unsigned)hz);
    if (hz == s_rate) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel         = AUDIO_CHANNELS,
        .channel_mask    = 0,
        .sample_rate     = hz,
        .mclk_multiple   = 256,
    };

    /* 麦克风跟播放共用同一条 I2S。播放这边是主机,改它的时钟
       等于把整条 I2S 的节奏都改了,所以麦克风也得跟着重开一次。 */
    bool mic_was_open = s_mic_open;
    if (mic_was_open) {
        esp_codec_dev_close(s_adc);
        s_mic_open = false;
    }

    /* 关掉再开:esp_codec_dev 内部会 disable -> reconfig slot/clock -> enable,
       ES8311 的寄存器也跟着按新采样率重算一遍。 */
    esp_codec_dev_close(s_dac);
    int ret = esp_codec_dev_open(s_dac, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "切到 %uHz 失败: %d", (unsigned)hz, ret);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_dac, s_volume);   /* 重开后音量会回默认值 */

    if (mic_was_open) {
        if (esp_codec_dev_open(s_adc, &fs) == ESP_CODEC_DEV_OK) {
            s_mic_open = true;
            esp_codec_dev_set_in_gain(s_adc, MIC_GAIN_DB);
        } else {
            ESP_LOGW(TAG, "麦克风没能跟着切到 %uHz", (unsigned)hz);
        }
    }

    s_rate = hz;
    ESP_LOGI(TAG, "I2S 采样率切到 %uHz", (unsigned)hz);
    return ESP_OK;
}

esp_err_t Audio_ReadMic(void *buf, size_t bytes)
{
    ESP_RETURN_ON_FALSE(buf && bytes, ESP_ERR_INVALID_ARG, TAG, "空缓冲");

    if (!s_mic_open) {
        esp_err_t err = Audio_OpenMic();
        if (err != ESP_OK) {
            return err;
        }
    }
    int ret = esp_codec_dev_read(s_adc, buf, (int)bytes);
    return (ret == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}

bool Audio_IsMicOpen(void)
{
    return s_mic_open;
}

esp_err_t Audio_Mute(bool mute)
{
    ESP_RETURN_ON_FALSE(s_opened, ESP_ERR_INVALID_STATE, TAG, "播放通道未打开");
    int ret = esp_codec_dev_set_out_mute(s_dac, mute);
    /* 每次静音/解除静音都记一笔:该响却没响时,一眼就能看出
       是不是有人(比如音乐任务或推流任务)把 DAC 静音了。 */
    if (ret == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "DAC %s", mute ? "静音" : "解除静音");
    } else {
        ESP_LOGW(TAG, "DAC %s 失败: %d", mute ? "静音" : "解除静音", ret);
    }
    return (ret == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}
