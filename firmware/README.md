# firmware — Presence Card 固件

权威规范：`../docs/02-device-api-v1.md`（§6 实现 checklist / §7 验收用例是验收标准）。

## 构建与烧录

```bash
pio run             # 构建（CI 对每个 PR 也构建并产出 firmware.bin artifact）
pio run -t upload   # 本机烧录
pio device monitor  # 串口日志（格式约定见 docs/04 §5）
```

## 板型

`platformio.ini` 默认 `esp32-s3-devkitc-1`；按实际 M5Stack 板型（CAMS3 Lite / CoreS3 SE / AtomS3）修改 `board`。

## 给技术一号位（本地同款设备复现 CI 产物）

从 GitHub Actions 的 `firmware-bin` artifact 下载 `firmware.bin`：

```bash
# 首次需用 pio run -t upload 全量烧录（含 bootloader）；此后验证 CI 产物只刷 app 即可：
esptool.py --chip esp32s3 --port /dev/cu.usbmodem* write_flash 0x10000 firmware.bin
```
