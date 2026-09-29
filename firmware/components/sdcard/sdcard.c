#include "sdcard.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdcard";

#define SD_PIN_CLK      38
#define SD_PIN_CMD      21
#define SD_PIN_D0       39

/* 卡不是 FAT32 时(常见于 64GB 以上的卡,Windows 默认给 exFAT),
   要不要直接把它格成 FAT32?

   ⚠ 会抹掉卡上全部数据,所以默认关掉。确认过再改成 1 烧一次,
     格完立即改回 0 —— 否则以后卡里只要读出错就有被抹的风险。

   背景:ESP-IDF 的 fatfs 里 FF_FS_EXFAT 是硬编码 0,Kconfig 也没开关,
   所以 exFAT / NTFS 都挂不了,只认 FAT12/16/32。 */
#define SDCARD_ALLOW_FORMAT     0   /* 卡已经格好了,改回 0 免得以后误抹 */

static sdmmc_card_t *s_card = NULL;

static esp_err_t TryMountOnce(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = (SDCARD_ALLOW_FORMAT != 0),
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;                        /* 只走 D0 */
    slot.clk   = (gpio_num_t)SD_PIN_CLK;
    slot.cmd   = (gpio_num_t)SD_PIN_CMD;
    slot.d0    = (gpio_num_t)SD_PIN_D0;
    /* TF 卡的插座上没有外部上拉,让芯片内部的顶上 */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host,
                                            &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;                     /* 挂载失败时它可能已经被改了 */
    }
    return err;
}

esp_err_t Sdcard_Mount(void)
{
    if (s_card != NULL) {
        return ESP_OK;
    }

    esp_err_t err = ESP_FAIL;

    /* 多试两次:SD 初始化对卡座接触很敏感,
       偶尔一次 send_op_cond 超时但重试就过了。 */
    const int kAttempts = 3;
    for (int i = 1; i <= kAttempts; i++) {
        err = TryMountOnce();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TF 卡已挂载到 %s(第 %d 次尝试)", SDCARD_MOUNT_POINT, i);
            sdmmc_card_print_info(stdout, s_card);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "第 %d/%d 次挂载失败: %s",
                 i, kAttempts, esp_err_to_name(err));
        if (i < kAttempts) {
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }

    /* 关键是分清两层失败,不然会把人带错方向:
         ESP_FAIL        = 卡读到了,但文件系统不认 -> 格式问题
         其它(超时等)   = 卡在底层就没应答     -> 接触/硬件问题 */
    ESP_LOGE(TAG, "TF 卡挂载失败(试了 %d 次): %s(%d)",
             kAttempts, esp_err_to_name(err), (int)err);
    if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "→ 卡读到了,但文件系统不认。ESP-IDF 只支持 FAT32:");
        ESP_LOGE(TAG, "  exFAT / NTFS 都不行。超过 32GB 的卡 Windows 自带");
        ESP_LOGE(TAG, "  格式化不给 FAT32 选项,得用 Rufus / DiskGenius。");
    } else {
        ESP_LOGE(TAG, "→ 卡在底层就没应答,不是格式问题。逐条查:");
        ESP_LOGE(TAG, "  1) 卡有没有插到底、有没有弹出卡住");
        ESP_LOGE(TAG, "  2) 金手指是否氧化/脏污,擦一下再试");
        ESP_LOGE(TAG, "  3) 换一张卡,排除卡本身坏了");
    }
    return err;
}

void Sdcard_Unmount(void)
{
    if (s_card == NULL) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    s_card = NULL;
    ESP_LOGI(TAG, "TF 卡已卸载");
}

bool Sdcard_IsMounted(void)
{
    return s_card != NULL;
}

void Sdcard_List(const char *dir, int max_items)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "打不开目录 %s", dir);
        return;
    }

    ESP_LOGI(TAG, "---- %s 的内容 ----", dir);
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL && n < max_items) {
        if (e->d_name[0] == '.') {         /* 跳过 . 和 .. */
            continue;
        }
        n++;
        if (e->d_type == DT_DIR) {
            ESP_LOGI(TAG, "  <目录>          %s", e->d_name);
        } else {
            char full[300];
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            struct stat st;
            long size = (stat(full, &st) == 0) ? (long)st.st_size : -1;
            ESP_LOGI(TAG, "  %9ld 字节  %s", size, e->d_name);
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "---- 共列出 %d 项 ----", n);
}
