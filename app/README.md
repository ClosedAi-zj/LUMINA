# LUMINA 桌面 UI（Electron + Vite + React + TypeScript）

风光/建筑摄影师一站式工作台的前端工程。本目录只包含 **UI 层**（渲染进程 + Electron 主进程壳）；
引擎为独立 C++ 进程（`build/engine/lumina-engine.exe`），经 stdio JSON-RPC 2.0 通信，
契约以 `docs/ipc-protocol.md` v1.0.1 为准（95 方法 / 16 域 / 7 事件 / 24 错误码）。

## 1. 实际版本事实（2026-10-23 核对 node_modules）

| 组件 | package.json 声明 | 实际安装 | 说明 |
|---|---|---|---|
| vite | `^5.4.8` | **5.4.21** | 架构文档 §7 指定 Vite 5.x，未偏离 |
| @vitejs/plugin-react | `^4.3.1` | 4.x | React Fast Refresh（dev） |
| tailwindcss | `^3.4.13` | **3.4.19** | 架构文档 §7 指定 3.4.13；caret 范围内小版本漂移，**未使用 Tailwind 4** |
| @tailwindcss/vite | — | **未使用** | 该插件是 Tailwind 4 的 Vite 集成；本项目走 PostCSS 通道（`postcss.config.js` + `tailwind.config.ts`） |
| postcss / autoprefixer | `^8.4` / `^10.4` | 8.x / 10.x | Tailwind 3 的标准链路 |
| esbuild | `^0.24.0` | 0.24.x | 仅用于打包 Electron 主进程 |
| electron | `^33.0.0` | 33.x | 见 `scripts/dev.ps1` 的镜像安装方式 |
| react / react-dom | `^18.3.1` | 18.3.x | |
| react-window | `^1.8.10` | 1.8.x | 素材库 18,192 张虚拟滚动 |
| zustand | `^4.5.5` | 4.5.x | 全局 store |
| typescript | `^5.5.4` | 5.5.x | `npm run typecheck` 零错误为准入 |

> 澄清：本工程**没有**升级到 Vite 8 / Tailwind 4.1 / `@tailwindcss/vite`。
> 若在其他分支看到此类声明，以本表与 `node_modules` 实际安装为准。

## 2. 设计令牌（单一来源，不可违反）

- 唯一样式来源：`src/theme/tokens.css`（`design-tokens.css` 的完整副本 + 少量派生令牌）。
- `tailwind.config.ts` 把所有颜色映射到 `var(--Lumina-Tokens-*)`；组件类名只允许引用令牌派生的 utility。
- **禁止**在组件里硬编码十六进制颜色；唯一例外是照片观察区（查看器/地图/延时画面）固定 `#08090B`
  （`--Lumina-Tokens-viewer`，深浅主题下同值，红线 R5）。
- 深浅主题：根元素 `data-theme="Light"` 切换，令牌变量整体换值（`ThemeProvider` 持久化到
  localStorage 并同步 `system.setSettings`）。

## 3. 构建

```bash
npm install                       # 首次；electron 二进制若缺失见 scripts/dev.ps1 的镜像处理
npm run typecheck                 # tsc --noEmit，必须 0 错误
npm run build                     # ① vite build → ../build/app  ② esbuild → ../build/app/electron/*.cjs
npm start                         # electron .（main 指向 ../build/app/electron/main.cjs）
```

产物布局（`D:/LUMINA/build/app/`）：`index.html` + `assets/`（渲染进程）、`electron/main.cjs` +
`electron/preload.cjs`（主进程）。

### 3.1 主进程打包为何关闭 minify（`--minify` 不传）

- esbuild 0.24 在 **Node 22（win32-x64）** 下对主进程 bundle 开 `--minify` 会触发内部
  transform 崩溃（minify 阶段的已知兼容问题，非本项目代码问题）。
- 主进程产物只在本机磁盘运行，压缩收益≈0，却损失堆栈可读性；因此 `build:electron`
  **不传 `--minify`**。渲染进程由 Vite 默认压缩（esbuild minify，renderer 侧无此崩溃）。
- 若未来升级 esbuild ≥0.25 并升级 Node LTS 后想开启：在 `build:electron` 脚本追加
  `--minify` 即可，无其他改动点。

### 3.2 Windows junction 约定

`build/app/electron/main.cjs` 运行时 `require('electron')` 需要解析到 `app/node_modules`。
工程约定：`D:/LUMINA/build/app/node_modules` 是指向 `D:/LUMINA/app/node_modules` 的 **junction**
（`New-Item -ItemType Junction`）。`npm run build` 不会创建它；重新生成 build 目录后如提示
`Cannot find module 'electron'`，按上述命令重建 junction 即可。

### 3.3 升级注意事项（给后续维护者）

1. **升 Tailwind 4 前**：必须先评估 `@theme` CSS-first 迁移成本——当前 22 个令牌全部经
   `var(--Lumina-Tokens-*)` 暴露，Tailwind 4 的 `@theme inline` 可以无损承接，但
   `tailwind.config.ts` 的颜色映射需整体改写为 CSS 变量引用；`postcss.config.js` 可删除、
   改挂 `@tailwindcss/vite`。升级属破坏性变更，需重新走一遍 DoD（见下）。
2. **升 Vite 6/7+**：`base: './'` 与 `outDir: '../build/app'`、`emptyOutDir: false` 三个设置
   是 Electron file:// 加载与双产物共存的关键，不要改；升版本后重点回归冒烟截图。
3. **升 Electron**：`registerSchemesAsPrivileged` 的 `lumina-file:` 自定义协议依赖
   `protocol.handle`（Electron 25+ API），大版本升级时优先验证缩略图/预览图加载。
4. **协议版本**：`src/api/protocol.ts` 的 `PROTOCOL_VERSION` 必须与引擎侧 `engine/src/rpc/version.h`
   一致（CI 校验）；任何字段增改先改 `docs/ipc-protocol.md` 再同步实现。

## 4. Mock 模式（引擎 exe 缺失时自动降级）

- `electron/engine-bridge.ts` spawn `build/engine/lumina-engine.exe`；不存在时动态导入
  `electron/mock/mock-engine.ts` 的 `MockTransport`（与真实传输同接口，UI 无感）。
- Mock 全量实现协议 95 方法 + 7 事件：18,192 张模拟照片（RW2 16,470 / JPG 1,387 / DNG 273 /
  ARW 15 / MP4 45 / PSD 1 / PNG 1；缺失 421；已编辑 4,347），确定性 PRNG（seed=20261022），
  分页/搜索/筛选/ facets /时间线/文件夹树全部可用；缩略图与预览为程序化生成的真实落盘文件。
- `mock.debug.simulateExternalEdit` / `mock.debug.simulateCorruption` 是 **Mock 专属调试方法**
  （非协议方法，仅 `capabilities.mock=true` 时 UI 展示入口），用于验证 1005 冲突与 4002 隔离链路。
- 冒烟验证：`LUMINA_SMOKE=1` 启动（禁用硬件加速，渲染进程 console 转发主进程 stdout），
  `src/smoke.ts` 依次驱动各模块并输出 `[smoke-scenario]` 证据行。

## 5. 完成定义（DoD）

1. `npm install` 成功；
2. `npx tsc --noEmit` 0 错误；
3. `npm run build` 产出 `D:/LUMINA/build/app/`（renderer + electron）；
4. Electron 启动并正常渲染（含 mock 模式）；
5. 主题切换无残色，viewer 恒 `#08090B`；
6. 冒烟场景全链路通过（素材库/修图台/导入/健康/协议契约）。
