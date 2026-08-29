# 效果分层与选型

核验日期：2026-08-27。用途：统一算法、吉吉、硬件、全栈的效果选取；以下是官方能力与工程建议，不是实测排名。模型权限、地域、价格以开通时为准。

## 选取

- 原创滤镜独立于生成模型：轻颗粒、纸白、浅灰绿、少量柔红／蓝；自然肤色，不默认磨皮、美白、瘦脸。
- AI 合照：`qwen-image-3.0-pro` 为国内候选；`gpt-image-2-2026-04-21` 做对照。主力最终由同素材验收确定，不因“最新”直接判优。
- 风格参考 Quiet Color / Product Story / Indie Magazine；Dazz 只参考质感，不复制名称、素材、LUT 或界面。
- 失败保留原图，可提供明确标注的双图拼贴；不把拼贴、原图或演示素材冒充 AI 合照，不自动切换照片接收方。

## Tiny / Live / AI

| 层级 | 内容 | 执行与对接 |
|---|---|---|
| Tiny Effects | 淡色调、轻颗粒、小贴纸、固定相框 | 小卡 RGB565；算法给色表与混合规则，吉吉给原创素材，硬件验收 |
| Live Effects | 随脸移动的图案、轮廓框、轻量面部装饰 | 手机 Web；算法给跟踪／绘制，吉吉给纹理与锚点，全栈接相机与交互 |
| AI Effects | 好友合照、场景共创 | 云端异步任务；双方授权后调用，保存生成标记，再提供 Web／小卡衍生图 |

- QVGA `320×240`：RGB565 单帧 `153,600 bytes`（150 KiB），RGBA 单帧 300 KiB；完整 RGB565→RGB565 色表 128 KiB／款，均为容量计算，不是运行内存总量。
- 色表只表达逐像素颜色映射；颗粒、暗角、贴纸需独立处理。RGB565 无透明通道，贴纸需另带遮罩；明确大小端、方向、边界及混合顺序。
- 原创纹理先导出 PNG／遮罩与混合模式，再量化为小卡资源；PSD、桌面效果工程不能直接刷入固件。小卡性能、色阶与内存仍待实机验收，不承诺 FPS。
- Live 候选为 MediaPipe Face Landmarker：Web 包提供关键点、表情系数及变换矩阵；检测会同步阻塞线程，宜放 Worker。多脸模式的平滑行为与单脸不同；帧率、弱光、遮挡与手机兼容待验收，不承诺 ESP32-S3 运行同模型。[官方 Web 指南](https://developers.google.com/edge/mediapipe/solutions/vision/face_landmarker/web_js)
- 默认浏览器本地跟踪；未确认保存／上传前不发送相机帧。云端合照先取得双方授权，照片不足清晰度时提示补手机原图，不靠生成补猜身份。

## 图像模型对比

价格为单次标准调用口径，不含税、网络／存储、额外搜索、重试及废片；币种不换算。输入图上限不等于可靠保留同样数量的人物。

| 家族／模型 | 已核验能力与版本 | 标价口径 | 本项目位置 |
|---|---|---|---|
| Qwen `qwen-image-3.0-pro`／`qwen-image-3.0` | 已提供生成／编辑 API；1–3 参考图，1–6 输出 | 北京 1K：Pro 输出 ¥0.25，标准 ¥0.18；输入 ¥0.02／张。双参考单输出分别 ¥0.29／¥0.22 | Pro 国内候选；标准版做成本对照 |
| OpenAI `gpt-image-2`，固定快照 `gpt-image-2-2026-04-21` | 已提供生成／编辑 API；最多 16 参考图；自动高保真，不传 `input_fidelity` | 1024² 输出：low $0.006、medium $0.053、high $0.211；另加输入文本／图像 token | medium 作为身份与编辑能力对照 |
| Google `gemini-3.1-flash-image`／`gemini-3-pro-image` | 稳定版；最多 14 参考图，官方分别描述最多 4／5 人物参考一致性 | 标准 1K 输出分别约 $0.067／$0.134；另加输入及文本／思考输出 | 满足地域与年龄条件后再对照 |
| BytePlus `dola-seedream-5-0-pro-260628` | Seedream 5.0 Pro 已提供 API；最多 10 参考图；支持标注编辑、图层分解 | 输出 ≤2.61 MP $0.045，超过 $0.09；首张参考免费，后续 $0.003／张；双参考 1K 为 $0.048 | 复杂编辑／分层候选，非国内服务默认配置 |
| BFL `flux-2-pro`／`flux-2-max` | 固定版本；Pro 的 `flux-2-pro-preview` 为滚动预览；API 最多 8 参考图 | 编辑起价 Pro $0.045、Max $0.07；随分辨率等变化，不是多参考固定总价 | 默认不接真实社群人像，先解决数据条款 |

能力与价格来源：

- Qwen：[API](https://help.aliyun.com/zh/model-studio/qwen-image-generation-and-editing-api-reference)、[价格](https://help.aliyun.com/zh/model-studio/model-pricing)。中文文档已更新 3.0；不要用旧英文 2.0 页面替代。
- OpenAI：[模型](https://developers.openai.com/api/docs/models/gpt-image-2)、[图像指南与计价](https://developers.openai.com/api/docs/guides/image-generation)、[编辑 API](https://developers.openai.com/api/reference/resources/images)。
- Google：[图像指南](https://ai.google.dev/gemini-api/docs/image-generation)、[稳定版模型](https://ai.google.dev/gemini-api/docs/models/gemini-3.1-flash-image)、[价格](https://ai.google.dev/gemini-api/docs/pricing)、[弃用记录](https://ai.google.dev/gemini-api/docs/deprecations)。
- BytePlus：[Seedream 概览](https://docs.byteplus.com/en/docs/ModelArk/1541523)、[5.0 Pro](https://docs.byteplus.com/en/docs/ModelArk/2582774)、[价格](https://docs.byteplus.com/en/docs/ModelArk/1824121)、[图像 API](https://docs.byteplus.com/en/docs/ModelArk/1544106)。
- BFL：[FLUX.2](https://docs.bfl.ai/flux_2/flux2_overview)、[多图编辑](https://docs.bfl.ai/flux_2/flux2_image_editing)、[价格](https://docs.bfl.ai/quick_start/pricing)。

补充：Gemini `gemini-3.1-flash-lite-image` 1K 输出约 $0.0336，但不能沿用 Flash／Pro 的人物一致性承诺。FLUX.2 klein 4B 权重为 Apache 2.0，9B／dev 有非商用限制；自托管需独立 GPU 资源，不是小卡能力。

## 调用边界

- Qwen：北京 `POST https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation`；原 `dashscope.aliyuncs.com` 仍支持。模型、Key、endpoint 须同地域。
- Qwen 请求：`input.messages[0].content` 中按顺序放图与一个文本；图片可用 data URI。单图上限 10 MB，建议边长 384–2048；低分辨率告警不是恢复身份的保证。
- 品牌基线建议 `n=1`、`size="1024*1024"`、`prompt_extend=false`、`watermark=true`；关闭改写是可控性选择，不代表质量更优。`prompt_extend_mode=agent` 不支持图生图。
- Qwen 结果从 `output.choices[].message.content[].image` 取 URL，24 小时有效；后端尽快存私有对象存储，签名 URL 不写日志、不当永久地址。
- OpenAI：`POST https://api.openai.com/v1/images/edits`，multipart `image[]` 加固定模型、prompt、size、quality；输出取 `data[].b64_json`。固定快照便于复测，不代表逐次像素相同。
- Google／BytePlus／BFL 若后续接入，分别按官方 Interactions／`images/generations`／任务轮询协议写独立适配器；不混用不同供应商、地域或产品的账号与条款。
- 上云前解码验证、限制字节／像素、去 EXIF/GPS；Key 仅服务端。任务幂等、双方权限复核、取消、删除及错误信息脱敏由业务接线保证。
- 已提交请求的超时／断流不能判定未计费；下载失败不要重新提交生成。所有额外尝试计入成本，并区分“可恢复”与“可安全自动重试”。

## 数据与使用条件

- 阿里云称 API 数据不用于训练，但存在调用数据存储；不等于零保留。模型广场体验与商业 API 条款不同，不能把体验页结果默认用于商用。[隐私](https://help.aliyun.com/zh/model-studio/privacy-notice)、[体验服务说明](https://help.aliyun.com/zh/model-studio/bailian-service-notes)
- OpenAI API 默认不训练；滥用监测日志通常最多保留 30 天，零数据保留需资格与配置。账号地域、组织验证及模型权限另核验。[数据控制](https://developers.openai.com/api/docs/guides/your-data)
- Gemini 付费 API 数据不用于改进产品；仍有防滥用保留。开发者须年满 18 岁，客户端不得面向或可能被未满 18 岁者使用；开发者及终端用户须在支持地域，中国大陆不在列表。海外服务器不能替代资格审查。[条款](https://ai.google.dev/gemini-api/terms)、[地域](https://ai.google.dev/gemini-api/docs/available-regions)
- BytePlus 的数据说明不授权任意训练，但被内容过滤命中的输入／输出可能在马来西亚保留 180 天；不能将其条款直接套用到火山引擎中国区。[数据处理](https://docs.byteplus.com/en/docs/modelark/BytePlus_ModelArk_Data_Processing)
- BFL API 条款 §2(b) 包含输入／输出训练用途；开发者条款另有限制未成年人图像、保留来源凭证等要求。没有适用的不同书面约定前，不作为私密人像默认供应商。[API 条款](https://bfl.ai/legal/flux-api-service-terms)、[开发者条款](https://bfl.ai/legal/developer-terms-of-service)
- 商用许可不替代肖像、素材、商标授权；任何模型都不保证身份完全不变。原始生成文件保留来源信息；去 EXIF/GPS 与删除 AI 标识不是同一件事。衍生 JPEG 可能不保留嵌入凭证，须保留生成记录及用户可见 AI 标记，不移除供应商要求保留的标识。

## 用户参考与素材选择

| 参考 | 可借鉴内容 | 使用边界 |
|---|---|---|
| [Fotor AI Group Photo](https://www.fotor.com/ai-image-generator/group-photo/) | 1–4 图输入；2–4 图合照、场景与拍立得方向 | 云端产品体验参考，不是 Tiny 算法；本次未核验可接入项目的公开合照 API |
| [Effect House Face Mask](https://effecthouse.tiktok.com/learn/guides/workspace/assets/downloadable-assets/2d-face-mask) | 整脸、眼、睫毛、唇的分层纹理与绑定思路 | 官方模板／运行时面向 TikTok，不直接复制到自家 Web／ESP32 |
| [Freepik Overlay PSD](https://www.freepik.com/psd/overlay) | 颗粒、纸张、漏光的视觉方向 | 当前转向 Magnific；PSD 是设计资源，不是固件。普通商用／免署名不等于社群自动合成、素材库再分发许可 |
| [itch.io](https://itch.io/docs/legal/terms) | 小图案、贴纸、像素资源 | 逐包核对商用、修改、嵌入、导出及再分发；免费不等于 CC0 |

- Fotor 允许 AI 生成图片商用，但模板／图库资源许可不同，禁止的外用或转授权不因付费消失。[商用说明](https://support.fotor.com/hc/en-us/articles/900006654446-Commercial-Use)、[条款](https://support.fotor.com/hc/en-us/articles/900006549786-Fotor-Terms-of-Service)
- Effect House 条款 §6／§9 限制内置版权内容外用，软件授权仅用于 TikTok 平台相关效果；本项目使用独立原创素材与实现。[条款](https://effecthouse.tiktok.com/learn/guides/support/terms-of-service)
- Freepik／Magnific 条款 §8.1 限制集合分发及转授权；第三方使用例外要求人工定制，不能据此开放自动 overlay 服务。[条款](https://www.magnific.com/legal/terms-of-use)
- itch 作者条款有实质差异：[Kenney Splat Pack](https://kenney-assets.itch.io/splat-pack) 明确 CC0；[Yutami 示例许可](https://itch.io/blog/929708/general-paid-asset-license) 虽可商用，却禁止自动为第三方生成衍生作品。仅作许可范例，不代表视觉选中。
- 因此吉吉交付原创贴纸、面框、透明纹理及锚点；算法交付色表、颗粒／混合规则。保留源稿、版本和来源说明，不把受限素材提交公共仓库。

## 待验收

- 本次未调用项目付费图像 API。示例原图由内置 image_gen 生成虚构成人，只用于确定性滤镜 QA；不是 CoreS3 实拍、身份保真或五家模型对比结果。[素材说明](../../server/effects/examples/source-provenance.md)
- 对比使用同一批双方已授权参考图、同场景提示词、相近输出规格、固定预算与重复次数；保存所有尝试，不只挑最好一张。
- 覆盖手机／QVGA、不同肤色、眼镜、弱光、侧脸、双人及遮挡；人物本人确认身份，再盲评自然肤色、皮肤纹理、人数／手部正确、场景遵从与品牌一致性。
- 分别记录原始生成结果与品牌后处理结果，避免把滤镜改善归因于模型；固定模型、参数、提示词版本及输入摘要。
- 延迟报告用户提交到可显示结果的 P50／P95，包含排队、生成、下载、后处理与重试；失败率、超时率和样本数单列。文档宣传延迟不作 SLA。
- 成本按全部尝试实际账单计算；每张验收通过成本 = 全部费用 ÷ 验收通过张数，零通过时不伪造有限成本；免费额度不用于推算长期成本。
- 硬件验收 RGB565 字节序、色阶／肤色、贴纸边缘、内存与拍照上传；Web 验收跟踪、相机授权／关闭、取消／删除和移动端性能。真实质量、延迟、单位成功成本及完整联调均待验收。
