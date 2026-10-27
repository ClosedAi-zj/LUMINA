/**
 * 素材库主页（帧 01）：左 = 文件夹树 + 智能收藏夹 + 时间线；
 * 中 = 搜索/筛选 chips + 网格虚拟滚动；右 = 照片检查器；底 = 胶片条。
 * 键盘：方向键导航 / Enter 修图 / Delete 移除（5s 可撤销）/ Ctrl+A 全选。
 */

import { useEffect, useMemo } from 'react';
import { useLibrary } from '../../store/libraryStore';
import { useUi } from '../../store/uiStore';
import { useDevelop, MAX_DEVELOP_TABS } from '../../store/developStore';
import { api } from '../../api/engine';
import { errText } from '../../api/rpc-client';
import { useJobStore } from '../../store/jobStore';
import { useUndoToast } from '../../api/hooks';
import { formatCount } from '../../utils/format';
import { Dialog, KeycapBar } from '../../components/shell';
import { Button } from '../../components/primitives';
import { StateEmpty, StateLoading, StateError } from '../../components/states';
import { useState } from 'react';
import SidebarFolderTree from './SidebarFolderTree';
import GridVirtual from './GridVirtual';
import Filmstrip from './Filmstrip';
import SearchBar, { FilterChips } from './SearchBar';
import PhotoInspector from './PhotoInspector';

export default function LibraryPage() {
  const total = useLibrary((s) => s.total);
  const selectedIds = useLibrary((s) => s.selectedIds);
  const select = useLibrary((s) => s.select);
  const currentPhotoId = useLibrary((s) => s.currentPhotoId);
  const setCurrent = useLibrary((s) => s.setCurrent);
  const clearSelection = useLibrary((s) => s.clearSelection);
  const refresh = useLibrary((s) => s.refresh);
  const stats = useLibrary((s) => s.stats);
  const lastError = useLibrary((s) => s.lastError);
  const filter = useLibrary((s) => s.filter);
  const searchText = useLibrary((s) => s.searchText);
  const hasFilter = Object.keys(filter).length > 0 || searchText.trim() !== '';
  const navigate = useUi((s) => s.navigate);
  const addUndoToast = useUndoToast();
  const [confirmDelete, setConfirmDelete] = useState(false);
  const requestSearchFocusTick = useUi((s) => s.searchFocusTick);
  void requestSearchFocusTick;

  // 展开为按当前排序的已加载条目（供键盘导航 / Shift 范围选）
  const flatItems = useMemo(() => {
    const st = useLibrary.getState();
    const pages = st.pages;
    const maxOffset = Object.keys(pages)
      .map(Number)
      .sort((a, b) => a - b)
      .pop();
    if (maxOffset === undefined) return [];
    const out: (typeof pages)[number] = [];
    for (let off = 0; off <= maxOffset; off += 1) {
      const page = pages[off];
      if (page) out.push(...page);
    }
    return out;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [useLibrary((s) => s.pages), total]);

  // 键盘流（PRD §5-①：方向键 / Enter / Delete / Ctrl+A / Esc）
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA') return;
      const st = useLibrary.getState();
      const items = flatItems;
      if (items.length === 0) return;
      const idx = items.findIndex((p) => p.id === st.currentPhotoId);
      if (e.key === 'ArrowRight' || e.key === 'ArrowDown') {
        e.preventDefault();
        const next = items[Math.min(items.length - 1, Math.max(0, idx) + (e.key === 'ArrowRight' ? 1 : 5))];
        if (next) (e.shiftKey ? select(next.id, 'range', items) : setCurrent(next.id));
      } else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') {
        e.preventDefault();
        const next = items[Math.max(0, Math.max(0, idx) - (e.key === 'ArrowLeft' ? 1 : 5))];
        if (next) (e.shiftKey ? select(next.id, 'range', items) : setCurrent(next.id));
      } else if (e.key === 'Enter') {
        if (st.currentPhotoId !== null) {
          // G9 文档 Tab：多选时把选中的一起开成文档（当前照片排第一），
          // 实现「接力修图」——Ctrl+Tab 在已开文档间切换。单选仍是打开单张。
          const sel = st.selectedIds;
          const ids =
            sel.length > 1 && sel.includes(st.currentPhotoId)
              ? [st.currentPhotoId, ...sel.filter((id) => id !== st.currentPhotoId)]
              : [st.currentPhotoId];
          const capped = ids.slice(0, MAX_DEVELOP_TABS);
          void useDevelop.getState().openTabs(capped);
          if (capped.length > 1) {
            useJobStore
              .getState()
              .toast(
                `已打开 ${capped.length} 个文档：Ctrl+Tab 切换、Ctrl+W 关闭${ids.length > capped.length ? `（选中超 ${MAX_DEVELOP_TABS} 张，余下未打开）` : ''}`,
                'info',
                4500,
              );
          }
          navigate('develop');
        }
      } else if (e.key === 'Delete' || e.key === 'Backspace') {
        if (st.selectedIds.length > 0) setConfirmDelete(true);
      } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a') {
        e.preventDefault();
        select(items[0]?.id ?? 0, 'all', items);
      } else if (e.key === 'Escape') {
        clearSelection();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [flatItems, select, setCurrent, clearSelection, navigate]);

  const doDelete = async () => {
    const ids = [...useLibrary.getState().selectedIds];
    setConfirmDelete(false);
    if (ids.length === 0) return;
    try {
      await api.library.removePhotos(ids);
      // 5s 撤销窗口（PRD §6.3 + §5-① Delete 验收）
      addUndoToast(`已移除 ${ids.length} 张（仅移索引，原片与 sidecar 不动）`, async () => {
        await api.library.restorePhotos(ids);
        await refresh();
      });
      clearSelection();
      await refresh();
    } catch (err) {
      useJobStore.getState().toast(`移除失败：${errText(err)}`, 'error');
    }
  };

  return (
    <div className="h-full flex flex-col">
      <div className="flex-1 min-h-0 flex">
        {/* 左侧栏 */}
        <aside className="w-56 shrink-0 border-r border-line flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
          <SidebarFolderTree />
        </aside>

        {/* 中部 */}
        <section className="flex-1 min-w-0 flex flex-col">
          <div className="px-3 pt-2.5 pb-1 shrink-0" style={{ background: 'var(--Lumina-Tokens-browserBar)' }}>
            <SearchBar />
            <FilterChips />
          </div>
          <div className="relative flex-1 min-h-0">
            {lastError ? (
              <StateError
                title="图库加载失败"
                error={lastError}
                onRetry={() => {
                  useLibrary.setState({ lastError: null, pages: {}, total: 0 });
                  void refresh();
                }}
              />
            ) : total === 0 && stats !== null ? (
              hasFilter ? (
                <StateEmpty
                  icon="search"
                  title="无匹配照片"
                  desc="当前筛选/搜索条件下没有照片。"
                  action={<Button onClick={() => void useLibrary.getState().clearFilter()}>清除筛选</Button>}
                />
              ) : (
                <StateEmpty
                  icon="folder"
                  title="素材库还是空的"
                  desc="从存储卡 / 文件夹导入照片开始管理；原片永远只读，所有调整都写入 sidecar。"
                  action={<Button variant="primary" onClick={() => navigate('ingest')}>导入照片</Button>}
                />
              )
            ) : total === 0 && stats === null ? (
              <StateLoading label="库加载中…" desc="正在读取目录树与统计信息" />
            ) : (
              <GridVirtual />
            )}
          </div>
          <Filmstrip items={flatItems} />
        </section>

        {/* 右侧检查器 */}
        <aside className="w-[276px] shrink-0 border-l border-line overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
          <PhotoInspector />
        </aside>
      </div>

      {/* 底部状态条 */}
      <div className="flex items-center h-8 px-3 gap-3 border-t border-line text-2xs text-txt3 shrink-0" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <span className="num">{stats ? `${formatCount(stats.totalPhotos)} 张` : '…'}</span>
        {selectedIds.length > 1 && <span className="text-accent">已选 {selectedIds.length} 张</span>}
        <div className="flex-1" />
        <span>单击选中 · Ctrl 多选 · Shift 范围 · Enter 修图 · Delete 移除</span>
      </div>

      <Dialog open={confirmDelete} title="移除照片" onClose={() => setConfirmDelete(false)}>
        <div className="text-xs text-txt2 leading-5">
          将从索引移除 <span className="num text-txt">{selectedIds.length}</span> 张照片。
          <br />
          原片与 XMP sidecar 不会被改动；5 秒内可撤销。
        </div>
        <div className="flex justify-end gap-2 mt-4">
          <Button onClick={() => setConfirmDelete(false)}>取消</Button>
          <Button variant="danger" onClick={() => void doDelete()}>
            移除
          </Button>
        </div>
      </Dialog>
      <KeycapBarHidden />
    </div>
  );
}

function KeycapBarHidden() {
  // 键帽提示条（与底部状态条合并，此处保留组件位以便 M2 快捷键总表扩展）
  void KeycapBar;
  return null;
}
