/**
 * 修图工作区左栏（v1.2.0 阶段 3）：导航器 / 预设 两张卡片；另提供右上「快照 / 图层」入口。
 * 历史继续复用既有 HistoryPanel，用同样的卡片外壳承载。
 */

import { useEffect, useState } from 'react';
import { useDevelop, normalizeParams } from '../../../store/developStore';
import { api } from '../../../api/engine';
import { errText, fileUrl } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import { Button } from '../../../components/primitives';
import { Dialog } from '../../../components/shell';
import { PanelCard } from './panelKit';
import LayersPanel from './LayersPanel';
import type { PresetInfo, SnapshotInfo } from '../../../api/protocol';

/** 导航器：底图缩略 + 取景框（裁剪开启时按 crop 边距收窄） */
export function NavigatorCard() {
  const baseImageUrl = useDevelop((s) => s.baseImageUrl);
  const params = useDevelop((s) => s.params);
  const g = params?.geometry;
  const inset = {
    top: g?.hasCrop ? Math.min(40, Math.max(0, g.cropTop ?? 0)) : 0,
    right: g?.hasCrop ? Math.min(40, Math.max(0, g.cropRight ?? 0)) : 0,
    bottom: g?.hasCrop ? Math.min(40, Math.max(0, g.cropBottom ?? 0)) : 0,
    left: g?.hasCrop ? Math.min(40, Math.max(0, g.cropLeft ?? 0)) : 0,
  };
  return (
    <PanelCard title="导航器" defaultOpen hint="取景">
      <div className="px-2 pb-1">
        <div className="relative rounded-mdC overflow-hidden" style={{ background: 'var(--Lumina-Tokens-viewer)', aspectRatio: '3 / 2' }}>
          {baseImageUrl ? (
            <img src={fileUrl(baseImageUrl)} alt="导航器" className="w-full h-full object-contain" style={{ display: 'block' }} />
          ) : (
            <div className="w-full h-full flex items-center justify-center text-2xs text-txt3">无底图</div>
          )}
          {(inset.top > 0 || inset.right > 0 || inset.bottom > 0 || inset.left > 0) && (
            <div
              className="absolute pointer-events-none"
              style={{
                top: `${inset.top}%`,
                right: `${inset.right}%`,
                bottom: `${inset.bottom}%`,
                left: `${inset.left}%`,
                border: '1px solid var(--Lumina-Tokens-accent)',
                borderRadius: 4,
              }}
            />
          )}
        </div>
      </div>
    </PanelCard>
  );
}

/** 预设：引擎预设列表 + 应用（失败不静默） */
export function PresetCard() {
  const [presets, setPresets] = useState<PresetInfo[]>([]);
  const [err, setErr] = useState<string | null>(null);
  const applyPreset = useDevelop((s) => s.applyPreset);
  const isPro = useDevelop((s) => s.mode) === 'pro';

  useEffect(() => {
    let cancelled = false;
    api.presets
      .list()
      .then((list) => {
        if (!cancelled) {
          setPresets(list);
          setErr(null);
        }
      })
      .catch((e) => {
        if (!cancelled) setErr(errText(e));
      });
    return () => {
      cancelled = true;
    };
  }, []);

  return (
    <PanelCard title="预设" defaultOpen hint={err ? '加载失败' : `${presets.length} 组`}>
      <div className="px-2 pb-1 flex flex-col gap-1">
        {err && <div className="text-2xs px-1" style={{ color: 'var(--Lumina-Tokens-danger)' }}>{err}</div>}
        {!err && presets.length === 0 && <div className="text-2xs text-txt3 px-1">无预设</div>}
        {isPro && presets.length > 0 && (
          <div className="text-2xs text-txt3 px-1 leading-4">专业模式为逐项参数视图，预设应用后参数即时可见</div>
        )}
        {presets.map((p) => (
          <button
            key={p.id}
            onClick={() => {
              void applyPreset(p.params, p.name).catch((e: unknown) => {
                useJobStore.getState().toast(`应用预设失败：${errText(e)}`, 'error', 6000);
              });
            }}
            className="h-6 px-2 rounded-mdC text-2xs text-txt2 hover:text-txt hover:bg-hover text-left truncate transition-colors"
            style={{ transitionTimingFunction: 'cubic-bezier(.2,.8,.2,1)' }}
            title={p.name}
          >
            {p.name}
          </button>
        ))}
      </div>
    </PanelCard>
  );
}

/**
 * 右上入口（02-6）：快照 / 图层。
 * 设计稿把这两个入口放在右栏顶部；这里做成两个 chip 按钮 + 弹层，避免重排既有面板。
 */
export function PanelEntries() {
  const photoId = useDevelop((s) => s.photoId);
  const stack = useDevelop((s) => s.params?.adjustmentStack ?? []);
  const [snapOpen, setSnapOpen] = useState(false);
  const [layerOpen, setLayerOpen] = useState(false);
  const [snaps, setSnaps] = useState<SnapshotInfo[]>([]);
  const [loading, setLoading] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    if (!snapOpen || photoId === null) return;
    setLoading(true);
    api.edit
      .getSnapshots(photoId)
      .then((list) => {
        setSnaps(list);
        setErr(null);
      })
      .catch((e) => setErr(errText(e)))
      .finally(() => setLoading(false));
  }, [snapOpen, photoId]);

  const chip = 'h-6 px-2 rounded-pill text-2xs border border-line text-txt2 hover:text-txt hover:bg-hover transition-colors';

  return (
    <div className="flex items-center gap-1.5 px-1">
      <button className={chip} onClick={() => setSnapOpen(true)} style={{ background: 'var(--Lumina-Tokens-card)' }}>
        快照 {snaps.length > 0 ? snaps.length : ''}
      </button>
      <button className={chip} onClick={() => setLayerOpen(true)} style={{ background: 'var(--Lumina-Tokens-card)' }}>
        图层 {stack.length}
      </button>
      <span className="flex-1" />

      <Dialog open={snapOpen} title="快照" onClose={() => setSnapOpen(false)} width={420}>
        <div className="flex items-center gap-2 mb-2">
          <Button
            size="sm"
            variant="primary"
            onClick={async () => {
              await useDevelop.getState().saveSnapshot(`快照 ${new Date().toLocaleTimeString('zh-Hans-CN', { hour: '2-digit', minute: '2-digit' })}`);
              if (photoId !== null) setSnaps(await api.edit.getSnapshots(photoId));
            }}
          >
            新建快照
          </Button>
          <span className="text-2xs text-txt3">{loading ? '加载中…' : `${snaps.length} 条`}</span>
        </div>
        {err && <div className="text-2xs mb-2" style={{ color: 'var(--Lumina-Tokens-danger)' }}>{err}</div>}
        {snaps.length === 0 && !err && <div className="text-2xs text-txt3 py-3">暂无快照</div>}
        <div className="flex flex-col gap-1 max-h-64 overflow-y-auto">
          {snaps.map((s) => (
            <div key={s.snapshotId} className="flex items-center gap-2 h-7 px-2 rounded-mdC hover:bg-hover">
              <span className="text-2xs text-txt2 flex-1 truncate">{s.name}</span>
              <span className="num text-2xs text-txt3">{new Date(s.createdAt).toLocaleString('zh-Hans-CN')}</span>
              <button
                className="text-2xs text-accent hover:opacity-80"
                onClick={async () => {
                  if (photoId === null) return;
                  try {
                    const res = await api.edit.restoreSnapshot(photoId, s.snapshotId);
                    await useDevelop.getState().applyPatch(normalizeParams(res.params), true, `恢复快照 · ${s.name}`);
                    setSnapOpen(false);
                  } catch (e) {
                    useJobStore.getState().toast(`恢复快照失败：${errText(e)}`, 'error', 6000);
                  }
                }}
              >
                恢复
              </button>
            </div>
          ))}
        </div>
      </Dialog>

      <Dialog open={layerOpen} title="调整图层" onClose={() => setLayerOpen(false)} width={460}>
        <LayersPanel />
      </Dialog>
    </div>
  );
}
