# 分支与分工

`main` 保存统一协议与集成版本；各角色在独立分支工作，通过 PR 合并。

| 角色 | 分支 | 主责 | 交付对接 |
|------|------|------|----------|
| 全栈 | `codex/fullstack` | Web、业务 API、云端部署 | [交付单](handoffs/fullstack.md) |
| 硬件 | `codex/hardware` | 固件、联网、拍照与显示、整机 | [交付单](handoffs/hardware.md) |
| 算法 | `codex/algorithm` | 滤镜、image API、AI 合照与配置 | [交付单](handoffs/algorithm.md) |
| 吉吉 | `codex/jiji` | 效果图、图标及全部 UI 设计 | [交付单](handoffs/jiji.md) |
| xana | `codex/xana` | 路演功能、小卡外观方案（3D 打印外壳） | [交付单](handoffs/xana.md) |

分支不限制权限，不单独定义接口。[需求](00-project-plan.md) · [联调验收](04-kickoff-playbook.md)
