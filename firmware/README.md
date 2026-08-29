# 固件

硬件:M5Stack CoreS3(ESP32-S3,16MB flash / 8MB PSRAM,GC0308 DVP 摄像头,ILI9342C 320×240 触摸屏)。
框架:PlatformIO + ESP-IDF v5.5 + LVGL 9(CoreS3 BSP)+ esp32-camera。

接口与用例:[设备规范](../docs/02-device-api-v1.md)。交付:[硬件交付单](../docs/handoffs/hardware.md)。

## 构建与烧录

```bash
cd firmware
pio run -e cores3              # 编译
pio run -e cores3 -t upload    # 烧录 (USB-C, 必要时长按 RST 3 秒进下载模式)
pio device monitor -b 115200   # 串口日志
```

不要默认执行全盘擦除。分区布局变化时，先备份，再以本次构建生成的
`flash_args` 为准，单独初始化 NVS、OTA data、PHY 和应用分区。

BLE 配网手机端:App Store / Google Play 搜索 **"ESP BLE Provisioning"**
(Espressif 官方),或用串口打印的 QR JSON 扫码直连。

本地联调(不依赖云端): 见 [test/testplan.md](test/testplan.md) 环境准备节 ——
仓库内 server 以 demo 模式本地运行, `PVC_API_BASE` 指向局域网即可闭环。

联网配置在 [platformio.ini](platformio.ini) build_flags:

- `PVC_API_BASE` — API 环境(dev/prod 按构建目标区分)
- `PVC_WEB_BASE` — 配对二维码指向的 Web 地址
- `PVC_WIFI_SSID` / `PVC_WIFI_PASS` — 留空走 BLE 配网;填写则为开发直连后门

> 改过 `sdkconfig.defaults` 后需删除生成的 `sdkconfig.cores3` 再编译。

## 模块结构

```
main/
├── main.c               入口: 显示/SD/UI/相机/联网/省电 装配
├── app_camera.c         GC0308 DVP 驱动 (I2S DMA, PSRAM 双缓冲, 零拷贝)
├── hw2d.c               定点 2D 算子: LUT 滤镜/磨皮/缩放/混合 (含性能统计)
├── ui_beauty_camera.c   LVGL UI: 预览/滤镜/美颜/贴纸/相册/好友 feed 页/配对与配网引导
├── pvc_power.c          省电 (§6): 60s 无操作 deep sleep, 5 分钟静默轮询, 触摸唤醒
└── net/
    ├── pvc_net.c        状态机: WiFi -> 配对 -> 在线 (上传排空 + feed 轮询); 401 回配对
    ├── pvc_prov.c       BLE 配网 (wifi_provisioning + NimBLE, Security1/POP)
    ├── pvc_pair.c       §1 配对: 领码屏显 -> 3s 轮询 -> token 入 NVS
    ├── pvc_upload.c     §2 幂等上传: 同键重试, 退避 1s→4s→15s, SD 待传队列断电补传
    ├── pvc_feed.c       §3 好友动态: state -> etag feed -> 缓存 8 张 (SD 镜像离线可看), 点赞
    ├── pvc_http.c       HTTPS 封装: 证书 bundle, Bearer, 定长 body, 请求日志
    └── pvc_store.c      NVS: token / boot 计数 / feed etag; device_id = dvc_+MAC
```

## 串口日志(联调约定)

- 每次请求:`[FW] METHOD PATH STATUS MS`,例 `[FW] POST /photos 201 6320`
- 配网:`[FW] PROV ble name=PVC_xxxxxx pop=pvcxxxx` + QR JSON
- 入睡:`[FW] SLEEP reason=idle|poll_done|poll_timeout uptime_ms=… queue=… synced=…`
- 工程模式(长按屏幕「好友」键):`[FW] ENG token=xxxxxxxx.. state=… queue=… heap=…`

## 滤镜登记(规范 §5)

固件 `X-Filter-Id`:`none / fair / warm / cool / bw / vintage`(对应 UI 原图/白皙/暖阳/冷调/黑白/复古)。

## 自动化测试

埋点统一为 `[EV] <event> k=v ...`(清单见 [main/pvc_trace.h](main/pvc_trace.h)),
配合 `[FW]` 请求/睡眠日志构成完整判据。上板后:

```bash
pio device monitor -b 115200 | tee test_run.log     # 采集
python3 test/analyze_log.py test_run.log            # 分析 (FAIL 退出码 1, 可接 CI)
```

用例矩阵(T1 冷启动 … T14 无 SD 降级)与检查项(C1 崩溃检测 … C12 延迟统计)见
[test/testplan.md](test/testplan.md)。耐久测试看 `stat` 心跳的堆水位趋势(C11)。

## 真机待验证项

- 上传/下发两处 RGB565↔JPEG 字节序(`upload_photo_qvga` / `render_feed` 内有注释)
- 触摸唤醒 INT 引脚(默认 GPIO21,`-D PVC_TOUCH_INT_GPIO=xx` 覆盖)
- SD 卡挂载、BLE 配网、TLS 内存余量
