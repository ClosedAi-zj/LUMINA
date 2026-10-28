/**
 * 未定位照片面板（定位写入主入口）：
 * map.listUnlocated 分页列表（缩略图 thumb.ensureBatch 320px，与图库同规格）
 * → 多选 → 输入地点名称 + 经纬度（默认取已选地点 / 当前地图中心）→ map.setPhotoLocation。
 * 写入结果按零静默红线呈现：sidecarFailed>0 时警告 + warnings 逐条可展开。
 */

import { useEffect, useMemo, useState } from 'react';
import { useMap } from './store/mapStore';
import { api } from '../../api/engine';
import { errText, fileUrl } from '../../api/rpc-client';
import { useJobStore } from '../../store/jobStore';
import { Button, Spinner } from '../../components/primitives';
import { formatCount, formatDateTime } from '../../utils/format';
import type { MapWriteReport } from './store/mapStore';
import type { PhotoSummary, MapLocation } from '../../api/protocol';

const PAGE_SIZE = 60;

/** 当前地图中心（与 MapCanvas 包围盒一致的近似：全部顶级地点经纬度均值）。
 *  纯函数：locations 由调用方订阅后传入 —— 旧写法在渲染期 useMap.getState().locations，
 *  不订阅 → 地点数据加载/变化后这里永远算的是首帧的空列表（「地图暂无地点数据」常驻）。 */
function mapCenter(locs: MapLocation[]): { lat: number; lng: number } | null {
  if (locs.length === 0) return null;
  const lat = locs.reduce((a, l) => a + l.lat, 0) / locs.length;
  const lng = locs.reduce((a, l) => a + l.lng, 0) / locs.length;
  return { lat: Number(lat.toFixed(4)), lng: Number(lng.toFixed(4)) };
}

/** 单页照片行（缩略图经 ensureBatch 就绪后加载） */
function UnlocatedRow({
  photo,
  checked,
  onToggle,
}: {
  photo: PhotoSummary;
  checked: boolean;
  onToggle: () => void;
}) {
  const [src, setSrc] = useState<string | null>(null);
  useEffect(() => {
    let cancelled = false;
    api.thumb
      .ensureBatch([photo.id], 320)
      .then((res) => {
        const item = res.items.find((i) => i.id === photo.id);
        if (!cancelled && item?.state === 'ready' && item.path) setSrc(item.path);
      })
      .catch(() => {
        /* 缩略图失败仅显示占位，不阻断定位流程 */
      });
    return () => {
      cancelled = true;
    };
  }, [photo.id]);
  return (
    <button
      onClick={onToggle}
      className={`w-full flex items-center gap-2 px-2.5 py-1.5 text-left transition-colors border-b border-line ${
        checked ? 'bg-hover' : 'hover:bg-hover'
      }`}
    >
      <span
        className={`w-3.5 h-3.5 rounded-sm border shrink-0 flex items-center justify-center text-[9px] ${
          checked ? 'border-accent bg-accent text-onaccent' : 'border-line'
        }`}
      >
        {checked ? '✓' : ''}
      </span>
      <span className="w-14 h-9 rounded-sm overflow-hidden shrink-0 flex items-center justify-center" style={{ background: 'var(--Lumina-Tokens-card)' }}>
        {src ? <img src={fileUrl(src)} alt="" className="w-full h-full object-cover" draggable={false} /> : <Spinner size={10} />}
      </span>
      <span className="flex-1 min-w-0">
        <span className="block text-2xs text-txt truncate">{photo.fileName}</span>
        <span className="block num text-2xs text-txt3">{formatDateTime(photo.capturedAt)}</span>
      </span>
    </button>
  );
}

/** 写入结果条（零静默：sidecarFailed>0 红字 + 警告逐条可展开） */
function WriteReportStrip({ report }: { report: MapWriteReport }) {
  const [expanded, setExpanded] = useState(report.sidecarFailed > 0);
  const bad = report.sidecarFailed > 0;
  return (
    <div
      className="rounded-md border p-2 flex flex-col gap-1"
      style={{
        background: 'var(--Lumina-Tokens-card)',
        borderColor: bad ? 'var(--Lumina-Tokens-danger)' : 'var(--Lumina-Tokens-line)',
      }}
    >
      <div className="flex items-center gap-1.5">
        <span className="text-2xs" style={{ color: bad ? 'var(--Lumina-Tokens-danger)' : 'var(--Lumina-Tokens-success)' }}>
          {bad ? '⚠' : '✓'}
        </span>
        <span className="text-2xs text-txt2">
          {report.action}：已更新 <span className="num">{report.updated}</span> 张
          {bad && (
            <>
              ，<span className="num" style={{ color: 'var(--Lumina-Tokens-danger)' }}>{report.sidecarFailed}</span> 张未写入 XMP sidecar
            </>
          )}
        </span>
        {(bad || report.warnings.length > 0) && (
          <button className="ml-auto text-2xs text-accent hover:opacity-80 shrink-0" onClick={() => setExpanded((v) => !v)}>
            {expanded ? '收起明细' : `展开明细（${report.warnings.length}）`}
          </button>
        )}
      </div>
      {expanded && (
        <div className="flex flex-col gap-0.5">
          {report.warnings.length === 0 ? (
            <span className="text-2xs text-txt3">无警告 · GPS 已同时写入库与 sidecar</span>
          ) : (
            report.warnings.map((w, i) => (
              <span key={i} className="text-2xs leading-4" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
                · {w}
              </span>
            ))
          )}
        </div>
      )}
    </div>
  );
}

export default function UnlocatedPanel() {
  const unlocatedCount = useMap((s) => s.unlocatedCount);
  const selectedId = useMap((s) => s.selectedId);
  const detailsById = useMap((s) => s.detailsById);
  const writeReport = useMap((s) => s.writeReport);
  const assigning = useMap((s) => s.assigning);
  /** 订阅而非 getState()：地图地点变化（加载/新增/删除）后坐标默认值要跟着重算 */
  const locations = useMap((s) => s.locations);

  const [items, setItems] = useState<PhotoSummary[]>([]);
  const [total, setTotal] = useState(0);
  const [loading, setLoading] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [checkedIds, setCheckedIds] = useState<Set<number>>(new Set());

  const selectedDetail = selectedId !== null ? detailsById[selectedId] : undefined;
  const center = useMemo(() => mapCenter(locations), [locations, selectedId, items.length]);
  const defaultLat = selectedDetail ? selectedDetail.lat : center?.lat;
  const defaultLng = selectedDetail ? selectedDetail.lng : center?.lng;

  const [name, setName] = useState('');
  const [latText, setLatText] = useState(defaultLat !== undefined ? String(defaultLat) : '');
  const [lngText, setLngText] = useState(defaultLng !== undefined ? String(defaultLng) : '');
  const [altText, setAltText] = useState('');

  // 已选地点变化时同步坐标默认值（「合并到该地点」的直读值）
  useEffect(() => {
    if (selectedDetail) {
      setLatText(String(selectedDetail.lat));
      setLngText(String(selectedDetail.lng));
      if (!name) setName(selectedDetail.name);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedId]);

  // 首次打开拉取第一页
  useEffect(() => {
    void loadPage(0, true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  async function loadPage(offset: number, replace: boolean) {
    setLoading(true);
    setLoadError(null);
    try {
      const res = await api.map.listUnlocated({ offset, limit: PAGE_SIZE });
      setTotal(res.total);
      setItems((prev) => (replace ? res.items : [...prev, ...res.items]));
    } catch (err) {
      setLoadError(errText(err));
    } finally {
      setLoading(false);
    }
  }

  const toggleOne = (id: number) => {
    setCheckedIds((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  };
  const selectAllPage = () => setCheckedIds((prev) => new Set([...prev, ...items.map((p) => p.id)]));
  const invertPage = () =>
    setCheckedIds((prev) => {
      const next = new Set<number>();
      for (const p of items) if (!prev.has(p.id)) next.add(p.id);
      for (const id of prev) if (!items.some((p) => p.id === id)) next.add(id);
      return next;
    });

  const parseCoord = (text: string, min: number, max: number): number | null => {
    const v = Number.parseFloat(text);
    if (!Number.isFinite(v) || v < min || v > max) return null;
    return v;
  };

  const doAssign = async (useSelected: boolean) => {
    const ids = [...checkedIds];
    let lat: number | null;
    let lng: number | null;
    let locName: string;
    if (useSelected && selectedDetail) {
      lat = selectedDetail.lat;
      lng = selectedDetail.lng;
      locName = selectedDetail.name;
    } else {
      lat = parseCoord(latText, -90, 90);
      lng = parseCoord(lngText, -180, 180);
      locName = name;
    }
    if (ids.length === 0) {
      useJobStore.getState().toast('请先勾选要定位的照片', 'warn', 4000);
      return;
    }
    if (lat === null || lng === null) {
      useJobStore.getState().toast('经纬度无效（纬度 -90..90，经度 -180..180）', 'error', 6000);
      return;
    }
    const alt = altText.trim() === '' ? undefined : Number.parseFloat(altText);
    const ok = await useMap.getState().assignLocation(ids, lat, lng, Number.isFinite(alt as number) ? (alt as number) : undefined, locName);
    if (ok) {
      // 已定位的照片从未定位列表移除，重拉第一页
      setCheckedIds(new Set());
      await loadPage(0, true);
    }
  };

  return (
    <div className="absolute left-0 top-0 bottom-0 w-96 z-20 border-r border-line flex flex-col shadow-2xl" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      {/* 头部 */}
      <div className="flex items-center h-9 px-3 border-b border-line shrink-0">
        <span className="text-xs font-medium">未定位照片</span>
        <span className="num text-2xs text-txt3 ml-1.5">{unlocatedCount === null ? '…' : formatCount(unlocatedCount)} 张</span>
        <div className="flex-1" />
        <button className="text-txt3 hover:text-txt text-sm px-1" title="关闭（Esc）" onClick={() => useMap.getState().closeUnlocated()}>
          ✕
        </button>
      </div>

      {/* 列表 */}
      <div className="flex-1 min-h-0 overflow-y-auto">
        {loadError && (
          <div className="p-3 text-2xs" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
            未定位列表加载失败：{loadError}
            <button className="ml-1.5 text-accent" onClick={() => void loadPage(0, true)}>
              重试
            </button>
          </div>
        )}
        {!loadError && items.length === 0 && !loading && (
          <div className="p-6 text-center text-2xs text-txt3 leading-5">
            没有未定位照片
            <br />
            全部照片都已有 GPS（或在图库用 GPS 筛选补定位）
          </div>
        )}
        {items.map((p) => (
          <UnlocatedRow key={p.id} photo={p} checked={checkedIds.has(p.id)} onToggle={() => toggleOne(p.id)} />
        ))}
        {loading && (
          <div className="flex justify-center py-3">
            <Spinner size={14} />
          </div>
        )}
        {!loading && items.length < total && (
          <div className="flex justify-center py-2">
            <Button size="sm" onClick={() => void loadPage(items.length, false)}>
              加载更多（{formatCount(items.length)}/{formatCount(total)}）
            </Button>
          </div>
        )}
      </div>

      {/* 写入结果（零静默明细条） */}
      {writeReport && (
        <div className="px-2.5 pt-2 shrink-0">
          <WriteReportStrip report={writeReport} />
        </div>
      )}

      {/* 批量选择条 */}
      <div className="shrink-0 border-t border-line px-2.5 py-1.5 flex items-center gap-2 text-2xs">
        <span className="num text-txt2">已选 {checkedIds.size}</span>
        <div className="flex-1" />
        <button className="text-txt3 hover:text-txt" onClick={selectAllPage}>
          全选本页
        </button>
        <button className="text-txt3 hover:text-txt" onClick={invertPage}>
          反选
        </button>
        <button className="text-txt3 hover:text-txt" onClick={() => setCheckedIds(new Set())}>
          清空
        </button>
      </div>

      {/* 操作条：地点名称 + 经纬度 → 指定地点 / 合并到已选地点 */}
      <div className="shrink-0 border-t border-line p-2.5 flex flex-col gap-2" style={{ background: 'var(--Lumina-Tokens-card)' }}>
        <input
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder="地点名称（必填，如：梅里雪山观景台）"
          className="h-7 px-2 rounded-md text-xs outline-none border border-line focus:border-accent"
          style={{ background: 'var(--Lumina-Tokens-panel)', color: 'var(--Lumina-Tokens-textPrimary)' }}
        />
        <div className="flex items-center gap-1.5">
          <input
            value={latText}
            onChange={(e) => setLatText(e.target.value)}
            placeholder="纬度"
            title="纬度 -90..90"
            className="num h-7 px-2 rounded-md text-2xs outline-none border border-line focus:border-accent w-full"
            style={{ background: 'var(--Lumina-Tokens-panel)', color: 'var(--Lumina-Tokens-textPrimary)' }}
          />
          <input
            value={lngText}
            onChange={(e) => setLngText(e.target.value)}
            placeholder="经度"
            title="经度 -180..180"
            className="num h-7 px-2 rounded-md text-2xs outline-none border border-line focus:border-accent w-full"
            style={{ background: 'var(--Lumina-Tokens-panel)', color: 'var(--Lumina-Tokens-textPrimary)' }}
          />
          <input
            value={altText}
            onChange={(e) => setAltText(e.target.value)}
            placeholder="海拔 m"
            title="海拔（可选）"
            className="num h-7 px-2 rounded-md text-2xs outline-none border border-line focus:border-accent w-20 shrink-0"
            style={{ background: 'var(--Lumina-Tokens-panel)', color: 'var(--Lumina-Tokens-textPrimary)' }}
          />
        </div>
        <div className="text-2xs text-txt3">
          {defaultLat !== undefined ? (
            <>
              坐标默认值：{selectedDetail ? `已选地点「${selectedDetail.name}」` : '当前地图中心'}
            </>
          ) : (
            '地图暂无地点数据，请手动输入经纬度'
          )}
        </div>
        <div className="flex items-center gap-1.5">
          <Button variant="primary" size="sm" disabled={assigning} onClick={() => void doAssign(false)}>
            {assigning ? '写入中…' : `指定地点（${checkedIds.size} 张）`}
          </Button>
          {selectedDetail && (
            <Button size="sm" disabled={assigning || checkedIds.size === 0} onClick={() => void doAssign(true)} title={`使用「${selectedDetail.name}」的坐标与名称`}>
              合并到该地点
            </Button>
          )}
          <div className="flex-1" />
          <span className="text-2xs text-txt3">写 sidecar exif: · 原片零写入</span>
        </div>
      </div>
    </div>
  );
}
