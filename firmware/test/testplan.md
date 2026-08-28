# 固件全功能全流程自动化测试计划

上板后执行。串口日志是唯一判据来源:全程用
`pio device monitor -b 115200 | tee test_<case>.log` 采集,
跑完(或跑的过程中)用 `python3 test/analyze_log.py test_*.log` 出报告。

埋点格式(`main/pvc_trace.h`):`[EV] <event> k=v ...`;
配合规范日志 `[FW] METHOD PATH STATUS MS`、`[FW] SLEEP ...`、`[FW] PROV ...`、`[FW] boot ...`。

## 环境准备

- dev server 或 mock(docs/02 §7);`PVC_API_BASE` 指向该环境后编译烧录
- 一台绑定了另一账号(好友)的 web 会话,用于触发 feed / 配置下发
- server 侧操作全部可用 curl 脚本化(见各用例"触发方式")

## 用例

| # | 用例 | 触发方式 | 期望日志(analyzer 检查) |
|---|------|---------|------------------------|
| T1 | 冷启动无异常 | 上电 | `[FW] boot quiet=0`;无 panic 类输出(C1);`[EV] net state=...` 序列合法 |
| T2 | BLE 配网 | 首次启动(或 `pvc_prov_reset` 后),手机 App 按屏显 POP 配网 | `[FW] PROV ble` → `[EV] wifi up`;错误密码一次 → `cred_fail` 后可重试 |
| T3 | 配对绑定 | web 输入屏显配对码 | `[EV] pair code=` → `[EV] pair bound` → `state=online`(C3) |
| T4 | 拍照上传闭环 | 点快门 ×3 | 每张:`photo captured`→`photo encoded`(≤100KB)→`upload queued`→`upload sent status=201 attempt=0`;web 可见(C4) |
| T5 | 断网补传不重复 | 拍 2 张 → 关 AP 30s → 恢复 | 断网期 `upload defer`;恢复后同 key `upload sent status=200/201`;**同 key 成功仅一次**(C4/C5) |
| T6 | 限额 429 | server 把日限额调低后连拍超额 | `[FW] POST /photos 429`,日志含按 retry_after 等待,无 drop(C6 白名单 429) |
| T7 | 解绑回配对 | web 解绑设备 | 下一次请求 401 → `state=pairing` → 屏显新配对码(C3 二次出现) |
| T8 | feed 下发显示 | 好友账号上传一张 | ≤5min(或点「好友」立即)`feed poll ... new=1 complete=1`;进「好友」页可见;再轮询 `not_modified=1`(304, C7) |
| T9 | 点赞 | 好友页点「点赞」 | `react send ... status=201`;**当前 server 未修设备 token 时为 401 → analyzer 记 WARN 不判失败**(C8) |
| T10 | 配置下发回执 | web `POST /device/config` | `config recv id=` → toast/滤镜生效 → `config ack id= status=204`;web 查 state 见 active_config(C9) |
| T11 | 静默轮询周期 | 无操作 60s 入睡后等定时唤醒 | `[FW] SLEEP reason=idle` → 5min 后 `[FW] boot quiet=1` → 无亮屏 → `SLEEP reason=poll_done uptime_ms<45000`(C10) |
| T12 | 静默期触摸唤醒 | quiet 窗口内触屏 | `quiet=1` boot 后出现触摸 → 亮屏恢复 UI,不误入睡 |
| T13 | 耐久 2h | 脚本每 5min 好友上传一张;设备自然睡醒循环 | `stat` 心跳 heap 无持续下滑(C11);无 panic;所有 upload/feed 闭环 |
| T14 | 无 SD 降级 | 拔 SD 上电 | boot 报 sdcard mount failed;拍照 `upload_queued store=ram` 仍上传成功;相册不可用不崩溃 |
| T15 | 性能基准 | 预览静置 60s → 逐个切 6 种滤镜各 10s → 开贴纸 10s → 连拍 5 张 → 好友页翻 8 张 | `perf_preview` 帧率红线(C13):目标 25fps,均值 <20 WARN、<10 FAIL;`perf_photo`/`perf_encode` 快门→入队 avg<3s(C14);`perf_feed_decode` 单张 <500ms;滤镜/贴纸开启前后帧率差记录在报告里 |

## 真机专项验证(非自动判定,人工看一次)

- 上传照片在 web 端颜色正确(RGB565→JPEG 字节序;错了改 `upload_photo_qvga` 交换逻辑)
- 好友照片在设备屏上颜色正确(JPEG→RGB565 字节序;错了改 `render_feed`)
- 触摸能从 deep sleep 唤醒(不能则核对 INT 引脚,`-D PVC_TOUCH_INT_GPIO=` 覆盖)
- BLE 配网 App 全流程;配网完成后 BT 内存已释放(heap 回升)

## 已知豁免(analyzer 内置)

- `react send status=401`:server reactions 路由暂不认设备 token(已提全栈)→ WARN
- Prisma 生产模式下 `/device/state` 401:server 侧待修;demo 模式联调不受影响
