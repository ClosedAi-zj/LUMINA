/**
 * 修图台（帧 02 快速模式）：顶栏（模式切换 / XMP 写入指示灯 / undo/redo / 同步）+
 * 左 = 历史/快照/虚拟副本；中 = 画面（前后对比 \）；右 = 快速面板；底 = 胶片条。
 * 快速↔专业共享同一参数模型（PRD §6.1），切换零解码零闪黑。
 */

import { useCallback, useEffect, useState } from 'react';
import { useDevelop } from '../../store/developStore';
import { useLibrary } from '../../store/libraryStore';
import { useUi } from '../../store/uiStore';
import { useJobStore } from '../../store/jobStore';
import { errText } from '../../api/rpc-client';
import { Button, Segmented } from '../../components/primitives';
import { Dialog, EmptyState, KeycapBar } from '../../components/shell';
import ViewerCanvas from './ViewerCanvas';
import HistogramPanel, { type Histogram } from './panels/HistogramPanel';
import FastPanel from './panels/FastPanel';
import ProPanel from './panels/ProPanel';
import HistoryPanel from './panels/HistoryPanel';
import LayersPanel from './panels/LayersPanel';
import RangeMaskBar from './panels/RangeMaskBar';
import TransformBar from './panels/TransformBar';
import Filmstrip from '../library/Filmstrip';
import { fileUrl } from '../../api/rpc-client';
import { useMemo } from 'react';
import type { EditParams } from '../../api/protocol';

/** WP-B7：复制/粘贴设置的会话级剪贴板（LR 语义：修图设置独立于系统剪贴板） */
let settingsClipboard: EditParams | null = null;

export default function DevelopPage() {
  const photoId = useDevelop((s) => s.photoId);
  const mode = useDevelop((s) => s.mode);
  const xmpStatus = useDevelop((s) => s.xmpStatus);
  const syncMessage = useDevelop((s) => s.syncMessage);
  const [histogram, setHistogram] = useState<Histogram | null>(null);
  const navigate = useUi((s) => s.navigate);

  // 无选中照片 → 提示回图库选择
  if (photoId === null) {
    return (
      <div className="h-full">
        <EmptyState
          icon="✎"
          title="在图库中选择一张照片开始修图"
          desc="单击选中后按 Enter，或双击照片进入修图台。"
          action={
            <Button variant="primary" onClick={() => navigate('library')}>
              前往图库
            </Button>
          }
        />
      </div>
    );
  }

  return <DevelopLoaded mode={mode} xmpStatus={xmpStatus} syncMessage={syncMessage} onHistogram={setHistogram} histogram={histogram} navigate={navigate} photoId={photoId} />;
}

function DevelopLoaded({
  mode,
  xmpStatus,
  syncMessage,
  onHistogram,
  histogram,
  navigate,
  photoId,
}: {
  mode: 'fast' | 'pro';
  xmpStatus: string;
  syncMessage: string | null;
  onHistogram: (h: Histogram | null) => void;
  histogram: Histogram | null;
  navigate: (v: 'library') => void;
  photoId: number;
}) {
  const undo = useDevelop((s) => s.undo);
  const redo = useDevelop((s) => s.redo);
  const setMode = useDevelop((s) => s.setMode);
  const syncToSelection = useDevelop((s) => s.syncToSelection);
  const selectedIds = useLibrary((s) => s.selectedIds);
  const currentPhotoId = useLibrary((s) => s.currentPhotoId);
  const guides = useDevelop((s) => s.guides);
  const [syncTarget, setSyncTarget] = useState<{ seqId: number; count: number | null } | null>(null);
  // WP-B7：面板折叠记忆（localStorage 持久化）
  const [panels, setPanels] = useState<{ left: boolean; right: boolean }>(() => {
    try {
      const raw = localStorage.getItem('lumina.develop.panels');
      return raw ? (JSON.parse(raw) as { left: boolean; right: boolean }) : { left: true, right: true };
    } catch {
      return { left: true, right: true };
    }
  });
  const [showKeys, setShowKeys] = useState(false);
  const togglePanel = (side: 'left' | 'right') => {
    setPanels((prev) => {
      const next = { ...prev, [side]: !prev[side] };
      localStorage.setItem('lumina.develop.panels', JSON.stringify(next));
      return next;
    });
  };

  // 同步到序列：先查目标数量，弹确认框（列出目标数量）再执行
  const requestSyncConfirm = async (seqId: number) => {
    try {
      const { api } = await import('../../api/engine');
      const res = await api.library.queryPhotos({ seqId }, undefined, { offset: 0, limit: 1 });
      setSyncTarget({ seqId, count: Math.max(0, res.total - 1) });
    } catch (err) {
      setSyncTarget({ seqId, count: null });
      const { useJobStore } = await import('../../store/jobStore');
      useJobStore.getState().toast(`无法统计序列目标数量：${errText(err)}`, 'error', 6000);
    }
  };

  const onHist = useCallback(
    (h: Histogram) => {
      onHistogram(h);
    },
    [onHistogram],
  );

  // 修图台键盘：Ctrl+Z / Ctrl+Shift+Z；数字 1-5 评分；WP-B7：复制/粘贴设置、上/下一张循环、? 速查
  useEffect(() => {
    const onKey = async (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA') return;
      const ctrl = e.ctrlKey || e.metaKey;
      if (ctrl && e.key.toLowerCase() === 'z') {
        e.preventDefault();
        if (e.shiftKey) void redo();
        else void undo();
      } else if (ctrl && (e.key === "'" || e.key === '"')) {
        // 上/下一张循环（LR 习惯：Ctrl+' 下一张，Shift+Ctrl+' 上一张）
        e.preventDefault();
        const ids = Object.keys(useLibrary.getState().pages)
          .map(Number)
          .sort((a, b) => a - b)
          .flatMap((off) => (useLibrary.getState().pages[off] ?? []).map((p) => p.id));
        if (ids.length === 0) return;
        const idx = ids.indexOf(photoId);
        const next = e.shiftKey ? ids[(idx - 1 + ids.length) % ids.length] : ids[(idx + 1) % ids.length];
        if (next !== photoId) {
          await useDevelop.getState().openPhoto(next);
          useJobStore.getState().toast(e.shiftKey ? '上一张' : '下一张', 'info', 1200);
        }
      } else if (ctrl && e.key.toLowerCase() === 'c') {
        // 复制设置（LR 习惯：Ctrl+C / Ctrl+V；无选中文本时生效）
        const sel = window.getSelection();
        if (sel && sel.toString().length > 0) return;
        const params = useDevelop.getState().params;
        if (!params) return;
        settingsClipboard = structuredClone(params);
        useJobStore.getState().toast('已复制当前设置（Ctrl+V 粘贴到其他照片）', 'info', 2500);
      } else if (ctrl && e.key.toLowerCase() === 'v') {
        if (!settingsClipboard) {
          useJobStore.getState().toast('剪贴板里还没有设置：先在任一照片上 Ctrl+C 复制', 'warn', 3000);
          return;
        }
        await useDevelop.getState().applyPatch(structuredClone(settingsClipboard), true, '粘贴设置');
        useJobStore.getState().toast('已粘贴设置', 'success', 2000);
      } else if (e.key === '?') {
        e.preventDefault();
        setShowKeys((v) => !v);
      } else if (/^[1-5]$/.test(e.key) && currentPhotoId !== null) {
        await useLibrary.getState().select(currentPhotoId, 'set');
        const { api } = await import('../../api/engine');
        await api.library.updatePhotos([currentPhotoId], { rating: Number(e.key) });
        useJobStore.getState().toast(`已评 ${e.key} 星`, 'success', 2000);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [undo, redo, currentPhotoId, photoId]);

  return (
    <div className="h-full flex flex-col">
      {/* 修图台顶栏 */}
      <div className="flex items-center h-10 px-3 gap-3 border-b border-line shrink-0" style={{ background: 'var(--Lumina-Tokens-browserBar)' }}>
        <Button size="sm" variant="ghost" onClick={() => navigate('library')}>
          ← 图库
        </Button>
        <Segmented
          value={mode}
          onChange={(v) => setMode(v)}
          options={[
            { value: 'fast' as const, label: '快速' },
            { value: 'pro' as const, label: '专业 Pro' },
          ]}
        />
        {/* XMP 写入指示灯（500ms 防抖语义） */}
        <span className="inline-flex items-center gap-1.5 text-2xs" style={{ color: xmpStatus === 'written' ? 'var(--Lumina-Tokens-success)' : xmpStatus === 'debouncing' ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-textMuted)' }}>
          <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'currentColor' }} />
          {xmpStatus === 'written' ? '参数已写入 XMP' : xmpStatus === 'debouncing' ? '写入中…' : '参数未修改'}
        </span>
        {syncMessage && <span className="text-2xs text-accent">{syncMessage}</span>}
        <div className="flex-1" />
        <button
          className="h-6 px-2 rounded-sm border border-line text-2xs text-txt3 hover:text-txt2 hover:bg-hover"
          onClick={() => togglePanel('left')}
          title="折叠/展开左栏（记忆到本机）"
        >
          {panels.left ? '⟨ 左栏' : '⟩ 左栏'}
        </button>
        <button
          className="h-6 px-2 rounded-sm border border-line text-2xs text-txt3 hover:text-txt2 hover:bg-hover"
          onClick={() => togglePanel('right')}
          title="折叠/展开右栏（记忆到本机）"
        >
          {panels.right ? '右栏 ⟩' : '⟨ 右栏'}
        </button>
        <button className="h-6 px-2 rounded-sm text-2xs text-txt3 hover:text-txt2" onClick={() => setShowKeys(true)} title="快捷键速查（?）">
          ⌨ 快捷键
        </button>
        <span className="num text-2xs text-txt3">照片 #{photoId}</span>
        <Button size="sm" disabled={selectedIds.length === 0} onClick={() => void syncToSelection(selectedIds.filter((id) => id !== photoId))} title="将当前参数同步到所选照片">
          同步设置到所选{selectedIds.length > 1 ? `（${selectedIds.length - 1}）` : ''}
        </Button>
        <Button
          size="sm"
          onClick={() => {
            const st = useLibrary.getState();
            const item = Object.values(st.pages)
              .flat()
              .find((p) => p.id === photoId);
            const seqId = item?.seqId ?? null;
            if (seqId === null) {
              useJobStore.getState().toast('当前照片不属于任何序列', 'warn');
              return;
            }
            void requestSyncConfirm(seqId);
          }}
        >
          同步到序列
        </Button>
      </div>

      {mode === 'pro' ? (
        /* 专业模式：左 = 图层栈 + 历史；中 = 直方图 + 画面 + 范围蒙版/变换工具条；右 = 7 标签面板 */
        <div className="flex-1 min-h-0 flex">
          {panels.left && (
            <aside className="w-[300px] shrink-0 border-r border-line overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
              <LayersPanel />
              <HistoryPanel />
            </aside>
          )}
          <section className="flex-1 min-w-0 flex flex-col">
            <HistogramPanel histogram={histogram} />
            <div className="relative flex-1 min-h-0 flex flex-col">
              <div className="relative flex-1 min-h-0 flex">
                <ViewerCanvas onHistogram={onHist} />
                <GuidesOverlay />
              </div>
              <RangeMaskBar />
              <TransformBar />
            </div>
          </section>
          {panels.right && (
            <aside className="w-[340px] shrink-0 border-l border-line overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
              <ProPanel />
            </aside>
          )}
        </div>
      ) : (
        /* 快捷模式：Phase 1 形态原样保留 */
        <div className="flex-1 min-h-0 flex">
          {/* 左：历史/快照/虚拟副本 */}
          {panels.left && (
            <aside className="w-52 shrink-0 border-r border-line overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
              <HistoryPanel />
            </aside>
          )}

          {/* 中：直方图 + 画面 */}
          <section className="flex-1 min-w-0 flex flex-col">
            <HistogramPanel histogram={histogram} />
            <ViewerCanvas onHistogram={onHist} />
          </section>

          {/* 右：快速面板 */}
          {panels.right && (
            <aside className="w-[296px] shrink-0 border-l border-line overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
              <FastPanel />
            </aside>
          )}
        </div>
      )}

      <Filmstrip items={useFilmstripItems()} />
      <KeycapBar
        items={[
          { keys: ['\\'], label: '前后对比' },
          ...(mode === 'pro' ? [{ keys: ['Space'], label: '长按看原图' }] : []),
          { keys: ['Ctrl', 'Z'], label: '撤销' },
          { keys: ['Ctrl', '⇧', 'Z'], label: '重做' },
          { keys: ['Ctrl', "'"], label: '下一张' },
          { keys: ['Ctrl', 'C/V'], label: '复制/粘贴设置' },
          { keys: ['1-5'], label: '评分' },
          { keys: ['?'], label: '快捷键' },
          { keys: ['Esc'], label: '返回图库' },
        ]}
      />
      <EscHandler navigate={navigate} />

      {/* 同步到序列确认框（列出目标数量） */}
      <Dialog open={syncTarget !== null} title="同步参数到序列" onClose={() => setSyncTarget(null)}>
        <div className="text-xs text-txt2 leading-5">
          将把当前照片的<b className="text-txt">全部编辑参数</b>（含蒙版与调整图层）同步到同序列的其它照片。
          <div className="mt-2">
            目标数量：<span className="num text-accent">{syncTarget?.count ?? '统计中…'}</span> 张（不含当前照片）
          </div>
        </div>
        <div className="flex justify-end gap-2 mt-4">
          <Button onClick={() => setSyncTarget(null)}>取消</Button>
          <Button
            variant="primary"
            disabled={syncTarget === null || (syncTarget.count !== null && syncTarget.count === 0)}
            onClick={() => {
              if (syncTarget) void useDevelop.getState().syncToSequence(syncTarget.seqId);
              setSyncTarget(null);
            }}
          >
            确认同步
          </Button>
        </div>
      </Dialog>

      {/* WP-B7：? 快捷键速查浮层 */}
      <Dialog open={showKeys} title="修图台快捷键" onClose={() => setShowKeys(false)} width={480}>
        <div className="text-xs text-txt2 leading-6 grid grid-cols-[auto_1fr] gap-x-4">
          {SHORTCUT_TABLE.map((row) => (
            <div key={row.keys.join('+')} className="contents">
              <KeycapBar items={[{ keys: row.keys, label: '' }]} />
              <span>{row.label}</span>
            </div>
          ))}
        </div>
        <div className="text-2xs text-txt3 mt-3">完整快捷键表见 docs/ui-1.1/快捷键表.md（按 ? 随时唤出）</div>
      </Dialog>
    </div>
  );
}

/** WP-B7：快捷键表（UI 浮层与文档同源，DoD 要求表内按键全部生效） */
const SHORTCUT_TABLE: { keys: string[]; label: string }[] = [
  { keys: ['Ctrl', 'Z'], label: '撤销' },
  { keys: ['Ctrl', '⇧', 'Z'], label: '重做' },
  { keys: ['Ctrl', "'"], label: '下一张（循环）' },
  { keys: ['Ctrl', '⇧', "'"], label: '上一张（循环）' },
  { keys: ['Ctrl', 'C'], label: '复制当前设置' },
  { keys: ['Ctrl', 'V'], label: '粘贴设置' },
  { keys: ['\\'], label: '前后对比开/关' },
  { keys: ['Space'], label: '长按临时看原图（专业模式）' },
  { keys: ['1-5'], label: '评星' },
  { keys: ['?'], label: '本速查浮层' },
  { keys: ['Esc'], label: '返回图库' },
];

/** 参考线叠加层（三分/中心/网格，纯视觉，不落盘） */
function GuidesOverlay() {
  const guides = useDevelop((s) => s.guides);
  if (guides === 'off') return null;
  const line = (style: React.CSSProperties) => (
    <div className="absolute pointer-events-none" style={{ background: 'var(--Lumina-Tokens-accent)', opacity: 0.35, ...style }} />
  );
  return (
    <div className="absolute inset-0 pointer-events-none z-10">
      {guides === 'thirds' && (
        <>
          {line({ left: '33.33%', top: 0, bottom: 0, width: 1 })}
          {line({ left: '66.66%', top: 0, bottom: 0, width: 1 })}
          {line({ top: '33.33%', left: 0, right: 0, height: 1 })}
          {line({ top: '66.66%', left: 0, right: 0, height: 1 })}
        </>
      )}
      {guides === 'center' && (
        <>
          {line({ left: '50%', top: 0, bottom: 0, width: 1 })}
          {line({ top: '50%', left: 0, right: 0, height: 1 })}
        </>
      )}
      {guides === 'grid' && (
        <>
          {[1, 2, 3, 4, 5].map((i) => line({ left: `${(i / 6) * 100}%`, top: 0, bottom: 0, width: 1 }))}
          {[1, 2, 3].map((i) => line({ top: `${(i / 4) * 100}%`, left: 0, right: 0, height: 1 }))}
        </>
      )}
    </div>
  );
}

function useFilmstripItems() {
  const pages = useLibrary((s) => s.pages);
  const total = useLibrary((s) => s.total);
  const thumbUrls = useLibrary((s) => s.thumbUrls);
  void thumbUrls;
  return useMemo(() => {
    const offsets = Object.keys(pages)
      .map(Number)
      .sort((a, b) => a - b);
    if (offsets.length === 0) return [];
    const out: import('../../api/protocol').PhotoSummary[] = [];
    for (let off = 0; off <= offsets[offsets.length - 1]; off += 1) {
      const page = pages[off];
      if (page) out.push(...page);
      if (out.length >= total) break;
    }
    return out;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pages, total]);
}

function EscHandler({ navigate }: { navigate: (v: 'library') => void }) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        void useDevelop.getState().closePhoto();
        navigate('library');
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [navigate]);
  return null;
}

// fileUrl 引用保持（直方图采样等场景后续扩展使用）
void fileUrl;
