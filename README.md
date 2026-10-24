# LUMINA

An all-in-one workstation for landscape and architecture photographers — C++17 engine + Electron/React UI, hybrid architecture.

- Edit parameters live in open XMP sidecars; originals are never rewritten (no proprietary database lock-in)
- RAW decoding via LibRaw; search via SQLite FTS5; preview via GLSL (CPU software rendering takes over when no GPU is available)

## Repository Layout

| Directory | Contents |
|---|---|
| `engine/` | C++ engine: LibRaw RAW decoding, SQLite FTS5, XMP sidecar editing, ingest/health scanning, export (JPG/PNG/TIFF), RPC service |
| `app/` | Electron UI: library, develop module (quick/pro dual modes), culling, map, timelapse, portfolio, web client galleries |
| `scripts/` | Build, gate-test and acceptance scripts (RW2 benchmarks / zero-rewrite / 50k performance / 8-hour stress test, etc.) |
| `docs/` | PRD, architecture design, IPC protocol, release gate reports |
| `thirdparty/` | LibRaw / SQLite / stb sources (build artifacts are not committed) |

## Build

Prerequisites: MinGW-w64 (g++ 8.1.0), CMake, Ninja, Node.js 22, Python 3 (test scripts only), NSIS (installer only). Full guide with exact commands, toolchain paths and known pitfalls: [`docs/BUILD.md`](docs/BUILD.md).

- Engine (C++17 → `build/engine/lumina-engine.exe`): `powershell -File scripts/build-engine.ps1`
- UI (Electron + React → `build/app/`): `cd app && npm install && npm run typecheck && npm run build`
- Package (self-contained folder + portable zip → `build/pkg/`): `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-app.ps1 -Zip`
- Installer (NSIS → `build/pkg/LUMINA-1.1.0-setup.exe`): `makensis scripts/installer.nsi`
- Dev mode (Vite + Electron, hot reload): `powershell -File scripts/dev.ps1`

## Status

v1.1.0 released (2026-10-24) — download from [Releases](https://github.com/xjjj22/LUMINA/releases/tag/v1.1.0); see `docs/release-gate-1.1.0.md` for release gates.
