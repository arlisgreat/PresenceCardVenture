#include "pvc_clock.h"
#include "pvc_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/m5stack_core_s3.h"    /* BSP_I2C_NUM */

static const char *TAG = "pvc_clock";

#define BM8563_ADDR      0x51
#define REG_SECONDS      0x02        /* VL_seconds..years 共 7 字节, BCD */
#define VALID_EPOCH      1704067200  /* 2024-01-01: 早于此视为系统时间无效 */
#define I2C_TIMEOUT_MS   100

static i2c_master_dev_handle_t s_rtc;
static bool s_rtc_written;           /* 本次启动是否已把 SNTP 时间写回 RTC */

static uint8_t dec2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int     bcd2dec(uint8_t v) { return ((v >> 4) & 0x0F) * 10 + (v & 0x0F); }

static bool rtc_dev(void)
{
    if (s_rtc) return true;
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(BSP_I2C_NUM, &bus) != ESP_OK || !bus) {
        return false;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BM8563_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_rtc) == ESP_OK;
}

/* 读 BM8563 -> struct tm (本地时间)。VL 位置位 = 曾掉电, 时间不可信。 */
static bool rtc_read(struct tm *t)
{
    uint8_t reg = REG_SECONDS, d[7];
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, d, sizeof(d),
                                    I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    if (d[0] & 0x80) return false;              /* VL: 电压跌落, 无效 */
    memset(t, 0, sizeof(*t));
    t->tm_sec  = bcd2dec(d[0] & 0x7F);
    t->tm_min  = bcd2dec(d[1] & 0x7F);
    t->tm_hour = bcd2dec(d[2] & 0x3F);
    t->tm_mday = bcd2dec(d[3] & 0x3F);
    t->tm_wday = bcd2dec(d[4] & 0x07);
    t->tm_mon  = bcd2dec(d[5] & 0x1F) - 1;
    t->tm_year = bcd2dec(d[6]) + 100;           /* 存 2000 起两位年 */
    t->tm_isdst = 0;
    return t->tm_year >= 124;                   /* >=2024 才可信 */
}

static bool rtc_write(const struct tm *t)
{
    uint8_t buf[8] = {
        REG_SECONDS,
        dec2bcd(t->tm_sec), dec2bcd(t->tm_min), dec2bcd(t->tm_hour),
        dec2bcd(t->tm_mday), dec2bcd(t->tm_wday),
        dec2bcd(t->tm_mon + 1), dec2bcd(t->tm_year - 100),
    };
    return i2c_master_transmit(s_rtc, buf, sizeof(buf), I2C_TIMEOUT_MS) == ESP_OK;
}

bool pvc_clock_valid(void)
{
    return time(NULL) >= VALID_EPOCH;
}

void pvc_clock_init(void)
{
    setenv("TZ", "CST-8", 1);       /* 固定东八区; RTC 亦存本地时间 */
    tzset();

    if (pvc_clock_valid()) {
        /* deep sleep 唤醒: 内部 RTC 已保持系统时间, 无需外部恢复 */
        PVC_EV("clock src=internal");
        return;
    }
    struct tm t;
    if (rtc_dev() && rtc_read(&t)) {
        struct timeval tv = { .tv_sec = mktime(&t) };
        settimeofday(&tv, NULL);
        PVC_EV("clock src=bm8563 valid=%d", (int)pvc_clock_valid());
    } else {
        PVC_EV("clock src=none");   /* 等 SNTP */
    }
}

void pvc_clock_tick(void)
{
    if (s_rtc_written || !pvc_clock_valid()) return;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    bool ok = rtc_dev() && rtc_write(&t);
    s_rtc_written = true;            /* 每次启动写一次即可 (含失败不重试) */
    PVC_EV("clock sntp_ok=1 rtc_write=%d", (int)ok);
    if (!ok) ESP_LOGW(TAG, "BM8563 write failed (RTC absent?)");
}

void pvc_clock_hhmm(char *buf, size_t cap)
{
    if (!pvc_clock_valid()) {
        snprintf(buf, cap, "--:--");
        return;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, cap, "%H:%M", &t);
}
