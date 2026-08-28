# 算法交付

用途：按 Tiny / Live / AI Effects 接入。分支 `codex/algorithm`；基于全栈 `0b110b7`，不改 `main` 或全栈分支。

[选型与参考](research.md) · [验证结果](evaluation.md) · [滤镜对比](../../server/effects/examples/preview/comparison.png)

| 层级 | 已实现 | 对接 |
|---|---|---|
| Tiny Effects | 5 款色表、RGB565 C/C++ 查表、透明贴纸混合与导出 | 硬件接帧缓冲；吉吉给原创 PNG |
| Live Effects | 同款完整调色、脸颊星星／星环、MediaPipe Worker 跟踪、丢脸隐藏 | 全栈接相机／Canvas；吉吉验视觉 |
| AI Effects | Qwen 3.0 / GPT Image 2 双图适配、3 场景合照、授权复核、私有草稿、发布／取消 | 全栈接真实授权库、任务持久化与部署 |

## 运行

使用 Node 24；命令在 `server/effects` 执行。输出目录须是新目录，避免覆盖照片。

```sh
npm ci
npm test
npm run render -- --input examples/source.png --out output/review --all
npm run luts -- --out output/luts
npm run live-assets
npm run live-smoke
```

`npm ci --prefix server/effects` 后再安装／构建 API。Docker 构建上下文已改为 `server`；未执行公网部署。

## Tiny：原片默认，效果可关闭

| `filter_id` | 名称 | 质感 |
|---|---|---|
| `none` | 原片 | 不调色 |
| `warm` | 日光纸片 | 暖高光、轻颗粒 |
| `bw` | 银盐黑白 | 黑白层次、细颗粒 |
| `film` | 即时相纸 | 轻褪色、柔黑位 |
| `vivid` | 傍晚数码 | 雾蓝暗部、轻对比 |

- 色表为原创；不使用 Dazz 私有 LUT。17³ RGB565 表每款 9,826 bytes；最近邻会量化，不含颗粒／暗角／光晕。`none` 与强度 0 直接旁路。
- 生成的 `.h` 自带 `presence_warm_rgb565_lut_apply_rgb565(pixel)`；逐像素调用，无堆分配。输入是数值 RGB565，读写帧时由固件明确大小端。
- 原创贴纸用 `npm run overlay -- --input sticker.png --out output/sticker --symbol presence_star` 导出；调用 `hardware/presence_overlay.h`，颜色与 alpha 分开，支持裁切。
- CoreS3 Lite 的 GC0308 是 CMOS、RGB565 输入；HTTP 上传仍是 JPEG，需要固件软件编码。旧设备文档中的“相机直出 JPEG”不能照做；帧率由硬件实测。

## Live：手机／Web，本地处理

```js
import { applyLookToRgba } from '@pvc/effects/pixels';
import { createLiveTracker } from '@pvc/effects/mediapipe';
import { buildFaceOverlays, renderFaceOverlays } from '@pvc/effects/live';

const tracker = await createLiveTracker({
  wasmRoot: '/effects-assets/wasm',
  modelAssetPath: '/effects-assets/face_landmarker.task',
});
// 获得用户相机许可后，每帧交出一个 ImageBitmap；只保留一个在途帧。
const result = await tracker.detect(bitmap, performance.now());
if (result.status === 'ok') {
  // 先重画当前视频帧，再画效果；无脸返回空命令，不保留旧贴纸。
  renderFaceOverlays(ctx, buildFaceOverlays(result.landmarks, {
    width, height, trackingAccepted: result.trackingAccepted, style: 'cheek-stars',
  }));
}
// 页面退出：tracker.close(); 同时由全栈停止 camera MediaStream tracks。
```

- `live-assets` 下载固定模型与 SDK 1.0.1 WASM；把 `assets/live/` 挂载到同源 `/effects-assets/`，随附许可与哈希。不默认依赖 CDN。
- Vite 接入需 `worker: { format: 'es' }`；默认 IIFE 不支持 SDK 分块。`live-smoke` 用合成样片测试真实模型，不开相机。
- Worker 负责模型推理；Canvas 负责绘制。限帧／忙碌返回时跳过更新，丢脸时清空；不上传相机帧，不识别人脸身份，不假造置信分。
- SDK 自带遥测已在专用 Worker 中拦截；仅允许配置的同源模型／WASM 读取，不改用户浏览器或系统网络设置。
- 预览可用共享 `applyLookToRgba`。空间效果依赖分辨率，不承诺小卡 LUT 与 Web 逐像素相同。手机跟踪体验、遮挡与真实帧率仍需验收。

## AI：双人自然合照

- 场景：窗边／散步／咖啡店。保留人物与肤色，不默认换脸、美白、磨皮；这属于提示约束，不是质量保证。
- 输入：两张不同、已授权的原图；单张适配器限制 8 MiB，短边小于 128 拒绝，小于 384 告警，不自动放大补猜五官。
- 输出：保留模型原始字节；`warm / 0.6` 后处理，输出 Web JPEG 与带 AI 标识的 320×240 JPEG。可用 `none / 0` 做原始模型评测。
- Qwen 默认 `qwen-image-3.0-pro`；OpenAI 默认 `gpt-image-2-2026-04-21`。只选一家，不自动跨厂商传照片，不自动重试可能已计费的请求。
- 配置见 [`.env.example`](../../server/effects/.env.example)。密钥只放服务器环境，不进浏览器／Git／聊天。

独立验收：`npm run generate -- --first a.jpg --second b.jpg --out output/ai-review` 默认只校验、打印哈希与提示词，不上传。实际调用另加 `--authorization /private/grants.json --execute`；授权文件不入库，由服务端核验所有者同意后提供，两张素材各一条：

```json
{"materials":[{"sha256":"原图哈希","ownerId":"素材所有者","approved":true,"provider":"qwen","model":"qwen-image-3.0-pro","purpose":"generate","expiresAt":"有效期的 ISO 时间"}]}
```

此文件只是运维验收输入，不是面向用户的授权机制。命令不会自动读取 `.env`；先由部署环境注入配置。

现有 `buildApp({ imageProvider, authorizeAiMaterial })` 可直接注入：

```ts
const imageProvider = createImageProviderFromEnv();
await buildApp({ imageProvider, authorizeAiMaterial: async ctx => {
  // 查真实授权记录：ownerId/materialId/actorId/provider/model/purpose，校验有效期及撤销。
  // purpose 分 generate / publish；必须由素材所有者授权，不能信任发起者 consent:true。
  return consentRepository.isAllowed(ctx);
} });
```

`consentRepository` 是全栈需提供的持久化授权库，不是内置服务。缺回调时他人素材拒绝。沿用 `/v1/ai/jobs`、状态／发布／删除及 `/v1/photos/:id/image`，未新增外部端点。

- 队列为进程内：最多 16 个待处理、默认 2 个模型并发；同 job 在途／终态去重。正式部署换持久化队列，不以请求重发实现恢复。
- 生产模式若仍用演示账号，AI 入口直接拒绝，避免仅配置密钥就暴露付费能力；健康页通过也不能代替真实联调。
- 草稿仅本人可读，不进入动态／足迹／消息／互动；发布再验授权，强制 `AI 合照 · ` 标识。取消会终止等待并清理迟到结果，但不保证供应商撤销计费。
- 设备 token 或已有 `?size=320` 下载基线 JPEG，≤100 KiB；Web 无参数保留高清。不重复套滤镜，不裁切人脸。

## 接线前确认

1. 全栈现有浏览器上传先调色，不能当作未处理原图。接新流程时保留原图，只处理一次；旧固定 RGB 偏移不要再叠加。
2. Tiny / Live / AI 是效果分层，不直接替换现有 `play_type` 枚举；统一协议变动另行确认。
3. 全栈补真实账号、授权记录、任务／图片持久化与公网地址；硬件验 JPEG 解码／字节序；吉吉验样片和原创贴纸。
4. 真模型密钥与授权样本就绪后再验身份、质量、耗时和实际费用；当前不称“AI 效果已验收”。
