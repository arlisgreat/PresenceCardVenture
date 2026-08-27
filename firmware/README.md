# 固件

硬件：M5Stack CoreS3 Lite。板型配置由硬件验证：[platformio.ini](platformio.ini)。

接口与用例：[设备规范](../docs/02-device-api-v1.md)。交付：[硬件交付单](../docs/handoffs/hardware.md)。

在 `firmware/` 运行：

```bash
pio run
pio run -t upload
pio device monitor
```

交付固件版本、烧录说明和真机记录；日志格式：`[FW] METHOD PATH STATUS MS`。
