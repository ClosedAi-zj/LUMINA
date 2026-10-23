# LUMINA

风光 / 建筑摄影师一站式工作台 —— C++17 引擎 + Electron/React UI 混合架构。

- 编辑参数以开放 XMP sidecar 存储，原片零改写（不锁私有数据库）
- RAW 解码走 LibRaw；检索走 SQLite FTS5；预览走 GLSL（无 GPU 时 CPU 软渲染接管）

## 目录结构

| 目录 | 内容 |
|---|---|
| `engine/` | C++ 引擎：LibRaw RAW 解码、SQLite FTS5、XMP sidecar 编辑、导入/健康扫描、导出（JPG/PNG/TIFF）、RPC 服务 |
| `app/` | Electron UI：素材库、修图台（快捷/专业双模式）、精选、地图、延时、作品集、Web 客户画廊 |
| `scripts/` | 构建、门槛测试与验收脚本（RW2 实测 / 零改写 / 5 万性能 / 8 小时压测等） |
| `docs/` | PRD、架构设计、IPC 协议、发布门槛报告 |
| `thirdparty/` | LibRaw / SQLite / stb 源码（构建产物不入库） |

## 构建

- 引擎：`scripts/build-engine.ps1`（详见 `engine/README.md`）
- UI：`app/` 下 `npm install && npm run build`（详见 `app/README.md`）

## 状态

v1.1.0 RC：门槛验收进行中，见 `docs/release-gate-1.1.0.md`。
