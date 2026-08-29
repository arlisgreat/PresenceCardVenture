/*
 * pvc_ota_util.h - OTA 纯逻辑辅助 (无 ESP 依赖, host 可测)
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 语义版本比较: "x.y.z" 或 "x.y.z-suffix"。
 * 数字段逐级比较; 数字段相同时无后缀 > 有后缀 (0.2.0 > 0.2.0-dev),
 * 双后缀按 strcmp。非法/缺失数字段按 0。返回 <0 / 0 / >0。
 */
int pvc_semver_cmp(const char *a, const char *b);

/* 32 位十六进制 -> 16 字节 MD5。大小写均可。成功 0, 非法 -1。 */
int pvc_md5_hex_parse(const char *hex, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
