/**
 * 导入页（单页直导版，替代原 1→2→3→4→执行 分步向导）：
 * 用户裁定"不要按固定流程走"——来源选择、扫描摘要、导入设置、开始按钮
 * 全部一屏呈现；扫描完成即可直接开始导入，所有选项随手可改，不设步骤门槛。
 * 执行进度走真实 ingest 事件（importFileResults + job 进度）。
 * 整夹不可读 → 三选项弹窗（重定位 / 孤儿 / 回填），禁止静默跳过（PRD R3）。
 */

import { useEffect, useMemo, useState } from 'react';
import { api } from '../../api/engine';
import { errText } from '../../api/rpc-client';
import { useJobStore } from '../../store/jobStore';
import { useUi } from '../../store/uiStore';
import { useLibrary } from '../../store/libraryStore';
import { useJobProgress, useRpcQuery } from '../../api/hooks';
import { Button, Toggle, Chip } from '../../components/primitives';
import { Dialog, Icon } from '../../components/shell';
import Filmstrip from '../library/Filmstrip';
import { formatBytes, formatCount, formatEta } from '../../utils/format';
import type { ScanSourceResult, VolumeInfo } from '../../api/protocol';

interface SourcePick {
  kind: 'volume' | 'folder' | 'file';
  path: string;
  label: string;
}

/** 文件选择器过滤：RAW / JPG / PNG / TIFF / 视频（与引擎导入白名单同一套） */
const IMPORT_FILE_EXTENSIONS = [
  'rw2', 'dng', 'arw', 'cr2', 'cr3', 'nef', 'nrw', 'orf', 'raf', 'pef', 'ptx', 'r3d', 'srw', 'x3f', 'erf', 'kdc', 'dcr', 'raw',
  'jpg', 'jpeg', 'png', 'tif', 'tiff',
  'mp4', 'mov', 'm4v', 'avi', 'mpg', 'mpeg', 'mkv', 'webm',
];

/**
 * 重命名模板：`label` 给用户看，`template` 才是发给引擎的 token 串（协议 §5.8）。
 * 曾经这里只有中文标签，并且被**原样**当作 renameTemplate 传给引擎：
 * `RenameEngine::validate` 对不含 `{}` 的串直接放行，`apply` 于是把每一张照片都
 * 命名成同一个基名（"YYYY-MM-DD_地点_序号"），同目录内互相覆盖 ——
 * 用户看到的是"导入了 N 张，库里只有 1 张 / 根本导不进去"。
 */
const TEMPLATES: { label: string; template: string }[] = [
  { label: '日期_机型_原文件名', template: '{YYYY}-{MM}-{DD}_{camera}_{name}' },
  { label: '日期时间_原文件名', template: '{YYYY}{MM}{DD}_{HH}{mm}{ss}_{name}' },
  { label: '原文件名（不改名）', template: '{name}' },
];

/** 导入完成事件 result（引擎 importer.cpp 实际字段：imported / duplicated / failed） */
interface ImportResultPayload {
  imported?: number;
  duplicated?: number;
  failed?: number;
  totalFiles?: number;
  error?: string;
}

export default function IngestWizard() {
  const [source, setSource] = useState<SourcePick | null>(null);
  const [scanJobId, setScanJobId] = useState<number | null>(null);
  const [scanResult, setScanResult] = useState<ScanSourceResult | null>(null);
  const [missingFolder, setMissingFolder] = useState<string | null>(null);

  // 目标与备份（备份完全可选：默认仅导入主库，不强迫任何第二位置）
  const [target, setTarget] = useState('D:/Lumina/Photos/2026');
  const [backupEnabled, setBackupEnabled] = useState(false);
  const [backupTarget, setBackupTarget] = useState('N:/NAS-Photos/Lumina');
  const [backupTiming, setBackupTiming] = useState<'onImport' | 'nightly'>('onImport');
  const [verifyMd5, setVerifyMd5] = useState(true);
  const [targetCheck, setTargetCheck] = useState<{ freeBytes: number; warning: boolean } | null>(null);

  // 选项
  const [template, setTemplate] = useState(TEMPLATES[0].template);
  const [enterCulling, setEnterCulling] = useState(true);
  const [applyPreset, setApplyPreset] = useState<string | null>('风光 · 基础');

  // 执行
  const [importJobId, setImportJobId] = useState<number | null>(null);
  const [summary, setSummary] = useState<{ added: number; skipped: number; failed: number; error?: string } | null>(null);

  const volumes = useRpcQuery(() => api.system.listVolumes(), []);
  const recent = useRpcQuery(() => api.importApi.listRecentSources(), []);
  const scanJob = useJobProgress(scanJobId);
  const importJob = useJobProgress(importJobId);
  const importEvents = useJobStore((s) => s.importFileResults);

  // 扫描完成 → 摘要 + 容量预检（单页直导：不再有"下一步"触发预检，扫描完即查）
  useEffect(() => {
    if (scanJob?.state === 'done' && scanJob.result && !scanResult) {
      const r = scanJob.result as unknown as ScanSourceResult;
      setScanResult(r);
      // 整夹不可读 → 三选项弹窗（R3）
      if (r.unreadable && r.unreadable.length > 0) {
        setMissingFolder(r.unreadable[0].path);
      }
      api.importApi
        .checkTarget(target, r.totalBytes)
        .then(setTargetCheck)
        .catch((err) => {
          // 零静默：容量预检失败不能装作通过，要显式提示"未预检"
          setTargetCheck(null);
          useJobStore.getState().toast(`目标盘容量预检失败：${errText(err)}（将继续，但不会拦截空间不足）`, 'warn', 6000);
        });
    }
  }, [scanJob, scanResult, target]);

  // 导入完成 → 汇总（字段名必须与引擎 result 对齐：imported / duplicated / failed）
  useEffect(() => {
    if (importJob?.state === 'done' && importJob.result) {
      const r = importJob.result as unknown as ImportResultPayload;
      setSummary({
        added: r.imported ?? 0,
        skipped: r.duplicated ?? 0,
        failed: r.failed ?? 0,
        error: r.error,
      });
    }
  }, [importJob]);

  const startScan = async (pick: SourcePick) => {
    setSource(pick);
    setScanResult(null);
    setTargetCheck(null);
    try {
      const { jobId } = await api.importApi.scanSource({ kind: pick.kind, path: pick.path });
      setScanJobId(jobId);
    } catch (err) {
      useJobStore.getState().toast(`扫描失败：${errText(err)}`, 'error');
    }
  };

  const importing = importJob?.state === 'running';
  const canStart = !!scanResult && scanResult.totalFiles > 0 && !importing;

  const startImport = async () => {
    if (!source || !canStart) return;
    try {
      const { jobId } = await api.importApi.start({
        source: { kind: source.kind, path: source.path },
        target,
        backup: { enabled: backupEnabled, targetPath: backupTarget, timing: backupTiming },
        verifyMd5,
        renameTemplate: template,
        dedupe: 'skip',
        pairRawJpg: true,
        postActions: { enterCulling, applyPreset },
      });
      setImportJobId(jobId);
      setSummary(null);
    } catch (err) {
      useJobStore.getState().toast(`导入启动失败：${errText(err)}`, 'error');
    }
  };

  const finishImport = async () => {
    await useLibrary.getState().refresh();
    useUi.getState().navigate('library');
  };

  const jobEvents = useMemo(() => (importJobId === null ? [] : importEvents.filter((e) => e.jobId === importJobId)), [importEvents, importJobId]);
  // 失败明细（最多 5 条）：汇总弹窗里必须能看到"为什么失败"，不能只给一个数字
  const failedReasons = useMemo(() => jobEvents.filter((e) => e.status === 'failed').slice(-5), [jobEvents]);
  const removable = volumes.data ?? [];

  return (
    // relative：底部 footer 用 absolute bottom-0 贴底，必须有一个已定位的祖先，
    // 否则会按视口定位（在滚动容器里会漂到页面外）
    <div className="relative h-full flex" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      {/* 主区：单页直导 */}
      <section className="flex-1 min-w-0 p-4 overflow-y-auto flex flex-col gap-3 pb-16">
        <div>
          <div className="text-sm font-medium">导入照片</div>
          <div className="text-2xs text-txt3 mt-0.5">从相机、文件夹或单个文件导入 · 备份可选 · 选好来源即可直接开始</div>
        </div>

        {/* 来源选择 */}
        <div className="grid grid-cols-3 gap-2">
          {removable
            .filter((v) => v.isRemovable)
            .map((v: VolumeInfo) => (
              <button
                key={v.path}
                onClick={() => void startScan({ kind: 'volume', path: v.path, label: v.label })}
                className={`flex items-center gap-3 p-3 rounded-lg border text-left ${
                  sourceEquals(v.path, source?.path ?? null) ? 'border-accent bg-[var(--Lumina-Tokens-accentSoft)]' : 'border-line bg-card hover:bg-hover'
                }`}
              >
                <span className="w-9 h-6 rounded bg-accent text-onaccent text-2xs font-bold flex items-center justify-center shrink-0">SD</span>
                <span className="flex-1 min-w-0">
                  <span className="block text-xs text-txt truncate">{v.label}</span>
                  <span className="block text-2xs text-txt3 num">
                    {formatBytes(v.totalBytes)} · 剩余 {formatBytes(v.freeBytes)}
                  </span>
                </span>
              </button>
            ))}
          <button
            onClick={async () => {
              const picked = await window.lumina.pickDirectory({ title: '选择要导入的照片文件夹' });
              if (picked) void startScan({ kind: 'folder', path: picked, label: picked });
            }}
            className="flex items-center gap-3 p-3 rounded-lg border text-left border-line bg-card hover:bg-hover"
          >
            <span className="w-9 h-6 rounded bg-accent text-onaccent text-2xs font-bold flex items-center justify-center shrink-0">浏览</span>
            <span className="flex-1 min-w-0">
              <span className="block text-xs text-txt">选择文件夹…</span>
              <span className="block text-2xs text-txt3">从本机任意目录导入（RAW / JPG 等）</span>
            </span>
          </button>
          <button
            onClick={async () => {
              const picked = await window.lumina.pickFile({ title: '选择要导入的照片或视频', extensions: IMPORT_FILE_EXTENSIONS });
              if (picked) void startScan({ kind: 'file', path: picked, label: picked });
            }}
            className="flex items-center gap-3 p-3 rounded-lg border text-left border-line bg-card hover:bg-hover"
          >
            <span className="w-9 h-6 rounded bg-hover flex items-center justify-center text-txt2 text-2xs font-bold shrink-0">文件</span>
            <span className="flex-1 min-w-0">
              <span className="block text-xs text-txt">选择单个文件…</span>
              <span className="block text-2xs text-txt3">只导一张照片或一个视频也可以</span>
            </span>
          </button>
        </div>

        {scanJob?.state === 'running' && (
          <div className="rounded-md border border-line p-3">
            <div className="text-2xs text-txt2 mb-1.5">正在扫描（存在性预检 · RAW/JPG 配对 · 与库内 MD5 比重）…</div>
            <div className="h-1 rounded-full bg-hover overflow-hidden">
              <div className="h-full bg-accent transition-all" style={{ width: `${scanJob.progress}%` }} />
            </div>
          </div>
        )}

        {/* 扫描摘要（真实结果） */}
        {scanResult && (
          <div className="rounded-lg border border-line bg-card p-3.5">
            <div className="flex items-baseline justify-between mb-2">
              <span className="text-xs text-txt">
                扫描摘要{source ? <span className="num text-txt3"> · {source.label}</span> : null}
              </span>
              <span className="text-2xs text-txt3">来源可随时更改</span>
            </div>
            <div className="grid grid-cols-3 gap-2 text-xs mb-2">
              <Stat label="文件总数" value={formatCount(scanResult.totalFiles)} />
              <Stat label="总大小" value={formatBytes(scanResult.totalBytes)} />
              <Stat label="RAW+JPG 配对" value={String(scanResult.rawWithJpg)} />
              <Stat label="与库内重复" value={String(scanResult.duplicates)} accent />
              <Stat label="伴随 JPG" value={String(scanResult.rawWithJpg)} />
              <Stat label="读失败" value={String(scanResult.unreadable.length)} danger={scanResult.unreadable.length > 0} />
            </div>
            <div className="flex flex-wrap gap-1.5">
              {Object.entries(scanResult.byFormat).map(([fmt, n]) => (
                <Chip key={fmt} label={`${fmt} ${n}`} />
              ))}
            </div>
            {scanResult.unreadable.length > 0 && (
              <div className="mt-2 rounded-md p-2 text-2xs" style={{ background: 'rgba(217,96,79,0.12)', color: 'var(--Lumina-Tokens-danger)' }}>
                ⚠ {scanResult.unreadable.length} 个文件夹/文件读失败（已标红计入摘要，不中断扫描）
              </div>
            )}
          </div>
        )}

        {/* 导入设置（全部随手可改，无步骤门槛） */}
        <div className="rounded-lg border border-line bg-card p-4 grid grid-cols-2 gap-x-6 gap-y-3">
          <div>
            <label className="text-2xs text-txt3 block mb-1">主库路径（照片将复制到这里进入图库）</label>
            <input
              value={target}
              onChange={(e) => setTarget(e.target.value)}
              className="w-full h-8 px-2.5 rounded-md border border-line bg-panel text-xs text-txt focus:outline-none focus:border-accent num"
            />
          </div>
          <div>
            <label className="text-2xs text-txt3 block mb-1.5">重命名模板</label>
            <div className="flex flex-wrap gap-1.5">
              {TEMPLATES.map((t) => (
                <Chip key={t.template} label={t.label} active={template === t.template} onClick={() => setTemplate(t.template)} />
              ))}
            </div>
          </div>
          <div>
            <div className="flex items-center justify-between mb-1">
              <span className="text-2xs text-txt2">备份到第二位置（可选）</span>
              <Toggle checked={backupEnabled} onChange={setBackupEnabled} label="启用" />
            </div>
            {backupEnabled ? (
              <div className="flex flex-col gap-1.5">
                <input
                  value={backupTarget}
                  onChange={(e) => setBackupTarget(e.target.value)}
                  placeholder="NAS / 移动硬盘等任意可写位置"
                  className="w-full h-8 px-2.5 rounded-md border border-line bg-panel text-xs text-txt focus:outline-none focus:border-accent num"
                />
                <div className="flex items-center gap-2">
                  <Chip label="导入时" active={backupTiming === 'onImport'} onClick={() => setBackupTiming('onImport')} />
                  <Chip label="每晚 03:00" active={backupTiming === 'nightly'} onClick={() => setBackupTiming('nightly')} />
                  <Toggle checked={verifyMd5} onChange={setVerifyMd5} label="MD5 校验" />
                </div>
              </div>
            ) : (
              <div className="text-2xs text-txt3">关闭 = 仅导入主库；开启后备份路径任填（NAS / 移动硬盘…）</div>
            )}
          </div>
          <div>
            <div className="text-2xs text-txt2 mb-1.5">导入后动作</div>
            <div className="flex items-center justify-between mb-1.5">
              <span className="text-2xs text-txt2">自动进入精选模式</span>
              <Toggle checked={enterCulling} onChange={setEnterCulling} />
            </div>
            <div className="flex items-center justify-between">
              <span className="text-2xs text-txt2">套用「风光 · 基础」预设（只写 sidecar）</span>
              <Toggle checked={applyPreset !== null} onChange={(v) => setApplyPreset(v ? '风光 · 基础' : null)} />
            </div>
          </div>
        </div>

        {/* 容量预检结果 */}
        {targetCheck && (
          <div
            className="rounded-md p-2.5 text-2xs flex items-center gap-2"
            style={{
              background: targetCheck.warning ? 'rgba(232,194,104,0.12)' : 'rgba(125,217,166,0.1)',
              color: targetCheck.warning ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-success)',
            }}
          >
            {targetCheck.warning ? '⚠ 主库剩余空间不足需求的 110%：' : '✓ 主库空间充足：'}
            <span className="num">
              需求 {formatBytes(scanResult?.totalBytes ?? 0)} · 剩余 {formatBytes(targetCheck.freeBytes)}
            </span>
          </div>
        )}

        {/* 执行中进度 */}
        {importJobId !== null && <ExecutingBar jobId={importJobId} progress={importJob?.progress ?? 0} eta={importJob?.etaMs ?? null} events={jobEvents} />}
      </section>

      {/* 右侧常驻设置：备份状态 / 去重策略 / 预览胶片条 */}
      <PersistentSettings backupEnabled={backupEnabled} backupTarget={backupTarget} backupTiming={backupTiming} verifyMd5={verifyMd5} />

      {/* 底部：单一动作位 */}
      <footer className="absolute bottom-0 left-0 right-0 h-12 px-4 flex items-center gap-2 border-t border-line" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <span className="text-2xs text-txt3">
          {scanResult
            ? `已选 ${formatCount(scanResult.totalFiles)} 个文件 · ${formatBytes(scanResult.totalBytes)}${source ? ` · 来源 ${source.label}` : ''}`
            : '未选择来源 —— 点上方任意卡片开始扫描'}
        </span>
        <div className="flex-1" />
        {importJobId !== null && importing && (
          <Button
            variant="danger"
            onClick={async () => {
              await api.importApi.cancel(importJobId);
              useJobStore.getState().toast('导入已取消，无残留文件', 'info');
            }}
          >
            取消导入
          </Button>
        )}
        <Button variant="primary" disabled={!canStart} onClick={() => void startImport()}>
          {importing ? '导入中…' : '开始导入'}
        </Button>
      </footer>

      {/* 完成弹窗（成功 N · 跳过重复 N · 失败 N）；失败必须给出可读原因（R3） */}
      <Dialog open={summary !== null} title={summary && summary.added === 0 && summary.failed > 0 ? '导入失败' : '导入完成'} onClose={() => setSummary(null)}>
        {summary && (
          <div className="text-xs leading-6">
            <div className={summary.added > 0 ? 'text-success' : 'text-txt3'}>成功 {summary.added} 张</div>
            <div className="text-txt2">跳过重复 {summary.skipped} 张</div>
            <div className={summary.failed > 0 ? 'text-danger' : 'text-txt3'}>失败 {summary.failed} 张{summary.failed > 0 ? '（已进入失败清单与存储健康队列）' : ''}</div>
            {summary.error && (
              <div className="mt-2 rounded-md p-2 text-2xs text-danger" style={{ background: 'rgba(217,96,79,0.12)' }}>
                {summary.error}
              </div>
            )}
            {failedReasons.length > 0 && (
              <div className="mt-2 rounded-md border border-line p-2 text-2xs">
                <div className="text-danger mb-1">失败原因（最近 {failedReasons.length} 条）</div>
                {failedReasons.map((f, i) => (
                  <div key={i} className="num text-txt3 truncate" title={f.path}>
                    ✕ {f.path}{f.error ? ` · ${f.error}` : ''}
                  </div>
                ))}
              </div>
            )}
          </div>
        )}
        <div className="flex justify-end gap-2 mt-4">
          <Button onClick={() => setSummary(null)}>留在本页</Button>
          <Button variant="primary" onClick={() => void finishImport()}>
            进入图库
          </Button>
        </div>
      </Dialog>

      {/* 整夹缺失三选项（R3：禁止静默跳过） */}
      <Dialog open={missingFolder !== null} title="源文件夹不可读" onClose={() => setMissingFolder(null)} width={480}>
        <div className="text-xs text-txt2 leading-5 mb-3">
          以下路径无法读取：<span className="num text-danger">{missingFolder}</span>
          <br />
          请选择处理方式（不会静默跳过）：
        </div>
        <div className="flex flex-col gap-2">
          <Button
            onClick={async () => {
              // 必须让用户挑真实目录：旧代码把重定位目标硬编码成 'D:/Lumina/Relocated/105_PANA'
              // 直接提交（用户数据被指到一个可能不存在的路径，且不可撤销）
              const picked = await window.lumina.pickDirectory({
                title: '选择该文件夹的新位置',
                defaultPath: missingFolder ?? undefined,
              });
              if (!picked) {
                useJobStore.getState().toast('未选择新路径，重定位已取消', 'warn', 4000);
                return; // 弹窗保持打开，用户可改选其它处理方式
              }
              try {
                const res = await api.importApi.resolveMissingFolder(missingFolder!, 'relocate', picked);
                useJobStore.getState().toast(`已重定位到 ${picked} · 影响 ${res.affected} 张`, 'success', 6000);
                setMissingFolder(null);
              } catch (err) {
                useJobStore.getState().toast(`重定位失败：${errText(err)}`, 'error', 8000);
              }
            }}
          >
            重定位文件夹（指定新路径）
          </Button>
          <Button
            onClick={async () => {
              try {
                const res = await api.importApi.resolveMissingFolder(missingFolder!, 'orphan');
                useJobStore.getState().toast(`已跳过并记入孤儿清单 · 影响 ${res.affected} 张`, 'warn', 6000);
                setMissingFolder(null);
              } catch (err) {
                useJobStore.getState().toast(`登记孤儿失败：${errText(err)}`, 'error', 8000);
              }
            }}
          >
            跳过并记入孤儿清单
          </Button>
          <Button
            onClick={async () => {
              try {
                const res = await api.importApi.resolveMissingFolder(missingFolder!, 'backfill');
                // 数量必须来自引擎返回值：旧文案写死「回填 96 张」，与实际回填数无关
                useJobStore.getState().toast(
                  res.affected > 0 ? `已用衍生件（-已增强-降噪.dng）回填 ${res.affected} 张` : '未找到可回填的衍生件（0 张）',
                  res.affected > 0 ? 'success' : 'warn',
                  6000,
                );
                setMissingFolder(null);
              } catch (err) {
                useJobStore.getState().toast(`衍生件回填失败：${errText(err)}`, 'error', 8000);
              }
            }}
          >
            衍生件回填（用「-已增强-降噪.dng」衍生件）
          </Button>
        </div>
      </Dialog>
    </div>
  );
}

// ---------- 右侧常驻设置 ----------

function PersistentSettings({ backupEnabled, backupTarget, backupTiming, verifyMd5 }: { backupEnabled: boolean; backupTarget: string; backupTiming: 'onImport' | 'nightly'; verifyMd5: boolean }) {
  // 预览胶片条：真实数据 = 图库首页缓存（导入完成后 refresh 即显示新入库照片）
  const firstPage = useLibrary((s) => s.pages[0] ?? []);
  return (
    <aside className="w-72 shrink-0 border-l border-line p-3 flex flex-col gap-3 overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div>
        <div className="text-2xs text-txt2 font-medium mb-1.5">备份（可选）</div>
        <div className="rounded-md border border-line bg-card p-2.5 text-2xs flex flex-col gap-1">
          <span className={backupEnabled ? 'text-success' : 'text-txt3'}>{backupEnabled ? '✓ 已启用第二位置备份' : '未启用（仅导入主库）'}</span>
          {backupEnabled && <span className="num text-txt3">目标：{backupTarget || '（未填写）'}</span>}
          <span className="text-txt3">{backupEnabled ? `时机：${backupTiming === 'onImport' ? '导入时逐文件' : '每晚 03:00 定时'}` : '是否备份、备份到哪，随时可在左侧改'}</span>
          <span className="text-txt3">MD5 校验：{verifyMd5 ? '导入即校验' : '关闭'}</span>
        </div>
      </div>
      <div>
        <div className="text-2xs text-txt2 font-medium mb-1.5">去重策略</div>
        <div className="rounded-md border border-line bg-card p-2.5 text-2xs flex flex-col gap-1">
          <span className="text-txt2">MD5 相同 → 跳过并计数</span>
          <span className="text-txt2">RAW + JPG → 配对为一条目（固定开启）</span>
          <span className="text-txt3">重命名模板在左侧「重命名模板」处改</span>
        </div>
      </div>
      <div className="min-h-0">
        <div className="text-2xs text-txt2 font-medium mb-1.5">预览胶片条（当前图库首页）</div>
        {firstPage.length > 0 ? (
          <Filmstrip items={firstPage} />
        ) : (
          <div className="rounded-md border border-line bg-card p-2.5 text-2xs text-txt3">暂无（导入完成后显示新入库照片）</div>
        )}
      </div>
    </aside>
  );
}

/**
 * 路径等价比较（来源卡片高亮用）。
 * Windows 上同一目录可能写作 `E:/DCIM`、`E:\DCIM\`、`e:/dcim`，统一分隔符/大小写/尾斜杠后比较。
 */
function normalizePathForCompare(p: string): string {
  return p.replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();
}

function sourceEquals(path: string, selectedPath: string | null): boolean {
  if (!selectedPath) return false;
  return normalizePathForCompare(path) === normalizePathForCompare(selectedPath);
}

function Stat({ label, value, accent, danger }: { label: string; value: string; accent?: boolean; danger?: boolean }) {
  return (
    <div className="rounded-md px-2.5 py-2" style={{ background: 'var(--Lumina-Tokens-hover)' }}>
      <div className="text-2xs text-txt3">{label}</div>
      <div className={`num text-sm ${danger ? 'text-danger' : accent ? 'text-accent' : 'text-txt'}`}>{value}</div>
    </div>
  );
}

// ---------- 执行态 ----------

function ExecutingBar({
  jobId,
  progress,
  eta,
  events,
}: {
  jobId: number;
  progress: number;
  eta: number | null;
  events: { path: string; status: string; error?: string }[];
}) {
  const ok = events.filter((e) => e.status === 'ok').length;
  const dup = events.filter((e) => e.status === 'duplicate').length;
  const failed = events.filter((e) => e.status === 'failed');
  const current = events[events.length - 1];
  return (
    <div className="rounded-lg border border-line bg-card p-4">
      <div className="flex items-baseline justify-between mb-2">
        <span className="text-xs text-txt">正在导入…（任务 {jobId}）</span>
        <span className="num text-xs text-accent">{progress}% · 预计剩余 {formatEta(eta)}</span>
      </div>
      <div className="h-1.5 rounded-full bg-hover overflow-hidden mb-2">
        <div className="h-full bg-accent transition-all" style={{ width: `${progress}%` }} />
      </div>
      <div className="text-2xs text-txt3 num truncate">{current ? `${current.status === 'ok' ? '✓' : current.status === 'duplicate' ? '⧉' : '✕'} ${current.path}` : '准备中…'}</div>
      <div className="flex gap-3 mt-2 text-2xs">
        <span className="text-success">成功 {ok}</span>
        <span className="text-txt2">跳过重复 {dup}</span>
        <span className={failed.length > 0 ? 'text-danger' : 'text-txt3'}>失败 {failed.length}</span>
      </div>
      {failed.length > 0 && (
        <div className="mt-2 rounded-md border border-line p-2">
          <div className="text-2xs text-danger mb-1">失败清单（单文件失败不中断整批）</div>
          {failed.slice(-5).map((f, i) => (
            <div key={i} className="text-2xs text-txt3 num truncate">
              ✕ {f.path} {f.error ? `· ${f.error}` : ''}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
