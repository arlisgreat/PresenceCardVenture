#include <Arduino.h>

// Presence Card 固件骨架
// 权威规范: docs/02-device-api-v1.md（§6 实现 checklist / §7 验收用例）
// 串口日志格式（docs/04-kickoff-playbook.md）：
//   [FW] METHOD PATH STATUS MS        例: [FW] POST /v1/photos 201 6320

#ifndef PVC_API_BASE
#define PVC_API_BASE "https://api-dev.example.com/v1"
#endif
#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("[FW] boot presence-card fw=%s\n", FW_VERSION);
  Serial.printf("[FW] api_base=%s\n", PVC_API_BASE);

  // TODO(硬件): 相机初始化 —— 320x240 JPEG, quality≈12, ≤100KB (docs/02 §2)
  // TODO(硬件): 屏幕初始化 + UI 状态机（屏稿由吉吉交付）
  // TODO(硬件): 配网 → 配对流程 docs/02 §1
  // TODO(硬件): NVS 读写 device_token / etag / 待传队列
}

void loop() {
  delay(1000);
}
