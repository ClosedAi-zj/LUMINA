/**
 * 搜索框（防抖 200ms → library.search）+ 快捷筛选 chips。
 * 生效筛选 chips（单个可移除）在 FilterChips 组件（PRD §5-①）。
 */

import { useEffect, useRef, useState } from 'react';
import { useLibrary } from '../../store/libraryStore';
import { useUi } from '../../store/uiStore';
import { useDebounced } from '../../api/hooks';
import { Chip } from '../../components/primitives';
import { Icon } from '../../components/shell';
import type { Filter, PhotoFormat } from '../../api/protocol';

const QUICK: { key: string; label: string; patch: Filter; match: (f: Filter) => boolean }[] = [
  { key: 'all', label: '全部', patch: {}, match: (f) => Object.keys(f).length === 0 },
  { key: 'rated', label: '已评级', patch: { minRating: 1 }, match: (f) => f.minRating === 1 },
  { key: 'raw', label: 'RAW', patch: { formats: ['RW2', 'DNG', 'ARW'] }, match: (f) => (f.formats ?? []).length === 3 },
  { key: 'edited', label: '已编辑', patch: { edited: true }, match: (f) => f.edited === true },
  { key: 'gps', label: 'GPS', patch: { located: true }, match: (f) => f.located === true },
  { key: 'video', label: '视频', patch: { isVideo: true }, match: (f) => f.isVideo === true },
  { key: 'missing', label: '缺失', patch: { missing: true }, match: (f) => f.missing === true },
];

const FORMAT_LABEL: Record<string, string> = { RW2: 'RW2', JPG: 'JPG', DNG: 'DNG', ARW: 'ARW', MP4: 'MP4', PSD: 'PSD', PNG: 'PNG' };

export default function SearchBar() {
  const [text, setInput] = useState('');
  const debounced = useDebounced(text, 200);
  const inputRef = useRef<HTMLInputElement>(null);
  const focusTick = useUi((s) => s.searchFocusTick);

  useEffect(() => {
    if (debounced !== useLibrary.getState().searchText) {
      useLibrary.getState().setSearchText(debounced);
      void useLibrary.getState().refresh();
    }
  }, [debounced]);

  // 全局 Ctrl+F（App 已切换到图库视图后聚焦搜索框）
  useEffect(() => {
    if (focusTick > 0) inputRef.current?.focus();
  }, [focusTick]);

  return (
    <div className="flex items-center gap-2">
      <div className="relative flex-1 max-w-md">
        <span className="absolute left-2 top-1/2 -translate-y-1/2 text-txt3">
          <Icon name="search" size={13} />
        </span>
        <input
          ref={inputRef}
          value={text}
          onChange={(e) => setInput(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Escape') {
              setInput('');
              inputRef.current?.blur();
            }
          }}
          placeholder="搜索照片、文件夹、关键词（文件名 / 地点 / 日期）"
          className="w-full h-7 pl-7 pr-7 rounded-md border border-line bg-card text-xs text-txt placeholder:text-txt3 focus:outline-none focus:border-accent"
        />
        {text && (
          <button className="absolute right-2 top-1/2 -translate-y-1/2 text-txt3 hover:text-txt" onClick={() => setInput('')} aria-label="清空搜索">
            ✕
          </button>
        )}
      </div>
      <div className="flex items-center gap-1.5 flex-wrap">
        {QUICK.map((q) => (
          <Chip
            key={q.key}
            label={q.label}
            active={q.match(useLibrary.getState().filter)}
            onClick={() => {
              if (q.match(useLibrary.getState().filter) && q.key !== 'all') {
                useLibrary.getState().clearFilter();
              } else {
                useLibrary.getState().setActiveSmart(null, q.patch);
              }
            }}
          />
        ))}
        <FormatFacetChips />
        <SortChip />
      </div>
    </div>
  );
}

/** 生效筛选 chips（单维可移除，PRD §5-①） */
export function FilterChips() {
  const filter = useLibrary((s) => s.filter);
  const activeSmartId = useLibrary((s) => s.activeSmartId);
  const chips: { key: keyof Filter; label: string }[] = [];
  if (filter.folderIds) chips.push({ key: 'folderIds', label: '指定文件夹' });
  if (filter.rootIds) chips.push({ key: 'rootIds', label: '指定根目录' });
  if (filter.formats) chips.push({ key: 'formats', label: `格式：${filter.formats.map((f) => FORMAT_LABEL[f] ?? f).join('/')}` });
  if (filter.cameras) chips.push({ key: 'cameras', label: `机身：${filter.cameras.join('/')}` });
  if (filter.lenses) chips.push({ key: 'lenses', label: `镜头：${filter.lenses.join('/')}` });
  if (filter.minRating) chips.push({ key: 'minRating', label: `评分 ≥ ${filter.minRating}★` });
  if (filter.pick) chips.push({ key: 'pick', label: `旗标：${filter.pick}` });
  if (filter.located !== undefined) chips.push({ key: 'located', label: filter.located ? 'GPS 已定位' : '未定位' });
  if (filter.edited !== undefined) chips.push({ key: 'edited', label: filter.edited ? '已编辑' : '未编辑' });
  if (filter.missing !== undefined) chips.push({ key: 'missing', label: filter.missing ? '缺失' : '未缺失' });
  if (filter.isVideo !== undefined) chips.push({ key: 'isVideo', label: filter.isVideo ? '视频' : '照片' });
  if (filter.dateFrom || filter.dateTo)
    chips.push({
      key: 'dateFrom',
      label: `时间段：${filter.dateFrom ? new Date(filter.dateFrom).toISOString().slice(0, 10) : '…'} ~ ${filter.dateTo ? new Date(filter.dateTo).toISOString().slice(0, 10) : '…'}`,
    });
  if (filter.seqId) chips.push({ key: 'seqId', label: `序列 #${filter.seqId}` });

  if (chips.length === 0 && !activeSmartId) return null;
  return (
    <div className="flex items-center gap-1.5 pt-1.5 flex-wrap">
      <span className="text-2xs text-txt3">筛选：</span>
      {chips.map((c) => (
        <Chip key={c.key as string} label={c.label} active onRemove={() => useLibrary.getState().clearFilterKey(c.key)} />
      ))}
      <button className="text-2xs text-txt3 hover:text-txt ml-1" onClick={() => useLibrary.getState().clearFilter()}>
        清除全部
      </button>
    </div>
  );
}

function FormatFacetChips() {
  const facets = useLibrary((s) => s.facets);
  const filter = useLibrary((s) => s.filter);
  if (!facets || facets.formats.length === 0) return null;
  return (
    <>
      {facets.formats.slice(0, 4).map((f) => {
        const active = (filter.formats ?? []).includes(f.value as PhotoFormat) && (filter.formats ?? []).length === 1;
        return (
          <Chip
            key={f.value}
            label={f.value}
            count={f.count}
            active={active}
            onClick={() =>
              useLibrary
                .getState()
                .setFilter(active ? { ...useLibrary.getState().filter, formats: undefined } : { ...useLibrary.getState().filter, formats: [f.value as PhotoFormat] })
            }
          />
        );
      })}
    </>
  );
}

function SortChip() {
  const sort = useLibrary((s) => s.sort);
  const labels: Record<string, string> = { capturedAt: '拍摄时间', importedAt: '导入时间', fileName: '文件名', rating: '评分' };
  return (
    <button
      className="h-6 px-2 rounded-full border border-line bg-card text-2xs text-txt2 hover:text-txt hover:bg-hover"
      onClick={() => useLibrary.getState().setSort({ field: sort.field, order: sort.order === 'asc' ? 'desc' : 'asc' })}
      title="切换排序方向"
    >
      {labels[sort.field]} {sort.order === 'asc' ? '↑' : '↓'}
    </button>
  );
}
