/*
 * wifi.txt 的解析器。
 *
 * 为什么单独抽成一个只依赖 <stdio.h> / <string.h> 的头文件:
 *   板子上没法方便地往 TF 卡里塞文件来试这条路径,所以把解析逻辑抽出来,
 *   让 PC 上用【同一份代码】跑测试(见 L:\ESP32\tools\wifi_cfg_parse_test.c)。
 *
 * 文件格式 —— 一行一条,# 开头算注释,key 大小写不敏感:
 *
 *     # 我家的网
 *     ssid=我的WiFi
 *     pass=密码
 *
 * 返回值:
 *     1 = 解析到有效的 ssid(ssid/pass 已填好)
 *     0 = 文件打不开(卡里压根没这个文件)
 *    -1 = 文件在,但里面没有能用的 ssid=
 *
 * ★ 重要:返回 0 或 -1 时【绝不会】修改传入的 ssid/pass。
 *   这一点很关键 —— 调用者传进来的往往是自己的默认凭据,
 *   要是失败时把它清空了,屏幕会显示成 no wifi,WiFi 断了也再也重连不上。
 *
 * 长度上限按 802.11 来:SSID 32 字节、WPA2 密码 64 字节,都留 '\0' 的位置。
 */
#ifndef WIFI_CFG_PARSE_H
#define WIFI_CFG_PARSE_H

#include <stdio.h>
#include <string.h>

#define WIFI_CFG_SSID_MAX 33
#define WIFI_CFG_PASS_MAX 65

static int WifiCfgParse(const char *path,
                        char *ssid, size_t ssid_n,
                        char *pass, size_t pass_n)
{
    char  line[132];
    char  got_ssid[WIFI_CFG_SSID_MAX];
    char  got_pass[WIFI_CFG_PASS_MAX];
    int   found = 0;
    FILE *f;

    /* ★ 先解析到本地缓冲,确认成功之后才回写调用者的变量。
       这里踩过坑:原来一进来就 ssid[0]='\0',于是"卡里没这个文件"
       这种再正常不过的情况也会把调用者的默认凭据抹掉 ——
       屏幕变成 no wifi,而且 WiFi 断了就再也重连不上。
       回归测试见 L:\ESP32\tools\wifi_cfg_parse_test.c。 */
    got_ssid[0] = '\0';
    got_pass[0] = '\0';

    f = fopen(path, "r");
    if (!f) return 0;

    while (fgets(line, (int)sizeof(line), f)) {
        char        key[16];
        char       *p  = line;
        char       *end;
        const char *v  = line;
        int         kn = 0;
        int         eq = 0;

        /* ★ 最容易踩的坑:记事本存 UTF-8 会加 BOM(EF BB BF)。
           不认它的话第一行就成了 "\xEF\xBB\xBFssid=..." 而永远匹配不上。 */
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) {
            p += 3;
        }

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#' || *p == '\r' || *p == '\n') continue;

        /* 掐掉行尾的换行和空白 */
        end = p + strlen(p);
        while (end > p && (end[-1] == '\r' || end[-1] == '\n' ||
                           end[-1] == ' '  || end[-1] == '\t')) {
            *--end = '\0';
        }

        /* 取出 "=" 前面那段,统一成小写 —— 这样 ssid= / SSID= 都认。
           自己转小写是为了不引入 <ctype.h>/<strings.h>,少一个依赖。 */
        while (kn < (int)sizeof(key) - 1 && p[kn] != '\0' && p[kn] != '=') {
            char c = p[kn];
            key[kn++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        key[kn] = '\0';
        if (p[kn] != '=') continue;         /* 不是 key=value 的行,跳过 */
        eq = kn;                            /* 记住 '=' 的位置 */

        /* 容错:有人会写 "ssid = xxx"。key 尾巴上的空格去掉,
           '=' 后面的空格也跳过 —— 值的位置得用 eq 算,
           不能用去掉空格后的 kn(否则会跑到 '=' 上)。 */
        while (kn > 0 && (key[kn - 1] == ' ' || key[kn - 1] == '\t')) {
            key[--kn] = '\0';
        }
        v = p + eq + 1;
        while (*v == ' ' || *v == '\t') v++;

        if (strcmp(key, "ssid") == 0) {
            strncpy(got_ssid, v, sizeof(got_ssid) - 1);
            got_ssid[sizeof(got_ssid) - 1] = '\0';
            if (got_ssid[0] != '\0') found = 1;
        } else if (strcmp(key, "pass") == 0 || strcmp(key, "password") == 0) {
            strncpy(got_pass, v, sizeof(got_pass) - 1);
            got_pass[sizeof(got_pass) - 1] = '\0';
        }
    }
    fclose(f);

    if (!found) return -1;

    /* 到这里才动调用者的变量 */
    if (ssid_n) {
        strncpy(ssid, got_ssid, ssid_n - 1);
        ssid[ssid_n - 1] = '\0';
    }
    if (pass_n) {
        strncpy(pass, got_pass, pass_n - 1);
        pass[pass_n - 1] = '\0';
    }
    return 1;
}

#endif /* WIFI_CFG_PARSE_H */
