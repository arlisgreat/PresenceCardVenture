#include "pvc_ota_util.h"

#include <string.h>

/* 解析一个数字段, 前进到 '.' 或后缀/结尾。溢出钳位。 */
static long field(const char **p)
{
    long v = 0;
    while (**p >= '0' && **p <= '9') {
        if (v < 1000000) v = v * 10 + (**p - '0');
        (*p)++;
    }
    if (**p == '.') (*p)++;
    return v;
}

int pvc_semver_cmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    for (int i = 0; i < 3; i++) {
        long va = field(&a), vb = field(&b);
        if (va != vb) return va < vb ? -1 : 1;
    }
    /* 数字段相同: 跳到后缀 ('-' 起, 或残留非法字符也视作后缀) */
    int sa = (*a != '\0'), sb = (*b != '\0');
    if (sa != sb) return sa ? -1 : 1;      /* 有后缀 = 预发布, 更小 */
    if (!sa) return 0;
    int c = strcmp(a, b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int pvc_md5_hex_parse(const char *hex, uint8_t out[16])
{
    if (!hex || strlen(hex) != 32) return -1;
    for (int i = 0; i < 16; i++) {
        int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
