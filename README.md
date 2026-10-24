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

- Engine: `scripts/build-engine.ps1` (see `engine/README.md`)
- UI: `npm install && npm run build` under `app/` (see `app/README.md`)

## Status

v1.1.0 released (2026-10-24) — download from [Releases](https://github.com/xjjj22/LUMINA/releases/tag/v1.1.0); see `docs/release-gate-1.1.0.md` for release gates.
