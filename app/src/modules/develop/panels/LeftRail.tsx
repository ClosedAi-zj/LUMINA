/**
 * 修图工作区左栏（v1.2.0 阶段 3 皮肤；M2b-2 G4 常驻化）：
 * 导航器 / 预设 / 快照 / 图层 四张常驻卡片 + 历史（DevelopPage 承载）。
 * 左栏顺序 = 导航器 → 预设 → 快照 → 历史（R6 §3.2 LR 顺序）；
 * 快照/图层不再以 Dialog 为唯一入口（原「右上 chip + 弹层」PanelEntries 已移除，功能全部迁入常驻卡片）。
 */

import { useCallback, useEffect, useState } from 'react';
import { useDevelop, normalizeParams } from '../../../store/developStore';
import { api } from '../../../api/engine';
import { errText, fileUrl } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import { Button } from '../../../components/primitives';
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
    <PanelCard title="导航器" icon="navigator" defaultOpen hint="取景">
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
    <PanelCard title="预设" icon="preset" defaultOpen hint={err ? '加载失败' : `${presets.length} 组`}>
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
 * G4 快照常驻卡片（原右上 Dialog 迁移，功能不丢：新建 / 恢复 / 时间戳 / 失败不静默）。
 * photoId 变化自动刷新列表；保存后就地刷新。
 */
export function SnapshotCard() {
  const photoId = useDevelop((s) => s.photoId);
  const [snaps, setSnaps] = useState<SnapshotInfo[]>([]);
  const [loading, setLoading] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    if (photoId === null) {
      setSnaps([]);
      return;
    }
    setLoading(true);
    await api.edit
      .getSnapshots(photoId)
      .then((list) => {
        setSnaps(list);
        setErr(null);
      })
      .catch((e) => setErr(errText(e)))
      .finally(() => setLoading(false));
  }, [photoId]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  return (
    <PanelCard title="快照" icon="snapshot" defaultOpen hint={err ? '加载失败' : loading ? '加载中…' : `${snaps.length} 条`}>
      <div className="px-2 pb-1 flex flex-col gap-1">
        <Button
          size="sm"
          variant="primary"
          onClick={async () => {
            await useDevelop.getState().saveSnapshot(`快照 ${new Date().toLocaleTimeString('zh-Hans-CN', { hour: '2-digit', minute: '2-digit' })}`);
            await refresh();
          }}
        >
          新建快照
        </Button>
        {err && <div className="text-2xs px-1" style={{ color: 'var(--Lumina-Tokens-danger)' }}>{err}</div>}
        {snaps.length === 0 && !err && <div className="text-2xs text-txt3 py-1.5">暂无快照</div>}
        <div className="flex flex-col gap-px max-h-52 overflow-y-auto">
          {snaps.map((s) => (
            <div key={s.snapshotId} className="flex items-center gap-2 h-7 px-2 rounded-mdC hover:bg-hover shrink-0">
              <span className="text-2xs text-txt2 flex-1 truncate">{s.name}</span>
              <span className="num text-2xs text-txt3">{new Date(s.createdAt).toLocaleString('zh-Hans-CN')}</span>
              <button
                className="text-2xs text-accent hover:opacity-80 shrink-0"
                onClick={async () => {
                  if (photoId === null) return;
                  try {
                    const res = await api.edit.restoreSnapshot(photoId, s.snapshotId);
                    await useDevelop.getState().applyPatch(normalizeParams(res.params), true, `恢复快照 · ${s.name}`);
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
      </div>
    </PanelCard>
  );
}

/** G4 调整图层常驻卡片（原右上 Dialog 迁移；栈内容复用 LayersPanel） */
export function LayersCard() {
  return (
    <PanelCard title="图层" icon="layers" defaultOpen hint="调整图层栈">
      <LayersPanel />
    </PanelCard>
  );
}
