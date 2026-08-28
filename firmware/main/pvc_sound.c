#include "pvc_sound.h"
#include "pvc_trace.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_codec_dev.h"

static const char *TAG = "pvc_sound";

#define SND_RATE   16000
#define SND_MAX_MS 260
#define SND_VOL    75

static QueueHandle_t s_q;
static esp_codec_dev_handle_t s_spk;
static bool s_dead;                      /* 初始化失败后静音 */
static int16_t s_pcm[SND_RATE * SND_MAX_MS / 1000];   /* ~8.3KB .bss */

/* 简易合成: 正弦片段 (freq Hz, ms 时长, 指数衰减), 追加进 s_pcm */
static size_t synth_tone(size_t off, float freq, int ms, float amp, float decay)
{
    size_t n = (size_t)(SND_RATE * ms / 1000);
    if (off + n > sizeof(s_pcm) / sizeof(s_pcm[0])) return off;
    for (size_t i = 0; i < n; i++) {
        float env = amp * expf(-decay * (float)i / SND_RATE);
        s_pcm[off + i] = (int16_t)(env * 12000.0f *
                                   sinf(2.0f * (float)M_PI * freq * (float)i / SND_RATE));
    }
    return off + n;
}

static size_t synth_silence(size_t off, int ms)
{
    size_t n = (size_t)(SND_RATE * ms / 1000);
    if (off + n > sizeof(s_pcm) / sizeof(s_pcm[0])) return off;
    memset(&s_pcm[off], 0, n * 2);
    return off + n;
}

static size_t synth(pvc_sound_id_t id)
{
    size_t off = 0;
    switch (id) {
    case PVC_SND_SHUTTER:               /* 双短促咔嗒 */
        off = synth_tone(off, 2200, 18, 1.0f, 60.0f);
        off = synth_silence(off, 50);
        off = synth_tone(off, 1400, 22, 0.9f, 50.0f);
        break;
    case PVC_SND_DING:                  /* 到达: G6 -> C6 下落双音 */
        off = synth_tone(off, 1568, 110, 0.9f, 12.0f);
        off = synth_tone(off, 1046, 140, 0.8f, 10.0f);
        break;
    case PVC_SND_LIKE:                  /* 上扬滑音 660->990Hz */
        for (int seg = 0; seg < 6; seg++) {
            off = synth_tone(off, 660.0f + 55.0f * seg, 20, 0.8f, 8.0f);
        }
        break;
    }
    return off;
}

static void sound_task(void *arg)
{
    (void)arg;
    pvc_sound_id_t id;
    for (;;) {
        if (xQueueReceive(s_q, &id, portMAX_DELAY) != pdTRUE) continue;
        if (s_dead) continue;

        if (!s_spk) {                    /* 懒初始化 (首次播放时) */
            s_spk = bsp_audio_codec_speaker_init();
            if (!s_spk) {
                ESP_LOGW(TAG, "speaker init failed, muted");
                s_dead = true;
                continue;
            }
            esp_codec_dev_set_out_vol(s_spk, SND_VOL);
        }
        size_t n = synth(id);
        if (!n) continue;
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = SND_RATE,
            .channel = 1,
            .bits_per_sample = 16,
        };
        if (esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK) {
            esp_codec_dev_write(s_spk, s_pcm, (int)(n * 2));
            esp_codec_dev_close(s_spk);
        }
    }
}

void pvc_sound_init(void)
{
    if (s_q) return;
    s_q = xQueueCreate(4, sizeof(pvc_sound_id_t));
    if (s_q) xTaskCreatePinnedToCore(sound_task, "pvc_snd", 3072, NULL, 3, NULL, 0);
}

void pvc_sound_play(pvc_sound_id_t id)
{
    PVC_EV("sound id=%d", (int)id);
    if (s_q) xQueueSend(s_q, &id, 0);    /* 队满丢弃, 不阻塞调用方 */
}
