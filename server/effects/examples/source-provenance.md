# 滤镜测试原图

`source.png` 是内置 `image_gen` 生成的虚构成人合成素材，仅用于确定性滤镜 QA；不是小卡实拍，也不是多人合照模型的身份保真评测。

- 生成日期：2026-08-27（America/Los_Angeles）。未提供真人参考图，未调用项目的外部付费 API。
- 原图：1448 × 1086（4:3），PNG，2,132,374 bytes；直接复制工具产物，未做后处理。
- SHA-256：`1fc89139d87e4ff326029aad96fb7378f3386cf23ed49d03b07e6616a8926850`。
- 工具产物 ID：`exec-8b4c7e6c-7674-43a0-ae23-c76a6ed0b663.png`。
- 目视检查：两名虚构成人、可见皮肤纹理、红蓝杯、浅色背景、无文字边框；不代表真实拍摄色彩标定。

## 原始提示词

```text
Use case: photorealistic-natural.
Generate ONE original synthetic photographic reference image for deterministic filter QA, not a camera capture or an identity-model benchmark.
Format: landscape, exact 4:3 aspect ratio.
Scene: a simple off-white and sage room, soft diffuse neutral window daylight, with a small red object and a small blue object visibly included to test color separation.
Subjects: exactly two clearly adult fictional East Asian friends, one woman and one man, both around 25 years old. They have natural distinct faces, unretouched textured medium skin, and natural small smiles.
Composition: a realistic casual documentary snapshot, medium shot, both faces sufficiently large and clearly visible.
Rendering: photorealistic, clean neutral documentary camera exposure, natural skin pores and everyday texture.
Constraints: no logos, no text, no decorative borders, no grain, no color grading, no beautification, no skin smoothing, no fantasy or stylized AI effects.
```
