#include "pvc_face.h"
#include "pvc_trace.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "human_face_detect.hpp"

static const char *TAG = "pvc_face";

#define FACE_W 320
#define FACE_H 240
#define FACE_STALE_US (2 * 1000 * 1000)

static HumanFaceDetect *s_detect;
static uint16_t *s_frame;
static SemaphoreHandle_t s_go;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static pvc_face_box_t s_latest;
static int64_t s_latest_us;
static volatile bool s_busy;

static void face_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_go, portMAX_DELAY) != pdTRUE) continue;
        int64_t t0 = esp_timer_get_time();

        dl::image::img_t img = {};
        img.data = s_frame;
        img.width = FACE_W;
        img.height = FACE_H;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

        auto &results = s_detect->run(img);

        pvc_face_box_t best = {};
        int n = 0, best_area = 0;
        for (const auto &r : results) {
            n++;
            if (r.box.size() < 4) continue;
            int w = r.box[2] - r.box[0];
            int h = r.box[3] - r.box[1];
            if (w * h > best_area) {
                best_area = w * h;
                best.valid = true;
                best.x = r.box[0];
                best.y = r.box[1];
                best.w = w;
                best.h = h;
            }
        }
        taskENTER_CRITICAL(&s_mux);
        s_latest = best;
        s_latest_us = esp_timer_get_time();
        taskEXIT_CRITICAL(&s_mux);
        PVC_EV("perf_face ms=%d faces=%d",
               (int)((esp_timer_get_time() - t0) / 1000), n);
        s_busy = false;
    }
}

extern "C" bool pvc_face_init(void)
{
    if (s_detect) return true;
    s_frame = (uint16_t *)heap_caps_malloc(FACE_W * FACE_H * 2,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_go = xSemaphoreCreateBinary();
    if (!s_frame || !s_go) return false;
    s_detect = new (std::nothrow) HumanFaceDetect();
    if (!s_detect) {
        ESP_LOGE(TAG, "model load failed");
        return false;
    }
    /* core1 prio 2: 低于拍照 worker(3), 不与其抢占关键路径 */
    if (xTaskCreatePinnedToCore(face_task, "pvc_face", 12288, NULL, 2, NULL, 1)
        != pdPASS) {
        return false;
    }
    PVC_EV("face_init ok=1");
    return true;
}

extern "C" void pvc_face_submit(const uint16_t *qvga)
{
    if (!s_detect || s_busy) return;      /* 检测中丢帧, 永不阻塞预览 */
    s_busy = true;
    memcpy(s_frame, qvga, FACE_W * FACE_H * 2);
    xSemaphoreGive(s_go);
}

extern "C" bool pvc_face_latest(pvc_face_box_t *out)
{
    pvc_face_box_t b;
    int64_t ts;
    taskENTER_CRITICAL(&s_mux);
    b = s_latest;
    ts = s_latest_us;
    taskEXIT_CRITICAL(&s_mux);
    if (!b.valid || esp_timer_get_time() - ts > FACE_STALE_US) return false;
    *out = b;
    return true;
}
