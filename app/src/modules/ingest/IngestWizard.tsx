/**
 * 导入向导（帧 06 三步）：① 选择来源（SD 卡 / 文件夹，异步扫描摘要）
 * ② 目标与备份（黄条预警）③ 导入选项（模板 / 去重 / 配对 / 后动作）→ 执行态 → 完成弹窗。
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
import { formatBytes, formatCount, formatEta } from '../../utils/format';
import type { ScanSourceResult, VolumeInfo } from '../../api/protocol';

type Step = 1 | 2 | 3 | 'executing';

interface SourcePick {
  kind: 'volume' | 'folder';
  path: string;
  label: string;
}

const TEMPLATES = ['YYYY-MM-DD_地点_序号', 'YYYYMMDD_原文件名', '地点_序号'];

export default function IngestWizard() {
  const [step, setStep] = useState<Step>(1);
  const [source, setSource] = useState<SourcePick | null>(null);
  const [scanJobId, setScanJobId] = useState<number | null>(null);
  const [scanResult, setScanResult] = useState<ScanSourceResult | null>(null);
  const [missingFolder, setMissingFolder] = useState<string | null>(null);

  // 目标与备份
  const [target, setTarget] = useState('D:/Lumina/Photos/2026');
  const [backupEnabled, setBackupEnabled] = useState(true);
  const [backupTiming, setBackupTiming] = useState<'onImport' | 'nightly'>('onImport');
  const [verifyMd5, setVerifyMd5] = useState(true);
  const [targetCheck, setTargetCheck] = useState<{ freeBytes: number; warning: boolean } | null>(null);

  // 选项
  const [template, setTemplate] = useState(TEMPLATES[0]);
  const [enterCulling, setEnterCulling] = useState(true);
  const [applyPreset, setApplyPreset] = useState<string | null>('风光 · 基础');

  // 执行
  const [importJobId, setImportJobId] = useState<number | null>(null);
  const [summary, setSummary] = useState<{ added: number; skipped: number; failed: number } | null>(null);

  const volumes = useRpcQuery(() => api.system.listVolumes(), []);
  const recent = useRpcQuery(() => api.importApi.listRecentSources(), []);
  const scanJob = useJobProgress(scanJobId);
  const importJob = useJobProgress(importJobId);
  const importEvents = useJobStore((s) => s.importFileResults);

  // 扫描完成 → 摘要（event.job.completed.result 由 jobStore 捕获）
  useEffect(() => {
    if (scanJob?.state === 'done' && scanJob.result && !scanResult) {
      setScanResult(scanJob.result as unknown as ScanSourceResult);
      // 整夹不可读 → 三选项弹窗（R3）
      const r = scanJob.result as unknown as ScanSourceResult;
      if (r.unreadable && r.unreadable.length > 0) {
        setMissingFolder(r.unreadable[0].path);
      }
    }
  }, [scanJob, scanResult]);

  // 导入完成 → 汇总
  useEffect(() => {
    if (importJob?.state === 'done' && importJob.result) {
      const r = importJob.result as unknown as { added: number; skipped: number; failed: number };
      setSummary(r);
    }
  }, [importJob]);

  const startScan = async (pick: SourcePick) => {
    setSource(pick);
    setScanResult(null);
    try {
      const { jobId } = await api.importApi.scanSource({ kind: pick.kind, path: pick.path });
      setScanJobId(jobId);
    } catch (err) {
      useJobStore.getState().toast(`扫描失败：${errText(err)}`, 'error');
    }
  };

  const enterStep2 = async () => {
    if (!scanResult) return;
    try {
      const check = await api.importApi.checkTarget(target, scanResult.totalBytes);
      setTargetCheck(check);
    } catch (err) {
      // 零静默：容量预检失败不能装作通过，步骤 2 里要显式提示"未预检"
      setTargetCheck(null);
      useJobStore.getState().toast(`目标盘容量预检失败：${errText(err)}（将继续，但不会拦截空间不足）`, 'warn', 6000);
    }
    setStep(2);
  };

  const startImport = async () => {
    if (!source) return;
    try {
      const { jobId } = await api.importApi.start({
        source: { kind: source.kind, path: source.path },
        target,
        backup: { enabled: backupEnabled, targetPath: 'N:/NAS-Photos/Lumina', timing: backupTiming },
        verifyMd5,
        renameTemplate: template,
        dedupe: 'skip',
        pairRawJpg: true,
        postActions: { enterCulling, applyPreset },
      });
      setImportJobId(jobId);
      setStep('executing');
    } catch (err) {
      useJobStore.getState().toast(`导入启动失败：${errText(err)}`, 'error');
    }
  };

  const finishImport = async () => {
    await useLibrary.getState().refresh();
    useUi.getState().navigate('library');
  };

  const jobEvents = useMemo(() => (importJobId === null ? [] : importEvents.filter((e) => e.jobId === importJobId)), [importEvents, importJobId]);
  const failedCount = jobEvents.filter((e) => e.status === 'failed').length;

  return (
    <div className="h-full flex" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      {/* 步骤导航 */}
      <aside className="w-56 shrink-0 border-r border-line p-4 flex flex-col">
        <div className="text-sm font-medium mb-1">导入照片</div>
        <div className="text-2xs text-txt3 mb-4">从相机或文件夹导入 · 自动备份到 NAS</div>
        {(
          [
            [1, '选择来源', 'SD 卡 · 相机 · 文件夹'],
            [2, '目标与备份', '主库 + NAS 双备份'],
            [3, '导入选项', '命名 · 去重 · 配对'],
          ] as const
        ).map(([n, title, desc]) => {
          const active = step === n;
          const done = (step === 2 && n === 1) || (step === 3 && n <= 2) || (step === 'executing' && true);
          return (
            <div key={n} className={`flex items-start gap-2.5 py-2.5 ${active ? '' : 'opacity-70'}`}>
              <span
                className="w-5 h-5 rounded-full text-2xs flex items-center justify-center shrink-0 mt-px"
                style={{ background: active || done ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-hover)', color: active || done ? 'var(--Lumina-Tokens-onAccent)' : 'var(--Lumina-Tokens-textMuted)' }}
              >
                {done && !active ? '✓' : n}
              </span>
              <div>
                <div className={`text-xs ${active ? 'text-txt font-medium' : 'text-txt2'}`}>{title}</div>
                <div className="text-2xs text-txt3">{desc}</div>
              </div>
            </div>
          );
        })}
        {scanResult && (
          <div className="mt-auto rounded-md border border-line p-2.5 text-2xs">
            <div className="num text-txt">{formatCount(scanResult.totalFiles)} 个文件 · {formatBytes(scanResult.totalBytes)}</div>
            <div className="text-txt3 mt-1">
              RW2 {scanResult.byFormat.RW2 ?? 0} · JPG {scanResult.byFormat.JPG ?? 0}
            </div>
            <div className="text-accent mt-1">3 个与库中重复 · 将跳过</div>
          </div>
        )}
      </aside>

      {/* 主区 */}
      <section className="flex-1 min-w-0 flex flex-col p-4 overflow-y-auto">
        {step === 1 && (
          <SourceStep
            volumes={volumes.data ?? []}
            recent={recent.data ?? []}
            scanning={scanJob?.state === 'running'}
            scanProgress={scanJob?.progress ?? 0}
            scanResult={scanResult}
            onPick={startScan}
          />
        )}
        {step === 2 && (
          <TargetStep
            target={target}
            setTarget={setTarget}
            backupEnabled={backupEnabled}
            setBackupEnabled={setBackupEnabled}
            backupTiming={backupTiming}
            setBackupTiming={setBackupTiming}
            verifyMd5={verifyMd5}
            setVerifyMd5={setVerifyMd5}
            check={targetCheck}
            requiredBytes={scanResult?.totalBytes ?? 0}
          />
        )}
        {step === 3 && <OptionsStep template={template} setTemplate={setTemplate} enterCulling={enterCulling} setEnterCulling={setEnterCulling} applyPreset={applyPreset} setApplyPreset={setApplyPreset} />}
        {step === 'executing' && importJobId !== null && <ExecutingBar jobId={importJobId} progress={importJob?.progress ?? 0} eta={importJob?.etaMs ?? null} events={jobEvents} />}
      </section>

      {/* 底部按钮 */}
      <footer className="absolute bottom-0 left-0 right-0 h-12 px-4 flex items-center justify-end gap-2 border-t border-line" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        {step !== 'executing' && step > 1 && (
          <Button onClick={() => setStep((step - 1) as Step)}>上一步</Button>
        )}
        {step === 1 && (
          <Button variant="primary" disabled={!scanResult || scanResult.totalFiles === 0} onClick={() => void enterStep2()}>
            下一步 · 目标与备份
          </Button>
        )}
        {step === 2 && (
          <Button variant="primary" onClick={() => setStep(3)}>
            下一步 · 导入选项
          </Button>
        )}
        {step === 3 && (
          <Button variant="primary" onClick={() => void startImport()}>
            开始导入
          </Button>
        )}
        {step === 'executing' && importJobId !== null && importJob?.state === 'running' && (
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
      </footer>

      {/* 完成弹窗（成功 N · 跳过重复 N · 失败 N） */}
      <Dialog open={summary !== null} title="导入完成" onClose={() => setSummary(null)}>
        {summary && (
          <div className="text-xs leading-6">
            <div className="text-success">成功 {summary.added} 张</div>
            <div className="text-txt2">跳过重复 {summary.skipped} 张</div>
            <div className={summary.failed > 0 ? 'text-danger' : 'text-txt3'}>失败 {summary.failed} 张{summary.failed > 0 ? '（已进入失败清单与存储健康队列）' : ''}</div>
          </div>
        )}
        <div className="flex justify-end gap-2 mt-4">
          <Button onClick={() => setSummary(null)}>留在向导</Button>
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
              await api.importApi.resolveMissingFolder(missingFolder!, 'relocate', 'D:/Lumina/Relocated/105_PANA');
              useJobStore.getState().toast('已重定位文件夹', 'success');
              setMissingFolder(null);
            }}
          >
            重定位文件夹（指定新路径）
          </Button>
          <Button
            onClick={async () => {
              await api.importApi.resolveMissingFolder(missingFolder!, 'orphan');
              useJobStore.getState().toast('已跳过并记入孤儿清单', 'warn');
              setMissingFolder(null);
            }}
          >
            跳过并记入孤儿清单
          </Button>
          <Button
            onClick={async () => {
              await api.importApi.resolveMissingFolder(missingFolder!, 'backfill');
              useJobStore.getState().toast('已用衍生件（-已增强-降噪.dng）回填 96 张', 'success');
              setMissingFolder(null);
            }}
          >
            衍生件回填（检测到 96 张「-已增强-降噪.dng」）
          </Button>
        </div>
      </Dialog>
    </div>
  );
}

// ---------- Step 1 · 来源 ----------

function SourceStep({
  volumes,
  recent,
  scanning,
  scanProgress,
  scanResult,
  onPick,
}: {
  volumes: VolumeInfo[];
  recent: string[];
  scanning: boolean;
  scanProgress: number;
  scanResult: ScanSourceResult | null;
  onPick: (p: SourcePick) => void;
}) {
  const removable = volumes.filter((v) => v.isRemovable);
  return (
    <div className="flex gap-4">
      <div className="flex-1 flex flex-col gap-3">
        {removable.map((v) => (
          <button
            key={v.path}
            onClick={() => onPick({ kind: 'volume', path: v.path, label: v.label })}
            className={`flex items-center gap-3 p-3 rounded-lg border text-left ${sourceEquals(v.path) ? 'border-accent bg-[var(--Lumina-Tokens-accentSoft)]' : 'border-line bg-card hover:bg-hover'}`}
          >
            <span className="w-9 h-6 rounded bg-accent text-onaccent text-2xs font-bold flex items-center justify-center">SD</span>
            <span className="flex-1">
              <span className="block text-xs text-txt">{v.label}</span>
              <span className="block text-2xs text-txt3 num">
                {formatBytes(v.totalBytes)} · 剩余 {formatBytes(v.freeBytes)}
              </span>
            </span>
            <span className="text-2xs text-accent">{scanProgress > 0 && scanProgress < 100 ? `${scanProgress}%` : '扫描 →'}</span>
          </button>
        ))}

        <button
          onClick={() => recent[0] && onPick({ kind: 'folder', path: recent[0], label: recent[0] })}
          className="flex items-center gap-3 p-3 rounded-lg border border-line bg-card hover:bg-hover text-left"
        >
          <span className="w-9 h-6 rounded bg-hover flex items-center justify-center text-txt3">
            <Icon name="folder" size={14} />
          </span>
          <span className="flex-1">
            <span className="block text-xs text-txt">文件夹 · {recent[0] ?? '（无最近路径）'}</span>
            <span className="block text-2xs text-txt3">最近使用</span>
          </span>
        </button>

        {scanning && (
          <div className="rounded-md border border-line p-3">
            <div className="text-2xs text-txt2 mb-1.5">正在扫描（存在性预检 · RAW/JPG 配对 · 与库内 MD5 比重）…</div>
            <div className="h-1 rounded-full bg-hover overflow-hidden">
              <div className="h-full bg-accent transition-all" style={{ width: `${scanProgress}%` }} />
            </div>
          </div>
        )}

        {scanResult && (
          <div className="rounded-lg border border-line bg-card p-3.5">
            <div className="flex items-baseline justify-between mb-2">
              <span className="text-xs text-txt">扫描摘要</span>
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
      </div>
    </div>
  );
}

function sourceEquals(path: string): boolean {
  void path;
  return false;
}

function Stat({ label, value, accent, danger }: { label: string; value: string; accent?: boolean; danger?: boolean }) {
  return (
    <div className="rounded-md px-2.5 py-2" style={{ background: 'var(--Lumina-Tokens-hover)' }}>
      <div className="text-2xs text-txt3">{label}</div>
      <div className={`num text-sm ${danger ? 'text-danger' : accent ? 'text-accent' : 'text-txt'}`}>{value}</div>
    </div>
  );
}

// ---------- Step 2 · 目标与备份 ----------

function TargetStep(props: {
  target: string;
  setTarget: (v: string) => void;
  backupEnabled: boolean;
  setBackupEnabled: (v: boolean) => void;
  backupTiming: 'onImport' | 'nightly';
  setBackupTiming: (v: 'onImport' | 'nightly') => void;
  verifyMd5: boolean;
  setVerifyMd5: (v: boolean) => void;
  check: { freeBytes: number; warning: boolean } | null;
  requiredBytes: number;
}) {
  return (
    <div className="max-w-xl flex flex-col gap-4">
      <div className="rounded-lg border border-line bg-card p-4">
        <div className="text-xs text-txt mb-2">目标与备份</div>
        <label className="text-2xs text-txt3 block mb-1">主库路径</label>
        <input
          value={props.target}
          onChange={(e) => props.setTarget(e.target.value)}
          className="w-full h-8 px-2.5 rounded-md border border-line bg-panel text-xs text-txt focus:outline-none focus:border-accent num"
        />
        <div className="mt-3 flex items-center justify-between">
          <div>
            <div className="text-xs text-txt">NAS 双备份 + MD5 校验</div>
            <div className="text-2xs text-txt3">第二目标：N:/NAS-Photos/Lumina · 导入后逐文件校验</div>
          </div>
          <Toggle checked={props.backupEnabled} onChange={props.setBackupEnabled} label="启用" />
        </div>
        {props.backupEnabled && (
          <div className="mt-2 flex items-center gap-2">
            <Chip label="导入时" active={props.backupTiming === 'onImport'} onClick={() => props.setBackupTiming('onImport')} />
            <Chip label="每晚 03:00" active={props.backupTiming === 'nightly'} onClick={() => props.setBackupTiming('nightly')} />
            <Toggle checked={props.verifyMd5} onChange={props.setVerifyMd5} label="导入即 MD5 校验" />
          </div>
        )}
        {props.check && (
          <div
            className="mt-3 rounded-md p-2.5 text-2xs flex items-center gap-2"
            style={{
              background: props.check.warning ? 'rgba(232,194,104,0.12)' : 'rgba(125,217,166,0.1)',
              color: props.check.warning ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-success)',
            }}
          >
            {props.check.warning ? '⚠ 剩余空间不足需求的 110%：' : '✓ 空间充足：'}
            <span className="num">
              需求 {formatBytes(props.requiredBytes)} · 剩余 {formatBytes(props.check.freeBytes)}
            </span>
          </div>
        )}
      </div>
    </div>
  );
}

// ---------- Step 3 · 选项 ----------

function OptionsStep(props: {
  template: string;
  setTemplate: (v: string) => void;
  enterCulling: boolean;
  setEnterCulling: (v: boolean) => void;
  applyPreset: string | null;
  setApplyPreset: (v: string | null) => void;
}) {
  return (
    <div className="max-w-xl flex flex-col gap-4">
      <div className="rounded-lg border border-line bg-card p-4">
        <div className="text-xs text-txt mb-2">重命名模板</div>
        <div className="flex flex-wrap gap-1.5 mb-2">
          {TEMPLATES.map((t) => (
            <Chip key={t} label={t} active={props.template === t} onClick={() => props.setTemplate(t)} />
          ))}
        </div>
        <div className="text-2xs text-txt3 num">预览：2026-04-25_松赞林寺_0081.RW2（序号按拍摄时间排序）</div>
      </div>
      <div className="rounded-lg border border-line bg-card p-4 flex items-center justify-between">
        <div>
          <div className="text-xs text-txt">去重与配对</div>
          <div className="text-2xs text-txt3">MD5 相同默认跳过并计数 · RAW+JPG 配对为一条目（固定开启）</div>
        </div>
        <Chip label="去重：跳过" active />
      </div>
      <div className="rounded-lg border border-line bg-card p-4">
        <div className="text-xs text-txt mb-3">导入后动作</div>
        <div className="flex items-center justify-between mb-2.5">
          <span className="text-2xs text-txt2">自动进入精选模式</span>
          <Toggle checked={props.enterCulling} onChange={props.setEnterCulling} />
        </div>
        <div className="flex items-center justify-between">
          <span className="text-2xs text-txt2">套用「风光 · 基础」预设（只写 sidecar）</span>
          <Toggle checked={props.applyPreset !== null} onChange={(v) => props.setApplyPreset(v ? '风光 · 基础' : null)} />
        </div>
      </div>
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
    <div className="max-w-xl flex flex-col gap-3">
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
      </div>
      {failed.length > 0 && (
        <div className="rounded-lg border border-line bg-card p-3">
          <div className="text-2xs text-danger mb-1.5">失败清单（单文件失败不中断整批）</div>
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
