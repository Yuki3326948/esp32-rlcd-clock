/*
 * wifi_cfg_parse.h 的 PC 端测试 —— 用的是【和固件完全同一份】解析代码。
 *
 * 为什么要这么绕:
 *   板子上没法方便地往 TF 卡里塞文件来试,而"读到 wifi.txt 并解析成功"
 *   恰好是给朋友那版固件唯一要走的路 —— 不测不放心。
 *
 * 编译运行:
 *   $gcc -O1 -Wall -Wextra -I "L:\ESP32\projects\ESP32-S3-RLCD-4.2\02_Example\ESP-IDF\11_U8G2_Test\components\user_app" -o wifi_cfg_parse_test.exe wifi_cfg_parse_test.c
 *   .\wifi_cfg_parse_test.exe
 *
 * 返回值:0 = 全过,1 = 有失败
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi_cfg_parse.h"

#define TMP "wifi_cfg_test_tmp.txt"

static int g_pass = 0;
static int g_fail = 0;

static void Put(const char *s)
{
    FILE *f = fopen(TMP, "wb");
    if (!f) { printf("!! 打不开临时文件\n"); exit(2); }
    fwrite(s, 1, strlen(s), f);
    fclose(f);
}

static void Check(const char *name, int want_ret,
                  const char *want_ssid, const char *want_pass)
{
    char ssid[WIFI_CFG_SSID_MAX];
    char pass[WIFI_CFG_PASS_MAX];
    int  r, ok;

    /* 填个哨兵值:解析失败时【本来就不该】动这两个缓冲。
       不初始化的话打印出来是栈上的垃圾,看着像乱码。 */
    strcpy(ssid, "(未动)");
    strcpy(pass, "(未动)");

    r  = WifiCfgParse(TMP, ssid, sizeof(ssid), pass, sizeof(pass));
    ok = (r == want_ret);

    if (ok && want_ssid) ok = (strcmp(ssid, want_ssid) == 0);
    if (ok && want_pass) ok = (strcmp(pass, want_pass) == 0);

    printf("  %-32s ret=%-3d ssid=%-16s pass=%-12s %s\n",
           name, r, ssid[0] ? ssid : "-", pass[0] ? pass : "-",
           ok ? "OK" : "<<<< FAIL >>>>");

    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("      want: ret=%d ssid=%s pass=%s\n",
               want_ret, want_ssid ? want_ssid : "-",
               want_pass ? want_pass : "-");
    }
}

int main(void)
{
    char ssid[WIFI_CFG_SSID_MAX];
    char pass[WIFI_CFG_PASS_MAX];
    int  r;

    printf("=== wifi.txt parser test ===\n\n");

    Put("ssid=MIKU\npass=abc123\n");
    Check("LF", 1, "MIKU", "abc123");

    Put("ssid=MIKU\r\npass=abc123\r\n");
    Check("CRLF (windows notepad)", 1, "MIKU", "abc123");

    Put("\xEF\xBB\xBFssid=MIKU\r\npass=abc123\r\n");
    Check("UTF-8 BOM (notepad default)", 1, "MIKU", "abc123");

    Put("SSID=Home\r\nPASSWORD=pw\r\n");
    Check("uppercase keys", 1, "Home", "pw");

    Put("# comment\r\n\r\n  ssid = My Wifi  \r\n  pass =  abc  \r\n");
    Check("comment / blank / spaces", 1, "My Wifi", "abc");

    /* 中文 SSID:UTF-8 字节。用转义写,免得受源码编码影响 */
    Put("ssid=\xE6\x88\x91\xE7\x9A\x84WiFi\r\npass=\xE5\xAF\x86\xE7\xA0\x81\r\n");
    Check("chinese SSID (utf-8)", 1,
          "\xE6\x88\x91\xE7\x9A\x84WiFi", "\xE5\xAF\x86\xE7\xA0\x81");

    Put("#ssid=commented-out\npass=onlypass\n");
    Check("ssid commented out -> -1", -1, NULL, NULL);

    Put("pass=onlypass\n");
    Check("no ssid -> -1", -1, NULL, NULL);

    Put("ssid=\r\npass=abc\r\n");
    Check("empty ssid value -> -1", -1, NULL, NULL);

    Put("");
    Check("empty file -> -1", -1, NULL, NULL);

    Put("ssid=ThisIsAVeryLongSSIDNameThatExceeds32BytesForSure\r\npass=x\r\n");
    Check("SSID >32 bytes -> truncated", 1,
          "ThisIsAVeryLongSSIDNameThatExcee", "x");

    remove(TMP);

    /* ---------- 回归测试:失败时绝不能动调用者的变量 ----------
       踩过的坑:解析器原来一进来就 ssid[0]='\0',于是"卡里没这个文件"
       这种正常情况也会把默认凭据清空 —— 屏幕显示成 no wifi,
       而且 WiFi 断开后再也重连不上。 */
    strcpy(ssid, "KEEP-ME");
    strcpy(pass, "KEEP-PW");
    r = WifiCfgParse("no_such_file_here.txt", ssid, sizeof(ssid), pass, sizeof(pass));
    {
        int ok = (r == 0) && strcmp(ssid, "KEEP-ME") == 0 &&
                 strcmp(pass, "KEEP-PW") == 0;
        printf("  %-32s ret=%-3d ssid=%-16s %s\n", "file not found -> 0",
               r, ssid, ok ? "OK" : "<<<< FAIL >>>>");
        if (ok) g_pass++; else g_fail++;
    }

    strcpy(ssid, "KEEP-ME");
    strcpy(pass, "KEEP-PW");
    Put("pass=onlypass\n");
    r = WifiCfgParse(TMP, ssid, sizeof(ssid), pass, sizeof(pass));
    {
        int ok = (r == -1) && strcmp(ssid, "KEEP-ME") == 0 &&
                 strcmp(pass, "KEEP-PW") == 0;
        printf("  %-32s ret=%-3d ssid=%-16s %s\n", "no ssid -> keep caller vars",
               r, ssid, ok ? "OK" : "<<<< FAIL >>>>");
        if (ok) g_pass++; else g_fail++;
    }
    remove(TMP);

    printf("\npassed %d, failed %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
