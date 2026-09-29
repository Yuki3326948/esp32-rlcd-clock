#include "wav_player.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "wav";

static FILE        *s_f       = NULL;
static WavInfo      s_info    = {0};
static uint32_t     s_data_off = 0;    /* PCM 数据在文件里的偏移 */
static uint32_t     s_left     = 0;    /* 还剩多少 PCM 字节没读 */
static char         s_path[WAV_PATH_LEN] = {0};

/* 单声道转双声道时的中转缓冲。放静态区,别压在任务栈上。 */
static uint8_t      s_tmp[4096];

/* ---- 扫描结果 ---- */
static char s_files[WAV_MAX_FILES][WAV_PATH_LEN];
static int  s_file_cnt = 0;

static uint32_t RdU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t RdU16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

void Wav_Close(void)
{
    if (s_f != NULL) {
        fclose(s_f);
        s_f = NULL;
    }
    memset(&s_info, 0, sizeof(s_info));
    s_data_off = 0;
    s_left     = 0;
    s_path[0]  = 0;
}

bool Wav_IsOpen(void)
{
    return s_f != NULL;
}

const WavInfo *Wav_GetInfo(void)
{
    return &s_info;
}

const char *Wav_GetPath(void)
{
    return s_path;
}

esp_err_t Wav_Open(const char *path)
{
    Wav_Close();

    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "打不开: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* ---- 1. RIFF/WAVE 头 ---- */
    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), f) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "不是 WAV(RIFF/WAVE 标记不对): %s", path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 2. 顺着块找 fmt 和 data ---- */
    bool     got_data = false;
    uint16_t fmt_tag  = 0;
    while (!got_data) {
        uint8_t ch[8];
        if (fread(ch, 1, sizeof(ch), f) != sizeof(ch)) {
            break;                          /* 到头了还没看到 data */
        }
        uint32_t size = RdU32(ch + 4);

        if (memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fm[16];
            if (fread(fm, 1, sizeof(fm), f) != sizeof(fm)) {
                break;
            }
            fmt_tag             = RdU16(fm + 0);
            s_info.channels     = RdU16(fm + 2);
            s_info.sample_rate  = RdU32(fm + 4);
            s_info.bits         = RdU16(fm + 14);
            if (size > 16) {
                fseek(f, (long)(size - 16), SEEK_CUR);   /* 扩展部分跳过 */
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            s_data_off        = (uint32_t)ftell(f);
            s_info.data_bytes = size;
            got_data          = true;
        } else {
            /* 其它块(比如 LIST)跳过;块长度按偶数字节对齐 */
            fseek(f, (long)((size + 1u) & ~1u), SEEK_CUR);
        }
    }

    /* ---- 3. 能不能播 ---- */
    if (!got_data || s_info.sample_rate == 0 || s_info.channels == 0) {
        ESP_LOGE(TAG, "WAV 结构不完整,没有 data 块: %s", path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }
    if (fmt_tag != 1) {
        ESP_LOGE(TAG, "只支持未压缩 PCM(fmt=%u 是压缩格式): %s",
                 (unsigned)fmt_tag, path);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_info.bits != 16) {
        ESP_LOGE(TAG, "只支持 16-bit,这个文件是 %u-bit: %s",
                 (unsigned)s_info.bits, path);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_info.channels > 2) {
        ESP_LOGE(TAG, "只支持 1/2 声道,这个文件是 %u 声道",
                 (unsigned)s_info.channels);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_info.duration_ms = (uint32_t)((uint64_t)s_info.data_bytes * 1000ull /
                                    ((uint64_t)s_info.sample_rate *
                                     s_info.channels * 2ull));

    s_f    = f;
    s_left = s_info.data_bytes;
    strncpy(s_path, path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = 0;

    ESP_LOGI(TAG, "打开 %s: %uHz %u 声道 16bit %u 字节 ≈ %.1f 秒",
             path, (unsigned)s_info.sample_rate, (unsigned)s_info.channels,
             (unsigned)s_info.data_bytes, (double)s_info.duration_ms / 1000.0);
    return ESP_OK;
}

esp_err_t Wav_Rewind(void)
{
    if (s_f == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fseek(s_f, (long)s_data_off, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    s_left = s_info.data_bytes;
    return ESP_OK;
}

size_t Wav_Read(void *buf, size_t bytes)
{
    if (s_f == NULL || buf == NULL || bytes == 0) {
        return 0;
    }

    const size_t src_frame  = (size_t)s_info.channels * 2;   /* 源文件一帧的字节数 */
    size_t       want       = bytes;
    if (s_info.channels == 1) {
        want = bytes / 2;      /* 单声道要复制,读一半就够 */
    }
    if (want > s_left) {
        want = s_left;
    }
    want -= want % src_frame;
    if (want == 0) {
        return 0;
    }
    if (want > sizeof(s_tmp)) {
        want = sizeof(s_tmp) - (sizeof(s_tmp) % src_frame);
    }

    size_t got = fread(s_tmp, 1, want, s_f);
    got -= got % src_frame;
    if (got == 0) {
        return 0;
    }
    s_left -= (uint32_t)got;

    if (s_info.channels == 2) {
        memcpy(buf, s_tmp, got);
        return got;
    }

    /* 单声道 -> 双声道:同一份样本写两次 */
    const int16_t *src = (const int16_t *)s_tmp;
    int16_t       *dst = (int16_t *)buf;
    size_t         n   = got / 2;
    for (size_t i = 0; i < n; i++) {
        dst[2 * i]     = src[i];
        dst[2 * i + 1] = src[i];
    }
    return n * 4;
}

/* ============================================================
 *  目录扫描
 * ============================================================ */
static int NameCmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

int Wav_ScanDir(const char *dir)
{
    s_file_cnt = 0;

    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "打不开目录 %s", dir);
        return 0;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_file_cnt < WAV_MAX_FILES) {
        size_t len = strlen(e->d_name);
        if (len < 5 || len + strlen(dir) + 2 > WAV_PATH_LEN) {
            continue;
        }
        if (strcasecmp(e->d_name + len - 4, ".wav") != 0) {
            continue;
        }
        if (e->d_type == DT_DIR) {
            continue;
        }
        /* 目录最多 47 字符 + '/' + 文件名最多 46 字符 + 结尾 = 94,
           比 WAV_PATH_LEN(96) 小。写死宽度是为了避开
           -Werror=format-truncation:编译器看不到上面那个长度判断。 */
        snprintf(s_files[s_file_cnt], WAV_PATH_LEN, "%.47s/%.46s",
                 dir, e->d_name);
        s_file_cnt++;
    }
    closedir(d);

    if (s_file_cnt > 0) {
        /* 排一下序,这样"下一首"的顺序每次开机都一样 */
        qsort(s_files, (size_t)s_file_cnt, WAV_PATH_LEN, NameCmp);
        ESP_LOGI(TAG, "在 %s 找到 %d 个 WAV:", dir, s_file_cnt);
        for (int i = 0; i < s_file_cnt; i++) {
            ESP_LOGI(TAG, "  %d) %s", i + 1, Wav_FileName(i));
        }
    } else {
        ESP_LOGW(TAG, "%s 里没有 .wav 文件", dir);
    }
    return s_file_cnt;
}

int Wav_FileCount(void)
{
    return s_file_cnt;
}

const char *Wav_FilePath(int idx)
{
    if (idx < 0 || idx >= s_file_cnt) {
        return NULL;
    }
    return s_files[idx];
}

const char *Wav_FileName(int idx)
{
    const char *full = Wav_FilePath(idx);
    if (full == NULL) {
        return "";
    }
    const char *slash = strrchr(full, '/');
    return (slash != NULL) ? (slash + 1) : full;
}
