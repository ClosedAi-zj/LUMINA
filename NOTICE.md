# NOTICE — 第三方数据与许可声明（M2 T2.11 / N16）

## 镜头 profile 数据来源与许可

本仓库 `engine/data/lens-profiles/*.json` 为镜头光学校正数据（畸变 / 横向色差 /
暗角），来源分两类，以每个 JSON 的 `source` 字段为准：

| source | 含义 | 许可 | 义务 |
|---|---|---|---|
| `self-built` | 自建 profile（人工标定 / 架构文档示例值） | 本项目自有 | 无 |
| `lensfun-db(CC-BY-SA-3.0)` | 由 lensfun 数据库经 `engine/scripts/tools/lensfun2lumina.py` 离线转换 | **CC-BY-SA 3.0**（仅数据；lensfun 代码 LGPL-3.0 **未被链接**） | 保留署名 + 相同方式共享；本文件及安装包许可页保留本节 |

lensfun 项目：https://lensfun.github.io/ —— 其镜头校正如实归功于 lensfun
贡献者社区。依据 CC-BY-SA 3.0，凡包含 `source=lensfun-db(CC-BY-SA-3.0)` 数据
的发行物须同样以 CC-BY-SA 3.0 提供该数据文件。

## L4 合规声明（R1 红线）：**本仓库不含 Adobe LCP / DCP 数据**

- 运行时依赖清单中**没有** Adobe Lens Profile（.lcp）或 DNG Camera Profile
  （.dcp）相关组件；镜头校正仅依赖上表自建 JSON 与 lensfun 转换数据。
- 二进制与资源中**不得出现** LCP/DCP 文件或其派生内容（验收 L4 的扫描项：
  `*.lcp` / `*.dcp` 文件存在性 + 数据目录 `source` 字段白名单）。
- 若未来引入 lensfun 转换数据，须保留 `attribution` 字段并同步本文件。
