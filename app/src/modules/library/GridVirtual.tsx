/**
 * 网格虚拟滚动（react-window FixedSizeGrid + 分页缓存 ±2 行预取）。
 * 18,192 张规模：可视区外不渲染 DOM；页大小 200；可视区自动 thumb.ensureBatch(320)。
 */

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { FixedSizeGrid, type GridChildComponentProps } from 'react-window';
import { useLibrary, ZOOM_MIN, ZOOM_MAX } from '../../store/libraryStore';
import type { PhotoSummary } from '../../api/protocol';
import GridItem from './GridItem';

const GAP = 12;

export default function GridVirtual() {
  const [size, setSize] = useState({ w: 800, h: 600 });
  const containerRef = useRef<HTMLDivElement>(null);
  const zoom = useLibrary((s) => s.zoom);
  const total = useLibrary((s) => s.total);
  const pages = useLibrary((s) => s.pages);
  const filterVersion = useLibrary((s) => s.filterVersion);
  const fetchPage = useLibrary((s) => s.fetchPage);
  const ensureThumbs = useLibrary((s) => s.ensureThumbs);

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

  const cellW = zoom;
  const cellH = Math.round(zoom * 0.68);
  const cols = Math.max(1, Math.floor((size.w + GAP) / (cellW + GAP)));
  const rowCount = Math.ceil(Math.max(total, 1) / cols);
  const rowOffsetRef = useRef(0);

  const itemAt = useCallback(
    (index: number): PhotoSummary | null => {
      const offset = Math.floor(index / 200) * 200;
      const page = pages[offset];
      if (!page) return null;
      return page[index - offset] ?? null;
    },
    [pages],
  );

  // 可视区 ±2 行：补页 + 缩略图预取
  const onItemsRendered = useCallback(
    (ir: {
      visibleRowStartIndex: number;
      visibleRowStopIndex: number;
    }) => {
      const rowStart = ir.visibleRowStartIndex;
      const rowStop = ir.visibleRowStopIndex;
      rowOffsetRef.current = rowStart;
      const startRow = Math.max(0, rowStart - 2);
      const endRow = rowStop + 2;
      const offsets = new Set<number>();
      const ids: number[] = [];
      for (let row = startRow; row <= endRow; row++) {
        for (let col = 0; col < cols; col++) {
          const index = row * cols + col;
          if (index >= total) break;
          offsets.add(Math.floor(index / 200) * 200);
          const item = itemAt(index);
          if (item) ids.push(item.id);
        }
      }
      for (const off of offsets) {
        if (!pages[off]) void fetchPage(off);
      }
      if (ids.length > 0) void ensureThumbs(ids.slice(0, 400), 320);
    },
    [cols, total, itemAt, pages, fetchPage, ensureThumbs],
  );

  // 筛选/缩放变化后立即补首屏
  useEffect(() => {
    if (total === 0) void fetchPage(0);
  }, [total, fetchPage, filterVersion]);

  const prefetchRange = useCallback(
    (rowStart: number, rowStop: number) => {
      const offsets = new Set<number>();
      const ids: number[] = [];
      const startRow = Math.max(0, rowStart - 2);
      const endRow = rowStop + 2;
      for (let row = startRow; row <= endRow; row++) {
        for (let col = 0; col < cols; col++) {
          const index = row * cols + col;
          if (index >= total) break;
          offsets.add(Math.floor(index / 200) * 200);
          const item = itemAt(index);
          if (item) ids.push(item.id);
        }
      }
      for (const off of offsets) {
        if (!pages[off]) void fetchPage(off);
      }
      if (ids.length > 0) void ensureThumbs(ids.slice(0, 400), 320);
    },
    [cols, total, itemAt, pages, fetchPage, ensureThumbs],
  );

  useEffect(() => {
    prefetchRange(rowOffsetRef.current, rowOffsetRef.current + Math.ceil(size.h / (cellH + GAP)));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pages, cols, cellH, size.h]);

  const Cell = useMemo(
    () =>
      function Cell({ columnIndex, rowIndex, style, data }: GridChildComponentProps) {
        const index = rowIndex * data.cols + columnIndex;
        const photo = index < data.total ? data.itemAt(index) : null;
        if (!photo) {
          return (
            <div style={style} className="pr-[12px] pb-[12px]">
              <div className="w-full h-full rounded-md" style={{ background: 'var(--Lumina-Tokens-card)' }} />
            </div>
          );
        }
        return (
          <div style={style} className="pr-[12px] pb-[12px]">
            <GridItem photo={photo} />
          </div>
        );
      },
    [],
  );

  return (
    <div ref={containerRef} className="h-full w-full overflow-hidden px-1 pt-1">
      <FixedSizeGrid
        key={`${cols}_${cellW}_${filterVersion}`}
        columnCount={cols}
        rowCount={rowCount}
        columnWidth={cellW + GAP}
        rowHeight={cellH + GAP}
        width={size.w}
        height={size.h}
        overscanRowsCount={2}
        overscanColumnsCount={2}
        onItemsRendered={onItemsRendered}
        itemKey={({ columnIndex, rowIndex }) => {
          const index = rowIndex * cols + columnIndex;
          const p = itemAt(index);
          return p ? p.id : `ph_${index}`;
        }}
        itemData={{ cols, total, itemAt, thumbUrls: useLibrary.getState().thumbUrls }}
      >
        {Cell}
      </FixedSizeGrid>
      {/* 缩放控制（120–320 连续） */}
      <div className="absolute top-2 right-3 flex items-center gap-1 text-txt3 text-2xs bg-panel/80 rounded-md px-1.5 py-1 border border-line">
        <button
          className="px-1 hover:text-txt"
          onClick={() => useLibrary.getState().setZoom(Math.max(ZOOM_MIN, useLibrary.getState().zoom - 24))}
          aria-label="缩小"
        >
          −
        </button>
        <span className="num w-8 text-center">{zoom}px</span>
        <button
          className="px-1 hover:text-txt"
          onClick={() => useLibrary.getState().setZoom(Math.min(ZOOM_MAX, useLibrary.getState().zoom + 24))}
          aria-label="放大"
        >
          +
        </button>
      </div>
    </div>
  );
}
