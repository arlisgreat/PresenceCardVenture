/*
 * pvc_sound.h - 提示音 (CoreS3 板载喇叭, AW88298 经 esp_codec_dev)
 *
 * 独立小任务串行播放, pvc_sound_play 非阻塞可从任意任务调用。
 * 喇叭初始化失败只告警一次并静音, 不影响主流程。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PVC_SND_SHUTTER = 0,   /* 快门咔嚓 (双短促脉冲) */
    PVC_SND_DING,          /* 好友照片到达 (清脆下落双音) */
    PVC_SND_LIKE,          /* 点赞/被赞 (上扬滑音) */
} pvc_sound_id_t;

void pvc_sound_init(void);
void pvc_sound_play(pvc_sound_id_t id);

#ifdef __cplusplus
}
#endif
