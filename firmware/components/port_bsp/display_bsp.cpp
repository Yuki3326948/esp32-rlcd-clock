#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_system.h>
#include "display_bsp.h"

DisplayPort::DisplayPort(int mosi, int scl, int dc, int cs, int rst, int width, int height, spi_host_device_t spihost) : 
mosi_(mosi), 
scl_(scl), 
dc_(dc), 
cs_(cs), 
rst_(rst), 
width_(width), 
height_(height) 
{
    esp_err_t        ret;
    spi_bus_config_t buscfg   = {};
    int              transfer = width_ * height_;
    buscfg.miso_io_num                   = -1;
    buscfg.mosi_io_num                   = mosi;
    buscfg.sclk_io_num                   = scl;
    buscfg.quadwp_io_num                 = -1;
    buscfg.quadhd_io_num                 = -1;
    buscfg.max_transfer_sz               = transfer;
    ret                                  = spi_bus_initialize(spihost, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = (gpio_num_t)dc_;
    io_config.cs_gpio_num = (gpio_num_t)cs_;
    io_config.pclk_hz = 20 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;

    /* ★★ 让 SPI 直接从 PSRAM 取帧缓冲发出去(默认是关的)。
     *
     * 不打开会怎样 —— 这个坑藏得极深,值得写下来:
     *   帧缓冲 DispBuffer 在 PSRAM 里,而 SPI 的 DMA 只能从【内部 DMA 内存】
     *   读。esp_lcd 于是【每一帧】都:
     *       heap_caps_aligned_alloc(15000, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL)
     *       memcpy 整整 15000 字节
     *       ...发完再 free
     *   (见 esp_lcd_panel_io_spi.c -> spicommon_dma_setup_priv_buffer)
     *
     *   后果一(内存):每次刷屏都要一口气要到 15000 字节【连续】的内部 DMA 内存。
     *   电脑正在推音频流时(SetLowLatency 关掉 WiFi 省电,射频缓冲多吃掉 ~30KB),
     *   内部 RAM 只剩几 KB —— 这个分配就失败:
     *       E spi_common: Failed to allocate priv TX buffer
     *       E lcd_panel.io.spi: spi transmit (queue) color failed
     *   实测 150 秒里失败 4087 次,屏幕只能刷到 5fps 左右,
     *   而且刷屏失败会把调用它的频谱任务一起拖住。
     *
     *   后果二(速度):每帧平白多一次 15KB 的 PSRAM→内部RAM 大拷贝。
     *
     * 打开这个开关之后,GDMA 直接从 PSRAM 读 —— 不再分配、不再拷贝,
     * 内部 RAM 一个字节都不占。ESP32-S3 的 GDMA 本来就能访问 PSRAM,
     * 这就是这个开关存在的意义(见 esp_lcd_io_spi.h 里的 psram_dma_direct)。 */
    io_config.flags.psram_dma_direct = 1;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spihost, &io_config, &io_handle));

    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    Set_ResetIOLevel(1);

    DisplayLen                = transfer >> 3; //(1byte 8ipex)
    /* 帧缓冲放 PSRAM。★ 之所以敢放 PSRAM,是因为上面开了 psram_dma_direct:
       否则 esp_lcd 每帧都要临时在内部 RAM 里要一份 15000 字节的连续 DMA 内存
       (内存紧张时直接失败),而且每帧还多一次 15KB 拷贝。 */
    DispBuffer                = (uint8_t *) heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    assert(DispBuffer);

    /* 这里原本会分配两张巨大的查表:
           PixelIndexLUT = 400*300*2 = 240KB
           PixelBitLUT   = 400*300*1 = 120KB
       两张都在 PSRAM 里 —— 每画一个像素要随机读两次大表(几乎必然 cache miss),
       再读改写一次 DispBuffer,一共 3 次 PSRAM 访问,这是刷新慢的元凶。
       而索引和位掩码用几条整数运算就能算出来(见 RLCD_SetPixel),
       不查表反而快得多,还白省下 360KB PSRAM。 */
}

DisplayPort::~DisplayPort() {
}

void DisplayPort::RLCD_Init() {
    RLCD_Reset();

    RLCD_SendCommand(0xD6);  // NVM Load Control
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); //Booster Enable
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); //Gate Voltage Control
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); //VSHP Setting
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0x80);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(pdMS_TO_TICKS(200));     
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);

    RLCD_ColorClear(ColorWhite);
}

void DisplayPort::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void DisplayPort::RLCD_Display() {
    RLCD_SendCommand(0x2A);     // Column Address Set
  	RLCD_SendData(0x12);
  	RLCD_SendData(0x2A);

  	RLCD_SendCommand(0x2B);     // Page Address Set
  	RLCD_SendData(0x00);
  	RLCD_SendData(0xC7);

  	RLCD_SendCommand(0x2c);     // Page Address Set

	RLCD_Sendbuffera(DispBuffer,DisplayLen);
}

void DisplayPort::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void DisplayPort::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0));
}

void DisplayPort::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1));
}

void DisplayPort::RLCD_Sendbuffera(uint8_t *Data, int len) {
    /* ★ 这里绝对不能 ESP_ERROR_CHECK。
       内存紧张时 esp_lcd_panel_io_tx_color 会返回 ESP_ERR_NO_MEM
       (SPI 的 DMA 描述符分不出来),而 ESP_ERROR_CHECK 会直接 abort
       -> 整块板子重启。

       实测现象:电脑推音频流那一刻,SetLowLatency 关掉了 WiFi 省电
       (射频缓冲变多)、I2S 的 DMA 也开始占内存,堆一下子紧了;
       紧接着每秒一次的刷屏就 abort —— 表现是"一接上音频流板子就重启,
       永远没声音",而且看日志只能看到 ESP_ERR_NO_MEM,完全不像刷屏的问题。

       刷屏失败最坏也就是这一帧没送出去,下一秒的定时刷屏会重画,
       所以丢掉它就行,不值得把整个系统重启。 */
    esp_err_t err = esp_lcd_panel_io_tx_color(io_handle, -1, Data, len);

    /* 失败几乎都是"这一瞬间凑不出连续的 DMA 内存"(典型诱因:刚关掉 WiFi 省电,
       驱动正在补分配射频缓冲)。过几十毫秒那些临时分配就还回去了,
       所以【等 20ms 重试一次】的命中率很高 —— 实测重试就能成功。
       不重试的话这一帧白丢,屏幕要等 35ms 之后那一帧才补上。 */
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        err = esp_lcd_panel_io_tx_color(io_handle, -1, Data, len);
    }

    /* 成功/失败计数 + 日志。
     *
     * ★ 这里以前的写法有个很坑的地方,值得记下来:
     *     if (fail_n != 0) {
     *         if (fail_n <= 5 || fail_n - last_report >= 50) { 报 }
     *     }
     *   fail_n 一旦变成 1 就【永远不会回到 0】,而 fail_n <= 5 又绕过了
     *   后面的限流 —— 于是"一次失败之后,每一帧成功都要打一行『刷屏失败』"。
     *   实测 40 行/秒、每行 ~90 字节,115200 的串口根本吃不下;
     *   ESP_LOGW 一反压就把调用它的频谱任务一起拖慢。
     *   更糟的是真出问题时满屏都是假失败,真的那条反而淹没了。
     *   (调试时我就被这个坑过:看到几百行"刷屏失败"以为屏幕压根没刷,
     *    实际上只有第 1 帧失败,其余全是 ESP_OK。)
     *
     *   改成【正边缘触发】:失败时报,从失败恢复时报一次,然后闭嘴。 */
    static uint32_t ok_n, fail_n;
    static bool     in_fail = false;

    if (err == ESP_OK) {
        ok_n++;
        if (in_fail) {              /* 爬出来了,报一次收工 */
            in_fail = false;
            ESP_LOGW("RLCD", "刷屏恢复:期间共失败 %u 次,累计成功 %u 帧,内部RAM剩%u",
                     (unsigned)fail_n, (unsigned)ok_n,
                     (unsigned)esp_get_free_internal_heap_size());
            fail_n = 0;
        }
    } else {
        in_fail = true;
        fail_n++;
        /* 头 5 次每次报(看得到苗头),之后每 50 次报一次(限流但不静默) */
        if (fail_n <= 5 || (fail_n % 50) == 0) {
            ESP_LOGW("RLCD", "刷屏失败 第%u 次(累计成功%u): %s  内部RAM剩%u",
                     (unsigned)fail_n, (unsigned)ok_n,
                     esp_err_to_name(err),
                     (unsigned)esp_get_free_internal_heap_size());
        }
    }
}

void DisplayPort::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}
#if (AlgorithmOptimization != 3)

void DisplayPort::RLCD_SetPortraitPixel(uint16_t x, uint16_t y, uint8_t color) {
    if((x >= width_) || (y >= height_)) {
  	  	ESP_LOGE("Pixel","Beyond the limit : (%d,%d)",x ,y);
        return;
  	}
#if (AlgorithmOptimization == 2)
	const uint16_t W4 = width_ >> 2;  

    uint16_t byte_x = x >> 2;        
    uint16_t byte_y = y >> 1;        

    uint32_t index = byte_y * W4 + byte_x;

    uint8_t local_x = x & 0x03; 
    uint8_t local_y = y & 0x01; 

    uint8_t bit = 7 - ((local_x << 1) | local_y);

    uint8_t mask = 1 << bit;

    if (color)
        DispBuffer[index] |= mask;
    else
        DispBuffer[index] &= ~mask;
#else
    uint16_t byte_x = x / 4;
    uint16_t byte_y = y / 2;

    uint32_t index = byte_y * (width_ / 4) + byte_x;

    uint8_t local_x = x % 4;  
    uint8_t local_y = y % 2;  
    uint8_t bit = 7 - (local_x * 2 + local_y);
    if (color)
        DispBuffer[index] |=  (1 << bit);
    else
        DispBuffer[index] &= ~(1 << bit);
#endif
}

void DisplayPort::RLCD_SetLandscapePixel(uint16_t x, uint16_t y, uint8_t color) {
    if (x >= width_ || y >= height_)
        return;
#if (AlgorithmOptimization == 2)

	uint16_t inv_y = (height_ - 1 - y);
    const uint16_t H4 = height_ >> 2;  
    uint16_t byte_x = x >> 1;          
    uint16_t block_y = inv_y >> 2;     
    uint32_t index = byte_x * H4 + block_y;
    uint8_t local_x = x & 0x01;        
    uint8_t local_y = inv_y & 0x03;    
    uint8_t bit = 7 - ((local_y << 1) | local_x);
    uint8_t mask = 1 << bit;
    if (color)
        DispBuffer[index] |= mask;
    else
        DispBuffer[index] &= ~mask;
#else
    uint16_t inv_y = height_ - 1 - y;

    uint16_t byte_x  = x / 2;           // 0..199
    uint16_t block_y = inv_y / 4;       // 0..74

    uint32_t index = byte_x * (height_ / 4) + block_y; 

    uint8_t local_x = x % 2;            // 0 or 1
    uint8_t local_y = inv_y % 4;        // 0..3

    uint8_t bit = 7 - (local_y * 2 + local_x);

    if (color)
        DispBuffer[index] |= (1 << bit);
    else
        DispBuffer[index] &= ~(1 << bit);
#endif
}

#endif


#if (AlgorithmOptimization == 3)

void DisplayPort::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;

        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;

            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void DisplayPort::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;

    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;

        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;

            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

/* 横屏像素 -> 缓冲区位映射(和原来的 InitLandscapeLUT 完全等价,只是改成算出来)
     屏幕装成 180 度,所以 y 要反过来;
     每 2 个 x 共用一个字节,每 4 个 y 组成一个 block。
   以前是查 PSRAM 里的大表,现在是纯寄存器运算 + 一次 DispBuffer 访问。 */
void DisplayPort::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    if (x >= width_ || y >= height_) {
        return;
    }

    const uint16_t inv_y = height_ - 1 - y;
    const uint32_t idx   = ((uint32_t)(x >> 1) * (uint32_t)(height_ >> 2)) + (inv_y >> 2);
    const uint8_t  bit   = (uint8_t)(1u << (7 - (uint8_t)(((inv_y & 3u) << 1) | (x & 1u))));

    uint8_t *p = &DispBuffer[idx];

    if (color)
        *p |= bit;
    else
        *p &= ~bit;
}

#endif