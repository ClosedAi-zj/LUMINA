/**
 * 地图画布（观察区恒色 #08090B · 红线 R5）：纯 SVG 抽象底图 ——
 * 经纬度等距投影 + 经纬网 + 装饰等高线（确定性生成）+ 地点散点（半径按张数缩放）。
 * 无外部地图 SDK、无网络瓦片（v1.0.0 设计决策）；本地滚轮缩放 + 拖拽平移 + hover/点选。
 */

import { useEffect, useMemo, useRef, useState } from 'react';
import type { MapLocation } from '../../api/protocol';

interface Props {
  locations: MapLocation[];
  selectedId: number | null;
  onSelect: (id: number | null) => void;
}

interface Projected {
  location: MapLocation;
  x: number;
  y: number;
  r: number;
}

/** 半径按张数缩放（sqrt 压缩数量级差异） */
function dotRadius(count: number): number {
  return Math.min(16, 4 + Math.sqrt(Math.max(count, 1)) * 0.9);
}

export default function MapCanvas({ locations, selectedId, onSelect }: Props) {
  const containerRef = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ w: 800, h: 520 });
  const [view, setView] = useState({ k: 1, tx: 0, ty: 0 });
  const [hoverId, setHoverId] = useState<number | null>(null);
  const dragRef = useRef<{ startX: number; startY: number; tx: number; ty: number; moved: boolean } | null>(null);
  /**
   * 「本次 pointer 手势是否发生过移动」的独立标记。
   * 不能在 click 里读 dragRef：pointerup 先于 click，那时 dragRef 已被清空，
   * 于是每次「拖拽松手」都会被判成空白点击 → onSelect(null) 把刚选中的地点清掉。
   * 语义：pointerdown 重置，pointermove 越阈值置真，click 消费后清除。
   */
  const movedRef = useRef(false);

  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const ro = new ResizeObserver((entries) => {
      const r = entries[0].contentRect;
      setSize({ w: r.width, h: r.height });
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // ---- 投影：所有点（含子散点）的包围盒 → 画布，等距投影留 12% 边距 ----
  const { points, dLng } = useMemo(() => {
    const all: MapLocation[] = locations.flatMap((l) => (l.children && view.k === 1 ? l.children : [l]));
    const lats = all.map((l) => l.lat);
    const lngs = all.map((l) => l.lng);
    const defaultBounds = { minLat: 20, maxLat: 45, minLng: 95, maxLng: 115 };
    const bounds = all.length
      ? {
          minLat: Math.min(...lats),
          maxLat: Math.max(...lats),
          minLng: Math.min(...lngs),
          maxLng: Math.max(...lngs),
        }
      : defaultBounds;
    // 单点 / 极小范围兜底：保证有可用跨度
    const padLat = Math.max((bounds.maxLat - bounds.minLat) * 0.12, 0.5);
    const padLng = Math.max((bounds.maxLng - bounds.minLng) * 0.12, 0.5);
    const b = {
      minLat: bounds.minLat - padLat,
      maxLat: bounds.maxLat + padLat,
      minLng: bounds.minLng - padLng,
      maxLng: bounds.maxLng + padLng,
    };
    const dLat = Math.max(b.maxLat - b.minLat, 1e-6);
    const dLng = Math.max(b.maxLng - b.minLng, 1e-6);
    const proj = (lat: number, lng: number) => ({
      x: ((lng - b.minLng) / dLng) * size.w,
      y: ((b.maxLat - lat) / dLat) * size.h,
    });
    const pts: Projected[] = all.map((l) => {
      const { x, y } = proj(l.lat, l.lng);
      return { location: l, x, y, r: dotRadius(l.count) };
    });
    return { points: pts, dLng };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [locations, size, view.k]);

  // ---- 装饰等高线（确定性伪随机，纯视觉） ----
  const contours = useMemo(() => {
    const { w, h } = size;
    return Array.from({ length: 8 }, (_, i) => {
      const y0 = ((i + 0.6) / 9) * h;
      const segs: string[] = [];
      for (let j = 0; j <= 24; j++) {
        const x = (j / 24) * w;
        const y = y0 + Math.sin(j * 0.55 + i * 1.7) * 16 + Math.sin(j * 0.21 + i * 0.9) * 26;
        segs.push(`${j === 0 ? 'M' : 'L'}${x.toFixed(1)},${y.toFixed(1)}`);
      }
      return segs.join(' ');
    });
  }, [size]);

  // ---- 经纬网 ----
  const grid = useMemo(() => {
    const { w, h } = size;
    const lines: { x1: number; y1: number; x2: number; y2: number }[] = [];
    for (let i = 1; i < 6; i++) {
      lines.push({ x1: (i / 6) * w, y1: 0, x2: (i / 6) * w, y2: h });
    }
    for (let i = 1; i < 4; i++) {
      lines.push({ x1: 0, y1: (i / 4) * h, x2: w, y2: (i / 4) * h });
    }
    return lines;
  }, [size]);

  // ---- 交互：滚轮缩放（视觉层）、拖拽平移、点选 ----
  const onWheel = (e: React.WheelEvent) => {
    e.preventDefault();
    const factor = e.deltaY < 0 ? 1.15 : 1 / 1.15;
    setView((v) => ({ ...v, k: Math.max(1, Math.min(6, v.k * factor)) }));
  };

  const onPointerDown = (e: React.PointerEvent) => {
    (e.target as Element).setPointerCapture?.(e.pointerId);
    dragRef.current = { startX: e.clientX, startY: e.clientY, tx: view.tx, ty: view.ty, moved: false };
    movedRef.current = false;
  };
  const onPointerMove = (e: React.PointerEvent) => {
    const d = dragRef.current;
    if (!d) return;
    const dx = e.clientX - d.startX;
    const dy = e.clientY - d.startY;
    if (Math.abs(dx) + Math.abs(dy) > 3) {
      d.moved = true;
      movedRef.current = true;
    }
    setView((v) => ({ ...v, tx: d.tx + dx, ty: d.ty + dy }));
  };
  const onPointerUp = () => {
    // 只清 dragRef（拖拽态），movedRef 留给随后的 click 判定
    dragRef.current = null;
  };

  /** click 消费一次「是否拖拽过」：读后即清，避免残留标记影响下一次点击 */
  const consumeMoved = (): boolean => {
    const moved = movedRef.current;
    movedRef.current = false;
    return moved;
  };

  const handleClick = () => {
    if (!consumeMoved()) onSelect(null);
  };

  // 比例尺：经度方向每像素对应公里（等距投影近似 111.32 km/°）
  const kmPer100px = Math.max(1, Math.round((dLng / Math.max(size.w, 1)) * 111.32 * 100));

  const selected = points.find((p) => (p.location.locationId ?? -1) === selectedId) ?? null;
  const hovered = points.find((p) => (p.location.locationId ?? -2) === hoverId) ?? null;
  const labelPoint = hovered ?? selected;

  return (
    <div className="relative flex-1 min-h-0 overflow-hidden" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
      <div
        ref={containerRef}
        className="absolute inset-0 cursor-grab active:cursor-grabbing"
        onWheel={onWheel}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onClick={handleClick}
      >
        <svg width={size.w} height={size.h} className="block select-none">
          {/* 底图变换层（拖拽平移 + 滚轮缩放） */}
          <g transform={`translate(${view.tx} ${view.ty}) scale(${view.k})`}>
            {grid.map((l, i) => (
              <line key={i} x1={l.x1} y1={l.y1} x2={l.x2} y2={l.y2} className="stroke-line" strokeWidth={1} />
            ))}
            {contours.map((d, i) => (
              <path key={i} d={d} fill="none" className="stroke-txt3" strokeOpacity={0.07} strokeWidth={1} />
            ))}
            {points.map((p) => {
              const id = p.location.locationId ?? -1;
              const isSel = id === selectedId;
              return (
                <g
                  key={id}
                  className="cursor-pointer"
                  onPointerEnter={() => setHoverId(id)}
                  onPointerLeave={() => setHoverId((h) => (h === id ? null : h))}
                  onClick={(e) => {
                    e.stopPropagation();
                    if (!consumeMoved()) onSelect(id);
                  }}
                >
                  {isSel && <circle cx={p.x} cy={p.y} r={p.r + 6} fill="none" className="stroke-accent" strokeWidth={1.5} strokeOpacity={0.9} />}
                  <circle cx={p.x} cy={p.y} r={p.r} className="fill-accent" fillOpacity={isSel ? 1 : 0.82} />
                  <circle cx={p.x} cy={p.y} r={p.r * 2.4} fill="transparent" />
                </g>
              );
            })}
          </g>

          {/* 标签层（不随缩放变换，字号稳定） */}
          {labelPoint && (
            <g pointerEvents="none">
              {(() => {
                const lx = labelPoint.x * view.k + view.tx;
                const ly = labelPoint.y * view.k + view.ty - labelPoint.r - 10;
                const text = `${labelPoint.location.name} · ${labelPoint.location.count}`;
                const wpx = text.length * 6.4 + 14;
                return (
                  <>
                    <rect x={lx - wpx / 2} y={ly - 16} width={wpx} height={18} rx={4} className="fill-panel stroke-line" />
                    <text x={lx} y={ly - 4} textAnchor="middle" fontSize={10} className="fill-txt2 num">
                      {text}
                    </text>
                    <line x1={lx} y1={ly + 2} x2={lx} y2={ly + 8} className="stroke-line" />
                  </>
                );
              })()}
            </g>
          )}
        </svg>
      </div>

      {/* 比例尺 + 说明（左下） */}
      <div className="absolute left-3 bottom-3 flex items-center gap-2 pointer-events-none">
        <div className="w-[100px] h-px" style={{ background: 'var(--Lumina-Tokens-textMuted)' }} />
        <span className="text-2xs text-txt3 num">≈ {kmPer100px} km</span>
        <span className="text-2xs text-txt3">· 抽象投影</span>
      </div>

      {/* 右下署名（对应设计稿"抽象底图 · LUMINA"） */}
      <div className="absolute right-3 bottom-3 text-2xs text-txt3 pointer-events-none">抽象底图 · 无网络瓦片 · LUMINA v1.2.3</div>

      {/* 视图层缩放控制 */}
      <div className="absolute top-2 right-3 flex items-center gap-1 text-2xs text-txt3 rounded-md px-1.5 py-1 border border-line bg-panel/80">
        <button className="px-1 hover:text-txt" title="缩小" onClick={() => setView((v) => ({ ...v, k: Math.max(1, v.k / 1.4) }))}>
          −
        </button>
        <span className="num w-8 text-center">{view.k.toFixed(1)}×</span>
        <button className="px-1 hover:text-txt" title="放大" onClick={() => setView((v) => ({ ...v, k: Math.min(6, v.k * 1.4) }))}>
          +
        </button>
        {view.k > 1 && (
          <button className="px-1 hover:text-txt" title="复位视图" onClick={() => setView({ k: 1, tx: 0, ty: 0 })}>
            ⟳
          </button>
        )}
      </div>

      {locations.length === 0 && (
        <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
          <span className="text-2xs text-txt3">无定位照片 · 先在图库为照片写入 GPS</span>
        </div>
      )}
    </div>
  );
}
