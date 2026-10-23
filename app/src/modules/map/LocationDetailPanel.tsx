/**
 * 右侧地点详情面板：样张预览 + 坐标/海拔/张数/拍摄区间 + 收藏切换 +
 * 危险操作「清除该地点照片的定位」（clearPhotoLocation，二次确认，零静默上报）。
 * 数据来自 mapStore.detailsById（map.getLocationDetail）。
 */

import { useEffect, useState } from 'react';
import { useMap } from './store/mapStore';
import { api } from '../../api/engine';
import { errText, fileUrl } from '../../api/rpc-client';
import { useJobStore } from '../../store/jobStore';
import { useUi } from '../../store/uiStore';
import { Button, Spinner, Toggle } from '../../components/primitives';
import { formatCount, formatDateTime } from '../../utils/format';

/** 样张缩略（320px，与图库网格同规格） */
function SampleThumb({ photoId }: { photoId: number }) {
  const [src, setSrc] = useState<string | null>(null);
  useEffect(() => {
    let cancelled = false;
    setSrc(null);
    api.thumb
      .ensureBatch([photoId], 320)
      .then((res) => {
        const item = res.items.find((i) => i.id === photoId);
        if (!cancelled && item?.state === 'ready' && item.path) setSrc(item.path);
      })
      .catch((err) => useJobStore.getState().toast(`样张加载失败：${errText(err)}`, 'error', 6000));
    return () => {
      cancelled = true;
    };
  }, [photoId]);
  if (!src) {
    return (
      <div className="w-full h-36 rounded-md flex items-center justify-center" style={{ background: 'var(--Lumina-Tokens-card)' }}>
        <Spinner size={16} />
      </div>
    );
  }
  return <img src={fileUrl(src)} alt="地点样张" draggable={false} className="w-full h-36 rounded-md object-cover" />;
}

function fmtCoord(lat: number, lng: number): string {
  const ns = lat >= 0 ? 'N' : 'S';
  const ew = lng >= 0 ? 'E' : 'W';
  return `${Math.abs(lat).toFixed(4)}°${ns} · ${Math.abs(lng).toFixed(4)}°${ew}`;
}

export default function LocationDetailPanel() {
  const selectedId = useMap((s) => s.selectedId);
  const detailsById = useMap((s) => s.detailsById);
  const writeReport = useMap((s) => s.writeReport);
  const assigning = useMap((s) => s.assigning);
  const detail = selectedId !== null ? detailsById[selectedId] : undefined;
  const navigate = useUi((s) => s.navigate);
  const [clearing, setClearing] = useState(false);

  // Esc 关闭未定位面板（与地图画布选择清空互不干扰）
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && useMap.getState().unlocatedOpen) {
        useMap.getState().closeUnlocated();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  /** 危险操作：清除该地点全部照片的定位（写 sidecar，原片不动；二次确认 + 影响面提示） */
  const clearLocation = async () => {
    if (selectedId === null || !detail) return;
    const ok = await window.lumina.confirm({
      title: '清除定位',
      message: `将清除「${detail.name}」下 ${formatCount(detail.count)} 张照片的 GPS 定位（同时从库与 XMP sidecar 移除，原片文件不动）。此操作影响 ${formatCount(detail.count)} 张照片，确定继续？`,
      confirmLabel: '清除定位',
    });
    if (!ok) return;
    setClearing(true);
    const done = await useMap.getState().clearLocationPhotos(selectedId);
    setClearing(false);
    if (done) useMap.getState().select(null);
  };

  if (selectedId === null || !detail) {
    return (
      <aside className="shrink-0 w-[276px] border-l border-line flex flex-col items-center justify-center px-4" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <div className="text-txt3 text-3xl mb-2">◎</div>
        <div className="text-2xs text-txt3 text-center leading-5">点击地图散点或左侧地点，查看拍摄详情</div>
      </aside>
    );
  }

  return (
    <aside className="shrink-0 w-[276px] border-l border-line flex flex-col overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="p-3 flex flex-col gap-3">
        <SampleThumb photoId={detail.samplePhotoId} />
        <div>
          <div className="text-sm font-medium text-txt">{detail.name}</div>
          <div className="text-2xs text-txt3 mt-0.5">拍摄地 · {formatCount(detail.count)} 张</div>
        </div>
        <div className="rounded-md border border-line p-2.5 flex flex-col gap-1.5" style={{ background: 'var(--Lumina-Tokens-card)' }}>
          <div className="num text-2xs text-txt2">{fmtCoord(detail.lat, detail.lng)}</div>
          <div className="text-2xs text-txt3">
            海拔 <span className="num text-txt2">{formatCount(detail.altitude)} m</span> · <span className="num text-txt2">{formatCount(detail.count)}</span> 张
          </div>
          <div className="num text-2xs text-txt3">
            {formatDateTime(detail.timeSpan.from)} → {formatDateTime(detail.timeSpan.to)}
          </div>
        </div>
        <div className="flex items-center justify-between">
          <Toggle checked={detail.favorite} onChange={() => void useMap.getState().toggleFavorite()} label="收藏地点" />
        </div>
        <Button size="sm" onClick={() => navigate('library')} title="回图库按地点筛选照片">
          在图库中查看该地点
        </Button>

        {/* 定位写入结果上报（零静默：sidecarFailed>0 时红字 + 警告逐条展开） */}
        {writeReport && (
          <div
            className="rounded-md border p-2 flex flex-col gap-1"
            style={{
              background: 'var(--Lumina-Tokens-card)',
              borderColor: writeReport.sidecarFailed > 0 ? 'var(--Lumina-Tokens-danger)' : 'var(--Lumina-Tokens-line)',
            }}
          >
            <div className="text-2xs" style={{ color: writeReport.sidecarFailed > 0 ? 'var(--Lumina-Tokens-danger)' : 'var(--Lumina-Tokens-success)' }}>
              {writeReport.sidecarFailed > 0
                ? `⚠ ${writeReport.action}：${writeReport.updated} 张入库，${writeReport.sidecarFailed} 张未同步 XMP`
                : `✓ ${writeReport.action}：已更新 ${writeReport.updated} 张`}
            </div>
            {writeReport.warnings.map((w, i) => (
              <div key={i} className="text-2xs leading-4" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
                · {w}
              </div>
            ))}
          </div>
        )}

        {/* 危险区：清除定位（写 sidecar exif:，原片零写入；二次确认） */}
        <div className="rounded-md border border-line p-2.5 flex flex-col gap-1.5" style={{ background: 'var(--Lumina-Tokens-card)' }}>
          <div className="text-2xs text-txt2 font-medium">危险操作</div>
          <div className="text-2xs text-txt3 leading-4">清除该地点全部照片的 GPS（库与 sidecar 同步移除，原片文件不动）</div>
          <button
            className="h-7 rounded-md text-2xs font-medium disabled:opacity-40 disabled:cursor-not-allowed"
            style={{
              border: '1px solid var(--Lumina-Tokens-danger)',
              color: 'var(--Lumina-Tokens-danger)',
              background: 'transparent',
            }}
            disabled={clearing || assigning}
            onClick={() => void clearLocation()}
            title={`清除「${detail.name}」下 ${formatCount(detail.count)} 张照片的定位`}
          >
            {clearing ? '清除中…' : `清除该地点照片的定位（${formatCount(detail.count)} 张）`}
          </button>
        </div>
      </div>
    </aside>
  );
}
