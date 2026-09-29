#include "sysmon.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdcard.h"

static const char *TAG = "sysmon";

/* ---------------- 历史环形缓冲 ---------------- */
static SysSample_t      *s_hist  = NULL;    /* 在 PSRAM 里 */
static int               s_cap   = SYSMON_HIST_MAX;  /* 实际容量(PAM 不够时会降) */
static int               s_head  = 0;       /* 下一个写入位置 */
static int               s_count = 0;       /* 已有点数 */
static SemaphoreHandle_t s_lock  = NULL;

/* ---------------- 最新一帧 ---------------- */
static volatile bool s_connected = false;
static char          s_peer[20]  = "";
static SysStatus_t   s_st;

/* 实测采样间隔,用来把"点数"换算成"时间跨度"给横轴用 */
static int64_t s_last_us;
static int     s_interval_ms;

/* ============================================================
 *  TF 卡日志
 * ============================================================
 *  为什么存卡:PSRAM 里的环形缓冲只有 3 小时,存卡能长期保留,
 *  而且拔卡插电脑就能直接分析。
 *
 *  写入策略(对卡友好):先在内存攒着,写满接近 2KB 或者超过 30 秒
 *  才真正落一次盘。2Hz 下约 35 秒才写一次(5.5MB/天),
 *  不是每 0.5 秒就去折腾一次卡。
 *
 *  为什么不担心和播放器撞:IDF 的 FATFS 开着 FF_FS_REENTRANT,
 *  VFS 层还有一把锁,两个任务同时读写是安全的。
 */
#define LOG_DIR        "/sdcard/sysmon"
#define LOG_KEEP_DAYS  30           /* 只保留这么多天,自动删旧的 */
#define LOG_BUF_SIZE   2048
#define LOG_FLUSH_MS   30000

static char    s_log_buf[LOG_BUF_SIZE];
static int     s_log_len;
static FILE   *s_log_f;
static char    s_log_path[96];       /* 当前日志文件全路径 */
static int     s_log_day = -1;      /* 当前文件对应 YYYYMMDD */
static int64_t s_log_flush_us;
static bool    s_selfcheck_done;    /* 回读自检只跑一次 */

static void log_selfcheck(void);

static void log_flush(void)
{
    bool had = false;

    if (!s_log_f) {
        s_log_len = 0;
        return;
    }
    if (s_log_len > 0) {
        size_t w = fwrite(s_log_buf, 1, (size_t)s_log_len, s_log_f);
        if (w != (size_t)s_log_len) {
            /* 卡写满 / 拔卡时会短写。不报出来就会静默丢数据 */
            ESP_LOGW(TAG, "写卡短写: 只写了 %u/%d 字节", (unsigned)w, s_log_len);
        }
        had = true;
        s_log_len = 0;
    }
    fflush(s_log_f);
    /* fsync 走 VFS -> FATFS 的 f_sync:连 FAT 表和目录项一起刷下去,
       这样突然断电最多只丢最后一小段,不会把文件系统弄脏 */
    fsync(fileno(s_log_f));
    s_log_flush_us = esp_timer_get_time();

    /* 头一回真写出东西后回读一遍验一下,以后不再跑 */
    if (had && !s_selfcheck_done) {
        s_selfcheck_done = true;
        log_selfcheck();
    }
}

/* 把文件尾巴读回来打串口,证明数据确实落在卡上了。只跑一次 */
static void log_selfcheck(void)
{
    char buf[288];
    long sz;
    size_t n, i;
    FILE *f;

    fflush(s_log_f);
    f = fopen(s_log_path, "r");
    if (!f) {
        ESP_LOGW(TAG, "自检: 打不开 %s", s_log_path);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    sz = ftell(f);
    if (fseek(f, (sz > (long)sizeof(buf) - 1) ? sz - ((long)sizeof(buf) - 1) : 0,
              SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    /* 把前面的半行掐掉 */
    i = 0;
    while (i < n && buf[i] != '\n') {
        i++;
    }
    ESP_LOGI(TAG, "自检: %s 共 %ld 字节,末尾回读如下", s_log_path, sz);
    char *p = buf + i + 1;
    while (p < buf + n) {
        char *nl = p;
        while (nl < buf + n && *nl != '\n' && *nl != '\r') {
            nl++;
        }
        char c = *nl;
        *nl = 0;
        if (*p) {
            ESP_LOGI(TAG, "  | %s", p);
        }
        if (c == 0) {
            break;
        }
        p = nl + 1;
    }
}

static void log_close(void)
{
    if (s_log_f) {
        log_flush();
        fclose(s_log_f);
        s_log_f = NULL;
    }
    s_log_day = -1;
}

/* 删掉超过保留天数的旧文件 */
static void log_cleanup(void)
{
    DIR *d = opendir(LOG_DIR);
    if (!d) {
        return;
    }
    time_t now = time(NULL);
    struct dirent *e;
    int removed = 0;

    while ((e = readdir(d)) != NULL) {
        int y, m, dd;
        if (sscanf(e->d_name, "sysmon_%4d%2d%2d.csv", &y, &m, &dd) != 3) {
            continue;
        }
        struct tm t;
        memset(&t, 0, sizeof(t));
        t.tm_year = y - 1900;
        t.tm_mon  = m - 1;
        t.tm_mday = dd;
        t.tm_hour = 12;                 /* 用正午算,避开时区边界 */
        t.tm_isdst = -1;
        time_t ft = mktime(&t);
        if (ft <= 0) {
            continue;
        }
        if (now - ft > (time_t)LOG_KEEP_DAYS * 86400) {
            /* 显式限长:GCC 的 -Werror=format-truncation 无法证明
               "%s/%s" 不溢出(前面加运行时长检查也拦不住),必须写精度 */
            char path[300];
            snprintf(path, sizeof(path), "%s/%.200s", LOG_DIR, e->d_name);
            if (unlink(path) == 0) {
                removed++;
            }
        }
    }
    closedir(d);
    if (removed) {
        ESP_LOGI(TAG, "清理了 %d 个超过 %d 天的旧日志", removed, LOG_KEEP_DAYS);
    }
}

/* 打开(或跨天切换)当天的日志文件。成功返回 true */
static bool log_open_day(void)
{
    struct tm ti;
    time_t now = time(NULL);
    localtime_r(&now, &ti);

    int ymd = (ti.tm_year + 1900) * 10000 + (ti.tm_mon + 1) * 100 + ti.tm_mday;
    if (s_log_f && ymd == s_log_day) {
        return true;
    }
    log_close();

    mkdir(LOG_DIR, 0777);               /* 已存在也无所谓 */

    snprintf(s_log_path, sizeof(s_log_path), LOG_DIR "/sysmon_%04d%02d%02d.csv",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);

    struct stat st;
    bool is_new = (stat(s_log_path, &st) != 0);

    s_log_f = fopen(s_log_path, "a");
    if (!s_log_f) {
        ESP_LOGW(TAG, "打不开日志文件 %s", s_log_path);
        return false;
    }
    if (is_new) {
        fprintf(s_log_f,
                "# ESP32-RLCD \u8d44\u6e90\u65e5\u5fd7  %04d-%02d-%02d\n",
                ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        /* 表头。注意百分号要写成 %% —— 在 printf 格式串里单个 % 是转换符,
           写成 "cpu%" 编译器会报 unknown conversion type character ',' */
        fprintf(s_log_f, "time,cpu%%,gpu%%,ram%%,cput_c,gput_c,down_kbps,up_kbps\n");
    }
    s_log_day     = ymd;
    s_log_flush_us = esp_timer_get_time();
    ESP_LOGI(TAG, "日志文件: %s%s", s_log_path,
             is_new ? "  (新建)" : "  (续写)");
    return true;
}

/* 每个采样点调一次。卡不在就安静地跳过 */
static void log_tick(const SysSample_t *s)
{
    struct tm ti;
    time_t now;

    if (!Sdcard_IsMounted()) {
        if (s_log_f) {
            log_close();
        }
        return;
    }

    /* 时间戳必须取同一个时钟:之前秒用 time(NULL)、
       小数用 esp_timer 的 100ms 计数,两者相位不一致,
       拼出来会出现 "10.9 后面跟 10.4" 这种倒退 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    now = tv.tv_sec;
    localtime_r(&now, &ti);

    if (!s_log_f) {
        if (!log_open_day()) {
            return;
        }
    } else {
        int ymd = (ti.tm_year + 1900) * 10000 + (ti.tm_mon + 1) * 100 + ti.tm_mday;
        if (ymd != s_log_day) {          /* 跨天了:清旧文件 + 换新文件 */
            log_cleanup();
            if (!log_open_day()) {
                return;
            }
        }
    }

    /* 缓冲快满了先落盘,腾地方 */
    if (s_log_len > LOG_BUF_SIZE - 64) {
        log_flush();
    }

    int n = snprintf(s_log_buf + s_log_len, sizeof(s_log_buf) - (size_t)s_log_len,
                     "%02d:%02d:%02d.%d,%u,%u,%u,%u,%u,%u,%u\n",
                     ti.tm_hour, ti.tm_min, ti.tm_sec,
                     (int)(tv.tv_usec / 100000),
                     s->cpu, s->gpu, s->ram, s->cput, s->gput,
                     /* uint32_t 在这个平台上是 long unsigned(不是 unsigned),
                        直接配 %u 会被 -Werror=format= 拦下来 */
                     (unsigned)s->down, (unsigned)s->up);
    if (n > 0 && n < LOG_BUF_SIZE - s_log_len) {
        s_log_len += n;
    }

    /* 没写满也要定期落盘,否则半小时不写卡的话掉电就丢一大截 */
    if (esp_timer_get_time() - s_log_flush_us > (int64_t)LOG_FLUSH_MS * 1000) {
        log_flush();
    }
}

/* ============================================================
 *  解析 "HOST=ZXS CPU=12 RAM=34 ..." 这样一行
 * ============================================================ */
static void parse_line(char *line)
{
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            break;
        }

        char key[16];
        int  klen = (int)(eq - p);
        if (klen <= 0 || klen >= (int)sizeof(key)) {
            break;
        }
        memcpy(key, p, (size_t)klen);
        key[klen] = '\0';

        char *v   = eq + 1;
        char *end = v;
        while (*end && *end != ' ' && *end != '\t' && *end != '\r') {
            end++;
        }
        char save = *end;
        *end = '\0';

        if      (!strcmp(key, "HOST"))  snprintf(s_st.host, sizeof(s_st.host), "%s", v);
        else if (!strcmp(key, "CPU"))   s_st.cpu  = atoi(v);
        else if (!strcmp(key, "RAM"))   s_st.ram  = atoi(v);
        else if (!strcmp(key, "RAMU"))  s_st.ram_used_mb  = (unsigned)atoi(v);
        else if (!strcmp(key, "RAMT"))  s_st.ram_total_mb = (unsigned)atoi(v);
        else if (!strcmp(key, "CPUT"))  s_st.cput = atoi(v);
        else if (!strcmp(key, "GPU"))   s_st.gpu  = atoi(v);
        else if (!strcmp(key, "GPUT"))  s_st.gput = atoi(v);
        else if (!strcmp(key, "VRAMU")) s_st.vram_used  = (unsigned)atoi(v);
        else if (!strcmp(key, "VRAMT")) s_st.vram_total = (unsigned)atoi(v);
        else if (!strcmp(key, "NETRX")) s_st.down_kbps = (unsigned)atoi(v);
        else if (!strcmp(key, "NETTX")) s_st.up_kbps   = (unsigned)atoi(v);

        *end = save;
        p    = end;
    }
}

/* ============================================================
 *  把一个采样点记进环形缓冲
 * ============================================================ */
static void record_sample(void)
{
    if (!s_hist || !s_lock) {
        return;
    }

    /* 实测采样间隔(平滑一下),用来给曲线页算时间跨度 */
    int64_t now = esp_timer_get_time();
    if (s_last_us) {
        int ms = (int)((now - s_last_us) / 1000);
        if (ms > 0 && ms < 10000) {
            s_interval_ms = s_interval_ms ? (s_interval_ms * 7 + ms) / 8 : ms;
        }
    }
    s_last_us = now;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    SysSample_t *d = &s_hist[s_head];
    d->cpu  = (uint8_t)(s_st.cpu  < 0 ? 0 : (s_st.cpu  > 100 ? 100 : s_st.cpu));
    d->gpu  = (uint8_t)(s_st.gpu  < 0 ? 0 : (s_st.gpu  > 100 ? 100 : s_st.gpu));
    d->ram  = (uint8_t)(s_st.ram  < 0 ? 0 : (s_st.ram  > 100 ? 100 : s_st.ram));
    d->cput = (uint8_t)(s_st.cput < 0 ? 0 : (s_st.cput > 120 ? 120 : s_st.cput));
    d->gput = (uint8_t)(s_st.gput < 0 ? 0 : (s_st.gput > 120 ? 120 : s_st.gput));
    d->down = s_st.down_kbps;
    d->up   = s_st.up_kbps;

    s_head = (s_head + 1) % s_cap;
    if (s_count < s_cap) {
        s_count++;
    }

    /* 存卡放到锁外做 —— 写卡可能阻塞几十毫秒到一两百毫秒,
       占着锁会让界面线程取历史数据时卡一下 */
    SysSample_t copy = *d;
    xSemaphoreGive(s_lock);
    log_tick(&copy);
}

/* ============================================================
 *  网络任务:监听 3334,收 PC 推来的行
 * ============================================================ */
static void SysmonTask(void *arg)
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
    addr.sin_port        = htons(SYSMON_PORT);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(srv, 1) != 0) {
        ESP_LOGE(TAG, "监听 %d 端口失败: errno=%d", SYSMON_PORT, errno);
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "资源遥测已就绪:监听 %d 端口,等 PC 连过来", SYSMON_PORT);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int fd = accept(srv, (struct sockaddr *)&cli, &clen);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        snprintf(s_peer, sizeof(s_peer), "%s", inet_ntoa(cli.sin_addr));
        s_connected = true;
        s_last_us   = 0;                 /* 重新建立采样间隔基线 */
        ESP_LOGI(TAG, "PC %s 已连接,开始接收资源数据", s_peer);

        /* 按行读。TCP 是字节流,一行可能被拆成几个包,
           也可能一次来好几行 —— 所以要自己攒缓冲找 '\n' */
        char buf[512];
        int  have = 0;

        for (;;) {
            int got = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
            if (got <= 0) {
                break;
            }
            have += got;

            char *start = buf;
            char *nl;
            while ((nl = memchr(start, '\n', (size_t)(buf + have - start))) != NULL) {
                *nl = '\0';
                parse_line(start);
                record_sample();
                start = nl + 1;
            }

            int rest = (int)(buf + have - start);
            if (rest > 0) {
                memmove(buf, start, (size_t)rest);
            }
            have = rest;
            /* 行太长(不该发生):丢掉,免得缓冲一直满着 */
            if (have >= (int)sizeof(buf) - 1) {
                have = 0;
            }
        }

        close(fd);
        s_connected = false;
        ESP_LOGW(TAG, "PC %s 断开", s_peer);
        log_close();          /* 把缓冲里剩下的写下去 */
    }
}

/* ============================================================
 *  对外接口
 * ============================================================ */
esp_err_t Sysmon_Start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_FAIL;
    }

    /* 历史缓冲放 PSRAM:281 KB 从内部 DRAM 出不起,PSRAM 随便放。
       ★ 实际容量存进 s_cap —— 退化分配时环形下标必须用 s_cap,
         用 SYSMON_HIST_MAX 会越界写。 */
    size_t bytes = (size_t)SYSMON_HIST_MAX * sizeof(SysSample_t);
    s_hist = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hist) {
        ESP_LOGW(TAG, "PSRAM 分配 %u 字节失败,历史长度减半", (unsigned)bytes);
        s_hist = heap_caps_malloc(bytes / 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hist) {
            ESP_LOGE(TAG, "历史缓冲分配失败,只能显示实时值");
            return ESP_FAIL;
        }
        s_cap = SYSMON_HIST_MAX / 2;
    }
    ESP_LOGI(TAG, "历史缓冲就绪: %d 个点 x %u 字节 = %u KB,放 PSRAM",
             s_cap, (unsigned)sizeof(SysSample_t),
             (unsigned)((size_t)s_cap * sizeof(SysSample_t) / 1024));

    /* 日志目录和旧文件清理 —— 卡没插就什么都不做 */
    if (Sdcard_IsMounted()) {
        mkdir(LOG_DIR, 0777);
        log_cleanup();
        ESP_LOGI(TAG, "资源日志目录 %s,保留 %d 天", LOG_DIR, LOG_KEEP_DAYS);
    } else {
        ESP_LOGW(TAG, "TF 卡没挂载,资源数据只留在内存(不存卡)");
    }

    /* 挂核 0:WiFi / lwIP 都在那边,收包路径短 */
    if (xTaskCreatePinnedToCore(SysmonTask, "sysmon", 6144, NULL, 4,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool Sysmon_IsConnected(void)
{
    return s_connected;
}

const char *Sysmon_GetPeer(void)
{
    return s_peer;
}

void Sysmon_GetStatus(SysStatus_t *out)
{
    if (!out) {
        return;
    }
    *out = s_st;
    out->connected = s_connected;
    out->hist_count = s_count;
    out->sample_ms  = s_interval_ms ? s_interval_ms : 500;
}

int Sysmon_GetDecimated(SysSample_t *out, int n)
{
    if (!s_hist || !s_lock || !out || n <= 0) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }

    int cnt = s_count;
    if (cnt <= 0) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    if (n > cnt) {
        n = cnt;
    }

    /* 最旧的点在环形缓冲里的位置 */
    int oldest = (s_head - cnt + s_cap) % s_cap;

    for (int i = 0; i < n; i++) {
        int a = (int)((int64_t)i * cnt / n);
        int b = (int)((int64_t)(i + 1) * cnt / n);
        if (b <= a) {
            b = a + 1;
        }
        if (b > cnt) {
            b = cnt;
        }

        unsigned sc = 0, sg = 0, sr = 0, t1 = 0, t2 = 0, k = 0;
        /* 网速用 64 位累加:单点最高 ~4e9,一段可能有 600 个点,
           32 位会溢出 */
        uint64_t d1 = 0, d2 = 0;
        for (int j = a; j < b; j++) {
            const SysSample_t *p = &s_hist[(oldest + j) % s_cap];
            sc += p->cpu;  sg += p->gpu;  sr += p->ram;
            t1 += p->cput; t2 += p->gput;
            d1 += p->down; d2 += p->up;
            k++;
        }
        if (!k) {
            k = 1;
        }
        out[i].cpu  = (uint8_t)(sc / k);
        out[i].gpu  = (uint8_t)(sg / k);
        out[i].ram  = (uint8_t)(sr / k);
        out[i].cput = (uint8_t)(t1 / k);
        out[i].gput = (uint8_t)(t2 / k);
        out[i].down = (uint32_t)(d1 / k);
        out[i].up   = (uint32_t)(d2 / k);
    }

    xSemaphoreGive(s_lock);
    return n;
}
