# LUMINA 编译指南（从零到发布物）

> 本文是唯一完整的构建说明。根 `README.md` 只有速查命令；引擎/UI 的细节分别在
> `engine/README.md` / `app/README.md`，本文把整条流水线串起来。
> 适用平台：**Windows**（引擎用 MinGW-w64 编译；安装器为 NSIS，仅 Windows）。
> 文中所有命令在工程根 `D:/LUMINA` 下执行（PowerShell）。

---

## 0. 总览：四条命令

| 阶段 | 命令 | 产物 |
|---|---|---|
| ① 引擎 | `powershell -File scripts/build-engine.ps1` | `build/engine/lumina-engine.exe` |
| ② UI | `cd app; npm install; npm run typecheck; npm run build` | `build/app/` |
| ③ 打包 | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-app.ps1 -Zip` | `build/pkg/LUMINA/` + `LUMINA-1.1.0-portable.zip` |
| ④ 安装器 | `makensis scripts/installer.nsi` | `build/pkg/LUMINA-1.1.0-setup.exe` |

② 内部 = `vite build`（渲染进程）+ `esbuild`（Electron 主进程），一条 `npm run build` 全含。
③ 内部会先跑 ②（除非加 `-SkipBuild`），并把 Electron runtime + renderer + 引擎组装成
双击可跑的目录。④ 只打包 ③ 的产物，需先装 NSIS。

---

## 1. 前置工具链

| 工具 | 本机实测版本 | 本机路径 | 备注 |
|---|---|---|---|
| MinGW-w64 g++ | 8.1.0（x86_64-posix-sjlj） | `D:/work/tools/mingw_posix/mingw64/bin` | 引擎唯一编译器；产物静态链接 libgcc/libstdc++，无运行时 DLL 依赖 |
| CMake | 4.4.4 | `C:/Users/zj/.workbuddy/binaries/python/versions/3.13.12/Scripts` | 与 Ninja 同在受管 Python 的 Scripts 目录 |
| Ninja | 1.13.2 | 同上 | `-G Ninja` 生成器 |
| Node.js | 22.22.2 | `C:/Users/zj/.workbuddy/binaries/node/versions/22.22.2-6` | UI 构建与 Electron 运行；**Node 22 是硬约束**（esbuild 0.24 在 Node 22 下主进程 minify 会崩，见 §7） |
| Python | 3.13 | 受管版本 | 只用于跑 `scripts/*.py` 验证脚本，编译本身不需要 |
| NSIS | 3.13 | — | 只做安装器需要：`winget install NSIS.NSIS` |

**⚠️ 路径机器相关**：以上绝对路径写在 `scripts/env.ps1` 里（引擎构建前会被 `build-engine.ps1`
自动加载）。换机器编译有两种做法：把工具装到相同路径，或直接改 `env.ps1` 第 4 行的 PATH
指向本机实际的 g++/cmake/ninja 位置。脚本启动时会打印实际解析到的三个工具路径，可据此核对。

第三方源码已在仓库内（`thirdparty/`：libraw / sqlite / stb_image*.h），**无需额外下载**；
`engine/CMakeLists.txt` 直接引用这些路径。

---

## 2. 引擎编译（C++17）

```powershell
# 一键（内部自动加载 env.ps1 配好工具链 PATH）
powershell -File scripts/build-engine.ps1

# 干净重建（改了 CMakeLists / schema.sql 后建议）
powershell -File scripts/build-engine.ps1 -Clean

# 手动三步（等价）
powershell -File scripts/env.ps1
cmake -S D:/LUMINA/engine -B D:/LUMINA/build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build D:/LUMINA/build/engine
```

- 产物：`build/engine/lumina-engine.exe`
- 附带测试：`build/engine/test_xmp_roundtrip.exe`（7 用例）、`test_rpc_smoke.exe`（15 用例）、
  `test_query_perf.exe`（6000 条查询均 <100ms）。跑法见 `engine/README.md`。
- 注意：`engine/src/db/schema.sql` 是唯一事实源，CMake 配置期生成 `schema_sql.inc`——
  改 schema 后必须重新 configure（`-Clean` 或删 `build/engine`）。

## 3. UI 编译（Electron + Vite + React + TS）

```powershell
cd D:/LUMINA/app
npm install                # 首次；electron 二进制下载走国内镜像，见下
npm run typecheck          # tsc --noEmit，必须 0 错误（准入门禁）
npm run build              # ① vite build → ../build/app  ② esbuild → ../build/app/electron/*.cjs
npm start                  # electron . 启动（会 spawn 引擎 exe，缺失则自动 Mock 模式）
```

- 产物：`build/app/`（`index.html` + `assets/` 渲染进程；`electron/main.cjs` + `preload.cjs` 主进程）
- **electron 下载镜像**：国内网络 `npm install` 可能卡在 electron 二进制下载。
  `scripts/dev.ps1` 里预设了 `ELECTRON_MIRROR=https://npmmirror.com/mirrors/electron/`，
  手动 install 时也可先 `$env:ELECTRON_MIRROR = "https://npmmirror.com/mirrors/electron/"`。
- **junction 约定**：`build/app/node_modules` 必须是指向 `app/node_modules` 的 junction
  （主进程 bundle 运行时 `require('electron')` 需要）。`npm run build` 不创建它；
  重建 build 目录后若报 `Cannot find module 'electron'`，执行：
  `New-Item -ItemType Junction -Path D:/LUMINA/build/app/node_modules -Target D:/LUMINA/app/node_modules`

## 4. 开发模式（热重载）

```powershell
powershell -File scripts/dev.ps1
```

并行起 Vite dev server（:5173）+ Electron（`LUMINA_DEV=1`，主进程加载 dev server 而非打包产物）；
引擎 exe 存在则 spawn 真引擎，否则自动降级 Mock 模式（95 方法全mock，UI 无感）。
`LUMINA_HOME` 默认指向 `build/runtime`，与真实图库 `~/.lumina` 隔离。

## 5. 打包（自包含发布物）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-app.ps1 -Zip
# -SkipBuild：跳过 npm run build（UI 刚构建过时加速）
# -Zip：同时产便携包 LUMINA-1.1.0-portable.zip
```

组装结果 `build/pkg/LUMINA/`（v1.1.0 约 684MB）：

```
LUMINA.exe                  ← 重命名自 electron.exe
*.dll/*.pak/*.dat/locales/  ← Electron runtime
resources/app/package.json  ← main 指向 electron/main.cjs（必须在 app 目录内）
resources/build/app/        ← vite renderer bundle
resources/build/engine/     ← lumina-engine.exe（静态链接）
使用说明.txt                 ← 终端用户说明
```

脚本有 preflight：缺 Electron runtime / renderer / 主进程 bundle / 引擎 exe / 使用说明任一
都会直接报错退出，不会产出跑不起来的包。

## 6. 安装器（NSIS）

```powershell
makensis scripts/installer.nsi
```

- 输出：`build/pkg/LUMINA-1.1.0-setup.exe`（v1.1.0 约 106.9MB）
- 特性：MUI2 英文界面、每用户安装到 `%LOCALAPPDATA%\Programs\LUMINA`（无需管理员）、
  桌面快捷方式为可取消组件、`/S` 静默安装、卸载只清程序文件不动用户数据
  （`%USERPROFILE%\.lumina` 保留）
- **改版本号时同步** `scripts/installer.nsi` 的 `APP_VERSION`（否则安装器显示的版本滞后）

## 7. 已知坑（都踩过）

1. **`ELECTRON_RUN_AS_NODE=1`**：本机某些工具环境默认设了它，启动任何 Electron GUI
   （含打包版 LUMINA.exe）会退化成 Node REPL——现象是挂起无窗口、或秒退无报错。
   运行前先清除：bash 下 `env -u ELECTRON_RUN_AS_NODE ./LUMINA.exe`；
   PowerShell 下 `Remove-Item Env:ELECTRON_RUN_AS_NODE`。
2. **PowerShell 5.1 纯 ASCII 纪律**：Windows 自带 PowerShell 5.1 会把无 BOM 脚本按 ANSI
   解码，中文注释会静默吞语句。改 `scripts/*.ps1` 时**只写 ASCII**（`package-app.ps1`
   头部注释有完整事故记录；中文文件名用 `[char]0xXXXX` 拼接绕开）。
3. **esbuild 主进程不开 minify**：`build:electron` 有意不传 `--minify`——esbuild 0.24 在
   Node 22 下对主进程 bundle 开 minify 会内部崩溃。压缩收益≈0，别"优化"掉这个决定。
4. **换机器先改 `env.ps1`**：引擎构建依赖里面的绝对工具路径（§1）。
5. **版本号三处口径**：发版改版本要同步 ① `engine/src/rpc/version.h` 的 `kEngineVersion`
   （`kProtocolVersion` 是协议版本，别动）② `app/package.json` ③ `scripts/package-app.ps1`
   内嵌的 package.json here-string + zip 名。另 ④ 安装器见 §6。
6. **冒烟验证**：`LUMINA_SMOKE=1` 启动打包版跑 13 场景冒烟（36s 左右），
   输出 `[smoke-scenario] DONE` + exit 0 为通过；冒烟会在 `resources/build/` 写 5 张测试
   PNG，交付前需重建清掉。

## 8. 完整流水线（从零到可发布）

```powershell
# 0. 工具链就位后（§1），工程根执行：
powershell -File scripts/build-engine.ps1 -Clean          # 引擎
cd app; npm install; npm run typecheck; npm run build     # UI
cd ..; powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-app.ps1 -Zip   # 打包
makensis scripts/installer.nsi                            # 安装器
# 8. 验证（可选但发布前必跑）：gate_*.py / verify_*.py / stability_8h.py，用法见各脚本头部
```

---

*最近核对：2026-10-24（v1.1.0 发布态；工具版本为实测值）*
