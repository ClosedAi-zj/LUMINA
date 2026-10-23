@echo off
chcp 65001 >nul
setlocal

rem LUMINA 内测版启动器
rem 说明：宿主环境若设置了 ELECTRON_RUN_AS_NODE，Electron 会退化成纯 Node 进程，
rem 表现为 require('electron').app 为 undefined、窗口起不来。这里显式清空。

set ELECTRON_RUN_AS_NODE=
set LUMINA_DEV=0
rem 软件渲染兜底：远程桌面 / 虚拟机 / 部分显卡驱动下 GPU 进程会反复崩溃导致白屏。
rem 如果你的机器显卡正常、想要最好的渲染性能，把下面这行改成 rem 注释掉即可。
set LUMINA_SAFE_GL=1

cd /d "%~dp0app"

if not exist "node_modules\electron\dist\electron.exe" (
  echo [ERROR] 未找到 Electron 运行时，请在 app 目录执行 npm install
  pause
  exit /b 1
)

if not exist "..\build\app\electron\main.cjs" (
  echo [INFO] 首次启动，正在构建界面...
  call npm run build
  if errorlevel 1 (
    echo [ERROR] 构建失败
    pause
    exit /b 1
  )
)

echo [LUMINA] 正在启动...
start "" "node_modules\electron\dist\electron.exe" .
exit /b 0
